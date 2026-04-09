#pragma once
// ============================================================================
// NinjaApi.h — Utility functions: HTTP, league discovery, caching, conversion
// ============================================================================
// Shared helpers used by price-source implementations.
// All functions are inline / header-only.
// ============================================================================

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>

#include <Windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#include "../lib/nlohmann/json.hpp"

namespace NinjaApi {

using json = nlohmann::json;

// ============================================================================
// String Utilities
// ============================================================================

inline std::string WideToNarrow(const std::wstring& wide) {
    if (wide.empty()) return "";
    std::string result;
    result.reserve(wide.size());
    for (wchar_t c : wide) {
        result += (c < 128) ? static_cast<char>(c) : '?';
    }
    return result;
}

// ============================================================================
// HTTP Client (WinINet)
// ============================================================================

inline std::string HttpGet(const std::string& url) {
    HINTERNET hInternet = InternetOpenA(
        "NinjaPricer/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
    if (!hInternet) return "";

    HINTERNET hUrl = InternetOpenUrlA(
        hInternet, url.c_str(), NULL, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return "";
    }

    std::string result;
    result.reserve(32768);
    char buffer[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        result.append(buffer, bytesRead);
        bytesRead = 0;
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return result;
}

// ============================================================================
// League Discovery
// ============================================================================

// Fetch available POE2 league names from public API.
// New API: GET /api/poe2/Leagues -> [{Value: "...", ...}]
inline std::vector<std::string> FetchLeagues() {
    std::vector<std::string> leagues;

    std::string jsonStr = HttpGet("https://poe2scout.com/api/poe2/Leagues");
    if (!jsonStr.empty()) {
        try {
            json j = json::parse(jsonStr);
            if (j.is_array()) {
                for (auto& item : j) {
                    if (item.is_object() && item.contains("Value") && item["Value"].is_string())
                        leagues.push_back(item["Value"].get<std::string>());
                    else if (item.is_string())
                        leagues.push_back(item.get<std::string>());
                }
            }
        }
        catch (...) {}
    }

    return leagues;
}

// ============================================================================
// Cache (local JSON files)
// ============================================================================

inline void SaveCache(const std::string& dir, const std::string& league,
    const std::string& type, const std::string& jsonStr)
{
    namespace fs = std::filesystem;
    fs::path cacheDir = fs::path(dir) / "cache" / league;
    if (!fs::exists(cacheDir))
        fs::create_directories(cacheDir);

    std::ofstream f(cacheDir / (type + ".json"));
    if (f.is_open()) f << jsonStr;
}

inline std::string LoadCache(const std::string& dir, const std::string& league,
    const std::string& type)
{
    namespace fs = std::filesystem;
    fs::path cachePath = fs::path(dir) / "cache" / league / (type + ".json");
    if (!fs::exists(cachePath)) return "";

    std::ifstream f(cachePath);
    if (!f.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace NinjaApi
