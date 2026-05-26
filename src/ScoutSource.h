#pragma once
// ScoutSource.h — poe2scout.com IPriceSource implementation

#include "IPriceSource.h"
#include "NinjaApi.h"  // HttpGet, SaveCache, LoadCache
#include "../lib/nlohmann/json.hpp"
#include <Windows.h>

namespace PriceApi {

class ScoutSource : public IPriceSource {
public:
    const char* GetName() const override { return "poe2scout"; }

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
        isLoading.store(true);
        db = PriceDatabase{};

        std::string encodedLeague;
        encodedLeague.reserve(league.size() * 3);
        for (char c : league) {
            if (c == ' ') encodedLeague += "%20";
            else encodedLeague += c;
        }

        // --- Currency categories ---
        for (int i = 0; i < kMaxCurrencyCategories; i++) {
            if (!running.load()) break;
            if (!currencyEnabled[i]) continue;

            const char* apiId = kScoutCurrencyApiIds[i];
            FetchCurrencyCategory(apiId, encodedLeague, cacheDir, league, db, running, logFn);

            if (running.load()) Sleep(150);
        }

        // Extract conversion rates from fetched currency data
        ExtractConversionRates(db);

        // --- Unique categories ---
        for (int i = 0; i < kMaxUniqueCategories; i++) {
            if (!running.load()) break;
            if (!uniqueEnabled[i]) continue;

            const char* apiId = kUniqueCategories[i].apiId;
            FetchUniqueCategory(apiId, encodedLeague, cacheDir, league, db, running, logFn);

            if (running.load()) Sleep(150);
        }

        db.lastUpdate = std::chrono::steady_clock::now();
        db.loaded = true;
        isLoading.store(false);
    }

private:
    void FetchCurrencyCategory(
        const char* apiId,
        const std::string& encodedLeague,
        const std::filesystem::path& cacheDir,
        const std::string& league,
        PriceDatabase& db,
        std::atomic<bool>& running,
        LogFunc logFn)
    {
        std::string cacheKey = std::string("scout_") + apiId;
        int itemsBefore = db.totalItems;

        // Fetch page 1
        std::string url = std::string("https://poe2scout.com/api/poe2/Leagues/")
            + encodedLeague + "/Currencies/ByCategory?Category=" + apiId
            + "&ReferenceCurrency=chaos&PerPage=250";

        std::string jsonStr = NinjaApi::HttpGet(url, logFn);
        bool ok = false;

        if (!jsonStr.empty()) {
            int totalPages = 1;
            ok = ParseCurrencyPage(jsonStr, db, totalPages);
            if (ok) {
                NinjaApi::SaveCache(cacheDir, league, cacheKey, jsonStr);

                // Fetch remaining pages
                for (int page = 2; page <= totalPages && page <= 20 && running.load(); page++) {
                    std::string pageUrl = url + "&Page=" + std::to_string(page);
                    std::string pageJson = NinjaApi::HttpGet(pageUrl, logFn);
                    if (!pageJson.empty()) {
                        int dummy = 0;
                        ParseCurrencyPage(pageJson, db, dummy);
                    }
                    Sleep(150);
                }
            }
        }

        // Fallback to cache (page 1 only)
        if (!ok) {
            jsonStr = NinjaApi::LoadCache(cacheDir, league, cacheKey);
            if (!jsonStr.empty()) {
                int dummy = 0;
                ok = ParseCurrencyPage(jsonStr, db, dummy);
            }
        }

        if (logFn) {
            char msg[256];
            int added = db.totalItems - itemsBefore;
            snprintf(msg, sizeof(msg), "[NinjaPricer] scout/%s: %s (+%d items, %d total)",
                apiId, ok ? "OK" : "FAILED", added, db.totalItems);
            logFn("Info", msg);
        }
    }

    void FetchUniqueCategory(
        const char* apiId,
        const std::string& encodedLeague,
        const std::filesystem::path& cacheDir,
        const std::string& league,
        PriceDatabase& db,
        std::atomic<bool>& running,
        LogFunc logFn)
    {
        std::string cacheKey = std::string("scout_unique_") + apiId;
        int itemsBefore = db.totalItems;

        std::string url = std::string("https://poe2scout.com/api/poe2/Leagues/")
            + encodedLeague + "/Uniques/ByCategory?Category=" + apiId
            + "&ReferenceCurrency=chaos&PerPage=250";

        std::string jsonStr = NinjaApi::HttpGet(url, logFn);
        bool ok = false;

        if (!jsonStr.empty()) {
            int totalPages = 1;
            ok = ParseUniquePage(jsonStr, db, totalPages);
            if (ok) {
                NinjaApi::SaveCache(cacheDir, league, cacheKey, jsonStr);

                for (int page = 2; page <= totalPages && page <= 20 && running.load(); page++) {
                    std::string pageUrl = url + "&Page=" + std::to_string(page);
                    std::string pageJson = NinjaApi::HttpGet(pageUrl, logFn);
                    if (!pageJson.empty()) {
                        int dummy = 0;
                        ParseUniquePage(pageJson, db, dummy);
                    }
                    Sleep(150);
                }
            }
        }

        if (!ok) {
            jsonStr = NinjaApi::LoadCache(cacheDir, league, cacheKey);
            if (!jsonStr.empty()) {
                int dummy = 0;
                ok = ParseUniquePage(jsonStr, db, dummy);
            }
        }

        if (logFn) {
            char msg[256];
            int added = db.totalItems - itemsBefore;
            snprintf(msg, sizeof(msg), "[NinjaPricer] scout/unique/%s: %s (+%d items, %d total)",
                apiId, ok ? "OK" : "FAILED", added, db.totalItems);
            logFn("Info", msg);
        }
    }

    bool ParseCurrencyPage(const std::string& jsonStr, PriceDatabase& db, int& outTotalPages) {
        try {
            using json = nlohmann::json;
            json j = json::parse(jsonStr);

            outTotalPages = j.value("Pages", 1);

            if (!j.contains("Items") || !j["Items"].is_array()) return false;

            for (auto& item : j["Items"]) {
                float price = item.value("CurrentPrice", 0.0f);
                std::string text = item.value("Text", "");
                if (text.empty() || price <= 0.0f) continue;

                std::string catId = item.value("CategoryApiId", "currency");

                PriceItem pi;
                pi.name = text;
                pi.basetype = text;  // currency: basetype = name
                pi.chaosValue = price;
                pi.category = catId;

                std::string key = ToLower(text);
                db.items[key] = std::move(pi);
                db.totalItems++;
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool ParseUniquePage(const std::string& jsonStr, PriceDatabase& db, int& outTotalPages) {
        try {
            using json = nlohmann::json;
            json j = json::parse(jsonStr);

            outTotalPages = j.value("Pages", 1);

            if (!j.contains("Items") || !j["Items"].is_array()) return false;

            for (auto& item : j["Items"]) {
                float price = item.value("CurrentPrice", 0.0f);
                if (price <= 0.0f) continue;

                std::string uniqueName = item.value("Name", "");
                std::string baseType = item.value("Type", "");
                std::string catId = item.value("CategoryApiId", "unique");

                if (uniqueName.empty()) continue;

                PriceItem pi;
                pi.name = uniqueName;
                pi.basetype = baseType;
                pi.chaosValue = price;
                pi.category = catId;

                // Insert short name key (only if no collision with currency)
                std::string shortKey = ToLower(uniqueName);
                if (db.items.find(shortKey) == db.items.end()) {
                    db.items[shortKey] = pi;
                }

                // Insert full name key (name + basetype)
                if (!baseType.empty()) {
                    std::string fullKey = ToLower(uniqueName + " " + baseType);
                    db.items[fullKey] = pi;

                    // Insert base type key for inventory matching
                    // (ReadItemBaseTypeName returns base type like "Heavy Belt")
                    // If multiple uniques share a base type, higher price wins
                    std::string baseKey = ToLower(baseType);
                    auto baseIt = db.items.find(baseKey);
                    if (baseIt == db.items.end() || baseIt->second.chaosValue < pi.chaosValue) {
                        db.items[baseKey] = pi;
                    }
                }

                db.totalItems++;
            }

            return true;
        }
        catch (...) {
            return false;
        }
    }

    void ExtractConversionRates(PriceDatabase& db) {
        // Find Divine Orb and Exalted Orb prices (already in chaos)
        auto divIt = db.items.find("divine orb");
        if (divIt != db.items.end() && divIt->second.chaosValue > 0)
            db.divineInChaos = divIt->second.chaosValue;

        auto exIt = db.items.find("exalted orb");
        if (exIt != db.items.end() && exIt->second.chaosValue > 0)
            db.exaltedInChaos = exIt->second.chaosValue;
    }
};

} // namespace PriceApi
