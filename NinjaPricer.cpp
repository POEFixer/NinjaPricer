// ============================================================================
// NinjaPricer — multi-source price overlay plugin for POE2 (v6 SDK)
// ============================================================================
// Displays item prices from poe.ninja or poe2scout on dropped items and
// in inventory.
// ============================================================================

#include "sdk/PluginSDK.h"
#include <imgui.h>

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
#include <algorithm>

using namespace PriceApi;

// Fallback leagues if API fetch fails
static const char* kFallbackLeagues[] = {
    "Fate of the Vaal",
    "HC Fate of the Vaal",
    "Standard",
    "Hardcore",
};

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
        ImGui::Checkbox("Show prices in stash", &m_ShowOtherInventoryPrices);
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
            m_LastAreaChange = snap.AreaChangeCounter;
        }

        if (m_ShowInventoryPrices || m_ShowOtherInventoryPrices) {
            auto now = std::chrono::steady_clock::now();
            if (now - m_LastInventoryScan > std::chrono::seconds(1)) {
                ctx()->Inventory.Scan(-1);
                m_LastInventoryScan = now;
            }
        }

        std::shared_lock<std::shared_mutex> lock(m_DbMutex);
        if (!m_PriceDb.loaded) return;

        m_CachedDivineInChaos = m_PriceDb.divineInChaos;

        if (m_ShowGroundPrices) {
            DrawGroundItemPrices(snap);
        }

        // v6: FindPanelByStringId replaces the v5 brute-force scan over
        // child indices 25-35.
        uintptr_t gameUiRoot = ctx()->Ui.GetGameUiRoot();
        uintptr_t inventoryPanel = 0;
        uintptr_t stashPanel = 0;
        if (gameUiRoot != 0) {
            inventoryPanel = ctx()->Ui.FindPanelByStringId(gameUiRoot, "Inventory");
            stashPanel = ctx()->Ui.FindPanelByStringId(gameUiRoot, "Stash");
        }

        if (m_ShowInventoryPrices && inventoryPanel != 0
            && ctx()->Ui.IsVisible(inventoryPanel)) {
            DrawInventoryPrices();
        }

        if (m_ShowOtherInventoryPrices && stashPanel != 0
            && ctx()->Ui.IsVisible(stashPanel)) {
            DrawStashPrices(stashPanel);
        }
    }

    void SaveSettings() override {
        namespace fs = std::filesystem;
        fs::path configDir = fs::path(Directory()) / "config";
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
            j["refreshIntervalMin"] = m_RefreshIntervalMin;
            j["hideWhenUnfocused"] = m_HideWhenUnfocused;
            j["hideHotkey"] = m_HideHotkey;
            j["uiPricePosition"] = static_cast<int>(m_UiPricePosition);
            j["groundPricePosition"] = static_cast<int>(m_GroundPricePosition);

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

                std::string name = GetEntityLookupName(e);
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
    // Ground Item Prices (via UI tree: GameUi[7][0][0] and [7][0][1])
    // ========================================================================

    void DrawGroundItemPrices(const PluginSDK::Snapshot& /*snap*/) {
        uintptr_t gameUiAddr = ctx()->Ui.GetGameUiRoot();
        if (gameUiAddr == 0) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        const int containerIndices[] = { 7, 0 };
        uintptr_t containerBase = ctx()->Ui.FollowPath(gameUiAddr, containerIndices, 2);
        if (containerBase == 0) return;

        // Both containers: [0]=normal, [1]=hovered
        for (int ci = 0; ci < 2; ci++) {
            uintptr_t containerAddr = ctx()->Ui.GetChildAt(containerBase, ci);
            if (containerAddr == 0) continue;

            auto children = ctx()->Ui.GetChildren(containerAddr);
            for (uintptr_t childAddr : children) {
                if (childAddr == 0) continue;
                if (!ctx()->Ui.IsVisible(childAddr)) continue;

                PluginSDK::UiElement elemData = ctx()->Ui.Read(childAddr);
                if (elemData.ElementType != 0x4084) continue;

                std::string itemName = ctx()->Ui.GetStringId(childAddr);
                if (itemName.empty()) continue;

                int stackMultiplier = 1;
                std::string lookupName = ParseGroundItemName(itemName, stackMultiplier);
                if (lookupName.empty()) continue;

                PriceLookupResult price = LookupPrice(m_PriceDb, lookupName);
                if (!price.found) continue;

                if (IsUniqueCategory(price.category)) {
                    std::string lowerLookup = ToLower(lookupName);
                    std::string lowerUnique = ToLower(price.itemName);
                    if (lowerLookup.find(lowerUnique) == std::string::npos)
                        continue;
                }

                float displayValue = GetDisplayValue(price, m_DisplayCurrency) * stackMultiplier;
                if (displayValue < 0.001f) continue;

                float posX, posY, sizeW, sizeH;
                if (!ctx()->Ui.ComputeScreenRect(childAddr, posX, posY, sizeW, sizeH))
                    continue;
                if (sizeW < 1.0f) continue;

                std::string text = FormatPrice(displayValue, m_DisplayCurrency);
                ImVec2 ts = ImGui::CalcTextSize(text.c_str());
                float sw = ts.x * (baseFontSize / ImGui::GetFontSize());
                float sh = ts.y * (baseFontSize / ImGui::GetFontSize());

                float textX, textY;
                CalcGroundPricePos(posX, posY, sizeW, sizeH, sw, sh, baseFontSize, textX, textY);

                DrawPriceLabelAt(dl, baseFontSize, textX, textY, sw, sh, text,
                                 price.chaosValue * stackMultiplier);
            }
        }
    }

    // ========================================================================
    // Inventory Prices — uses InventoryService::Get(1) for main inventory
    // ========================================================================

    void DrawInventoryPrices() {
        PluginSDK::Inventory inv = ctx()->Inventory.Get(1);  // main inventory
        if (inv.InventoryId != 1) return;
        if (!inv.Grid.Valid || inv.Grid.CellSize < 1.0f) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        for (const auto& item : inv.Items) {
            std::string displayName = GetItemLookupName(item);
            if (displayName.empty()) continue;

            PriceLookupResult price = LookupPrice(m_PriceDb, displayName);
            if (!price.found) continue;

            if (IsUniqueCategory(price.category) && item.Address) {
                if (ctx()->Inventory.ReadItemRarity(item.Address) != 3)
                    continue;
            }

            int multiplier = (item.StackCount > 1) ? item.StackCount : 1;
            float displayValue = GetDisplayValue(price, m_DisplayCurrency) * multiplier;
            if (displayValue < 0.001f) continue;

            std::string text = FormatPrice(displayValue, m_DisplayCurrency);

            float cellX = inv.Grid.GridScreenX + item.SlotX * inv.Grid.CellSize;
            float cellY = inv.Grid.GridScreenY + item.SlotY * inv.Grid.CellSize;
            float cellW = item.Width * inv.Grid.CellSize;
            float cellH = item.Height * inv.Grid.CellSize;

            float fontSize = ComputeAdaptiveFontSize(baseFontSize, cellW, cellH);

            ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            float sw = ts.x * (fontSize / ImGui::GetFontSize());
            float sh = ts.y * (fontSize / ImGui::GetFontSize());

            float labelX, labelY;
            CalcUiPricePos(cellX, cellY, cellW, cellH, sw, sh, 2.0f, labelX, labelY);

            DrawPriceLabelAt(dl, fontSize, labelX, labelY, sw, sh, text,
                             price.chaosValue * multiplier);
        }
    }

    // ========================================================================
    // Stash Prices — inventory-data-driven, position-based rendering
    // ========================================================================

    void DrawStashPrices(uintptr_t stashRoot) {
        if (stashRoot == 0) return;

        // Navigate: root→2→0→0→0→1→1 → tab list
        const int tabListIndices[] = { 2, 0, 0, 0, 1, 1 };
        uintptr_t tabList = ctx()->Ui.FollowPath(stashRoot, tabListIndices, 6);
        if (tabList == 0) return;

        auto tabs = ctx()->Ui.GetChildren(tabList);
        int activeTabIdx = -1;
        for (int i = 0; i < (int)tabs.size(); i++) {
            if (tabs[i] != 0 && ctx()->Ui.IsVisible(tabs[i])) {
                activeTabIdx = i;
                break;
            }
        }
        if (activeTabIdx < 0) return;

        auto now = std::chrono::steady_clock::now();
        if (now - m_LastStashScan > std::chrono::seconds(1)) {
            ctx()->Inventory.Scan(-1);
            m_LastStashScan = now;
        }

        int stashInvId = 133 + activeTabIdx;
        PluginSDK::Inventory stashInv = ctx()->Inventory.Get(stashInvId);
        if (stashInv.Items.empty()) return;
        if (stashInv.TotalBoxesX <= 0 || stashInv.TotalBoxesY <= 0) return;

        uintptr_t activeTabAddr = tabs[activeTabIdx];
        const int itemContainerIndices[] = { 0, 0 };
        uintptr_t itemContainer = ctx()->Ui.FollowPath(activeTabAddr, itemContainerIndices, 2);
        if (itemContainer == 0) return;

        float containerX, containerY, containerW, containerH;
        if (!ctx()->Ui.ComputeScreenRect(itemContainer,
                                          containerX, containerY, containerW, containerH))
            return;
        if (containerW < 1.0f || containerH < 1.0f) return;

        float cellW = containerW / static_cast<float>(stashInv.TotalBoxesX);
        float cellH = containerH / static_cast<float>(stashInv.TotalBoxesY);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        for (const auto& item : stashInv.Items) {
            std::string dn = GetItemLookupName(item);
            if (dn.empty()) continue;

            PriceLookupResult price = LookupPrice(m_PriceDb, dn);
            if (!price.found) continue;

            if (IsUniqueCategory(price.category) && item.Address) {
                if (ctx()->Inventory.ReadItemRarity(item.Address) != 3)
                    continue;
            }

            int multiplier = (item.StackCount > 1) ? item.StackCount : 1;
            float displayValue = GetDisplayValue(price, m_DisplayCurrency) * multiplier;
            if (displayValue < 0.001f) continue;

            std::string text = FormatPrice(displayValue, m_DisplayCurrency);

            float posX = containerX + item.SlotX * cellW;
            float posY = containerY + item.SlotY * cellH;
            float itemW = item.Width * cellW;
            float itemH = item.Height * cellH;

            float fontSize = ComputeAdaptiveFontSize(baseFontSize, itemW, itemH);

            ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            float sw = ts.x * (fontSize / ImGui::GetFontSize());
            float sh = ts.y * (fontSize / ImGui::GetFontSize());

            float labelX, labelY;
            CalcUiPricePos(posX, posY, itemW, itemH, sw, sh, 2.0f, labelX, labelY);

            DrawPriceLabelAt(dl, fontSize, labelX, labelY, sw, sh, text,
                             price.chaosValue * multiplier);
        }
    }

    // ========================================================================
    // Drawing Helpers
    // ========================================================================

    void DrawPriceLabelAt(ImDrawList* dl, float fontSize, float x, float y,
        float w, float h, const std::string& text, float chaosValue)
    {
        float pad = 2.0f;
        dl->AddRectFilled(ImVec2(x - pad, y - pad), ImVec2(x + w + pad, y + h + pad),
            IM_COL32(0, 0, 0, 200), 2.0f);
        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(x, y),
            PriceApi::GetPriceColor(chaosValue, m_CachedDivineInChaos), text.c_str());
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
        wchar_t title[256] = {};
        GetWindowTextW(fg, title, 256);
        return (wcsstr(title, L"Path of Exile") != nullptr ||
                wcsstr(title, L"POEFixer") != nullptr);
    }

    // ========================================================================
    // Item-name helpers
    // ========================================================================

    static std::string ParseGroundItemName(const std::string& raw, int& outMultiplier) {
        outMultiplier = 1;
        if (raw.size() < 3) return raw;

        size_t i = 0;
        while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') i++;
        if (i > 0 && i < raw.size() && raw[i] == 'x'
            && i + 1 < raw.size() && raw[i + 1] == ' ') {
            outMultiplier = std::atoi(raw.c_str());
            if (outMultiplier < 1) outMultiplier = 1;
            return raw.substr(i + 2);
        }
        return raw;
    }

    std::string GetItemBaseTypeName(const PluginSDK::Entity& entity) {
        auto it = m_NameCache.find(entity.Id);
        if (it != m_NameCache.end()) return it->second;

        std::string name = ctx()->Inventory.ReadItemBaseTypeName(entity.Address);
        m_NameCache[entity.Id] = name;
        return name;
    }

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

    std::string GetEntityLookupName(const PluginSDK::Entity& entity) {
        std::string un = ctx()->Inventory.ReadItemUniqueName(entity.Address);
        if (!un.empty()) return un;
        return GetItemBaseTypeName(entity);
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
            while (m_Running.load()) {
                {
                    PriceDatabase tempDb;
                    IPriceSource& source = (m_DataSource == 0)
                        ? static_cast<IPriceSource&>(m_NinjaSource)
                        : static_cast<IPriceSource&>(m_ScoutSource);
                    source.FetchCategories(
                        m_League, std::string(Directory()),
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
        fs::path settingsPath = fs::path(Directory()) / "config" / "settings.json";
        if (!fs::exists(settingsPath)) return;

        try {
            std::ifstream f(settingsPath);
            if (!f.is_open()) return;
            nlohmann::json j = nlohmann::json::parse(f);

            if (j.contains("dataSource") && j["dataSource"].is_number_integer())
                m_DataSource = std::clamp(j["dataSource"].get<int>(), 0, 1);
            if (j.contains("league") && j["league"].is_string())
                m_League = j["league"].get<std::string>();
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

    std::string m_League = "Fate of the Vaal";
    DisplayCurrency m_DisplayCurrency = DisplayCurrency::Divine;
    float m_TextScale = 1.0f;
    bool m_ShowGroundPrices = true;
    bool m_ShowInventoryPrices = true;
    bool m_ShowOtherInventoryPrices = false;
    int m_RefreshIntervalMin = 15;
    bool m_HideWhenUnfocused = true;
    int m_HideHotkey = 0;
    UiPricePosition m_UiPricePosition = UiPricePosition::BottomRight;
    GroundPricePosition m_GroundPricePosition = GroundPricePosition::Top;
    int m_DataSource = 1;
    bool m_CurrencyEnabled[kMaxCurrencyCategories] = {
        true, true, true, true, true, true, true, true,
        true, true, true, true, true, true, true
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
    std::chrono::steady_clock::time_point m_LastStashScan;

    // League cache
    std::vector<std::string> m_Leagues;
    bool m_LeaguesFetched = false;

    // UI state
    bool m_CapturingHotkey = false;
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
