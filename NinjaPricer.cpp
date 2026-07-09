// ============================================================================
// NinjaPricer — price overlay plugin for POE2 (v6 SDK)
// ============================================================================
// Displays item prices from the host PriceService on dropped items and
// in inventory. All price loading is owned by the host; this plugin only
// queries ctx()->Prices.
// ============================================================================

#include "sdk/PluginSDK.h"
#include <imgui.h>
#include <d3d11.h>

// stb_image for loading currency icon PNGs (image price-display style).
// Implementation lives in this single plugin TU.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "lib/nlohmann/json.hpp"

#include <array>
#include <fstream>
#include <filesystem>
#include <string>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>

// Host-synthesized inventory ID for the Ritual shop ("Favours" window). The
// host's ScanInventoryGrid publishes the shop grid under this ID (it is not a
// real game inventory). Keep in sync with POEFixer/game_client/GameClientData.h
// kRitualShopInventoryId.
static constexpr int kRitualShopInventoryId = 10001;

// Price text position for UI items (inventory/stash)
enum class UiPricePosition : int {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
};
static const char* kUiPositionNames[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };

// Price text position for ground items
enum class GroundPricePosition : int {
    Top = 0,
    Bottom = 1,
    Left = 2,
    Right = 3,
};
static const char* kGroundPositionNames[] = { "Top", "Bottom", "Left", "Right" };

// How the price value is rendered: as text with a currency-letter suffix
// ("2.5 D"), or as the numeric value followed by the currency icon ("2.5" + img).
enum class PriceDisplayStyle : int {
    Image = 0,   // value + currency icon (default)
    Text  = 1,   // value + letter suffix
};
static const char* kPriceDisplayStyleNames[] = { "Image (icon)", "Text" };

// Display currency choice (local to NinjaPricer; not from the deleted headers).
// Ordinal matches SDK PriceResult fields: 0=Divine, 1=Exalted, 2=Chaos.
enum class DisplayCurrency : int {
    Divine  = 0,
    Exalted = 1,
    Chaos   = 2,
};

// ============================================================================
// Unique category definitions — used by IsUniqueCategory() to gate the
// Rarity==3 check in the draw paths.
// ============================================================================

inline constexpr int kMaxUniqueCategories = 7;

struct CategoryDef { const char* apiId; const char* label; };

inline constexpr CategoryDef kUniqueCategories[] = {
    { "accessory",  "Accessories" },
    { "armour",     "Armour" },
    { "flask",      "Flasks" },
    { "jewel",      "Jewels" },
    { "map",        "Maps" },
    { "weapon",     "Weapons" },
    { "sanctum",    "Sanctum Research" },
};

// Returns true when `category` belongs to a unique-item category.
// Used to require Rarity==3 before showing a price that hit via contains-match.
static bool IsUniqueCategory(const std::string& category) {
    for (int i = 0; i < kMaxUniqueCategories; i++) {
        if (category == kUniqueCategories[i].apiId)
            return true;
    }
    return false;
}

// ============================================================================
// Local display helpers (moved from deleted IPriceSource.h)
// ============================================================================

#ifndef IM_COL32
#define IM_COL32(R,G,B,A) (((unsigned int)(A)<<24) | ((unsigned int)(B)<<16) | ((unsigned int)(G)<<8) | ((unsigned int)(R)))
#endif
using ImU32 = unsigned int;

static const char* GetCurrencySuffix(DisplayCurrency c) {
    switch (c) {
    case DisplayCurrency::Divine:  return "D";
    case DisplayCurrency::Exalted: return "E";
    case DisplayCurrency::Chaos:   return "C";
    default: return "?";
    }
}

static std::string FormatPriceLocal(float value, DisplayCurrency currency) {
    char buf[32];
    const char* suffix = GetCurrencySuffix(currency);
    if (value >= 10000.0f) {
        float k = value / 1000.0f;
        if (k >= 10.0f) snprintf(buf, sizeof(buf), "%.0fk %s", k, suffix);
        else            snprintf(buf, sizeof(buf), "%.1fk %s", k, suffix);
    } else if (value >= 1000.0f) {
        snprintf(buf, sizeof(buf), "%.1fk %s", value / 1000.0f, suffix);
    } else if (value >= 100.0f) {
        snprintf(buf, sizeof(buf), "%.0f %s", value, suffix);
    } else if (value >= 1.0f) {
        snprintf(buf, sizeof(buf), "%.1f %s", value, suffix);
    } else if (value >= 0.01f) {
        snprintf(buf, sizeof(buf), "%.2f %s", value, suffix);
    } else {
        snprintf(buf, sizeof(buf), "%.3f %s", value, suffix);
    }
    return buf;
}

static std::string FormatPriceNumberLocal(float value) {
    char buf[32];
    if (value >= 10000.0f) {
        float k = value / 1000.0f;
        if (k >= 10.0f) snprintf(buf, sizeof(buf), "%.0fk", k);
        else            snprintf(buf, sizeof(buf), "%.1fk", k);
    } else if (value >= 1000.0f) snprintf(buf, sizeof(buf), "%.1fk", value / 1000.0f);
    else if (value >= 100.0f)  snprintf(buf, sizeof(buf), "%.0f", value);
    else if (value >= 1.0f)    snprintf(buf, sizeof(buf), "%.1f", value);
    else if (value >= 0.01f)   snprintf(buf, sizeof(buf), "%.2f", value);
    else                       snprintf(buf, sizeof(buf), "%.3f", value);
    return buf;
}

static ImU32 GetPriceColorLocal(float chaosValue, float divineInChaos) {
    float divEquiv = (divineInChaos > 0) ? chaosValue / divineInChaos : 0.0f;
    if (divEquiv >= 1.0f)  return IM_COL32(255, 215, 0, 255);   // Gold
    if (divEquiv >= 0.1f)  return IM_COL32(255, 255, 255, 255); // White
    return IM_COL32(180, 180, 180, 255);                          // Gray
}

// Extract the display value from a SDK PriceResult for the chosen currency.
static float GetDisplayValue(const PluginSDK::PriceResult& r, DisplayCurrency c) {
    switch (c) {
    case DisplayCurrency::Divine:  return r.divine;
    case DisplayCurrency::Exalted: return r.exalt;
    case DisplayCurrency::Chaos:   return r.chaos;
    default: return r.chaos;
    }
}

// Virtual key name helper
static const char* GetVkName(int vk) {
    if (vk == 0) return "None";
    static char buf[64];
    UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    char keyName[32] = {};
    if (GetKeyNameTextA(static_cast<LONG>(scanCode) << 16, keyName, sizeof(keyName)) > 0)
        snprintf(buf, sizeof(buf), "%s (0x%02X)", keyName, vk);
    else
        snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

class NinjaPricerPlugin : public PluginSDK::Plugin {
public:
    ~NinjaPricerPlugin() override {
        ReleaseCurrencyTextures();
    }

    // ========================================================================
    // PluginSDK::Plugin lifecycle
    // ========================================================================

    const char* GetName() const override { return "Ninja Pricer"; }
    bool WantsOverlay() const override { return true; }

    void OnEnable(bool /*isGameAttached*/) override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        LoadSettings();
        ctx()->Log.Info("[NinjaPricer] Plugin enabled");
    }

    void OnDisable() override {
        ReleaseCurrencyTextures();
        ctx()->Log.Info("[NinjaPricer] Plugin disabled");
    }

    // ========================================================================
    // Settings UI
    // ========================================================================

    void DrawSettings() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        if (!ImGui::BeginTabBar("##NinjaPricerTabs")) return;

        if (ImGui::BeginTabItem("Data Source")) {
            DrawTabDataSource();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display Settings")) {
            DrawTabDisplaySettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overlay Toggles")) {
            DrawTabOverlayToggles();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            DrawDebugPanel();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    void DrawTabDataSource() {
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
        ImGui::TextWrapped(
            "Warning: POE2 must be set to English. Item names are matched "
            "in English only - other languages will not work."
        );
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Read-only status from the host price service.
        auto st = ctx()->Prices.GetStatus();
        ImGui::Text("Prices: %s   Items: %d", st.loaded ? "loaded" : "loading...", st.totalItems);

        if (st.loaded) {
            ImGui::Text("Rates: 1 Divine = %.1f Chaos | 1 Exalted = %.1f Chaos",
                st.divineInChaos, st.exaltedInChaos);
        }
    }

    void DrawTabDisplaySettings() {
        ImGui::Spacing();

        ImGui::Text("Value Display:");
        int dc = static_cast<int>(m_DisplayCurrency);
        ImGui::RadioButton("Divine (D)", &dc, 0); ImGui::SameLine();
        ImGui::RadioButton("Exalted (E)", &dc, 1); ImGui::SameLine();
        ImGui::RadioButton("Chaos (C)", &dc, 2);
        m_DisplayCurrency = static_cast<DisplayCurrency>(dc);

        ImGui::Spacing();
        ImGui::Text("Price style:");
        int ps = static_cast<int>(m_PriceDisplayStyle);
        ImGui::RadioButton(kPriceDisplayStyleNames[0], &ps, 0); ImGui::SameLine();
        ImGui::RadioButton(kPriceDisplayStyleNames[1], &ps, 1);
        m_PriceDisplayStyle = static_cast<PriceDisplayStyle>(ps);
        ImGui::SameLine();
        ImGui::TextDisabled("(icon = value + currency image)");

        ImGui::SliderFloat("Text size", &m_TextScale, 0.5f, 2.0f, "%.1f");

        ImGui::Separator();

        ImGui::SetNextItemWidth(160.0f);
        int uiPos = static_cast<int>(m_UiPricePosition);
        if (ImGui::BeginCombo("Price position (Inventory/Stash)", kUiPositionNames[uiPos])) {
            for (int i = 0; i < 4; i++) {
                bool selected = (uiPos == i);
                if (ImGui::Selectable(kUiPositionNames[i], selected))
                    uiPos = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        m_UiPricePosition = static_cast<UiPricePosition>(uiPos);

        ImGui::SetNextItemWidth(160.0f);
        int gndPos = static_cast<int>(m_GroundPricePosition);
        if (ImGui::BeginCombo("Price position (Ground items)", kGroundPositionNames[gndPos])) {
            for (int i = 0; i < 4; i++) {
                bool selected = (gndPos == i);
                if (ImGui::Selectable(kGroundPositionNames[i], selected))
                    gndPos = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        m_GroundPricePosition = static_cast<GroundPricePosition>(gndPos);
    }

    // Hotkey capture row: "<label>  <binding>  [Set Hotkey] [Clear]". While this
    // row is capturing (m_CaptureTarget == &vk) the next pressed key is bound;
    // ESC cancels. One capture at a time across all rows by construction.
    // Returns true when a key was bound THIS frame — the key is physically down
    // at that moment, so callers can re-arm edge detectors against it.
    bool DrawHotkeyCaptureRow(const char* label, const char* idSuffix, int& vk) {
        bool boundNow = false;
        ImGui::Text("%s", label);
        ImGui::SameLine();
        if (m_CaptureTarget == &vk) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                "Press any key... (ESC to cancel)");
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                m_CaptureTarget = nullptr;
            }
            else {
                for (int k = 0x08; k < 0xFF; k++) {
                    if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                    if (k == VK_ESCAPE) continue;
                    if (GetAsyncKeyState(k) & 0x8000) {
                        vk = k;
                        m_CaptureTarget = nullptr;
                        boundNow = true;
                        break;
                    }
                }
            }
        }
        else {
            ImGui::Text("%s", GetVkName(vk));
            ImGui::SameLine();
            char btn[48];
            snprintf(btn, sizeof(btn), "Set Hotkey##%s", idSuffix);
            if (ImGui::Button(btn, ImVec2(100.0f, 0.0f)))
                m_CaptureTarget = &vk;
            if (vk != 0) {
                ImGui::SameLine();
                snprintf(btn, sizeof(btn), "Clear##%s", idSuffix);
                if (ImGui::Button(btn, ImVec2(60.0f, 0.0f)))
                    vk = 0;
            }
        }
        return boundNow;
    }

    void DrawTabOverlayToggles() {
        ImGui::Spacing();

        ImGui::Checkbox("Show prices on dropped items", &m_ShowGroundPrices);

        ImGui::Checkbox("Show prices in inventory", &m_ShowInventoryPrices);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(may affect FPS)");

        ImGui::Checkbox("Show prices in stash", &m_ShowOtherInventoryPrices);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(may affect FPS)");

        ImGui::Checkbox("Ritual", &m_ShowRitualPrices);
        ImGui::SameLine();
        ImGui::TextDisabled("(price items in the Ritual \"Favours\" shop)");

        ImGui::Checkbox("Runeshape", &m_ShowRuneshapePrices);
        ImGui::SameLine();
        ImGui::TextDisabled("(price rewards in the Runeshape Combinations panel)");

        ImGui::Checkbox("Runeshape window", &m_ShowRuneshapeWindow);
        ImGui::SameLine();
        ImGui::TextDisabled("(movable overlay listing each Runeshape with prices)");

        // Advanced settings for the Runeshape window. Always visible (not gated
        // on the checkbox above) so the toggle hotkey stays discoverable while
        // the window is hidden.
        ImGui::Indent();
        if (ImGui::TreeNode("Runeshape window: advanced settings")) {
            ImGui::Spacing();

            if (DrawHotkeyCaptureRow("Show/hide hotkey:", "rswin", m_RuneshapeWinHotkey))
                m_RuneshapeWinHotkeyWasDown = true;   // key is down right now — don't fire the toggle

            ImGui::Checkbox("Hide on mouse hover", &m_RuneshapeWinHideOnHover);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The overlay disappears while the mouse cursor is over it and\n"
                                  "reappears when the cursor leaves. Not applied while the\n"
                                  "Fixer menu is open (so it can still be dragged / collapsed).");

            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat("Opacity##rswin", &m_RuneshapeWinAlpha, 0.1f, 1.0f, "%.2f");

            ImGui::Spacing();
            ImGui::Text("Price display:");
            ImGui::SameLine();
            int rsps = static_cast<int>(m_RuneshapeWinPriceStyle);
            ImGui::RadioButton("Currency icon##rswin", &rsps, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Text##rswin", &rsps, 1);
            m_RuneshapeWinPriceStyle = static_cast<PriceDisplayStyle>(rsps);
            ImGui::SameLine();
            ImGui::TextDisabled("(reward rows + header best reward)");

            ImGui::Spacing();
            ImGui::TextDisabled("Header elements:");
            ImGui::Checkbox("Color square", &m_RsShowHdrColor);
            ImGui::SameLine();
            ImGui::Checkbox("Rune sockets", &m_RsShowHdrRunes);
            ImGui::SameLine();
            ImGui::Checkbox("Best reward price", &m_RsShowHdrBest);

            ImGui::TextDisabled("Expanded list elements:");
            ImGui::Checkbox("Reward icon", &m_RsShowRowIcon);
            ImGui::SameLine();
            ImGui::Checkbox("Reward name", &m_RsShowRowName);
            ImGui::SameLine();
            ImGui::Checkbox("Reward quantity", &m_RsShowRowQty);
            ImGui::Checkbox("Price##rswinrow", &m_RsShowRowPrice);
            ImGui::SameLine();
            ImGui::Checkbox("Propagating runes (yellow glow)", &m_RsShowRowPropRunes);

            ImGui::Spacing();
            ImGui::TreePop();
        }
        ImGui::Unindent();

        ImGui::Checkbox("Runeshape weights", &m_ShowRuneshapeWeights);
        ImGui::SameLine();
        ImGui::TextDisabled("(total rune weight per combination; edit weights in Radar -> RuneShape)");

        ImGui::Checkbox("Show item icons", &m_ShowItemIcons);

        ImGui::Checkbox("Hide when game not focused", &m_HideWhenUnfocused);

        ImGui::Separator();

        DrawHotkeyCaptureRow("Hold-to-hide hotkey:", "hold", m_HideHotkey);
    }

    // ========================================================================
    // Overlay Rendering
    // ========================================================================

    void DrawUI() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        if (m_HideWhenUnfocused && !IsGameWindowFocused()) return;
        if (m_HideHotkey != 0 && (GetAsyncKeyState(m_HideHotkey) & 0x8000)) return;

        // Per-frame gates use cheap accessors — NOT GetSnapshot(), which enumerates
        // every entity (hundreds–1000+ in a busy map). The only thing that needs the
        // entity list is the ground-item scan, which is throttled + cached below.
        if (!ctx()->Game.IsAttached()) return;
        if (!ctx()->Game.IsInGame()) return;

        // Runeshape-window show/hide hotkey — edge-triggered on key press.
        // Paused while a capture row is armed in the settings UI; right after a
        // capture completes, m_RuneshapeWinHotkeyWasDown was set true, so the
        // still-held freshly-bound key doesn't immediately toggle the window.
        if (m_RuneshapeWinHotkey != 0 && m_CaptureTarget == nullptr) {
            const bool down = (GetAsyncKeyState(m_RuneshapeWinHotkey) & 0x8000) != 0;
            if (down && !m_RuneshapeWinHotkeyWasDown) {
                m_ShowRuneshapeWindow = !m_ShowRuneshapeWindow;
                SaveSettings();
            }
            m_RuneshapeWinHotkeyWasDown = down;
        }

        uint64_t areaChange = ctx()->Game.GetAreaChangeCounter();
        if (areaChange != m_LastAreaChange) {
            m_NameCache.clear();
            m_CachedInventories.clear();
            m_RarityCache.clear();
            m_PriceCache.clear();
            m_GroundTags.clear();
            m_GroundResolveCache.clear();
            m_LastInventoryScan = {};   // force immediate refresh in new area
            m_LastGroundScan = {};
            m_InvReadPending = false;
            m_RuneshapeListAddr = 0;
            m_RuneshapeWindowAddr = 0;
            m_RuneshapeRows.clear();
            m_LastRuneshapeScan = {};
            m_LastRuneshapeDiscover = {};   // allow immediate rediscovery in the new area
            m_LastAreaChange = areaChange;
        }

        // Gate on the host price service being ready.
        auto st = ctx()->Prices.GetStatus();
        if (!st.loaded) return;

        m_CachedDivineInChaos  = st.divineInChaos;
        if (st.exaltedInChaos > 0.0f) m_CachedExaltedInChaos = st.exaltedInChaos;

        // Invalidate the cross-frame price memo when the host DB refreshes (its
        // chaos rates shift) or on a periodic safety net, so cached prices can't
        // go stale within a long-lived area (e.g. sitting in the hideout).
        {
            auto nowPC = std::chrono::steady_clock::now();
            if (st.divineInChaos  != m_PriceCacheDivineSeen ||
                st.exaltedInChaos != m_PriceCacheExaltedSeen ||
                nowPC - m_LastPriceCacheFlush > std::chrono::seconds(30)) {
                m_PriceCache.clear();
                m_GroundResolveCache.clear();   // cached resolves hold PriceResults — refresh with prices
                m_PriceCacheDivineSeen  = st.divineInChaos;
                m_PriceCacheExaltedSeen = st.exaltedInChaos;
                m_LastPriceCacheFlush   = nowPC;
            }
        }

        // Reset per-frame perf accumulators (TimedLookup feeds these).
        m_PerfLookupMs = 0.0;
        m_PerfLookupExact = m_PerfLookupMiss = 0;

        if (m_ShowGroundPrices) {
            // Throttle the entity enumeration + per-item name/price/rarity/stack
            // resolves to the scan interval; redraw the cached tags every frame
            // (cheap WorldToScreen, no memory reads). Previously this iterated the
            // FULL entity list and re-resolved every item EVERY frame.
            auto nowG = std::chrono::steady_clock::now();
            if (nowG - m_LastGroundScan > std::chrono::milliseconds(m_PerfScanIntervalMs)) {
                m_LastGroundScan = nowG;
                ScanGroundItems();
            }
            double tg0 = PerfNowMs();
            DrawGroundTags();
            m_PerfGroundMs = PerfNowMs() - tg0;
            if (m_PerfGroundMs > m_PerfPeakGroundMs) m_PerfPeakGroundMs = m_PerfGroundMs;
        }

        if (m_ShowInventoryPrices || m_ShowOtherInventoryPrices || m_ShowRitualPrices) {
            auto now = std::chrono::steady_clock::now();

            // Read fires this long after the request. It MUST be strictly shorter
            // than the interval — otherwise the next request keeps resetting
            // m_LastInventoryScan before the read condition is ever reached, the
            // read never runs, and the overlay shows nothing (the 50 ms bug).
            int readDelayMs = (std::min)(kInvReadDelayMs, m_PerfScanIntervalMs / 2);

            // 1) Fire an async scan request on the interval. Scan() only flags the
            //    host worker (~0 ms); the worker fills the snapshot within ~1 cycle.
            if (now - m_LastInventoryScan > std::chrono::milliseconds(m_PerfScanIntervalMs)) {
                double ts0 = PerfNowMs();
                ctx()->Inventory.Scan(-1);
                m_PerfScanCallMs = PerfNowMs() - ts0;
                if (m_PerfScanCallMs > m_PerfPeakScanCallMs) m_PerfPeakScanCallMs = m_PerfScanCallMs;
                m_LastInventoryScan = now;
                m_InvReadPending = true;
            }

            // 2) Read the result ~one worker cycle AFTER the request — NOT in the
            //    same tick. The old same-tick GetAll() always returned the PREVIOUS
            //    request's snapshot, so the overlay lagged reality by a full extra
            //    interval (the 1-3 s delay). Decoupling makes total latency ≈
            //    interval + one worker cycle, so a modest interval already feels
            //    instant without scanning fast enough to burden the worker.
            if (m_InvReadPending &&
                now - m_LastInventoryScan >= std::chrono::milliseconds(readDelayMs)) {
                double tg0 = PerfNowMs();
                m_CachedInventories = ctx()->Inventory.GetAll();
                m_PerfGetAllMs = PerfNowMs() - tg0;
                if (m_PerfGetAllMs > m_PerfPeakGetAllMs) m_PerfPeakGetAllMs = m_PerfGetAllMs;
                m_RarityCache.clear();
                m_PerfCachedInvCount = static_cast<int>(m_CachedInventories.size());
                int items = 0;
                for (const auto& iv : m_CachedInventories) items += static_cast<int>(iv.Items.size());
                m_PerfCachedItemCount = items;
                m_InvReadPending = false;
            }

            double td0 = PerfNowMs();
            DrawInventoryOverlays();
            m_PerfDrawInvMs = PerfNowMs() - td0;
            if (m_PerfDrawInvMs > m_PerfPeakDrawInvMs) m_PerfPeakDrawInvMs = m_PerfDrawInvMs;
        }

        // Finalize per-frame lookup peaks (after all draw paths contributed).
        if (m_PerfLookupMs > m_PerfPeakLookupMs) m_PerfPeakLookupMs = m_PerfLookupMs;

        if (m_ShowRuneshapePrices || m_ShowRuneshapeWeights) {
            auto nowR = std::chrono::steady_clock::now();
            // 400 ms refresh: when the panel is open the rebuild is cheap (cached
            // list, fast-path), so a tighter cadence just makes prices appear and
            // track sooner. The expensive UI-tree rediscovery is separately backed
            // off inside FindRuneshapeRowList.
            if (nowR - m_LastRuneshapeScan > std::chrono::milliseconds(400)) {
                ScanRuneshapeRows();
                m_LastRuneshapeScan = nowR;
            }
            DrawRuneshapeOverlay();
        }

        if (m_ShowRuneshapeWindow) {
            DrawRuneshapeWindow();
        } else {
            // Release the overlay-input request when the window is off.
            ctx()->Overlay.SetWantsOverlayInput(false);
        }
    }

    void SaveSettings() override {
        namespace fs = std::filesystem;
        fs::path configDir = DirectoryPath() / "config";
        std::error_code ec;
        fs::create_directories(configDir, ec);

        try {
            nlohmann::json j;
            j["displayCurrency"] = static_cast<int>(m_DisplayCurrency);
            j["textScale"] = m_TextScale;
            j["showGroundPrices"] = m_ShowGroundPrices;
            j["showInventoryPrices"] = m_ShowInventoryPrices;
            j["showOtherInventoryPrices"] = m_ShowOtherInventoryPrices;
            j["showRitualPrices"] = m_ShowRitualPrices;
            j["showRuneshapePrices"] = m_ShowRuneshapePrices;
            j["showRuneshapeWeights"] = m_ShowRuneshapeWeights;
            j["showItemIcons"] = m_ShowItemIcons;
            j["showRuneshapeWindow"] = m_ShowRuneshapeWindow;
            j["runeshapeWinX"] = m_RuneshapeWinX;
            j["runeshapeWinY"] = m_RuneshapeWinY;
            j["runeshapeWinAlpha"] = m_RuneshapeWinAlpha;
            j["runeshapeWinCollapsed"] = m_RuneshapeWinCollapsed;
            { nlohmann::json arr = nlohmann::json::array(); for (auto c : m_RuneshapeCollapsed) arr.push_back(c); j["runeshapeCollapsed"] = arr; }
            j["rsWinToggleHotkey"] = m_RuneshapeWinHotkey;
            j["rsWinHideOnHover"] = m_RuneshapeWinHideOnHover;
            j["rsWinPriceStyle"] = static_cast<int>(m_RuneshapeWinPriceStyle);
            j["rsWinShowColor"] = m_RsShowHdrColor;
            j["rsWinShowRunes"] = m_RsShowHdrRunes;
            j["rsWinShowBestReward"] = m_RsShowHdrBest;
            j["rsWinShowRewardIcon"] = m_RsShowRowIcon;
            j["rsWinShowRewardText"] = m_RsShowRowName;
            j["rsWinShowRewardQty"] = m_RsShowRowQty;
            j["rsWinShowRewardPrice"] = m_RsShowRowPrice;
            j["rsWinShowPropRunes"] = m_RsShowRowPropRunes;
            j["hideWhenUnfocused"] = m_HideWhenUnfocused;
            j["hideHotkey"] = m_HideHotkey;
            j["uiPricePosition"] = static_cast<int>(m_UiPricePosition);
            j["groundPricePosition"] = static_cast<int>(m_GroundPricePosition);
            j["priceDisplayStyle"] = static_cast<int>(m_PriceDisplayStyle);
            j["scanIntervalMs"] = m_PerfScanIntervalMs;

            std::ofstream f(configDir / "settings.json");
            if (f.is_open())
                f << j.dump(2);
        }
        catch (...) {}
    }

private:
    // ========================================================================
    // Debug Panel
    // ========================================================================

    // Live performance breakdown for diagnosing the price-overlay display delay.
    // All numbers are render-thread (DrawUI). The host-side (worker-thread) scan
    // cost lives separately in the app's Debug -> DataVis -> "Inventory Scan".
    void DrawPerfSection() {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Performance (render thread):");

        // The single knob most likely to affect perceived latency. Lower it and
        // watch whether the delay shrinks — if it does and the timings below stay
        // low, the bottleneck was the throttle, not the read.
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("Inventory scan interval (ms)", &m_PerfScanIntervalMs, 50, 2000);
        ImGui::SameLine();
        ImGui::TextDisabled("(default 100; read is decoupled, so this stays cheap)");

        auto row = [](const char* label, double last, double peak, double warn, double bad) {
            ImVec4 c = (peak > bad) ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                     : (peak > warn) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                     : ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
            ImGui::Text("  %-26s", label);
            ImGui::SameLine(240);
            ImGui::TextColored(c, "%.3f ms   (peak %.3f ms)", last, peak);
        };

        ImGui::Spacing();
        row("Scan() request:",     m_PerfScanCallMs, m_PerfPeakScanCallMs, 0.5, 2.0);
        row("GetAll() (ABI):",     m_PerfGetAllMs,   m_PerfPeakGetAllMs,   1.0, 5.0);
        row("DrawInventory/frame:",m_PerfDrawInvMs,  m_PerfPeakDrawInvMs,  1.0, 4.0);
        row("DrawGround/frame:",   m_PerfGroundMs,   m_PerfPeakGroundMs,   1.0, 4.0);
        row("LookupPrice/frame:",  m_PerfLookupMs,   m_PerfPeakLookupMs,   0.5, 3.0);

        ImGui::Spacing();
        ImGui::Text("  LookupPrice this frame: %d found, %d miss",
            m_PerfLookupExact, m_PerfLookupMiss);
        ImGui::Text("  Cached: %d inventories, %d items",
            m_PerfCachedInvCount, m_PerfCachedItemCount);

        if (ImGui::Button("Reset peaks")) {
            m_PerfPeakScanCallMs = m_PerfPeakGetAllMs = m_PerfPeakDrawInvMs = 0.0;
            m_PerfPeakGroundMs = m_PerfPeakLookupMs = 0.0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Worker-thread scan cost: app Debug -> DataVis -> Inventory Scan");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    void DrawDebugPanel() {
        PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot();

        // Price service status
        auto st = ctx()->Prices.GetStatus();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Price Service (host):");
        ImGui::Text("  Loaded: %s", st.loaded ? "YES" : "NO");
        ImGui::Text("  Total items: %d", st.totalItems);
        ImGui::Text("  DivineInChaos: %.2f", st.divineInChaos);
        ImGui::Text("  ExaltedInChaos: %.2f", st.exaltedInChaos);
        ImGui::Text("  Categories: ok=%d / pending=%d / failed=%d",
            st.catsOk, st.catsPending, st.catsFailed);

        DrawPerfSection();

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Game State:");
        ImGui::Text("  Attached: %s", snap.IsAttached ? "YES" : "NO");
        ImGui::Text("  State: %d (InGame=4)", (int)snap.State);
        ImGui::Text("  Area: %s (level %d)",
            snap.CurrentAreaName.c_str(), snap.CurrentAreaLevel);
        ImGui::Text("  IsHideout: %s, IsTown: %s",
            snap.IsHideout ? "YES" : "NO", snap.IsTown ? "YES" : "NO");

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Ground Items (Entities):");
        int totalEntities = (int)snap.Entities.size();
        int worldItems = 0;
        for (const auto& e : snap.Entities) {
            if (e.EntityType == PluginSDK::EntityType::Item
                && e.EntitySubtype == PluginSDK::EntitySubtype::WorldItem)
                worldItems++;
        }
        ImGui::Text("  Total entities: %d", totalEntities);
        ImGui::Text("  WorldItem entities: %d", worldItems);
        ImGui::Text("  Name cache size: %d", (int)m_NameCache.size());

        if (worldItems > 0 && ImGui::TreeNode("WorldItem details (first 5)")) {
            int count = 0;
            for (const auto& e : snap.Entities) {
                if (e.EntityType != PluginSDK::EntityType::Item
                    || e.EntitySubtype != PluginSDK::EntitySubtype::WorldItem) continue;
                if (count++ >= 5) break;

                std::string name = GetGroundLookupName(e);
                if (name.empty()) name = "(not read yet)";

                auto price = ctx()->Prices.LookupPrice(name);

                ImGui::Text("  ID:%u Addr:0x%llX Valid:%d",
                    e.Id, (unsigned long long)e.Address, (int)e.IsValid);
                ImGui::Text("    Name: '%s'", name.c_str());
                ImGui::Text("    Pos: (%.0f, %.0f, %.0f) Zone:%d",
                    e.WorldX, e.WorldY, e.WorldZ, (int)e.Zone);
                float sx, sy;
                bool vis = ctx()->Render.WorldToScreen(e.WorldX, e.WorldY, e.WorldZ, sx, sy);
                ImGui::Text("    Screen: (%.0f, %.0f) Visible:%s",
                    sx, sy, vis ? "YES" : "NO");
                ImGui::Text("    Price found: %s%s", price.found ? "YES" : "NO",
                    price.found
                    ? (std::string(" -> ") + FormatPriceLocal(
                        GetDisplayValue(price, m_DisplayCurrency), m_DisplayCurrency)).c_str()
                    : "");
                ImGui::Spacing();
            }
            ImGui::TreePop();
        }

        ImGui::Separator();

        std::vector<PluginSDK::Inventory> invs = ctx()->Inventory.GetAll();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Inventory Data:");
        ImGui::Text("  Inventories available: %d", (int)invs.size());

        for (const auto& inv : invs) {
            const char* invName = ctx()->Inventory.GetName(inv.InventoryId);
            if (!invName) invName = "(unknown)";

            if (ImGui::TreeNode((void*)(intptr_t)inv.InventoryId,
                "Inventory #%d '%s' (%dx%d, %d items)",
                inv.InventoryId, invName, inv.TotalBoxesX, inv.TotalBoxesY,
                (int)inv.Items.size()))
            {
                ImGui::Text("  Grid: valid=%s pos=(%.0f, %.0f) cell=%.1f",
                    inv.Grid.Valid ? "YES" : "NO",
                    inv.Grid.GridScreenX, inv.Grid.GridScreenY, inv.Grid.CellSize);

                int matchCount = 0;
                for (const auto& item : inv.Items) {
                    std::string dn = GetItemLookupName(item);
                    auto price = ctx()->Prices.LookupPrice(dn);
                    if (price.found) matchCount++;
                }
                ImGui::Text("  Items with price match: %d / %d",
                    matchCount, (int)inv.Items.size());

                if (ImGui::TreeNode("Item list (first 10)")) {
                    int count = 0;
                    for (const auto& item : inv.Items) {
                        if (count++ >= 10) break;
                        std::string dn = GetItemLookupName(item);
                        auto price = ctx()->Prices.LookupPrice(dn);

                        ImGui::Text("  [%d,%d] Lookup:'%s' (stack:%d) path:'%s'",
                            item.SlotX, item.SlotY, dn.c_str(),
                            item.StackCount, item.Path.c_str());
                        ImGui::Text("    Price: %s", price.found
                            ? FormatPriceLocal(GetDisplayValue(price, m_DisplayCurrency),
                                m_DisplayCurrency).c_str()
                            : "NOT FOUND");
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        }

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Rendering:");
        ImGui::Text("  IsOverlayMode: %s",
            ctx()->Game.IsOverlayMode() ? "YES" : "NO");
        ImGui::Text("  Screen: %dx%d", snap.ScreenWidth, snap.ScreenHeight);
    }

    // ========================================================================
    // Ground Item Prices (entity-based; mirrors ExamplePlugin's
    // ComponentReader.h "Items on Ground" pattern). Name + stack come from
    // the WorldItem entity / its inner item; position comes from
    // WorldToScreen on the entity's world coordinates.
    // ========================================================================

    // Enumerate ground WorldItems and resolve everything needed to draw a price tag
    // (world pos + display value + chaos value for the colour threshold + icon).
    // RPM-heavy (entity list + per-item name/inner/mods/stack reads), so DrawUI runs
    // this on the scan interval, NOT every frame. Result is cached in m_GroundTags.
    void ScanGroundItems() {
        m_GroundTags.clear();
        PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot();
        const uint32_t epoch = ++m_GroundScanEpoch;

        for (const auto& e : snap.Entities) {
            if (e.EntityType != PluginSDK::EntityType::Item || !e.Address) continue;

            auto cit = m_GroundResolveCache.find(e.Address);
            if (cit == m_GroundResolveCache.end()) {
                // New ground item: do the expensive resolve (name read + price
                // lookup + inner/mods/stack RPM) ONCE and cache it. A ground item
                // doesn't change while on the floor, so later scans reuse this and
                // only newly-dropped items pay the cost — the throttled scan stays
                // cheap instead of re-resolving every persistent item each tick.
                GroundResolve r;
                r.wx = e.WorldX; r.wy = e.WorldY; r.wz = e.WorldZ;
                std::string name = GetGroundLookupName(e);
                if (!name.empty()) {
                    PluginSDK::PriceResult price = TimedLookup(name);
                    if (price.found) {
                        // Resolve the inner item once; reused for both rarity gate
                        // and stack multiplier so we never call GetWorldItemInner twice.
                        auto inner = ctx()->Entities.GetWorldItemInner(e.Address);
                        bool ok = true;
                        // Unique-category guard: LookupPrice can land on a unique entry
                        // via its contains-match path (e.g. base-type "Heavy Belt"
                        // hitting "Headhunter"). Require entity Rarity == 3 (Unique).
                        if (IsUniqueCategory(price.category)) {
                            if (!inner || !inner->Components.HasMods()) ok = false;
                            else {
                                auto mods = ctx()->Components.ReadMods(inner->Components.Mods);
                                if (!mods.Valid || mods.Rarity != 3) ok = false;
                            }
                        }
                        if (ok) {
                            r.priceable = true;
                            r.price = price;
                            r.stackMultiplier = 1;
                            if (inner && inner->Components.HasStack()) {
                                int sc = ctx()->Components.GetStackCount(inner->Components.Stack);
                                if (sc > 1) r.stackMultiplier = sc;
                            }
                        }
                    }
                }
                cit = m_GroundResolveCache.emplace(e.Address, std::move(r)).first;
            }
            cit->second.epoch = epoch;   // mark present this scan (for eviction)

            const GroundResolve& gr = cit->second;
            if (!gr.priceable) continue;

            // Currency-dependent values are cheap — recompute from the cached
            // PriceResult each scan so a currency/icon toggle applies without an
            // RPM re-read.
            float displayValue = GetDisplayValue(gr.price, m_DisplayCurrency) * gr.stackMultiplier;
            if (displayValue < 0.001f) continue;

            GroundTag t;
            t.wx = gr.wx; t.wy = gr.wy; t.wz = gr.wz;
            t.displayValue = displayValue;
            t.chaos = gr.price.chaos * gr.stackMultiplier;
            t.iconPath = m_ShowItemIcons ? gr.price.iconPath : std::string();
            m_GroundTags.push_back(std::move(t));
        }

        // Evict resolves for items no longer on the ground (picked up / despawned).
        for (auto it = m_GroundResolveCache.begin(); it != m_GroundResolveCache.end(); ) {
            if (it->second.epoch != epoch) it = m_GroundResolveCache.erase(it);
            else ++it;
        }
    }

    // Overlay draw list: the price/weight chips draw over the game normally,
    // but drop BEHIND the Fixer menu while it is open (background list sits
    // under all ImGui windows) so they never cover the UI.
    ImDrawList* OverlayDrawList() {
        return ctx()->Game.IsMenuVisible() ? ImGui::GetBackgroundDrawList()
                                           : ImGui::GetForegroundDrawList();
    }

    // Per-frame render of the cached ground tags. No memory reads — only
    // WorldToScreen (the camera pans every frame, so screen pos must refresh) +
    // measure + draw.
    void DrawGroundTags() {
        if (m_GroundTags.empty()) return;
        ImDrawList* dl = OverlayDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;
        if (ImGui::GetFontSize() <= 0.f) return;

        for (const auto& t : m_GroundTags) {
            float sx, sy;
            if (!ctx()->Render.WorldToScreen(t.wx, t.wy, t.wz, sx, sy))
                continue;

            PriceTag tag = MeasurePriceTag(t.displayValue, baseFontSize, t.iconPath);

            // Anchor is a single screen point (zero-sized box). CalcGroundPricePos
            // already centres the block around (sx, sy) with the four presets.
            float blockX, blockY;
            CalcGroundPricePos(sx, sy, /*labelW=*/0.0f, /*labelH=*/0.0f,
                               tag.totalW, tag.totalH, baseFontSize, blockX, blockY);

            DrawPriceTag(dl, baseFontSize, blockX, blockY, tag, t.chaos);
        }
    }

    // ========================================================================
    // Inventory / Stash Overlays — Grid.Valid-gated. Iterates every inventory
    // returned by InventoryService.GetAll() and draws prices on each one whose
    // host-populated Grid is valid (host sets Grid.Valid only when the panel
    // UI is open). No UI-tree identification — robust against locale, panel
    // reorganization, and StringId changes.
    // ========================================================================

    void DrawInventoryGrid(const PluginSDK::Inventory& inv) {
        ImDrawList* dl = OverlayDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        for (const auto& item : inv.Items) {
            std::string displayName = GetItemLookupName(item);
            if (displayName.empty()) continue;

            PluginSDK::PriceResult price = TimedLookup(displayName);
            if (!price.found) continue;

            if (IsUniqueCategory(price.category) && item.Address) {
                // Cache rarity per item address to avoid the bridge round-
                // trip + RPM on every frame. m_RarityCache is cleared on
                // every Scan tick (and on area change) for freshness.
                int rarity;
                auto it = m_RarityCache.find(item.Address);
                if (it != m_RarityCache.end()) {
                    rarity = it->second;
                } else {
                    rarity = ctx()->Inventory.ReadItemRarity(item.Address);
                    m_RarityCache[item.Address] = rarity;
                }
                if (rarity != 3)
                    continue;
            }

            int multiplier = (item.StackCount > 1) ? item.StackCount : 1;
            float displayValue = GetDisplayValue(price, m_DisplayCurrency) * multiplier;
            if (displayValue < 0.001f) continue;

            // Special stash tabs (currency/fragments/expedition/...) arrange their
            // cells freely, not on a uniform grid — the host resolves each item's
            // real screen rect from its per-slot UI element. Use it when present;
            // otherwise fall back to grid math (regular tabs / player inventory).
            float cellX, cellY, cellW, cellH;
            if (item.ScreenValid) {
                cellX = item.ScreenX;
                cellY = item.ScreenY;
                cellW = item.ScreenW;
                cellH = item.ScreenH;
            } else {
                cellX = inv.Grid.GridScreenX + item.SlotX * inv.Grid.CellSize;
                cellY = inv.Grid.GridScreenY + item.SlotY * inv.Grid.CellSize;
                cellW = item.Width * inv.Grid.CellSize;
                cellH = item.Height * inv.Grid.CellSize;
            }

            float fontSize = ComputeAdaptiveFontSize(baseFontSize, cellW, cellH);

            PriceTag tag = MeasurePriceTag(displayValue, fontSize,
                m_ShowItemIcons ? price.iconPath : std::string(), cellW - 4.0f);

            float labelX, labelY;
            CalcUiPricePos(cellX, cellY, cellW, cellH, tag.totalW, tag.totalH, 2.0f, labelX, labelY);

            DrawPriceTag(dl, fontSize, labelX, labelY, tag, price.chaos * multiplier);
        }
    }

    void DrawInventoryOverlays() {
        // Iterates the cached inventory vector (refreshed by DrawUI once per
        // Scan tick) rather than calling GetAll() per frame.
        for (const auto& inv : m_CachedInventories) {
            if (!inv.Grid.Valid || inv.Grid.CellSize < 1.0f) continue;

            const bool isPlayer = (inv.InventoryId == 1);
            const bool isRitual = (inv.InventoryId == kRitualShopInventoryId);
            if (isRitual) {
                if (!m_ShowRitualPrices) continue;
            } else if (isPlayer) {
                if (!m_ShowInventoryPrices) continue;
            } else {
                if (!m_ShowOtherInventoryPrices) continue;
            }

            DrawInventoryGrid(inv);
        }
    }

    // ========================================================================
    // Drawing Helpers
    // ========================================================================

    // ---- Currency icon textures (image price-display style) ----------------

    struct CurrencyTex {
        ImTextureID id = ImTextureID{};
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0;
        int h = 0;
        bool valid = false;
    };

    // A measured, ready-to-draw price label. In text style it is plain text; in
    // image style it is value-text + currency icon. totalW/totalH bound the whole
    // block so callers anchor it exactly like the old text label.
    struct PriceTag {
        std::string text;
        float textW = 0.0f, textH = 0.0f;
        const CurrencyTex* tex = nullptr;   // null => render text only
        float iconW = 0.0f, iconH = 0.0f, gap = 0.0f;
        float totalW = 0.0f, totalH = 0.0f;
        const CurrencyTex* itemTex = nullptr;   // item icon (left of everything); null => none
        float itemIconW = 0.0f, itemIconH = 0.0f, itemGap = 0.0f;
    };

    static constexpr float kIconHeightMul = 1.4f;   // icon height vs text line height

    void LoadCurrencyTexture(int idx) {
        if (idx < 0 || idx > 2) return;
        auto* device = static_cast<ID3D11Device*>(ctx()->D3DDevice);
        if (!device) return;

        // Index matches DisplayCurrency: Divine=0, Exalted=1, Chaos=2.
        static const wchar_t* kFiles[3] = { L"divine.png", L"exalted.png", L"chaos.png" };

        // Resources sit next to the host executable. Absolute + wide path so a
        // non-ASCII install directory (Russian/Chinese/...) still resolves.
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;
        std::filesystem::path p = std::filesystem::path(exePath).parent_path()
            / L"Resources" / L"currency" / L"poe2" / kFiles[idx];

        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return;
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty()) return;

        int w = 0, h = 0;
        unsigned char* data = stbi_load_from_memory(
            bytes.data(), static_cast<int>(bytes.size()), &w, &h, nullptr, 4);
        if (!data) return;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = data;
        init.SysMemPitch = static_cast<UINT>(w * 4);

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, &init, &tex);
        stbi_image_free(data);
        if (FAILED(hr) || !tex) return;

        ID3D11ShaderResourceView* srv = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
        tex->Release();
        if (FAILED(hr) || !srv) return;

        m_CurrencyTex[idx].srv   = srv;
        m_CurrencyTex[idx].id    = reinterpret_cast<ImTextureID>(srv);
        m_CurrencyTex[idx].w     = w;
        m_CurrencyTex[idx].h     = h;
        m_CurrencyTex[idx].valid = true;
    }

    const CurrencyTex* GetCurrencyTexture(DisplayCurrency c) {
        int idx = static_cast<int>(c);   // Divine=0, Exalted=1, Chaos=2
        if (idx < 0 || idx > 2) return nullptr;
        if (!m_CurrencyTexTried[idx]) {
            m_CurrencyTexTried[idx] = true;   // try once; don't retry every frame
            LoadCurrencyTexture(idx);
        }
        return m_CurrencyTex[idx].valid ? &m_CurrencyTex[idx] : nullptr;
    }

    void ReleaseCurrencyTextures() {
        for (int i = 0; i < 3; i++) {
            if (m_CurrencyTex[i].srv) m_CurrencyTex[i].srv->Release();
            m_CurrencyTex[i] = CurrencyTex{};
            m_CurrencyTexTried[i] = false;
        }
        for (auto& kv : m_ItemTex) if (kv.second.srv) kv.second.srv->Release();
        m_ItemTex.clear();
    }

    // Path-keyed cache of item icons loaded from PriceIconCache PNGs.
    std::unordered_map<std::string, CurrencyTex> m_ItemTex;   // key = absolute UTF-8 path

    const CurrencyTex* GetItemTexture(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = m_ItemTex.find(path);
        if (it != m_ItemTex.end()) return it->second.valid ? &it->second : nullptr;
        CurrencyTex t{};   // default-invalid; inserted even on failure so we don't retry every frame
        auto* device = static_cast<ID3D11Device*>(ctx()->D3DDevice);
        if (device) {
            // UTF-8 path -> wide for Unicode-safe open (install dir may be non-ASCII).
            int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            std::wstring wp(wn > 0 ? wn - 1 : 0, L'\0');
            if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), wn);
            std::ifstream f(wp, std::ios::binary);
            if (f.is_open()) {
                std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                int w = 0, h = 0;
                unsigned char* data = bytes.empty() ? nullptr
                    : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, nullptr, 4);
                if (data) {
                    D3D11_TEXTURE2D_DESC desc = {};
                    desc.Width=(UINT)w; desc.Height=(UINT)h; desc.MipLevels=1; desc.ArraySize=1;
                    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
                    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA init = {}; init.pSysMem=data; init.SysMemPitch=(UINT)(w*4);
                    ID3D11Texture2D* tex=nullptr;
                    if (SUCCEEDED(device->CreateTexture2D(&desc,&init,&tex)) && tex) {
                        ID3D11ShaderResourceView* srv=nullptr;
                        D3D11_SHADER_RESOURCE_VIEW_DESC sd={}; sd.Format=desc.Format;
                        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
                        if (SUCCEEDED(device->CreateShaderResourceView(tex,&sd,&srv)) && srv) {
                            t.srv=srv; t.id=reinterpret_cast<ImTextureID>(srv); t.w=w; t.h=h; t.valid=true;
                        }
                        tex->Release();
                    }
                    stbi_image_free(data);
                }
            }
        }
        auto& slot = m_ItemTex.emplace(path, t).first->second;
        return slot.valid ? &slot : nullptr;
    }

    // ---- Price label measure + draw ----------------------------------------

    // Measure a price label at the given font size, honoring the display style.
    // Falls back to text when the icon texture is unavailable (missing PNG / no device).
    // iconPath: optional item icon to prepend at the far left (clamped by maxWidth).
    // maxWidth: when > 0, the item icon is dropped if adding it would exceed this width.
    PriceTag MeasurePriceTag(float displayValue, float fontSize,
                             const std::string& iconPath = "", float maxWidth = 0.0f) {
        PriceTag tag;
        float ref = ImGui::GetFontSize();
        float scale = (ref > 0.0f) ? (fontSize / ref) : 1.0f;

        const CurrencyTex* tex = (m_PriceDisplayStyle == PriceDisplayStyle::Image)
            ? GetCurrencyTexture(m_DisplayCurrency) : nullptr;

        if (tex) {
            tag.text = FormatPriceNumberLocal(displayValue);
            ImVec2 ts = ImGui::CalcTextSize(tag.text.c_str());
            tag.textW = ts.x * scale;
            tag.textH = ts.y * scale;
            tag.tex   = tex;
            tag.iconH = tag.textH * kIconHeightMul;
            float aspect = (tex->h > 0) ? (static_cast<float>(tex->w) / tex->h) : 1.0f;
            tag.iconW = tag.iconH * aspect;
            tag.gap   = fontSize * 0.15f;
            tag.totalW = tag.textW + tag.gap + tag.iconW;
            tag.totalH = tag.iconH;   // icon is the taller element
        } else {
            tag.text = FormatPriceLocal(displayValue, m_DisplayCurrency);
            ImVec2 ts = ImGui::CalcTextSize(tag.text.c_str());
            tag.textW = ts.x * scale;
            tag.textH = ts.y * scale;
            tag.totalW = tag.textW;
            tag.totalH = tag.textH;
        }

        // Prepend the item icon if available and it fits (maxWidth<=0 => no cap).
        if (!iconPath.empty()) {
            const CurrencyTex* itex = GetItemTexture(iconPath);
            if (itex) {
                float ih = tag.totalH;                         // match the block height
                float aspect = (itex->h > 0) ? ((float)itex->w / itex->h) : 1.0f;
                float iw = ih * aspect;
                float ig = fontSize * 0.15f;
                float candidate = iw + ig + tag.totalW;
                if (maxWidth <= 0.0f || candidate <= maxWidth) {
                    tag.itemTex = itex; tag.itemIconW = iw; tag.itemIconH = ih; tag.itemGap = ig;
                    tag.totalW = candidate;
                }
            }
        }

        return tag;
    }

    // Draw a measured price label with its top-left at (x, y).
    void DrawPriceTag(ImDrawList* dl, float fontSize, float x, float y,
        const PriceTag& tag, float chaosValue)
    {
        float pad = 2.0f;
        dl->AddRectFilled(ImVec2(x - pad, y - pad),
            ImVec2(x + tag.totalW + pad, y + tag.totalH + pad),
            IM_COL32(0, 0, 0, 200), 2.0f);

        float cursorX = x;
        if (tag.itemTex) {
            float iy = y + (tag.totalH - tag.itemIconH) * 0.5f;
            dl->AddImage(tag.itemTex->id, ImVec2(cursorX, iy),
                ImVec2(cursorX + tag.itemIconW, iy + tag.itemIconH));
            cursorX += tag.itemIconW + tag.itemGap;
        }

        ImU32 col = GetPriceColorLocal(chaosValue, m_CachedDivineInChaos);
        if (tag.tex) {
            float textY = y + (tag.totalH - tag.textH) * 0.5f;
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(cursorX, textY), col, tag.text.c_str());
            float iconX = cursorX + tag.textW + tag.gap;
            float iconY = y + (tag.totalH - tag.iconH) * 0.5f;
            dl->AddImage(tag.tex->id, ImVec2(iconX, iconY),
                ImVec2(iconX + tag.iconW, iconY + tag.iconH));
        } else {
            float textY = tag.itemTex ? (y + (tag.totalH - tag.textH) * 0.5f) : y;
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(cursorX, textY), col, tag.text.c_str());
        }
    }

    float ComputeAdaptiveFontSize(float baseFontSize, float cellW, float cellH) {
        float minDim = (cellW < cellH) ? cellW : cellH;
        float maxTextH = minDim * 0.35f;
        if (baseFontSize > maxTextH && maxTextH > 6.0f)
            return maxTextH;
        return baseFontSize;
    }

    void CalcUiPricePos(float cellX, float cellY, float cellW, float cellH,
        float textW, float textH, float pad, float& outX, float& outY)
    {
        switch (m_UiPricePosition) {
        case UiPricePosition::TopLeft:
            outX = cellX + pad;
            outY = cellY + pad;
            break;
        case UiPricePosition::TopRight:
            outX = cellX + cellW - textW - pad;
            outY = cellY + pad;
            break;
        case UiPricePosition::BottomLeft:
            outX = cellX + pad;
            outY = cellY + cellH - textH - pad;
            break;
        case UiPricePosition::BottomRight:
        default:
            outX = cellX + cellW - textW - pad;
            outY = cellY + cellH - textH - pad;
            break;
        }
    }

    void CalcGroundPricePos(float labelX, float labelY, float labelW, float labelH,
        float textW, float textH, float fontSize, float& outX, float& outY)
    {
        float cx = labelX + labelW * 0.5f;
        switch (m_GroundPricePosition) {
        case GroundPricePosition::Top:
        default:
            outX = cx - textW * 0.5f;
            outY = labelY - fontSize - 4.0f;
            break;
        case GroundPricePosition::Bottom:
            outX = cx - textW * 0.5f;
            outY = labelY + labelH + 4.0f;
            break;
        case GroundPricePosition::Left:
            outX = labelX - textW - 6.0f;
            outY = labelY + (labelH - textH) * 0.5f;
            break;
        case GroundPricePosition::Right:
            outX = labelX + labelW + 6.0f;
            outY = labelY + (labelH - textH) * 0.5f;
            break;
        }
    }

    // ========================================================================
    // Game Window Focus Check
    // ========================================================================

    bool IsGameWindowFocused() {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        // Match by window class — the game's title bar is localized (e.g. the
        // Chinese client shows "流放之路：降临"), so title matching fails there.
        wchar_t cls[256] = {};
        GetClassNameW(fg, cls, 256);
        if (wcscmp(cls, L"POEWindowClass") == 0 || wcscmp(cls, L"POE2WindowClass") == 0)
            return true;
        // Our own overlay window: its class AND title are RANDOMIZED per launch
        // (anti-detection — see main.cpp CreateWindowW with GenerateRandomString),
        // so title/class matching can't identify it. Match by PROCESS instead —
        // the overlay HWND belongs to this (the host) process. Without this,
        // clicking the interactive Runeshape overlay focuses it, the game loses
        // focus, and this hide-when-unfocused gate hid the whole overlay.
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        return fgPid == GetCurrentProcessId();
    }

    // ========================================================================
    // Item-name helpers
    // ========================================================================

    std::string GetItemLookupName(const PluginSDK::InventoryItem& item) {
        if (!item.UniqueName.empty())
            return item.UniqueName;
        if (item.Address) {
            std::string un = ctx()->Inventory.ReadItemUniqueName(item.Address);
            if (!un.empty()) return un;
        }
        if (!item.BaseTypeName.empty())
            return item.BaseTypeName;
        if (item.Address)
            return ctx()->Inventory.ReadItemBaseTypeName(item.Address);
        return "";
    }

    // Lookup name for a WorldItem entity on the ground. UniqueName preferred,
    // BaseTypeName as fallback. Both calls auto-resolve WorldItem containers
    // (see PluginSDK.h:1493-1511). Only non-empty results are cached, so an
    // entity whose name has not streamed in yet retries next frame.
    std::string GetGroundLookupName(const PluginSDK::Entity& entity) {
        auto it = m_NameCache.find(entity.Id);
        if (it != m_NameCache.end()) return it->second;

        std::string un = ctx()->Inventory.ReadItemUniqueName(entity.Address);
        std::string name = !un.empty()
            ? un
            : ctx()->Inventory.ReadItemBaseTypeName(entity.Address);
        if (!name.empty())
            m_NameCache[entity.Id] = name;
        return name;
    }

    // ========================================================================
    // Perf diagnostics helpers
    // ========================================================================

    // High-resolution wall clock in milliseconds (QPC). Frequency is constant for
    // the process lifetime, so it is queried once and cached.
    static double PerfNowMs() {
        static const LARGE_INTEGER freq = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
        }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    }

    // Memoized price lookup (positive AND negative). The host LookupPrice
    // degrades to an O(DB-size) whole-word scan on every exact miss (most
    // gear/rares), and the overlays call it per item EVERY frame — so the miss
    // is exactly the path that must be cached. Keyed by the game's display name,
    // which maps deterministically to a price until the DB refreshes. Render
    // thread only (all callers run inside DrawUI), so no locking is needed.
    PluginSDK::PriceResult CachedLookupPrice(const std::string& name) {
        auto it = m_PriceCache.find(name);
        if (it != m_PriceCache.end()) return it->second;
        PluginSDK::PriceResult r = ctx()->Prices.LookupPrice(name);
        m_PriceCache.emplace(name, r);
        return r;
    }

    // LookupPrice wrapper that accumulates per-frame timing + match-kind counts
    // for the Debug perf panel. Routes through the cross-frame memo, so a cache
    // hit costs ~0 and the panel's "LookupPrice/frame" reads near-zero once the
    // cache is warm (the fix visible in situ). The found/miss counts still tick
    // every frame, so "X found, Y miss" keeps reflecting items priced this frame.
    PluginSDK::PriceResult TimedLookup(const std::string& name) {
        double t0 = PerfNowMs();
        PluginSDK::PriceResult r = CachedLookupPrice(name);
        m_PerfLookupMs += (PerfNowMs() - t0);
        if (r.found) ++m_PerfLookupExact;
        else         ++m_PerfLookupMiss;
        return r;
    }

    // ========================================================================
    // Runeshape Combinations overlay — UI tree helpers
    // ========================================================================

    // Parent address of a UI element (0 on failure / invalid).
    uintptr_t UiParent(uintptr_t addr) {
        return addr ? ctx()->Ui.Read(addr).ParentAddress : 0;
    }

    // Read a UI element's StringId via the host bridge (canonical StringIdPtr
    // offset, host-narrowed to ASCII — identical to the old raw read).
    std::string ReadUiStringId(uintptr_t elemAddr) {
        if (!elemAddr) return std::string();
        return ctx()->Ui.GetStringId(elemAddr);
    }

    // Parse a reward label "Nx ItemName" -> {qty, name}; no "Nx " prefix -> {1, label}.
    // Returns false for labels we never price: the panel title, the unused tab
    // placeholders, and skill rewards.
    static bool ParseReward(const std::string& label, int& outQty, std::string& outName) {
        if (label.empty()) return false;
        if (label == "Runeshape Combinations") return false;
        if (label.rfind("[dnt-", 0) == 0) return false;   // "[dnt-unused] tab one/two"
        if (label.rfind("Skill:", 0) == 0) return false;   // skill rewards: not tradeable

        int qty = 0;
        size_t i = 0;
        while (i < label.size() && label[i] >= '0' && label[i] <= '9') {
            qty = qty * 10 + (label[i] - '0');
            i++;
        }
        if (i > 0 && i + 1 < label.size() && label[i] == 'x' && label[i + 1] == ' ') {
            outQty = (qty < 1) ? 1 : qty;
            outName = label.substr(i + 2);   // strip the "Nx " quantity prefix
        } else {
            outQty = 1;
            outName = label;
        }
        if (outName.empty()) return false;

        // Generic unique-class rewards ("Unique Ring", "Unique Belt", "Unique
        // Amulet", "Unique Body Armour", ...) describe a RANDOM unique of that slot
        // — there is no single tradeable item, so they have no meaningful price.
        // Left unfiltered they slip past the exact lookup and land on LookupPrice's
        // contains-match onto an unrelated unique whose name is a substring (e.g.
        // "Unique Ring" -> a ~23 div unique), painting a bogus price on the panel.
        // No real reward item name begins with the rarity word "Unique ".
        if (outName.rfind("Unique ", 0) == 0) return false;

        return true;
    }

    // Return a recipe row's reward-label element: the first child that carries a
    // non-empty StringId (the symbol icons carry none). 0 if the row has no label.
    uintptr_t GetRowLabelElement(uintptr_t row) {
        if (!row) return 0;
        for (uintptr_t c : ctx()->Ui.GetChildren(row)) {
            if (c && !ctx()->Ui.GetStringId(c).empty())
                return c;
        }
        return 0;
    }

    // A displayed recipe row has rune-symbol icon children (~50x50 unscaled). The
    // label-only reward-pool rows ("Bonus Reward", ...) have none — this tells the
    // recipe list apart from pools during discovery.
    bool RowHasSymbol(uintptr_t row) {
        if (!row) return false;
        for (uintptr_t c : ctx()->Ui.GetChildren(row)) {
            if (!c) continue;
            PluginSDK::UiElement e = ctx()->Ui.Read(c);
            if (e.Valid && e.UnscaledWidth >= 30.0f && e.UnscaledWidth <= 80.0f
                        && e.UnscaledHeight >= 30.0f && e.UnscaledHeight <= 80.0f)
                return true;
        }
        return false;
    }

    // Count a recipe row's rune-symbol children (same ~50x50 heuristic as
    // RowHasSymbol) — the row's combination size, used to disambiguate
    // same-name rewards coming from different-size recipes.
    int CountRowSymbols(uintptr_t row) {
        if (!row) return 0;
        int n = 0;
        for (uintptr_t c : ctx()->Ui.GetChildren(row)) {
            if (!c) continue;
            PluginSDK::UiElement e = ctx()->Ui.Read(c);
            if (e.Valid && e.UnscaledWidth >= 30.0f && e.UnscaledWidth <= 80.0f
                        && e.UnscaledHeight >= 30.0f && e.UnscaledHeight <= 80.0f)
                ++n;
        }
        return n;
    }

    // Bounded BFS for the element whose StringId == target. When visibleOnly,
    // only descend into visible nodes (cheap while the panel is open).
    uintptr_t BfsFindStringId(uintptr_t root, const char* target,
                              bool visibleOnly, int budget) {
        if (!root) return 0;
        std::vector<uintptr_t> q;
        q.reserve(1024);
        q.push_back(root);
        std::unordered_set<uintptr_t> seen;
        size_t head = 0;
        int n = 0;
        while (head < q.size() && n < budget) {
            uintptr_t node = q[head++];
            if (!node || seen.count(node)) continue;
            seen.insert(node);
            n++;
            std::string id = ReadUiStringId(node);
            if (!id.empty() && id == target) return node;
            if (node == root || !visibleOnly || ctx()->Ui.IsVisible(node)) {
                for (uintptr_t c : ctx()->Ui.GetChildren(node))
                    if (c && !seen.count(c)) q.push_back(c);
            }
        }
        return 0;
    }

    // Bounded BFS for the first element whose StringId parses as a reward label AND
    // whose row (parent) has rune-symbol children — i.e. a real recipe row, not a
    // flat reward-pool entry. Anchors discovery onto the recipe list specifically.
    uintptr_t BfsFindRecipeLabel(uintptr_t root, int budget) {
        if (!root) return 0;
        std::vector<uintptr_t> q;
        q.push_back(root);
        std::unordered_set<uintptr_t> seen;
        size_t head = 0;
        int n = 0;
        while (head < q.size() && n < budget) {
            uintptr_t node = q[head++];
            if (!node || seen.count(node)) continue;
            seen.insert(node);
            n++;
            std::string id = ReadUiStringId(node);
            if (!id.empty()) {
                int qty; std::string name;
                if (ParseReward(id, qty, name)
                    && RowHasSymbol(ctx()->Ui.Read(node).ParentAddress))
                    return node;
            }
            for (uintptr_t c : ctx()->Ui.GetChildren(node))
                if (c && !seen.count(c)) q.push_back(c);
        }
        return 0;
    }

    // Find (and cache) the recipe row-list of the open "Runeshape Combinations"
    // panel (the list whose rows carry rune symbols — not the flat reward pools).
    // Returns 0 when the panel isn't present. Once cached, the address is
    // revalidated cheaply on every call (no BFS); the expensive BFS rediscovery is
    // backed off to ~3 s so a closed panel (the common case) costs little — the
    // trade-off is up to ~3 s before prices appear after opening the panel.
    uintptr_t FindRuneshapeRowList() {
        // Fast path: cached list still hosts at least one reward row?
        if (m_RuneshapeListAddr) {
            for (uintptr_t row : ctx()->Ui.GetChildren(m_RuneshapeListAddr)) {
                if (GetRowLabelElement(row)) return m_RuneshapeListAddr;
            }
            m_RuneshapeListAddr = 0;   // stale — rediscover below
            m_RuneshapeWindowAddr = 0;
        }

        // Rediscovery backoff. The cheap fast-path above handles the steady state
        // (open panel, cached list), so this only gates the expensive visible-tree
        // BFS while no list is cached — i.e. before the first open in an area, or
        // briefly after the game tears the subtree down. 800 ms keeps first-open
        // latency low while the visibleOnly BFS stays cheap when the panel is shut.
        auto now = std::chrono::steady_clock::now();
        if (now - m_LastRuneshapeDiscover < std::chrono::milliseconds(800)) return 0;
        m_LastRuneshapeDiscover = now;

        // True UI root = parent of the HUD root that GetUiRoot() returns.
        uintptr_t hudRoot = ctx()->Ui.GetUiRoot();
        if (!hudRoot) return 0;
        uintptr_t urs = UiParent(hudRoot);
        if (!urs) return 0;

        // Title is the stable anchor; visible-pruned BFS reaches it only while the
        // panel is open (its ancestor chain is visible then) — also keeps it cheap.
        uintptr_t title = BfsFindStringId(urs, "Runeshape Combinations", true, 40000);
        if (!title) return 0;

        // Walk up to the panel's top-level window (the direct child of urs). Its own
        // visible bit is the open/closed gate used by ScanRuneshapeRows.
        uintptr_t window = title;
        for (int i = 0; i < 16; i++) {
            uintptr_t p = UiParent(window);
            if (p == urs || p == 0) break;
            window = p;
        }

        // Find one recipe-row label (its row has rune symbols) inside the window;
        // cache its grandparent (the row-list): label.parent == row, row.parent == list.
        uintptr_t anyLabel = BfsFindRecipeLabel(window, 8000);
        if (!anyLabel) return 0;
        uintptr_t row = UiParent(anyLabel);
        uintptr_t list = row ? UiParent(row) : 0;
        if (!list) return 0;
        m_RuneshapeWindowAddr = window;
        m_RuneshapeListAddr = list;
        return list;
    }

    // ========================================================================
    // Runeshape window — movable per-Runeshape overlay (KillCount pattern)
    // ========================================================================

    // Convert a chaos value to the configured display currency.
    float ChaosToDisplay(float chaosVal, float divineInChaos, float exaltedInChaos) const {
        switch (m_DisplayCurrency) {
        case DisplayCurrency::Divine:
            return (divineInChaos > 0.0f) ? chaosVal / divineInChaos : 0.0f;
        case DisplayCurrency::Exalted:
            return (exaltedInChaos > 0.0f) ? chaosVal / exaltedInChaos : 0.0f;
        case DisplayCurrency::Chaos:
        default:
            return chaosVal;
        }
    }

    void DrawRuneshapeWindow() {
        auto rs = ctx()->Runeshape.Runeshapes();

        bool menuVisible = ctx()->Game.IsMenuVisible();

        // Hide-on-hover: vanish while the cursor sits inside LAST frame's window
        // rect. The remembered rect is deliberately kept while hidden (the
        // window isn't submitted then, so it can't refresh) — the window stays
        // gone until the cursor leaves that area, preventing show/hide flicker.
        // Never applied while the Fixer menu is open, so the window can still be
        // configured, dragged and collapsed from the menu.
        if (m_RuneshapeWinHideOnHover && !menuVisible && m_RuneshapeWinRectValid) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= m_RuneshapeWinRectMin.x && mp.x < m_RuneshapeWinRectMax.x &&
                mp.y >= m_RuneshapeWinRectMin.y && mp.y < m_RuneshapeWinRectMax.y) {
                ctx()->Overlay.SetWantsOverlayInput(false);
                return;
            }
        }

        // Render only when there's something to show or the menu is open.
        // While rendered, request interactive overlay input so the title-bar
        // header can be dragged / collapsed even in pure overlay mode: the host
        // (radar/render UpdateInput) only lifts WS_EX_TRANSPARENT while the
        // cursor is over a hit-testable (non-NoInputs) window, and treats a
        // collapsed window's title bar as hit-testable.
        const bool render = !(rs.empty() && !menuVisible);
        ctx()->Overlay.SetWantsOverlayInput(render);
        if (!render) return;

        // Title bar (drag handle) + collapse arrow are intentionally ENABLED.
        // No NoInputs/NoMove: interactivity in overlay mode is gated by the host
        // via the overlay-input request above (clicks still pass through to the
        // game everywhere except while the cursor is over this window).
        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoScrollbar;

        ImGui::SetNextWindowBgAlpha(m_RuneshapeWinAlpha);
        ImGui::SetNextWindowPos(ImVec2(m_RuneshapeWinX, m_RuneshapeWinY),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowCollapsed(m_RuneshapeWinCollapsed, ImGuiCond_Appearing);

        // Begin returns false when the window is collapsed — still save pos +
        // the collapsed state, then bail (the title bar remains interactive).
        // Title-bar X closes the window: re-enable via the "Runeshape window"
        // checkbox in the plugin settings.
        bool keepOpen = true;
        const bool open = ImGui::Begin("Runeshape###RuneshapeWindow", &keepOpen, flags);
        m_RuneshapeWinCollapsed = !open;

        // Remember the on-screen rect (collapsed => title bar only) for the
        // hide-on-hover check above — it runs BEFORE Begin() next frame.
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            m_RuneshapeWinRectMin = wp;
            m_RuneshapeWinRectMax = ImVec2(wp.x + ws.x, wp.y + ws.y);
            m_RuneshapeWinRectValid = true;
        }

        if (!keepOpen) {
            m_ShowRuneshapeWindow = false;
            SaveSettings();
            ImGui::End();
            return;
        }
        {
            ImVec2 wp = ImGui::GetWindowPos();
            if (wp.x != m_RuneshapeWinX || wp.y != m_RuneshapeWinY) {
                m_RuneshapeWinX = wp.x; m_RuneshapeWinY = wp.y;
            }
        }
        if (!open) {
            ImGui::End();
            return;
        }

        // Fetch rates once for the whole window render.
        auto rates = ctx()->Prices.GetRates();
        float divInChaos  = (rates.divineInChaos  > 0.0f) ? rates.divineInChaos  : m_CachedDivineInChaos;
        float exInChaos   = (rates.exaltedInChaos > 0.0f) ? rates.exaltedInChaos
                          : (m_CachedExaltedInChaos > 0.0f ? m_CachedExaltedInChaos : 0.0f);

        if (rs.empty()) {
            ImGui::TextDisabled("No Runeshapes");
        } else {
            float baseFontSize = ImGui::GetFontSize() * m_TextScale;
            const float kSquareSz = baseFontSize * 1.1f;   // small colored square

            for (const auto& r : rs) {
                auto rewards = ctx()->Runeshape.Rewards(r.entityId);
                const bool completed = r.completed;   // activated & cleared → gray entry

                // — measure rune sockets + best price (currency icon + number) so the
                //   auto-resize header reserves room; the header shows, left to right:
                //   [sockets] [price] [weight] (no anchor name — the anchor rune is
                //   visible in its socket). —
                const float lineH = ImGui::GetTextLineHeight();
                const float slotSz  = lineH;                 // compact socket, one line tall
                const float slotGap = 3.0f;
                const float dotsW = (m_RsShowHdrRunes && r.holeCount > 0)
                    ? (r.holeCount * slotSz + (r.holeCount - 1) * slotGap) : 0.0f;

                bool  bestPriced = m_RsShowHdrBest &&
                                   (r.bestIndex >= 0 &&
                                    r.bestIndex < static_cast<int>(rewards.size()) &&
                                    rewards[r.bestIndex].priced);
                float bestDisplay = 0.0f;
                const CurrencyTex* ctex = nullptr;
                std::string bestNum, bestText;   // bestNum (icon path) OR bestText (fallback, has suffix)
                float priceW = 0.0f;
                if (bestPriced) {
                    bestDisplay = ChaosToDisplay(rewards[r.bestIndex].totalChaos, divInChaos, exInChaos);
                    // Shared price style: icon+number, or plain text ("2.5 D").
                    // Text is also the fallback when the icon texture is missing.
                    ctex = (m_RuneshapeWinPriceStyle == PriceDisplayStyle::Image)
                         ? GetCurrencyTexture(m_DisplayCurrency) : nullptr;
                    if (ctex && ctex->valid) {
                        bestNum = FormatPriceNumberLocal(bestDisplay);
                        const float iw = lineH * (ctex->h ? static_cast<float>(ctex->w) / static_cast<float>(ctex->h) : 1.0f);
                        priceW = iw + 4.0f + ImGui::CalcTextSize(bestNum.c_str()).x;
                    } else {
                        bestText = FormatPriceLocal(bestDisplay, m_DisplayCurrency);
                        priceW = ImGui::CalcTextSize(bestText.c_str()).x;
                    }
                }

                // — station best-combination weight (host RuneShape tab), shown
                //   left of the hole dots in the header decorations —
                char  hdrWBuf[16] = {};
                ImU32 hdrWCol = IM_COL32(190, 190, 190, 255);
                float hdrWW = 0.0f;
                if (m_ShowRuneshapeWeights) {
                    snprintf(hdrWBuf, sizeof(hdrWBuf),
                             (r.comboWeight > 0) ? "+%d" : "%d", r.comboWeight);
                    if (r.comboWeight > 0)      hdrWCol = IM_COL32(110, 235, 110, 255);
                    else if (r.comboWeight < 0) hdrWCol = IM_COL32(255, 110, 110, 255);
                    hdrWW = ImGui::CalcTextSize(hdrWBuf).x;
                }

                // Reserve header width for the left-anchored content (the label
                // itself is just padding — sockets/price/weight draw over it).
                const float reserve = dotsW + (priceW > 0.0f ? priceW + 10.0f : 0.0f)
                                    + (hdrWW > 0.0f ? hdrWW + 10.0f : 0.0f) + 8.0f;
                const float spaceW  = ImGui::CalcTextSize(" ").x;
                const int   padCnt  = (spaceW > 0.0f) ? static_cast<int>(ceilf(reserve / spaceW)) : 0;

                // — colored square (vertically centered on the framed header) —
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (m_RsShowHdrColor) {
                    const float frameH = ImGui::GetFrameHeight();
                    const float sqYOff = (frameH > kSquareSz) ? (frameH - kSquareSz) * 0.5f : 0.0f;
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(ImVec2(p.x, p.y + sqYOff),
                                      ImVec2(p.x + kSquareSz, p.y + sqYOff + kSquareSz),
                                      completed ? IM_COL32(120, 120, 120, 255)
                                                : static_cast<ImU32>(r.color), 2.0f);
                    ImGui::Dummy(ImVec2(kSquareSz, frameH));
                    ImGui::SameLine();
                }

                // — collapsible header (per-Runeshape; ###id keyed by color so the
                //   collapsed state persists across maps and sessions) —
                const bool wantOpen = (m_RuneshapeCollapsed.find(r.color) == m_RuneshapeCollapsed.end());
                ImGui::SetNextItemOpen(wantOpen);
                char header[256];
                snprintf(header, sizeof(header), "%s###rscol%08X",
                         std::string(static_cast<size_t>(padCnt), ' ').c_str(),
                         static_cast<unsigned>(r.color));
                const bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
                if (open == !wantOpen) {   // user toggled this frame
                    if (open) m_RuneshapeCollapsed.erase(r.color); else m_RuneshapeCollapsed.insert(r.color);
                    SaveSettings();
                }

                // — header content, drawn left-to-right after the collapse arrow:
                //   [rune sockets] [best price] [weight]. No anchor name — the
                //   anchor rune is visible in its socket. Grayed when completed. —
                {
                    const ImVec2 hmin = ImGui::GetItemRectMin();
                    const ImVec2 hmax = ImGui::GetItemRectMax(); (void)hmax;
                    const float  midY = (hmin.y + hmax.y) * 0.5f;
                    const ImU32  imgTint = completed ? IM_COL32(150, 150, 150, 200)
                                                     : IM_COL32(255, 255, 255, 255);
                    const ImU32  txtCol  = completed ? IM_COL32(150, 150, 150, 220)
                                                     : IM_COL32(255, 255, 255, 255);

                    float xl = hmin.x + ImGui::GetTreeNodeToLabelSpacing();

                    // 1) rune sockets (slot 0 leftmost), game Remnant-bar look:
                    //    RuneBgRegular/-Purple bg (purple = rare rune in that slot),
                    //    each slot filled with the best-priced recipe's rune (anchor
                    //    fallback when no recipe resolved), golden RunePropagation
                    //    crown over the propagating slot(s). Falls back to circles
                    //    when the art is missing.
                    if (m_RsShowHdrRunes) {
                        static const std::string kRsUi    = "Resources/runeshape/ui/";
                        static const std::string kRsRunes = "Resources/runeshape/runes/";
                        // Expedition2Runes row -> name (mirrors expedition2_recipes.json).
                        static const char* kRuneNames[34] = {
                            "Fire", "Cold", "Lightning", "Tempest", "Momentum", "Bloodletting",
                            "Stone", "Adaptive", "Arcane", "Toxic", "Electrocuting", "Protective",
                            "Cyclonic", "Vision", "Tidal", "Rebirth", "Prismatic", "Gasp",
                            "Moon", "Celestial", "Opulent", "Rage", "Wisdom", "Sky",
                            "Earth", "Life", "Bond", "Ward", "Soul", "Death",
                            "Oath", "Time", "Power", "Bait",
                        };
                        const CurrencyTex* bgReg = GetItemTexture(kRsUi + "RuneBgRegular.png");
                        const CurrencyTex* bgPur = GetItemTexture(kRsUi + "RuneBgPurple.png");
                        const CurrencyTex* glow  = GetItemTexture(kRsUi + "RunePropagation.png");

                        for (int slot = 0; slot < r.holeCount; ++slot) {
                            bool prop = false;
                            for (int ps : r.propagatingSlots) if (ps == slot) { prop = true; break; }
                            int rIdx = (slot < static_cast<int>(r.bestRunes.size())) ? r.bestRunes[slot] : -1;
                            if (rIdx < 0 && r.anchorRuneIdx >= 0 && slot == r.anchorPos)
                                rIdx = r.anchorRuneIdx;
                            const bool slotRare = rIdx >= 23 && rIdx <= 32;

                            const ImVec2 p0(xl, midY - slotSz * 0.5f);
                            const ImVec2 p1(xl + slotSz, midY + slotSz * 0.5f);
                            const CurrencyTex* bg = (slotRare && bgPur) ? bgPur : bgReg;
                            if (bg && bg->valid) {
                                dl->AddImage(bg->id, p0, p1, ImVec2(0, 0), ImVec2(1, 1), imgTint);
                            } else {
                                const float dr = slotSz * 0.5f - 1.0f;
                                dl->AddCircleFilled(ImVec2(xl + slotSz * 0.5f, midY), dr, IM_COL32(20,20,20,200));
                                dl->AddCircleFilled(ImVec2(xl + slotSz * 0.5f, midY), dr - 1.0f,
                                                    prop ? IM_COL32(255,210,60,255) : IM_COL32(220,220,220,235));
                            }
                            const CurrencyTex* runeTex = (rIdx >= 0 && rIdx < 34)
                                ? GetItemTexture(kRsRunes + std::string(kRuneNames[rIdx]) + ".png") : nullptr;
                            if (runeTex && runeTex->valid) {
                                const float inset = slotSz * 0.14f;
                                dl->AddImage(runeTex->id, ImVec2(p0.x + inset, p0.y + inset),
                                             ImVec2(p1.x - inset, p1.y - inset),
                                             ImVec2(0, 0), ImVec2(1, 1), imgTint);
                            }
                            if (prop && !completed) {   // crown only while it still matters
                                if (glow && glow->valid) {
                                    // Game crown proportions: 108x176 art on a 100px socket,
                                    // arc hugging the upper rim (top ≈ centerY − 1.2·socket).
                                    const float cx2 = xl + slotSz * 0.5f;
                                    const float gw  = slotSz * 1.08f;
                                    const float gt  = midY - slotSz * 1.20f;
                                    dl->AddImage(glow->id, ImVec2(cx2 - gw * 0.5f, gt),
                                                 ImVec2(cx2 + gw * 0.5f, gt + slotSz * 1.76f));
                                } else if (bg && bg->valid) {
                                    dl->AddCircle(ImVec2(xl + slotSz * 0.5f, midY),
                                                  slotSz * 0.52f, IM_COL32(255,210,60,255), 0, 1.5f);
                                }
                            }
                            xl += slotSz + ((slot + 1 < r.holeCount) ? slotGap : 0.0f);
                        }
                    }

                    // 2) best price (currency icon + number, or text fallback)
                    if (bestPriced) {
                        xl += 10.0f;
                        if (ctex && ctex->valid) {
                            const float iw = lineH * (ctex->h ? static_cast<float>(ctex->w) / static_cast<float>(ctex->h) : 1.0f);
                            dl->AddImage(ctex->id, ImVec2(xl, midY - lineH * 0.5f),
                                         ImVec2(xl + iw, midY + lineH * 0.5f),
                                         ImVec2(0, 0), ImVec2(1, 1), imgTint);
                            xl += iw + 4.0f;
                            dl->AddText(ImVec2(xl, midY - lineH * 0.5f), txtCol, bestNum.c_str());
                            xl += ImGui::CalcTextSize(bestNum.c_str()).x;
                        } else {
                            dl->AddText(ImVec2(xl, midY - lineH * 0.5f), txtCol, bestText.c_str());
                            xl += ImGui::CalcTextSize(bestText.c_str()).x;
                        }
                    }

                    // 3) combination weight
                    if (hdrWW > 0.0f) {
                        xl += 10.0f;
                        dl->AddText(ImVec2(xl, midY - lineH * 0.5f),
                                    completed ? txtCol : hdrWCol, hdrWBuf);
                    }
                }

                // — reward rows (only when expanded); dimmed for a cleared
                //   station. Each row is assembled from toggleable segments:
                //   [icon] [name + qty (+ text-style price)] [image-style price]
                //   [weight] [propagating runes]. SameLine runs BEFORE a segment
                //   (never after it), so a hidden tail can't leak the cursor
                //   onto the next row's line. Weight chips are governed by the
                //   separate "Runeshape weights" checkbox, so they keep the rows
                //   alive even with every per-element toggle off.
                const bool anyRowElement = m_RsShowRowIcon || m_RsShowRowName ||
                                           m_RsShowRowQty || m_RsShowRowPrice ||
                                           m_RsShowRowPropRunes || m_ShowRuneshapeWeights;
                if (open && !rewards.empty() && anyRowElement) {
                    if (completed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
                    ImGui::Indent(kSquareSz + 4.0f);
                    // Currency icon for the image price style (null => text style;
                    // also the fallback when the icon texture is missing).
                    const CurrencyTex* rowCTex =
                        (m_RuneshapeWinPriceStyle == PriceDisplayStyle::Image)
                        ? GetCurrencyTexture(m_DisplayCurrency) : nullptr;
                    for (const auto& rw : rewards) {
                        bool lineStarted = false;

                        // 1) reward item icon (icon path comes from the price DB,
                        //    so unpriced rewards have none to show)
                        if (m_RsShowRowIcon && rw.priced) {
                            auto pr = CachedLookupPrice(rw.name);
                            const CurrencyTex* itex = GetItemTexture(pr.iconPath);
                            if (itex) {
                                float h = ImGui::GetTextLineHeight();
                                ImGui::Image(itex->id, ImVec2(h, h));
                                lineStarted = true;
                            }
                        }

                        // 2) name + quantity — one text item; the TEXT-style price
                        //    is merged into it so spacing matches the old look.
                        std::string txt;
                        if (m_RsShowRowName) txt = rw.name;
                        if (m_RsShowRowQty) {
                            if (!txt.empty()) txt += "  ";
                            txt += "x" + std::to_string(rw.count);
                        }
                        std::string priceStr;   // non-empty => image-style price pending
                        if (m_RsShowRowPrice && rw.priced) {
                            float displayVal = ChaosToDisplay(rw.totalChaos, divInChaos, exInChaos);
                            if (rowCTex) {
                                priceStr = FormatPriceNumberLocal(displayVal);
                            } else {
                                if (!txt.empty()) txt += "   ";
                                txt += FormatPriceLocal(displayVal, m_DisplayCurrency);
                            }
                        }
                        if (!txt.empty()) {
                            if (lineStarted) ImGui::SameLine(0.0f, 4.0f);
                            if (rw.priced) ImGui::TextUnformatted(txt.c_str());
                            else           ImGui::TextDisabled("%s", txt.c_str());   // no price found — dimmed
                            lineStarted = true;
                        }

                        // 3) image-style price: number + currency icon (aspect-
                        //    corrected width, matching the header rendering)
                        if (!priceStr.empty()) {
                            if (lineStarted) ImGui::SameLine(0.0f, 12.0f);
                            ImGui::TextUnformatted(priceStr.c_str());
                            ImGui::SameLine(0.0f, 4.0f);
                            const float ih = ImGui::GetTextLineHeight();
                            const float iw = ih * (rowCTex->h
                                ? static_cast<float>(rowCTex->w) / static_cast<float>(rowCTex->h) : 1.0f);
                            ImGui::Image(rowCTex->id, ImVec2(iw, ih));
                            lineStarted = true;
                        }

                        // Total rune weight of THIS combination (host RuneShape tab).
                        if (m_ShowRuneshapeWeights) {
                            if (lineStarted) ImGui::SameLine(0.0f, 10.0f);
                            const ImVec4 wc = (rw.comboWeight > 0) ? ImVec4(0.43f, 0.92f, 0.43f, 1.0f)
                                            : (rw.comboWeight < 0) ? ImVec4(1.0f, 0.43f, 0.43f, 1.0f)
                                                                   : ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
                            char wbuf[16];
                            snprintf(wbuf, sizeof(wbuf),
                                     (rw.comboWeight > 0) ? "+%d" : "%d", rw.comboWeight);
                            ImGui::TextColored(wc, "%s", wbuf);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Total rune weight of this combination");
                            lineStarted = true;
                        }

                        // Propagating-rune marker (0.5.4 carryover): the propagating
                        // rune ICON(s) with a thin gold ring (replaces the old gold
                        // dot) + the rune name(s). Purple name = rare ("valuable").
                        if (m_RsShowRowPropRunes && rw.propagatingCount > 0 && !rw.propagatingRunes.empty()) {
                            if (lineStarted) ImGui::SameLine(0.0f, 10.0f);
                            ImDrawList* rdl = ImGui::GetWindowDrawList();
                            const float  lh  = ImGui::GetTextLineHeight();
                            const ImVec2 cp  = ImGui::GetCursorScreenPos();

                            static const std::string kRsRunes = "Resources/runeshape/runes/";
                            float px = cp.x;
                            int   drawn = 0;
                            // rw.propagatingRunes = "Power" / "Cold, Time" — split on ", ".
                            for (size_t pos = 0; pos < rw.propagatingRunes.size(); ) {
                                size_t comma = rw.propagatingRunes.find(", ", pos);
                                std::string rn = rw.propagatingRunes.substr(
                                    pos, comma == std::string::npos ? std::string::npos : comma - pos);
                                pos = (comma == std::string::npos) ? rw.propagatingRunes.size() : comma + 2;
                                const CurrencyTex* t = rn.empty() ? nullptr : GetItemTexture(kRsRunes + rn + ".png");
                                if (!t || !t->valid) continue;
                                rdl->AddImage(t->id, ImVec2(px, cp.y), ImVec2(px + lh, cp.y + lh));
                                rdl->AddCircle(ImVec2(px + lh * 0.5f, cp.y + lh * 0.5f),
                                               lh * 0.55f, IM_COL32(255, 210, 60, 220), 0, 1.2f);
                                px += lh + 4.0f;
                                ++drawn;
                            }
                            if (drawn == 0) {   // icons missing — legacy gold dot
                                const float rad = lh * 0.26f;
                                rdl->AddCircleFilled(ImVec2(cp.x + rad, cp.y + lh * 0.5f), rad,
                                                     IM_COL32(255, 210, 60, 255));
                                px = cp.x + rad * 2.0f;
                            }
                            ImGui::Dummy(ImVec2((px - cp.x) + 4.0f, lh));
                            ImGui::SameLine(0.0f, 0.0f);
                            const ImVec4 rcol = rw.propagatingHasRare
                                ? ImVec4(0.80f, 0.52f, 1.0f, 1.0f)   // purple = rare / valuable
                                : ImVec4(1.0f, 0.82f, 0.27f, 1.0f);  // gold
                            ImGui::TextColored(rcol, "%s", rw.propagatingRunes.c_str());
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Propagating rune - carries over to the next remnant");
                        }
                    }
                    ImGui::Unindent(kSquareSz + 4.0f);
                    if (completed) ImGui::PopStyleVar();
                }

                ImGui::Spacing();
            }
        }

        ImGui::End();
    }

    // ========================================================================
    // Runeshape Combinations overlay — scan + draw
    // ========================================================================

    // Rebuild m_RuneshapeRows from the open panel. Runs on the 400 ms tick.
    // Each entry is a priced reward; geometry is intentionally NOT captured here
    // — rects go stale within one scroll step, so DrawRuneshapeOverlay re-reads
    // them every frame.
    void ScanRuneshapeRows() {
        m_RuneshapeRows.clear();
        if (!m_ShowRuneshapePrices && !m_ShowRuneshapeWeights) return;

        uintptr_t list = FindRuneshapeRowList();
        if (!list) return;
        // Panel-open gate: the runeshape window's own visible bit clears when the
        // panel is closed, even though the cached subtree (and row bits) persist.
        if (m_RuneshapeWindowAddr && !ctx()->Ui.IsVisible(m_RuneshapeWindowAddr)) return;

        // (name-lower, qty, size) -> combo weights across all resolved stations
        // (SDK). A row gets a weight only when its match is UNAMBIGUOUS: one
        // distinct weight among the candidates that share the reward name, the
        // quantity and (when countable) the symbol/recipe size — the panel rows
        // carry rune symbols, not rune ids, so composition can't be read back.
        auto toLowerAscii = [](std::string s) {
            for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
            return s;
        };
        std::unordered_map<std::string, std::vector<std::array<int, 3>>> weightIdx;
        if (m_ShowRuneshapeWeights) {
            for (const auto& st : ctx()->Runeshape.Runeshapes()) {
                for (const auto& rw : ctx()->Runeshape.Rewards(st.entityId)) {
                    if (rw.name.empty()) continue;
                    weightIdx[toLowerAscii(rw.name)].push_back(
                        { rw.count, rw.recipeSize, rw.comboWeight });
                }
            }
        }

        for (uintptr_t row : ctx()->Ui.GetChildren(list)) {
            // The list holds hundreds of rows; only the materialized ones carry the
            // row's own visible bit — the rest are parked at relY=0 and would smear
            // across the top of the panel if priced. NOTE: when the list is long
            // enough to scroll, rows below the fold ALSO keep their visible bit
            // (the game clips them to the viewport at render time), so this filter
            // alone is not enough — the draw pass clips geometrically.
            if (!ctx()->Ui.IsVisible(row)) continue;

            uintptr_t label = GetRowLabelElement(row);
            if (!label) continue;
            std::string text = ReadUiStringId(label);
            int qty; std::string name;
            if (!ParseReward(text, qty, name)) continue;

            RuneshapeRow rr;
            rr.rowAddr = row;
            rr.labelAddr = label;

            if (m_ShowRuneshapePrices) {
                PluginSDK::PriceResult price = CachedLookupPrice(name);
                if (price.found) {
                    float total = GetDisplayValue(price, m_DisplayCurrency) * qty;
                    if (total >= 0.001f) {
                        rr.total = total;
                        rr.chaos = price.chaos * qty;
                        rr.iconPath = price.iconPath;
                    }
                }
            }

            if (m_ShowRuneshapeWeights && !weightIdx.empty()) {
                auto it = weightIdx.find(toLowerAscii(name));
                if (it != weightIdx.end()) {
                    const int symCount = CountRowSymbols(row);
                    bool have = false, ambiguous = false;
                    int  wval = 0;
                    for (const auto& cand : it->second) {
                        if (cand[0] != qty) continue;
                        if (symCount > 0 && cand[1] > 0 && cand[1] != symCount) continue;
                        if (!have)                 { have = true; wval = cand[2]; }
                        else if (cand[2] != wval)  { ambiguous = true; break; }
                    }
                    if (have && !ambiguous) { rr.hasWeight = true; rr.weight = wval; }
                }
            }

            // Keep the row only when it has something to draw.
            if (rr.total < 0.001f && !rr.hasWeight) continue;
            m_RuneshapeRows.push_back(rr);
        }
    }

    // Draw the cached priced rows, reading geometry fresh each frame. The price
    // block is placed just LEFT of the reward label (the empty mid-gap between
    // symbols and reward text), so it never overflows the panel's right edge.
    //
    // Scroll clipping: when the recipe list is long enough to scroll, the game
    // lays ALL materialized rows out at their true Y offsets and clips them to
    // the list viewport at render time — scrolled-out rows keep their visible
    // bit, so geometry is the only displayed-row discriminator. The viewport is
    // the intersection of the ancestor container rects (recipe list up to the
    // panel window): whichever ancestor is the real clipper bounds the result,
    // and a content-sized ancestor rect just contributes a looser bound.
    void DrawRuneshapeOverlay() {
        if (m_RuneshapeRows.empty()) return;
        // Per-frame open/closed gate: rows cached by the 400 ms scan must vanish
        // the instant the panel closes (the subtree persists, bits cleared).
        if (m_RuneshapeWindowAddr && !ctx()->Ui.IsVisible(m_RuneshapeWindowAddr)) return;

        PluginSDK::ScreenSize screen = ctx()->Game.GetScreenSize();
        float clipX0 = 0.0f, clipY0 = 0.0f;
        float clipX1 = screen.Width;
        float clipY1 = screen.Height;
        uintptr_t node = m_RuneshapeListAddr;
        for (int i = 0; node && i < 8; ++i) {
            float x, y, w, h;
            if (ctx()->Ui.ComputeScreenRect(node, x, y, w, h) && w > 1.0f && h > 1.0f) {
                clipX0 = (std::max)(clipX0, x);
                clipY0 = (std::max)(clipY0, y);
                clipX1 = (std::min)(clipX1, x + w);
                clipY1 = (std::min)(clipY1, y + h);
            }
            if (node == m_RuneshapeWindowAddr) break;
            node = UiParent(node);
        }
        if (clipX1 <= clipX0 || clipY1 <= clipY0) return;

        ImDrawList* dl = OverlayDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        // Weight chips follow the HOST Radar->RuneShape "Show weights" toggle
        // (one switch controls the map badge and these chips) AND the plugin's
        // own checkbox.
        const bool weightsShown = m_ShowRuneshapeWeights && ctx()->Game.RuneshapeWeightsShown();

        for (const auto& r : m_RuneshapeRows) {
            if (!ctx()->Ui.IsVisible(r.rowAddr)) continue;
            float x, y, w, h;
            if (!ctx()->Ui.ComputeScreenRect(r.labelAddr, x, y, w, h)) continue;
            // A row counts as displayed while its label's center sits inside the
            // viewport; prices appear/disappear as rows scroll across the edge.
            const float cx = x + w * 0.5f, cy = y + h * 0.5f;
            if (cx < clipX0 || cx > clipX1 || cy < clipY0 || cy > clipY1) continue;

            // No adaptive shrink: the price draws in the open gap beside the reward
            // text (not inside a tiny cell), so use the full configured size.
            float fontSize = baseFontSize;
            float gap = fontSize * 0.4f;
            float rightEdge = x - gap;   // blocks stack right-to-left from the label

            if (r.total >= 0.001f) {
                PriceTag tag = MeasurePriceTag(r.total, fontSize,
                    m_ShowItemIcons ? r.iconPath : std::string());
                float labelX = rightEdge - tag.totalW;
                float labelY = y + (h - tag.totalH) * 0.5f;
                DrawPriceTag(dl, fontSize, labelX, labelY, tag, r.chaos);
                rightEdge = labelX - gap * 0.75f;
            }

            // Combination weight badge, left of the price block (or where the
            // price would be when the row is unpriced).
            if (weightsShown && r.hasWeight) {
                char wbuf[16];
                snprintf(wbuf, sizeof(wbuf), (r.weight > 0) ? "+%d" : "%d", r.weight);
                const ImU32 wcol = (r.weight > 0) ? IM_COL32(110, 235, 110, 255)
                                 : (r.weight < 0) ? IM_COL32(255, 110, 110, 255)
                                                  : IM_COL32(205, 205, 205, 255);
                const float ref   = ImGui::GetFontSize();
                const float scale = (ref > 0.0f) ? (fontSize / ref) : 1.0f;
                const ImVec2 ts0  = ImGui::CalcTextSize(wbuf);
                const float wW = ts0.x * scale, wH = ts0.y * scale;
                const float wx = rightEdge - wW;
                const float wy = y + (h - wH) * 0.5f;
                dl->AddRectFilled(ImVec2(wx - 2.0f, wy - 2.0f),
                                  ImVec2(wx + wW + 2.0f, wy + wH + 2.0f),
                                  IM_COL32(0, 0, 0, 200), 2.0f);
                dl->AddText(ImGui::GetFont(), fontSize, ImVec2(wx, wy), wcol, wbuf);
            }
        }
    }

    // ========================================================================
    // Settings Load
    // ========================================================================

    void LoadSettings() {
        namespace fs = std::filesystem;
        fs::path settingsPath = DirectoryPath() / "config" / "settings.json";
        if (!fs::exists(settingsPath)) return;

        try {
            std::ifstream f(settingsPath);
            if (!f.is_open()) return;
            nlohmann::json j = nlohmann::json::parse(f);

            if (j.contains("displayCurrency") && j["displayCurrency"].is_number_integer())
                m_DisplayCurrency =
                    static_cast<DisplayCurrency>(j["displayCurrency"].get<int>());
            if (j.contains("textScale") && j["textScale"].is_number())
                m_TextScale = j["textScale"].get<float>();
            if (j.contains("showGroundPrices") && j["showGroundPrices"].is_boolean())
                m_ShowGroundPrices = j["showGroundPrices"].get<bool>();
            if (j.contains("showInventoryPrices") && j["showInventoryPrices"].is_boolean())
                m_ShowInventoryPrices = j["showInventoryPrices"].get<bool>();
            if (j.contains("showOtherInventoryPrices") &&
                j["showOtherInventoryPrices"].is_boolean())
                m_ShowOtherInventoryPrices = j["showOtherInventoryPrices"].get<bool>();
            if (j.contains("showRitualPrices") && j["showRitualPrices"].is_boolean())
                m_ShowRitualPrices = j["showRitualPrices"].get<bool>();
            if (j.contains("showRuneshapePrices") && j["showRuneshapePrices"].is_boolean())
                m_ShowRuneshapePrices = j["showRuneshapePrices"].get<bool>();
            if (j.contains("showRuneshapeWeights") && j["showRuneshapeWeights"].is_boolean())
                m_ShowRuneshapeWeights = j["showRuneshapeWeights"].get<bool>();
            if (j.contains("showItemIcons") && j["showItemIcons"].is_boolean())
                m_ShowItemIcons = j["showItemIcons"].get<bool>();
            if (j.contains("showRuneshapeWindow") && j["showRuneshapeWindow"].is_boolean())
                m_ShowRuneshapeWindow = j["showRuneshapeWindow"].get<bool>();
            if (j.contains("runeshapeWinX") && j["runeshapeWinX"].is_number())
                m_RuneshapeWinX = j["runeshapeWinX"].get<float>();
            if (j.contains("runeshapeWinY") && j["runeshapeWinY"].is_number())
                m_RuneshapeWinY = j["runeshapeWinY"].get<float>();
            if (j.contains("runeshapeWinAlpha") && j["runeshapeWinAlpha"].is_number())
                m_RuneshapeWinAlpha = std::clamp(j["runeshapeWinAlpha"].get<float>(), 0.1f, 1.0f);
            if (j.contains("runeshapeWinCollapsed") && j["runeshapeWinCollapsed"].is_boolean())
                m_RuneshapeWinCollapsed = j["runeshapeWinCollapsed"].get<bool>();
            if (j.contains("runeshapeCollapsed") && j["runeshapeCollapsed"].is_array()) { m_RuneshapeCollapsed.clear(); for (auto& c : j["runeshapeCollapsed"]) if (c.is_number_unsigned()) m_RuneshapeCollapsed.insert(c.get<uint32_t>()); }
            if (j.contains("rsWinToggleHotkey") && j["rsWinToggleHotkey"].is_number_integer())
                m_RuneshapeWinHotkey = j["rsWinToggleHotkey"].get<int>();
            if (j.contains("rsWinHideOnHover") && j["rsWinHideOnHover"].is_boolean())
                m_RuneshapeWinHideOnHover = j["rsWinHideOnHover"].get<bool>();
            if (j.contains("rsWinPriceStyle") && j["rsWinPriceStyle"].is_number_integer())
                m_RuneshapeWinPriceStyle = static_cast<PriceDisplayStyle>(
                    std::clamp(j["rsWinPriceStyle"].get<int>(), 0, 1));
            if (j.contains("rsWinShowColor") && j["rsWinShowColor"].is_boolean())
                m_RsShowHdrColor = j["rsWinShowColor"].get<bool>();
            if (j.contains("rsWinShowRunes") && j["rsWinShowRunes"].is_boolean())
                m_RsShowHdrRunes = j["rsWinShowRunes"].get<bool>();
            if (j.contains("rsWinShowBestReward") && j["rsWinShowBestReward"].is_boolean())
                m_RsShowHdrBest = j["rsWinShowBestReward"].get<bool>();
            if (j.contains("rsWinShowRewardIcon") && j["rsWinShowRewardIcon"].is_boolean())
                m_RsShowRowIcon = j["rsWinShowRewardIcon"].get<bool>();
            if (j.contains("rsWinShowRewardText") && j["rsWinShowRewardText"].is_boolean())
                m_RsShowRowName = j["rsWinShowRewardText"].get<bool>();
            if (j.contains("rsWinShowRewardQty") && j["rsWinShowRewardQty"].is_boolean())
                m_RsShowRowQty = j["rsWinShowRewardQty"].get<bool>();
            if (j.contains("rsWinShowRewardPrice") && j["rsWinShowRewardPrice"].is_boolean())
                m_RsShowRowPrice = j["rsWinShowRewardPrice"].get<bool>();
            if (j.contains("rsWinShowPropRunes") && j["rsWinShowPropRunes"].is_boolean())
                m_RsShowRowPropRunes = j["rsWinShowPropRunes"].get<bool>();
            if (j.contains("hideWhenUnfocused") && j["hideWhenUnfocused"].is_boolean())
                m_HideWhenUnfocused = j["hideWhenUnfocused"].get<bool>();
            if (j.contains("hideHotkey") && j["hideHotkey"].is_number_integer())
                m_HideHotkey = j["hideHotkey"].get<int>();
            if (j.contains("uiPricePosition") && j["uiPricePosition"].is_number_integer())
                m_UiPricePosition =
                    static_cast<UiPricePosition>(j["uiPricePosition"].get<int>());
            if (j.contains("groundPricePosition") &&
                j["groundPricePosition"].is_number_integer())
                m_GroundPricePosition =
                    static_cast<GroundPricePosition>(j["groundPricePosition"].get<int>());
            if (j.contains("priceDisplayStyle") && j["priceDisplayStyle"].is_number_integer())
                m_PriceDisplayStyle = static_cast<PriceDisplayStyle>(
                    std::clamp(j["priceDisplayStyle"].get<int>(), 0, 1));
            if (j.contains("scanIntervalMs") && j["scanIntervalMs"].is_number_integer())
                m_PerfScanIntervalMs = std::clamp(j["scanIntervalMs"].get<int>(), 50, 2000);
        }
        catch (...) {
            ctx()->Log.Warn("[NinjaPricer] Failed to load settings, using defaults");
        }
    }

    // ========================================================================
    // Members
    // ========================================================================

    DisplayCurrency m_DisplayCurrency = DisplayCurrency::Divine;
    PriceDisplayStyle m_PriceDisplayStyle = PriceDisplayStyle::Image;  // default: icon
    float m_TextScale = 1.0f;
    bool m_ShowGroundPrices = true;
    bool m_ShowInventoryPrices = false;       // Off by default — minor frame-time impact
    bool m_ShowOtherInventoryPrices = false;  // Off by default — minor frame-time impact
    bool m_ShowRitualPrices = true;           // On by default — Ritual "Favours" shop pricing
    bool m_ShowRuneshapePrices = true;        // On by default — Runeshape Combinations reward pricing
    bool m_ShowRuneshapeWeights = true;       // On by default — total rune weight per combination (host RuneShape tab weights)
    bool m_ShowItemIcons = true;              // On by default — item icon left of price tag
    bool m_HideWhenUnfocused = true;
    int m_HideHotkey = 0;
    UiPricePosition m_UiPricePosition = UiPricePosition::BottomRight;
    GroundPricePosition m_GroundPricePosition = GroundPricePosition::Top;

    // Cached divine rate (for color thresholds in GetPriceColorLocal).
    float m_CachedDivineInChaos   = 1.0f;
    // Cached exalted rate — warm fallback when GetRates() returns 0 between fetches.
    float m_CachedExaltedInChaos  = 0.0f;

    // Runtime caches
    std::unordered_map<uint32_t, std::string> m_NameCache;
    uint64_t m_LastAreaChange = 0;
    std::chrono::steady_clock::time_point m_LastInventoryScan;
    // Ground-item price tags, rebuilt by ScanGroundItems() on the scan interval and
    // drawn every frame by DrawGroundTags(). Decouples the heavy per-item resolve
    // (entity enumeration + RPM) from the cheap per-frame WorldToScreen render.
    struct GroundTag { float wx, wy, wz; float displayValue; float chaos; std::string iconPath; };
    std::vector<GroundTag> m_GroundTags;
    std::chrono::steady_clock::time_point m_LastGroundScan;
    // Per-entity-address resolve cache so the throttled ground scan re-resolves only
    // newly-dropped items (persistent items reuse the prior result). Holds the
    // RPM-derived bits (price + stack); currency-dependent display value is cheap and
    // recomputed each scan. Cleared on area change and on every price-cache flush
    // (DB refresh / 30 s) so prices stay fresh. `epoch` marks presence for eviction.
    struct GroundResolve {
        bool priceable = false;
        float wx = 0, wy = 0, wz = 0;
        PluginSDK::PriceResult price{};
        int stackMultiplier = 1;
        uint32_t epoch = 0;
    };
    std::unordered_map<uintptr_t, GroundResolve> m_GroundResolveCache;
    uint32_t m_GroundScanEpoch = 0;

    // Inventory data cache — refreshed once per Scan tick. Avoids
    // 60-FPS bridge round-trips through Inventory.GetAll() + per-item
    // FetchString. Cleared on area change and on every scan refresh.
    std::vector<PluginSDK::Inventory> m_CachedInventories;

    // Per-item rarity cache for the high-value unique gate. Keyed by
    // InventoryItem::Address, cleared alongside m_CachedInventories.
    std::unordered_map<uintptr_t, int> m_RarityCache;

    // Cross-frame price memo (positive AND negative). Without it the inventory/
    // ground overlays re-run the host LookupPrice for every visible item every
    // frame; that lookup is O(DB-size) on a miss (most gear/rares), so a full
    // stash tab sustains DrawUI > 50 ms and the host perf-watchdog disables the
    // plugin. Memoizing by display name collapses the per-frame cost to O(items)
    // hash lookups. Cleared on area change and when the host DB refreshes (rate
    // shift) or on a 30 s safety interval. See CachedLookupPrice / TimedLookup.
    std::unordered_map<std::string, PluginSDK::PriceResult> m_PriceCache;
    float m_PriceCacheDivineSeen  = 0.0f;
    float m_PriceCacheExaltedSeen = 0.0f;
    std::chrono::steady_clock::time_point m_LastPriceCacheFlush;

    // ---- Perf diagnostics (Debug tab "Performance" section) ----------------
    // All render-thread (DrawUI) timings, in milliseconds, peak-held. The scan-
    // call/get-all timings update only on a scan tick; draw/lookup update every
    // frame. Purpose: localize the price-overlay delay to one of —
    //   Scan()     : host async request cost (expected ~0; confirms decoupling)
    //   GetAll()   : ABI marshaling of every inventory/item across the bridge
    //   DrawInv    : per-frame inventory overlay (incl. LookupPrice)
    //   Ground     : per-frame ground-item overlay (incl. LookupPrice + ABI)
    //   Lookup     : time + count of LookupPrice
    // m_PerfScanIntervalMs is the (tunable) inventory refresh throttle. With the
    // decoupled read below, perceived latency ≈ interval + one worker cycle, so a
    // modest default already feels instant. kInvReadDelayMs is how long after the
    // async Scan() request we pick up the worker's result (≈ one 60 FPS cycle).
    static constexpr int kInvReadDelayMs = 60;
    int    m_PerfScanIntervalMs = 100;    // was hardcoded 1000 ms (caused 1-3 s lag)
    bool   m_InvReadPending = false;      // a Scan() was requested; result not yet read
    double m_PerfScanCallMs = 0.0;
    double m_PerfGetAllMs = 0.0;
    double m_PerfDrawInvMs = 0.0;
    double m_PerfGroundMs = 0.0;
    double m_PerfLookupMs = 0.0;          // per-frame sum across all draw paths
    double m_PerfPeakScanCallMs = 0.0;
    double m_PerfPeakGetAllMs = 0.0;
    double m_PerfPeakDrawInvMs = 0.0;
    double m_PerfPeakGroundMs = 0.0;
    double m_PerfPeakLookupMs = 0.0;
    int    m_PerfLookupExact = 0;         // per-frame counts (reset each DrawUI)
    int    m_PerfLookupMiss = 0;
    int    m_PerfCachedInvCount = 0;
    int    m_PerfCachedItemCount = 0;

    // ---- Runeshape movable window ----
    bool  m_ShowRuneshapeWindow = true;
    float m_RuneshapeWinX = 100.0f;
    float m_RuneshapeWinY = 100.0f;
    float m_RuneshapeWinAlpha = 0.85f;
    bool  m_RuneshapeWinCollapsed = false;   // persisted collapse state of the window
    std::unordered_set<uint32_t> m_RuneshapeCollapsed; // persisted per-element collapse (keyed by color)

    // Advanced overlay settings (Overlay Toggles -> "Runeshape window: advanced").
    int   m_RuneshapeWinHotkey = 0;              // VK toggling the window; 0 = unbound
    bool  m_RuneshapeWinHotkeyWasDown = false;   // edge-detector state (not persisted)
    bool  m_RuneshapeWinHideOnHover = false;     // hide while hovered (never while the menu is open)
    PriceDisplayStyle m_RuneshapeWinPriceStyle = PriceDisplayStyle::Image; // shared: reward rows + header best reward
    bool  m_RsShowHdrColor  = true;              // header: colored square
    bool  m_RsShowHdrRunes  = true;              // header: rune-socket strip (+ propagation crowns)
    bool  m_RsShowHdrBest   = true;              // header: best-reward price
    bool  m_RsShowRowIcon   = true;              // rows: reward item icon
    bool  m_RsShowRowName   = true;              // rows: reward name
    bool  m_RsShowRowQty    = true;              // rows: xN quantity
    bool  m_RsShowRowPrice  = true;              // rows: price
    bool  m_RsShowRowPropRunes = true;           // rows: propagating (yellow-glow) runes
    // Last on-screen window rect for hide-on-hover. Deliberately KEPT while the
    // window is hover-hidden (it isn't submitted then, so it can't refresh) —
    // the window stays hidden until the cursor leaves this area (no flicker).
    ImVec2 m_RuneshapeWinRectMin = ImVec2(0.0f, 0.0f);
    ImVec2 m_RuneshapeWinRectMax = ImVec2(0.0f, 0.0f);
    bool   m_RuneshapeWinRectValid = false;

    // ---- Runeshape Combinations overlay ----
    // Cached row-list container of the open "Runeshape Combinations" panel
    // (0 = not found). Rediscovered via bounded BFS on cache-miss, throttled.
    uintptr_t m_RuneshapeListAddr = 0;
    uintptr_t m_RuneshapeWindowAddr = 0;   // runeshape window; own visible bit = panel open/closed
    std::chrono::steady_clock::time_point m_LastRuneshapeDiscover;
    std::chrono::steady_clock::time_point m_LastRuneshapeScan;

    // One priced reward row. Rebuilt on the 400 ms scan tick (parse + price
    // lookup only); geometry is re-read each frame at draw time so prices track
    // scrolling instantly and clip to the list viewport.
    struct RuneshapeRow {
        uintptr_t rowAddr = 0;      // recipe-row element (own visible bit)
        uintptr_t labelAddr = 0;    // reward-label child (price anchor rect)
        float total = 0;            // display-currency value (unit * qty)
        float chaos = 0;            // chaos value (unit * qty), for color
        std::string iconPath;       // item icon path (from PriceIconCache)
        // Total rune weight of this row's combination (SDK comboWeight), set
        // only when the (name, qty, symbol-count) match is unambiguous.
        int  weight = 0;
        bool hasWeight = false;
    };
    std::vector<RuneshapeRow> m_RuneshapeRows;

    // UI state — hotkey capture. Points at the binding currently being captured
    // (one capture at a time across all hotkey rows); null when idle.
    int* m_CaptureTarget = nullptr;

    // Currency icon textures (image price style). Lazily loaded from
    // Resources/currency/poe2/{divine,exalted,chaos}.png, indexed by
    // DisplayCurrency; released on disable/destroy. CurrencyTex is declared in
    // the Drawing Helpers section above.
    CurrencyTex m_CurrencyTex[3];
    bool        m_CurrencyTexTried[3] = { false, false, false };
};

// ============================================================================
// v6 factory exports
// ============================================================================

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    return new NinjaPricerPlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* plugin) {
    delete plugin;
}
