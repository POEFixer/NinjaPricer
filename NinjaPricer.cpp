// ============================================================================
// NinjaPricer — multi-source price overlay plugin for POE2 (v6 SDK)
// ============================================================================
// Displays item prices from poe.ninja or poe2scout on dropped items and
// in inventory.
// ============================================================================

#include "sdk/PluginSDK.h"
#include <imgui.h>
#include <d3d11.h>

// stb_image for loading currency icon PNGs (image price-display style).
// Implementation lives in this single plugin TU.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "src/IPriceSource.h"
#include "src/NinjaSource.h"
#include "src/ScoutSource.h"
#include "src/NinjaApi.h"

#include <fstream>
#include <filesystem>
#include <string>
#include <chrono>
#include <thread>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>

using namespace PriceApi;

// Fallback leagues if API fetch fails (current season first).
// The live list comes from NinjaApi::FetchLeagues(); this is only used when that
// request fails. Refresh the season entries each league.
static const char* kFallbackLeagues[] = {
    "Runes of Aldur",
    "HC Runes of Aldur",
    "Standard",
    "Hardcore",
};

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

// Bridge: PluginSDK::LogService -> PriceApi::LogFunc (C function pointer).
// The price source threads expect a `void(*)(const char*, const char*)`. The
// SDK LogService is an object, so we wrap it in a thread-local pointer and
// expose a free function. Only used during fetch thread lifetime; SetActiveLog
// is called before StartFetchThread and cleared in StopFetchThread.
static thread_local const PluginSDK::LogService* tls_logSvc = nullptr;
static void PriceApiLogBridge(const char* level, const char* msg) {
    if (tls_logSvc) tls_logSvc->Log(level ? level : "Info", msg ? msg : "");
}

class NinjaPricerPlugin : public PluginSDK::Plugin {
public:
    ~NinjaPricerPlugin() override {
        StopFetchThread();
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
        StartFetchThread();
        ctx()->Log.Info("[NinjaPricer] Plugin enabled");
    }

    void OnDisable() override {
        StopFetchThread();
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
        if (ImGui::BeginTabItem("Categories")) {
            DrawTabCategories();
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

        ImGui::Text("Price Source:");
        int prevSource = m_DataSource;
        ImGui::RadioButton("poe.ninja", &m_DataSource, 0); ImGui::SameLine();
        ImGui::RadioButton("poe2scout", &m_DataSource, 1);
        if (m_DataSource != prevSource)
            RestartFetchThread();

        ImGui::Spacing();

        ImGui::Text("League:");
        ImGui::SetNextItemWidth(250.0f);

        const auto& leagues = GetLeagues();
        if (ImGui::BeginCombo("##League", m_League.c_str())) {
            for (int i = 0; i < (int)leagues.size(); i++) {
                bool selected = (m_League == leagues[i]);
                if (ImGui::Selectable(leagues[i].c_str(), selected)) {
                    if (m_League != leagues[i]) {
                        m_League = leagues[i];
                        RestartFetchThread();
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        bool loading = m_IsLoading.load();
        if (loading) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Loading...");
        }
        else {
            if (ImGui::Button("Refresh Prices"))
                TriggerRefresh();
        }

        ImGui::SliderInt("Refresh interval (min)", &m_RefreshIntervalMin, 15, 180);

        ImGui::Spacing();

        {
            std::shared_lock<std::shared_mutex> lock(m_DbMutex);
            if (m_PriceDb.loaded) {
                auto elapsed = std::chrono::steady_clock::now() - m_PriceDb.lastUpdate;
                auto mins = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
                ImGui::Text("Items loaded: %d | Updated %lld min ago",
                    m_PriceDb.totalItems, (long long)mins);
                ImGui::Text("Rates: 1 Divine = %.1f Chaos | 1 Exalted = %.1f Chaos",
                    m_PriceDb.divineInChaos, m_PriceDb.exaltedInChaos);
            }
            else {
                ImGui::TextDisabled("No price data loaded");
            }
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

        ImGui::Checkbox("Hide when game not focused", &m_HideWhenUnfocused);

        ImGui::Separator();

        ImGui::Text("Hold-to-hide hotkey:");
        ImGui::SameLine();
        if (m_CapturingHotkey) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                "Press any key... (ESC to cancel)");
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                m_CapturingHotkey = false;
            }
            else {
                for (int vk = 0x08; vk < 0xFF; vk++) {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    if (vk == VK_ESCAPE) continue;
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        m_HideHotkey = vk;
                        m_CapturingHotkey = false;
                        break;
                    }
                }
            }
        }
        else {
            ImGui::Text("%s", GetVkName(m_HideHotkey));
            ImGui::SameLine();
            if (ImGui::Button("Set Hotkey", ImVec2(100.0f, 0.0f))) {
                m_CapturingHotkey = true;
            }
            if (m_HideHotkey != 0) {
                ImGui::SameLine();
                if (ImGui::Button("Clear", ImVec2(60.0f, 0.0f)))
                    m_HideHotkey = 0;
            }
        }
    }

    void DrawTabCategories() {
        ImGui::Spacing();

        float availW = ImGui::GetContentRegionAvail().x;
        int columns = (availW > 600.0f) ? 4 : (availW > 400.0f) ? 3 : 2;

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Currency Categories");
        ImGui::Spacing();
        if (ImGui::BeginTable("##CurrCats", columns)) {
            for (int i = 0; i < kMaxCurrencyCategories; i++) {
                ImGui::TableNextColumn();
                bool isScoutOnly = (i >= kScoutOnlyCurrencyStart);
                if (isScoutOnly && m_DataSource == 0) ImGui::BeginDisabled();
                ImGui::Checkbox(kCurrencyCategories[i].label, &m_CurrencyEnabled[i]);
                if (isScoutOnly && m_DataSource == 0) ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Unique Categories (poe2scout only)");
        ImGui::Spacing();
        if (ImGui::BeginTable("##UniqCats", columns)) {
            for (int i = 0; i < kMaxUniqueCategories; i++) {
                ImGui::TableNextColumn();
                if (m_DataSource == 0) ImGui::BeginDisabled();
                ImGui::Checkbox(kUniqueCategories[i].label, &m_UniqueEnabled[i]);
                if (m_DataSource == 0) ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
    }

    // ========================================================================
    // Overlay Rendering
    // ========================================================================

    void DrawUI() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        if (m_HideWhenUnfocused && !IsGameWindowFocused()) return;
        if (m_HideHotkey != 0 && (GetAsyncKeyState(m_HideHotkey) & 0x8000)) return;

        PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot();
        if (!snap.IsAttached) return;
        if (snap.State != PluginSDK::GameState::InGame) return;

        if (snap.AreaChangeCounter != m_LastAreaChange) {
            m_NameCache.clear();
            m_CachedInventories.clear();
            m_RarityCache.clear();
            m_LastInventoryScan = {};   // force immediate refresh in new area
            m_LastAreaChange = snap.AreaChangeCounter;
        }

        std::shared_lock<std::shared_mutex> lock(m_DbMutex);
        if (!m_PriceDb.loaded) return;

        m_CachedDivineInChaos = m_PriceDb.divineInChaos;

        if (m_ShowGroundPrices) {
            DrawGroundItemPrices(snap);
        }

        if (m_ShowInventoryPrices || m_ShowOtherInventoryPrices || m_ShowRitualPrices) {
            auto now = std::chrono::steady_clock::now();
            if (now - m_LastInventoryScan > std::chrono::seconds(1)) {
                ctx()->Inventory.Scan(-1);
                // Single GetAll() per scan tick — without this cache, every
                // frame at 60 FPS pays the full ABI cost: bridge enumerate
                // + per-inventory enumerate_items + 3× FetchString per item.
                m_CachedInventories = ctx()->Inventory.GetAll();
                m_RarityCache.clear();
                m_LastInventoryScan = now;
            }
            DrawInventoryOverlays();
        }
    }

    void SaveSettings() override {
        namespace fs = std::filesystem;
        fs::path configDir = DirectoryPath() / "config";
        std::error_code ec;
        fs::create_directories(configDir, ec);

        try {
            nlohmann::json j;
            j["dataSource"] = m_DataSource;
            j["league"] = m_League;
            j["displayCurrency"] = static_cast<int>(m_DisplayCurrency);
            j["textScale"] = m_TextScale;
            j["showGroundPrices"] = m_ShowGroundPrices;
            j["showInventoryPrices"] = m_ShowInventoryPrices;
            j["showOtherInventoryPrices"] = m_ShowOtherInventoryPrices;
            j["showRitualPrices"] = m_ShowRitualPrices;
            j["refreshIntervalMin"] = m_RefreshIntervalMin;
            j["hideWhenUnfocused"] = m_HideWhenUnfocused;
            j["hideHotkey"] = m_HideHotkey;
            j["uiPricePosition"] = static_cast<int>(m_UiPricePosition);
            j["groundPricePosition"] = static_cast<int>(m_GroundPricePosition);
            j["priceDisplayStyle"] = static_cast<int>(m_PriceDisplayStyle);

            nlohmann::json curr = nlohmann::json::array();
            for (int i = 0; i < kMaxCurrencyCategories; i++)
                curr.push_back(m_CurrencyEnabled[i]);
            j["currencyEnabled"] = curr;

            nlohmann::json uniq = nlohmann::json::array();
            for (int i = 0; i < kMaxUniqueCategories; i++)
                uniq.push_back(m_UniqueEnabled[i]);
            j["uniqueEnabled"] = uniq;

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

    void DrawDebugPanel() {
        PluginSDK::Snapshot snap = ctx()->Game.GetSnapshot();

        ImGui::Text("Source: %s", (m_DataSource == 0) ? "poe.ninja" : "poe2scout");

        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Price Database:");
        {
            std::shared_lock<std::shared_mutex> lock(m_DbMutex);
            ImGui::Text("  Loaded: %s", m_PriceDb.loaded ? "YES" : "NO");
            ImGui::Text("  Total items: %d", m_PriceDb.totalItems);
            ImGui::Text("  DivineInChaos: %.2f", m_PriceDb.divineInChaos);
            ImGui::Text("  ExaltedInChaos: %.2f", m_PriceDb.exaltedInChaos);
            ImGui::Text("  Map size: %d", (int)m_PriceDb.items.size());

            if (m_PriceDb.loaded && ImGui::TreeNode("Sample DB entries (first 10)")) {
                int count = 0;
                for (auto& [key, item] : m_PriceDb.items) {
                    if (count++ >= 10) break;
                    ImGui::Text("  '%s' -> %.1f chaos (%.4f div)", key.c_str(), item.chaosValue,
                        (m_PriceDb.divineInChaos > 0)
                            ? item.chaosValue / m_PriceDb.divineInChaos : 0.0f);
                }
                ImGui::TreePop();
            }
        }

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

                std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                auto price = LookupPrice(m_PriceDb, name);

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
                    ? (std::string(" -> ") + FormatPrice(
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
                    std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                    auto price = LookupPrice(m_PriceDb, dn);
                    if (price.found) matchCount++;
                }
                ImGui::Text("  Items with price match: %d / %d",
                    matchCount, (int)inv.Items.size());

                if (ImGui::TreeNode("Item list (first 10)")) {
                    int count = 0;
                    for (const auto& item : inv.Items) {
                        if (count++ >= 10) break;
                        std::string dn = GetItemLookupName(item);

                        std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                        auto price = LookupPrice(m_PriceDb, dn);

                        ImGui::Text("  [%d,%d] Lookup:'%s' (stack:%d) path:'%s'",
                            item.SlotX, item.SlotY, dn.c_str(),
                            item.StackCount, item.Path.c_str());
                        ImGui::Text("    Price: %s", price.found
                            ? FormatPrice(GetDisplayValue(price, m_DisplayCurrency),
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

    void DrawGroundItemPrices(const PluginSDK::Snapshot& snap) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;
        float baseRefSize = ImGui::GetFontSize();
        if (baseRefSize <= 0.f) return;

        for (const auto& e : snap.Entities) {
            if (e.EntityType != PluginSDK::EntityType::Item) continue;

            std::string name = GetGroundLookupName(e);
            if (name.empty()) continue;

            PriceLookupResult price = LookupPrice(m_PriceDb, name);
            if (!price.found) continue;

            // Resolve the inner item once; reused for both unique-rarity gate
            // and stack multiplier so we never call GetWorldItemInner twice.
            auto inner = ctx()->Entities.GetWorldItemInner(e.Address);

            // Unique-category guard: LookupPrice can land on a unique entry
            // via its contains-match path (e.g. base-type "Heavy Belt" hitting
            // "Headhunter"). Require the actual entity to have Rarity == 3
            // (Unique) before showing a unique price.
            if (IsUniqueCategory(price.category)) {
                if (!inner || !inner->Components.HasMods()) continue;
                auto mods = ctx()->Components.ReadMods(inner->Components.Mods);
                if (!mods.Valid || mods.Rarity != 3) continue;
            }

            int stackMultiplier = 1;
            if (inner && inner->Components.HasStack()) {
                int sc = ctx()->Components.GetStackCount(inner->Components.Stack);
                if (sc > 1) stackMultiplier = sc;
            }

            float displayValue =
                GetDisplayValue(price, m_DisplayCurrency) * stackMultiplier;
            if (displayValue < 0.001f) continue;

            float sx, sy;
            if (!ctx()->Render.WorldToScreen(e.WorldX, e.WorldY, e.WorldZ, sx, sy))
                continue;

            PriceTag tag = MeasurePriceTag(displayValue, baseFontSize);

            // Anchor is a single screen point (zero-sized box). CalcGroundPricePos
            // already centres the block around (sx, sy) with the four presets.
            float blockX, blockY;
            CalcGroundPricePos(sx, sy, /*labelW=*/0.0f, /*labelH=*/0.0f,
                               tag.totalW, tag.totalH, baseFontSize, blockX, blockY);

            DrawPriceTag(dl, baseFontSize, blockX, blockY, tag,
                         price.chaosValue * stackMultiplier);
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
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        for (const auto& item : inv.Items) {
            std::string displayName = GetItemLookupName(item);
            if (displayName.empty()) continue;

            PriceLookupResult price = LookupPrice(m_PriceDb, displayName);
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

            PriceTag tag = MeasurePriceTag(displayValue, fontSize);

            float labelX, labelY;
            CalcUiPricePos(cellX, cellY, cellW, cellH, tag.totalW, tag.totalH, 2.0f, labelX, labelY);

            DrawPriceTag(dl, fontSize, labelX, labelY, tag, price.chaosValue * multiplier);
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
    }

    // ---- Price label measure + draw ----------------------------------------

    // Measure a price label at the given font size, honoring the display style.
    // Falls back to text when the icon texture is unavailable (missing PNG / no device).
    PriceTag MeasurePriceTag(float displayValue, float fontSize) {
        PriceTag tag;
        float ref = ImGui::GetFontSize();
        float scale = (ref > 0.0f) ? (fontSize / ref) : 1.0f;

        const CurrencyTex* tex = (m_PriceDisplayStyle == PriceDisplayStyle::Image)
            ? GetCurrencyTexture(m_DisplayCurrency) : nullptr;

        if (tex) {
            tag.text = PriceApi::FormatPriceNumber(displayValue);
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
            tag.text = PriceApi::FormatPrice(displayValue, m_DisplayCurrency);
            ImVec2 ts = ImGui::CalcTextSize(tag.text.c_str());
            tag.textW = ts.x * scale;
            tag.textH = ts.y * scale;
            tag.totalW = tag.textW;
            tag.totalH = tag.textH;
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

        ImU32 col = PriceApi::GetPriceColor(chaosValue, m_CachedDivineInChaos);
        if (tag.tex) {
            // Value text vertically centered against the (taller) icon.
            float textY = y + (tag.totalH - tag.textH) * 0.5f;
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(x, textY), col, tag.text.c_str());
            float iconX = x + tag.textW + tag.gap;
            float iconY = y + (tag.totalH - tag.iconH) * 0.5f;
            dl->AddImage(tag.tex->id, ImVec2(iconX, iconY),
                ImVec2(iconX + tag.iconW, iconY + tag.iconH));
        } else {
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(x, y), col, tag.text.c_str());
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
    // League Discovery
    // ========================================================================

    const std::vector<std::string>& GetLeagues() {
        if (!m_LeaguesFetched) {
            m_LeaguesFetched = true;
            m_Leagues = NinjaApi::FetchLeagues();
            if (m_Leagues.empty()) {
                for (const char* name : kFallbackLeagues)
                    m_Leagues.push_back(name);
            }
        }
        return m_Leagues;
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
        // Our own overlay window (title contains "POEFixer", not localized).
        wchar_t title[256] = {};
        GetWindowTextW(fg, title, 256);
        return wcsstr(title, L"POEFixer") != nullptr;
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
    // Fetch Thread
    // ========================================================================

    void StartFetchThread() {
        if (m_FetchThread.joinable()) return;
        m_Running.store(true);

        m_FetchThread = std::thread([this]() {
            // Make the SDK log service available to PriceApiLogBridge on this thread.
            tls_logSvc = &ctx()->Log;
            // Hard exception boundary. A plugin worker thread must NEVER let an
            // exception escape — that calls std::terminate and kills the entire
            // host process. A malformed/truncated price-API response makes
            // nlohmann::json::parse throw parse_error; this is the last-resort
            // backstop should any inner handler be bypassed.
            try {
                while (m_Running.load()) {
                    {
                        PriceDatabase tempDb;
                        IPriceSource& source = (m_DataSource == 0)
                            ? static_cast<IPriceSource&>(m_NinjaSource)
                            : static_cast<IPriceSource&>(m_ScoutSource);
                        source.FetchCategories(
                            m_League, DirectoryPath(),
                            m_CurrencyEnabled, m_UniqueEnabled,
                            tempDb, m_IsLoading, m_Running,
                            &PriceApiLogBridge);

                        if (m_Running.load()) {
                            std::unique_lock<std::shared_mutex> lock(m_DbMutex);
                            m_PriceDb = std::move(tempDb);
                        }
                    }

                    for (int i = 0; i < m_RefreshIntervalMin * 60 && m_Running.load(); i++) {
                        Sleep(1000);
                        if (m_ForceRefresh.load()) {
                            m_ForceRefresh.store(false);
                            break;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                m_IsLoading.store(false);
                if (tls_logSvc)
                    tls_logSvc->Log("Error",
                        (std::string("[NinjaPricer] fetch thread aborted: ") + e.what()).c_str());
            }
            catch (...) {
                m_IsLoading.store(false);
                if (tls_logSvc)
                    tls_logSvc->Log("Error",
                        "[NinjaPricer] fetch thread aborted: unknown exception");
            }
            tls_logSvc = nullptr;
        });
    }

    void StopFetchThread() {
        m_Running.store(false);
        if (m_FetchThread.joinable())
            m_FetchThread.join();
    }

    void TriggerRefresh() {
        m_ForceRefresh.store(true);
    }

    void RestartFetchThread() {
        StopFetchThread();
        {
            std::unique_lock<std::shared_mutex> lock(m_DbMutex);
            m_PriceDb = PriceDatabase{};
        }
        StartFetchThread();
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

            if (j.contains("dataSource") && j["dataSource"].is_number_integer())
                m_DataSource = std::clamp(j["dataSource"].get<int>(), 0, 1);
            if (j.contains("league") && j["league"].is_string())
                m_League = j["league"].get<std::string>();
            // League rollover (one-time): bump last season's default to the current
            // league so existing configs stop pricing against the ended league.
            // Deliberate choices (Standard / Hardcore / other) are preserved.
            if (m_League == "Fate of the Vaal")          m_League = "Runes of Aldur";
            else if (m_League == "HC Fate of the Vaal")  m_League = "HC Runes of Aldur";
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
            if (j.contains("refreshIntervalMin") &&
                j["refreshIntervalMin"].is_number_integer())
                m_RefreshIntervalMin = std::clamp(j["refreshIntervalMin"].get<int>(), 15, 180);
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

            if (j.contains("currencyEnabled") && j["currencyEnabled"].is_array()) {
                auto& arr = j["currencyEnabled"];
                for (int i = 0; i < kMaxCurrencyCategories && i < (int)arr.size(); i++) {
                    if (arr[i].is_boolean())
                        m_CurrencyEnabled[i] = arr[i].get<bool>();
                }
            }
            else if (j.contains("categoryEnabled") && j["categoryEnabled"].is_array()) {
                auto& cats = j["categoryEnabled"];
                for (int i = 0; i < 13 && i < (int)cats.size(); i++) {
                    if (cats[i].is_boolean())
                        m_CurrencyEnabled[i] = cats[i].get<bool>();
                }
            }

            if (j.contains("uniqueEnabled") && j["uniqueEnabled"].is_array()) {
                auto& arr = j["uniqueEnabled"];
                for (int i = 0; i < kMaxUniqueCategories && i < (int)arr.size(); i++) {
                    if (arr[i].is_boolean())
                        m_UniqueEnabled[i] = arr[i].get<bool>();
                }
            }
        }
        catch (...) {
            ctx()->Log.Warn("[NinjaPricer] Failed to load settings, using defaults");
        }
    }

    // ========================================================================
    // Members
    // ========================================================================

    std::string m_League = "Runes of Aldur";
    DisplayCurrency m_DisplayCurrency = DisplayCurrency::Divine;
    PriceDisplayStyle m_PriceDisplayStyle = PriceDisplayStyle::Image;  // default: icon
    float m_TextScale = 1.0f;
    bool m_ShowGroundPrices = true;
    bool m_ShowInventoryPrices = false;       // Off by default — minor frame-time impact
    bool m_ShowOtherInventoryPrices = false;  // Off by default — minor frame-time impact
    bool m_ShowRitualPrices = true;           // On by default — Ritual "Favours" shop pricing
    int m_RefreshIntervalMin = 60;            // 60-min refresh - lighter on poe.ninja / poe2scout
    bool m_HideWhenUnfocused = true;
    int m_HideHotkey = 0;
    UiPricePosition m_UiPricePosition = UiPricePosition::BottomRight;
    GroundPricePosition m_GroundPricePosition = GroundPricePosition::Top;
    int m_DataSource = 1;
    bool m_CurrencyEnabled[kMaxCurrencyCategories] = {
        true, true, true, true, true, true, true, true,
        true, true, true, true, true, true, true, true,
        true
    };
    bool m_UniqueEnabled[kMaxUniqueCategories] = {
        true, true, true, true, true, true, true
    };

    // Price data
    PriceDatabase m_PriceDb;
    std::shared_mutex m_DbMutex;
    std::atomic<bool> m_IsLoading{ false };
    float m_CachedDivineInChaos = 1.0f;

    // Price source instances
    NinjaSource m_NinjaSource;
    ScoutSource m_ScoutSource;

    // Fetch thread
    std::thread m_FetchThread;
    std::atomic<bool> m_Running{ false };
    std::atomic<bool> m_ForceRefresh{ false };

    // Runtime caches
    std::unordered_map<uint32_t, std::string> m_NameCache;
    uint64_t m_LastAreaChange = 0;
    std::chrono::steady_clock::time_point m_LastInventoryScan;

    // Inventory data cache — refreshed once per Scan tick (1 Hz). Avoids
    // 60-FPS bridge round-trips through Inventory.GetAll() + per-item
    // FetchString. Cleared on area change and on every scan refresh.
    std::vector<PluginSDK::Inventory> m_CachedInventories;

    // Per-item rarity cache for the unique-category gate. Keyed by
    // InventoryItem::Address, cleared alongside m_CachedInventories.
    std::unordered_map<uintptr_t, int> m_RarityCache;

    // League cache
    std::vector<std::string> m_Leagues;
    bool m_LeaguesFetched = false;

    // UI state
    bool m_CapturingHotkey = false;

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
