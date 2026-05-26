#pragma once
// NinjaSource.h — poe.ninja Exchange API implementation of IPriceSource

#include "IPriceSource.h"
#include "NinjaApi.h"  // HttpGet, SaveCache, LoadCache
#include "../lib/nlohmann/json.hpp"
#include <Windows.h>

namespace PriceApi {

class NinjaSource : public IPriceSource {
public:
    const char* GetName() const override { return "poe.ninja"; }

    void FetchCategories(
        const std::string& league,
        const std::filesystem::path& cacheDir,
        const bool (&currencyEnabled)[kMaxCurrencyCategories],
        const bool (&uniqueEnabled)[kMaxUniqueCategories],
        PriceDatabase& db,
        std::atomic<bool>& isLoading,
        std::atomic<bool>& running,
        LogFunc logFn) override
    {
        (void)uniqueEnabled; // poe.ninja has no unique items

        isLoading.store(true);
        db = PriceDatabase{};

        std::string encodedLeague = league;
        for (auto& c : encodedLeague) {
            if (c == ' ') c = '+';
        }
        std::string baseUrl =
            "https://poe.ninja/poe2/api/economy/exchange/current/overview?league="
            + encodedLeague + "&type=";

        bool ratesParsed = false;

        // Only fetch indices 0..12 (poe.ninja categories)
        for (int i = 0; i < 13; i++) {
            if (!running.load()) break;
            if (!currencyEnabled[i]) continue;

            std::string fullUrl = baseUrl + kCurrencyCategories[i].apiId;
            std::string jsonStr = NinjaApi::HttpGet(fullUrl, logFn);

            bool ok = false;
            if (!jsonStr.empty()) {
                ok = ParseResponse(jsonStr, db, !ratesParsed);
                if (ok) {
                    ratesParsed = true;
                    NinjaApi::SaveCache(cacheDir, league,
                        kCurrencyCategories[i].apiId, jsonStr);
                }
            }

            // Fallback to cache
            if (!ok) {
                jsonStr = NinjaApi::LoadCache(cacheDir, league,
                    kCurrencyCategories[i].apiId);
                if (!jsonStr.empty()) {
                    ok = ParseResponse(jsonStr, db, !ratesParsed);
                    if (ok && !ratesParsed) ratesParsed = true;
                }
            }

            if (logFn) {
                char msg[256];
                snprintf(msg, sizeof(msg), "[NinjaPricer] %s: %s (%d items total)",
                    kCurrencyCategories[i].label, ok ? "OK" : "FAILED", db.totalItems);
                logFn("Info", msg);
            }

            if (i < 12 && running.load()) Sleep(150);
        }

        db.lastUpdate = std::chrono::steady_clock::now();
        db.loaded = true;
        isLoading.store(false);
    }

private:
    // Parse poe.ninja Exchange API response.
    // Extracts conversion rates from "core.rates" (first successful response).
    // Stores prices normalized to chaos.
    bool ParseResponse(const std::string& jsonStr, PriceDatabase& db, bool parseCoreRates) {
        try {
            using json = nlohmann::json;
            json j = json::parse(jsonStr);

            // Parse conversion rates
            if (parseCoreRates && j.contains("core") && j["core"].is_object()) {
                auto& core = j["core"];
                if (core.contains("rates") && core["rates"].is_object()) {
                    auto& rates = core["rates"];
                    float chaos = 1.0f, exalted = 1.0f;
                    if (rates.contains("chaos") && rates["chaos"].is_number())
                        chaos = rates["chaos"].get<float>();
                    if (rates.contains("exalted") && rates["exalted"].is_number())
                        exalted = rates["exalted"].get<float>();

                    db.divineInChaos = chaos;                            // e.g. 244.7
                    db.exaltedInChaos = (exalted > 0) ? chaos / exalted : 1.0f; // e.g. 30.6
                }
            }

            // Build id->name map from "items" array
            std::unordered_map<std::string, std::string> idToName;
            if (j.contains("items") && j["items"].is_array()) {
                for (auto& item : j["items"]) {
                    if (!item.contains("id") || !item.contains("name")) continue;
                    idToName[item["id"].get<std::string>()] = item["name"].get<std::string>();
                }
            }

            // Parse "lines" array for prices
            if (j.contains("lines") && j["lines"].is_array()) {
                for (auto& line : j["lines"]) {
                    if (!line.contains("id")) continue;
                    std::string id = line["id"].get<std::string>();
                    float divineValue = line.value("primaryValue", 0.0f);

                    auto it = idToName.find(id);
                    if (it == idToName.end()) continue;

                    PriceItem pi;
                    pi.name = it->second;
                    pi.basetype = pi.name;  // currency: basetype = name
                    pi.chaosValue = divineValue * db.divineInChaos;
                    pi.category = "currency";

                    std::string key = ToLower(pi.name);
                    db.items[key] = std::move(pi);
                    db.totalItems++;
                }
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }
};

} // namespace PriceApi
