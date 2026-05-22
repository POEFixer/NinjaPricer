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
#include <random>

#include <Windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#include "../lib/nlohmann/json.hpp"
#include "IPriceSource.h"  // PriceApi::LogFunc

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
// HTTP Client (WinINet) — browser-like headers, status-aware, retry+backoff
// ============================================================================

// Compute backoff sleep with ±25% jitter. base is in milliseconds.
// Uses a thread-local mt19937 to avoid clobbering the process-wide CRT rand state.
inline int JitteredSleepMs(int baseMs) {
    if (baseMs <= 0) return 0;
    int range = baseMs / 2;  // span = ±range/2 = ±25% of baseMs
    if (range < 2) return baseMs;
    thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(-range / 2, range / 2);
    int v = baseMs + dist(rng);
    return (v < 100) ? 100 : v;
}

// Forward declaration of the full-featured overload.
inline std::string HttpGet(const std::string& url, PriceApi::LogFunc logFn);

// Backward-compatible overload: no diagnostic logging.
inline std::string HttpGet(const std::string& url) {
    return HttpGet(url, nullptr);
}

// Browser-fingerprint HTTP GET with up to 3 attempts.
// On 200-299: returns body. On unrecoverable error or after 3 attempts: returns "".
// Retries on: connect/send/receive timeout, network errors, HTTP 429/503/5xx.
// Honors Retry-After header (capped at 30 sec) on 429/503.
inline std::string HttpGet(const std::string& url, PriceApi::LogFunc logFn) {
    constexpr int kMaxAttempts = 3;
    constexpr DWORD kConnectTimeoutMs = 10000;
    constexpr DWORD kSendTimeoutMs    = 10000;
    constexpr DWORD kReceiveTimeoutMs = 15000;
    constexpr int   kRetryAfterCapMs  = 30000;

    // --- Parse URL ---
    URL_COMPONENTSA urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    char hostBuf[256] = {};
    char pathBuf[2048] = {};
    urlComp.lpszHostName    = hostBuf;
    urlComp.dwHostNameLength = sizeof(hostBuf);
    urlComp.lpszUrlPath     = pathBuf;
    urlComp.dwUrlPathLength = sizeof(pathBuf);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComp)) {
        if (logFn) logFn("Error", "[NinjaPricer/HTTP] failed to parse URL");
        return "";
    }

    const bool isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
    const INTERNET_PORT port = urlComp.nPort ? urlComp.nPort : (isHttps ? 443 : 80);

    // Log path (no query string)
    std::string logPath = pathBuf;
    size_t qpos = logPath.find('?');
    if (qpos != std::string::npos) logPath.erase(qpos);

    // Referer = scheme + host + "/"
    char referer[512];
    snprintf(referer, sizeof(referer), "%s://%s/",
             isHttps ? "https" : "http", hostBuf);

    static const char* kUserAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Safari/537.36";

    static const char* kBaseHeaders =
        "Accept: application/json, text/plain, */*\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: keep-alive\r\n";

    HINTERNET hInternet = InternetOpenA(
        kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        if (logFn) logFn("Error", "[NinjaPricer/HTTP] InternetOpenA failed");
        return "";
    }
    DWORD connectTimeout = kConnectTimeoutMs;
    DWORD sendTimeout    = kSendTimeoutMs;
    DWORD receiveTimeout = kReceiveTimeoutMs;
    BOOL  optOk = TRUE;
    optOk &= InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT,
                                &connectTimeout, sizeof(connectTimeout));
    optOk &= InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT,
                                &sendTimeout, sizeof(sendTimeout));
    optOk &= InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                                &receiveTimeout, sizeof(receiveTimeout));
    if (!optOk && logFn) {
        logFn("Warning", "[NinjaPricer/HTTP] could not apply all timeouts (network may hang longer than configured)");
    }

    std::string lastError = "unknown";

    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        std::string attemptError = "unknown";
        bool shouldRetry = false;
        int explicitSleepMs = 0;  // set when Retry-After was parsed

        HINTERNET hConnect = InternetConnectA(
            hInternet, hostBuf, port, NULL, NULL,
            INTERNET_SERVICE_HTTP, 0, 0);

        if (!hConnect) {
            attemptError = "connect failed";
            shouldRetry  = true;
        } else {
            DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
            if (isHttps) flags |= INTERNET_FLAG_SECURE;

            const char* acceptTypes[] = { "*/*", NULL };
            HINTERNET hRequest = HttpOpenRequestA(
                hConnect, "GET", pathBuf, NULL, referer,
                acceptTypes, flags, 0);

            if (!hRequest) {
                attemptError = "open request failed";
                shouldRetry  = true;
            } else {
                // Enable transparent decompression of gzip/deflate response bodies.
                // Note: no INTERNET_FLAG_HTTP_DECODING exists in this SDK — use option
                // form (INTERNET_OPTION_HTTP_DECODING) which is the documented modern
                // equivalent. Per MSDN this option takes a BOOL, not DWORD.
                BOOL decodeOn = TRUE;
                if (!InternetSetOptionA(hRequest, INTERNET_OPTION_HTTP_DECODING,
                                        &decodeOn, sizeof(decodeOn)) && logFn) {
                    logFn("Warning", "[NinjaPricer/HTTP] gzip decoding option not supported by WinINet");
                }

                // Add base browser headers (Accept/Lang/Encoding/Connection)
                BOOL hdrOk = TRUE;
                hdrOk &= HttpAddRequestHeadersA(hRequest, kBaseHeaders, static_cast<DWORD>(-1),
                                                HTTP_ADDREQ_FLAG_ADD);

                // Add Referer (dynamic per URL)
                char refererHeader[600];
                snprintf(refererHeader, sizeof(refererHeader),
                         "Referer: %s\r\n", referer);
                hdrOk &= HttpAddRequestHeadersA(hRequest, refererHeader, static_cast<DWORD>(-1),
                                                HTTP_ADDREQ_FLAG_ADD);
                if (!hdrOk && logFn) {
                    logFn("Warning", "[NinjaPricer/HTTP] could not add browser headers (request may look non-browser)");
                }

                BOOL sent = HttpSendRequestA(hRequest, NULL, 0, NULL, 0);
                if (!sent) {
                    DWORD err = GetLastError();
                    if      (err == ERROR_INTERNET_TIMEOUT)        attemptError = "timeout";
                    else if (err == ERROR_INTERNET_CANNOT_CONNECT) attemptError = "cannot connect";
                    else                                           attemptError = "send failed";
                    shouldRetry = true;
                } else {
                    // Query HTTP status code. Treat query failure as a transient error
                    // (retryable) rather than letting statusCode=0 fall into the
                    // "permanent 4xx" branch below.
                    DWORD statusCode = 0;
                    DWORD statusSize = sizeof(statusCode);
                    BOOL  statusOk = HttpQueryInfoA(hRequest,
                        HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &statusCode, &statusSize, NULL);
                    if (!statusOk) {
                        attemptError = "status query failed";
                        shouldRetry = true;
                        InternetCloseHandle(hRequest);
                        InternetCloseHandle(hConnect);
                        goto attempt_end;
                    }

                    if (statusCode >= 200 && statusCode < 300) {
                        // --- SUCCESS: read body and return ---
                        std::string result;
                        result.reserve(32768);
                        char buffer[8192];
                        DWORD bytesRead = 0;
                        while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead)
                               && bytesRead > 0) {
                            result.append(buffer, bytesRead);
                            bytesRead = 0;
                        }
                        InternetCloseHandle(hRequest);
                        InternetCloseHandle(hConnect);
                        InternetCloseHandle(hInternet);

                        if (attempt > 1 && logFn) {
                            char buf[160];
                            snprintf(buf, sizeof(buf),
                                "[NinjaPricer/HTTP] %s: OK on attempt %d",
                                logPath.c_str(), attempt);
                            logFn("Info", buf);
                        }
                        return result;
                    }

                    // --- Non-2xx ---
                    if (statusCode == 429 || statusCode == 503) {
                        // Parse Retry-After
                        char retryAfter[32] = {};
                        DWORD raSize = sizeof(retryAfter);
                        if (HttpQueryInfoA(hRequest, HTTP_QUERY_RETRY_AFTER,
                                           retryAfter, &raSize, NULL)) {
                            int secs = atoi(retryAfter);
                            if (secs > 0 && secs < 600) {
                                int ms = secs * 1000;
                                explicitSleepMs = (ms < kRetryAfterCapMs) ? ms : kRetryAfterCapMs;
                            }
                        }
                        if (logFn) {
                            char buf[200];
                            snprintf(buf, sizeof(buf),
                                "[NinjaPricer/HTTP] %s: %lu rate-limited, Retry-After=%s",
                                logPath.c_str(), statusCode,
                                retryAfter[0] ? retryAfter : "(none)");
                            logFn("Warning", buf);
                        }
                        shouldRetry = true;
                    } else if (statusCode >= 500 && statusCode < 600) {
                        shouldRetry = true;
                    } else {
                        // 4xx (other) — not retryable
                        shouldRetry = false;
                    }

                    {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "HTTP %lu", statusCode);
                        attemptError = buf;
                    }
                }
                InternetCloseHandle(hRequest);
            }
            InternetCloseHandle(hConnect);
        }

    attempt_end:
        lastError = attemptError;

        if (!shouldRetry) {
            // Permanent failure — log and bail
            char buf[200];
            snprintf(buf, sizeof(buf),
                "[NinjaPricer/HTTP] %s: %s (no retry)",
                logPath.c_str(), attemptError.c_str());
            if (logFn) logFn("Error", buf);
            InternetCloseHandle(hInternet);
            return "";
        }

        if (attempt < kMaxAttempts) {
            int sleepMs = (explicitSleepMs > 0)
                ? explicitSleepMs
                : JitteredSleepMs(1000 * (1 << (attempt - 1)));  // 1s, 2s
            if (logFn) {
                char buf[200];
                snprintf(buf, sizeof(buf),
                    "[NinjaPricer/HTTP] %s: attempt %d failed (%s), retry in %dms",
                    logPath.c_str(), attempt, attemptError.c_str(), sleepMs);
                logFn("Info", buf);
            }
            Sleep(static_cast<DWORD>(sleepMs));
        }
    }

    InternetCloseHandle(hInternet);
    if (logFn) {
        char buf[200];
        snprintf(buf, sizeof(buf),
            "[NinjaPricer/HTTP] %s: failed after %d attempts (last: %s)",
            logPath.c_str(), kMaxAttempts, lastError.c_str());
        logFn("Error", buf);
    }
    return "";
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
