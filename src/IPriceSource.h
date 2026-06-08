#pragma once
// IPriceSource.h — Unified price source interface + shared types
// Extracted from NinjaApi.h to support multiple data sources.

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <filesystem>

// Forward declare ImGui types (avoid pulling full imgui.h in header)
#ifndef IM_COL32
#define IM_COL32(R,G,B,A) (((unsigned int)(A)<<24) | ((unsigned int)(B)<<16) | ((unsigned int)(G)<<8) | ((unsigned int)(R)))
#endif
using ImU32 = unsigned int;

namespace PriceApi {

// ============================================================================
// Constants
// ============================================================================

inline constexpr int kMaxCurrencyCategories = 17;
inline constexpr int kMaxUniqueCategories = 7;

// ============================================================================
// Enums
// ============================================================================

enum class DisplayCurrency : int {
    Divine = 0,
    Exalted = 1,
    Chaos = 2,
};

// ============================================================================
// Data Structures
// ============================================================================

struct PriceItem {
    std::string name;           // display name (e.g., "Chaos Orb", "Temporalis")
    std::string basetype;       // base type (for uniques: "Silk Robe", for currency: = name)
    float chaosValue = 0.0f;    // normalized price in chaos
    std::string category;       // "currency", "accessory", "armour", etc.
    std::string iconUrl;        // icon URL (optional, for future use)
};

struct PriceDatabase {
    std::unordered_map<std::string, PriceItem> items; // lowercase name -> PriceItem
    float divineInChaos = 1.0f;    // 1 divine = X chaos
    float exaltedInChaos = 1.0f;   // 1 exalted = X chaos
    std::chrono::steady_clock::time_point lastUpdate;
    int totalItems = 0;
    bool loaded = false;
};

struct PriceLookupResult {
    bool found = false;
    float chaosValue = 0.0f;
    float divineValue = 0.0f;
    float exaltedValue = 0.0f;
    std::string itemName;
    std::string category;       // category of matched item (e.g., "currency", "armour")
};

struct CategoryDef {
    const char* apiId;        // API query parameter (source-specific)
    const char* label;        // User-facing display name
};

// ============================================================================
// Category Arrays
// ============================================================================

// Currency categories — indices 0-12 match existing kCategories[] order.
// Indices 13-16 are poe2scout-only.
// Label is the user-facing name shown in the UI.
inline constexpr CategoryDef kCurrencyCategories[] = {
    { "Currency",          "Currency" },           // 0
    { "Fragments",         "Fragments" },          // 1
    { "Abyss",             "Abyssal Bones" },      // 2
    { "UncutGems",         "Uncut Gems" },         // 3
    { "Essences",          "Essences" },           // 4
    { "SoulCores",         "Soul Cores" },         // 5
    { "Idols",             "Idols" },              // 6
    { "Runes",             "Runes" },              // 7
    { "Ritual",            "Ritual Omens" },       // 8
    { "Expedition",        "Expedition" },         // 9
    { "Delirium",          "Delirium" },           // 10
    { "Breach",            "Breach" },             // 11
    { "LineageSupportGems","Lineage Support Gems" },// 12
    { "vaultkeys",         "Reliquary Keys" },     // 13 — poe2scout only
    { "incursion",         "Incursion" },          // 14 — poe2scout only
    { "verisium",          "Verisium" },           // 15 — poe2scout only
    { "vaal",              "Vaal" },               // 16 — poe2scout only
};

// Unique categories — all poe2scout only
inline constexpr CategoryDef kUniqueCategories[] = {
    { "accessory",  "Accessories" },    // 0
    { "armour",     "Armour" },         // 1
    { "flask",      "Flasks" },         // 2
    { "jewel",      "Jewels" },         // 3
    { "map",        "Maps" },           // 4
    { "weapon",     "Weapons" },        // 5
    { "sanctum",    "Sanctum Research" },// 6
};

// poe.ninja uses different API type names for some categories.
// kCurrencyCategories[].apiId stores poe.ninja names for indices 0-12.
// poe2scout API IDs for the same categories (used by ScoutSource):
inline constexpr const char* kScoutCurrencyApiIds[] = {
    "currency",            // 0 — Currency
    "fragments",           // 1 — Fragments
    "abyss",               // 2 — Abyssal Bones
    "uncutgems",           // 3 — Uncut Gems
    "essences",            // 4 — Essences
    "ultimatum",           // 5 — Soul Cores
    "idol",                // 6 — Idols
    "runes",               // 7 — Runes
    "ritual",              // 8 — Ritual Omens
    "expedition",          // 9 — Expedition
    "delirium",            // 10 — Delirium
    "breach",              // 11 — Breach
    "lineagesupportgems",  // 12 — Lineage Support Gems
    "vaultkeys",           // 13 — Reliquary Keys (poe2scout only)
    "incursion",           // 14 — Incursion (poe2scout only)
    "verisium",            // 15 — Verisium (poe2scout only)
    "vaal",                // 16 — Vaal (poe2scout only)
};

// First index that is poe2scout-only (currency categories)
inline constexpr int kScoutOnlyCurrencyStart = 13;

// ============================================================================
// String / Display Helpers
// ============================================================================

inline std::string ToLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

inline const char* GetCurrencySuffix(DisplayCurrency c) {
    switch (c) {
    case DisplayCurrency::Divine:  return "D";
    case DisplayCurrency::Exalted: return "E";
    case DisplayCurrency::Chaos:   return "C";
    default: return "?";
    }
}

inline const char* GetCurrencyName(DisplayCurrency c) {
    switch (c) {
    case DisplayCurrency::Divine:  return "Divine";
    case DisplayCurrency::Exalted: return "Exalted";
    case DisplayCurrency::Chaos:   return "Chaos";
    default: return "Unknown";
    }
}

inline std::string FormatPrice(float value, DisplayCurrency currency) {
    char buf[32];
    const char* suffix = GetCurrencySuffix(currency);

    if (value >= 10000.0f) {
        float k = value / 1000.0f;
        if (k >= 10.0f)
            snprintf(buf, sizeof(buf), "%.0fk %s", k, suffix);
        else
            snprintf(buf, sizeof(buf), "%.1fk %s", k, suffix);
    }
    else if (value >= 1000.0f) {
        snprintf(buf, sizeof(buf), "%.1fk %s", value / 1000.0f, suffix);
    }
    else if (value >= 100.0f) {
        snprintf(buf, sizeof(buf), "%.0f %s", value, suffix);
    }
    else if (value >= 1.0f) {
        snprintf(buf, sizeof(buf), "%.1f %s", value, suffix);
    }
    else if (value >= 0.01f) {
        snprintf(buf, sizeof(buf), "%.2f %s", value, suffix);
    }
    else {
        snprintf(buf, sizeof(buf), "%.3f %s", value, suffix);
    }
    return buf;
}

// Same number formatting as FormatPrice but WITHOUT the currency-letter suffix.
// Used by the image price-display style, where the currency is shown as an icon.
inline std::string FormatPriceNumber(float value) {
    char buf[32];
    if (value >= 10000.0f) {
        float k = value / 1000.0f;
        if (k >= 10.0f) snprintf(buf, sizeof(buf), "%.0fk", k);
        else            snprintf(buf, sizeof(buf), "%.1fk", k);
    }
    else if (value >= 1000.0f) snprintf(buf, sizeof(buf), "%.1fk", value / 1000.0f);
    else if (value >= 100.0f)  snprintf(buf, sizeof(buf), "%.0f", value);
    else if (value >= 1.0f)    snprintf(buf, sizeof(buf), "%.1f", value);
    else if (value >= 0.01f)   snprintf(buf, sizeof(buf), "%.2f", value);
    else                       snprintf(buf, sizeof(buf), "%.3f", value);
    return buf;
}

// ============================================================================
// Price Lookup (chaos-based)
// ============================================================================

inline PriceLookupResult MakeResult(const PriceDatabase& db, const PriceItem& item) {
    PriceLookupResult result;
    result.found = true;
    result.itemName = item.name;
    result.category = item.category;
    result.chaosValue = item.chaosValue;
    result.divineValue = (db.divineInChaos > 0) ? item.chaosValue / db.divineInChaos : 0.0f;
    result.exaltedValue = (db.exaltedInChaos > 0) ? item.chaosValue / db.exaltedInChaos : 0.0f;
    return result;
}

// Check if a category belongs to unique item categories
inline bool IsUniqueCategory(const std::string& category) {
    for (int i = 0; i < kMaxUniqueCategories; i++) {
        if (category == kUniqueCategories[i].apiId)
            return true;
    }
    return false;
}

// outMatchKind (optional, diagnostic): 0 = no match, 1 = exact O(1) hashmap hit,
// 2 = contains-match (the O(N) full-DB substring scan). NinjaPricer's perf panel
// uses this to count how often the expensive contains path runs per frame.
inline PriceLookupResult LookupPrice(const PriceDatabase& db, const std::string& itemName,
                                     int* outMatchKind = nullptr) {
    std::string key = ToLower(itemName);

    // 1. Exact match (fast, O(1))
    auto it = db.items.find(key);
    if (it != db.items.end()) {
        if (outMatchKind) *outMatchKind = 1;
        return MakeResult(db, it->second);
    }

    // 2. Contains match: if the game's display name contains a db key
    //    e.g. "headhunter heavy belt" contains "headhunter"
    //    Only check unique items (category != "currency") to avoid false positives
    if (key.size() > 3) {
        const PriceItem* bestMatch = nullptr;
        size_t bestLen = 0;
        for (const auto& [dbKey, item] : db.items) {
            if (item.category == "currency") continue;
            if (dbKey.size() <= bestLen) continue;
            if (key.find(dbKey) != std::string::npos) {
                bestMatch = &item;
                bestLen = dbKey.size();
            }
        }
        if (bestMatch) {
            if (outMatchKind) *outMatchKind = 2;
            return MakeResult(db, *bestMatch);
        }
    }

    if (outMatchKind) *outMatchKind = 0;
    return PriceLookupResult{};
}

inline float GetDisplayValue(const PriceLookupResult& r, DisplayCurrency c) {
    switch (c) {
    case DisplayCurrency::Divine:  return r.divineValue;
    case DisplayCurrency::Exalted: return r.exaltedValue;
    case DisplayCurrency::Chaos:   return r.chaosValue;
    default: return r.chaosValue;
    }
}

inline ImU32 GetPriceColor(float chaosValue, float divineInChaos) {
    float divEquiv = (divineInChaos > 0) ? chaosValue / divineInChaos : 0.0f;
    if (divEquiv >= 1.0f)
        return IM_COL32(255, 215, 0, 255);     // Gold
    if (divEquiv >= 0.1f)
        return IM_COL32(255, 255, 255, 255);    // White
    return IM_COL32(180, 180, 180, 255);         // Gray
}

// ============================================================================
// IPriceSource Interface
// ============================================================================

using LogFunc = void(*)(const char* level, const char* message);

class IPriceSource {
public:
    virtual ~IPriceSource() = default;
    virtual const char* GetName() const = 0;  // "poe.ninja" or "poe2scout"
    // cacheDir is std::filesystem::path (not std::string) so the wide
    // representation survives end-to-end without going through the system
    // ANSI codepage. Passing fs::path(Directory()) on Windows would
    // misinterpret UTF-8 as CP_ACP and create mojibake folders next to the
    // real plugin directory when the exe lives in a non-ASCII path.
    virtual void FetchCategories(
        const std::string& league,
        const std::filesystem::path& cacheDir,
        const bool (&currencyEnabled)[kMaxCurrencyCategories],
        const bool (&uniqueEnabled)[kMaxUniqueCategories],
        PriceDatabase& db,
        std::atomic<bool>& isLoading,
        std::atomic<bool>& running,
        LogFunc logFn) = 0;
};

} // namespace PriceApi
