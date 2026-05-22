// ============================================================================
// NinjaPricer — multi-source price overlay plugin for POE2
// ============================================================================
// Displays item prices from poe.ninja or poe2scout on dropped items and
// in inventory.
// ============================================================================

#include "sdk/PluginHelpers.h"
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

using namespace PluginSDK;
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

class NinjaPricerPlugin : public IPlugin {
public:
    ~NinjaPricerPlugin() {
        StopFetchThread();
    }

    // ========================================================================
    // IPlugin lifecycle
    // ========================================================================

    void SetPluginDirectory(const char* dir) override {
        m_Directory = dir;
    }

    void SetContext(PluginContext* context) override {
        m_Context = context;
        if (m_Context && m_Context->ImGuiContext) {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context->ImGuiContext));
        }
    }

    void OnEnable(bool isGameOpened) override {
        LoadSettings();
        StartFetchThread();
        if (m_Context) {
            m_Context->Log("Info", "[NinjaPricer] Plugin enabled");
        }
    }

    void OnDisable() override {
        StopFetchThread();
        if (m_Context) {
            m_Context->Log("Info", "[NinjaPricer] Plugin disabled");
        }
    }

    const char* GetName() override { return "Ninja Pricer"; }

    bool WantsOverlay() override { return true; }

    // ========================================================================
    // Settings UI
    // ========================================================================

    void DrawSettings() override {
        if (!m_Context) return;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context->ImGuiContext));

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

    // ========================================================================
    // Settings Tab Methods
    // ========================================================================

    void DrawTabDataSource() {
        ImGui::Spacing();

        // English-only matching warning
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
        ImGui::TextWrapped(
            "Warning: POE2 must be set to English. Item names are matched "
            "in English only - other languages will not work."
        );
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Source selector
        ImGui::Text("Price Source:");
        int prevSource = m_DataSource;
        ImGui::RadioButton("poe.ninja", &m_DataSource, 0); ImGui::SameLine();
        ImGui::RadioButton("poe2scout", &m_DataSource, 1);
        if (m_DataSource != prevSource) {
            RestartFetchThread();
        }

        ImGui::Spacing();

        // League combo box
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
            if (ImGui::Button("Refresh Prices")) {
                TriggerRefresh();
            }
        }

        ImGui::SliderInt("Refresh interval (min)", &m_RefreshIntervalMin, 15, 180);

        ImGui::Spacing();

        // Status
        {
            std::shared_lock<std::shared_mutex> lock(m_DbMutex);
            if (m_PriceDb.loaded) {
                auto elapsed = std::chrono::steady_clock::now() - m_PriceDb.lastUpdate;
                auto mins = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
                ImGui::Text("Items loaded: %d | Updated %lld min ago", m_PriceDb.totalItems, (long long)mins);
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

        // Price position for UI items (inventory/stash)
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

        // Price position for ground items
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

        // Hotkey to hide prices
        ImGui::Text("Hold-to-hide hotkey:");
        ImGui::SameLine();
        if (m_CapturingHotkey) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Press any key... (ESC to cancel)");
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
                if (ImGui::Button("Clear", ImVec2(60.0f, 0.0f))) {
                    m_HideHotkey = 0;
                }
            }
        }
    }

    void DrawTabCategories() {
        ImGui::Spacing();

        float availW = ImGui::GetContentRegionAvail().x;
        int columns = (availW > 600.0f) ? 4 : (availW > 400.0f) ? 3 : 2;

        // --- Currency Categories ---
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

        // --- Unique Categories (poe2scout only) ---
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
        if (!m_Context) return;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(m_Context->ImGuiContext));

        // Hide when game window is not focused
        if (m_HideWhenUnfocused && !IsGameWindowFocused()) return;

        // Hide when hotkey is held
        if (m_HideHotkey != 0 && (GetAsyncKeyState(m_HideHotkey) & 0x8000)) return;

        auto snap = m_Context->GetSnapshot();
        if (!snap || !snap->IsAttached) return;
        if (snap->CurrentState != GameStateTypes::InGameState) return;

        // Check area change — clear name cache
        if (snap->AreaChangeCounter != m_LastAreaChange) {
            m_NameCache.clear();
            m_InvRootIndex = -1;
            m_StashRootIndex = -1;
            m_LastAreaChange = snap->AreaChangeCounter;
        }

        // Request inventory scan if needed (do before lock)
        if (m_ShowInventoryPrices || m_ShowOtherInventoryPrices) {
            auto now = std::chrono::steady_clock::now();
            if (now - m_LastInventoryScan > std::chrono::seconds(1)) {
                if (m_Context->RequestInventoryScan)
                    m_Context->RequestInventoryScan(-1);
                m_LastInventoryScan = now;
            }
        }

        // Acquire price data (shared lock) — ONE lock for the entire frame
        std::shared_lock<std::shared_mutex> lock(m_DbMutex);
        if (!m_PriceDb.loaded) return;

        // Cache rate locally for GetPriceColor (avoids re-locking)
        m_CachedDivineInChaos = m_PriceDb.divineInChaos;

        if (m_ShowGroundPrices) {
            DrawGroundItemPrices(snap);
        }

        if (m_ShowInventoryPrices && IsInventoryPanelVisible()) {
            DrawInventoryPrices(snap);
        }

        if (m_ShowOtherInventoryPrices) {
            DrawStashPrices();
        }
    }

    // ========================================================================
    // Settings Persistence
    // ========================================================================

    void SaveSettings() override {
        namespace fs = std::filesystem;
        fs::path configDir = fs::path(m_Directory) / "config";
        if (!fs::exists(configDir))
            fs::create_directories(configDir);

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
    // Debug Diagnostics Panel
    // ========================================================================

    void DrawDebugPanel() {
        auto snap = m_Context->GetSnapshot();

        ImGui::Text("Source: %s", (m_DataSource == 0) ? "poe.ninja" : "poe2scout");

        // --- Price Database ---
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Price Database:");
        {
            std::shared_lock<std::shared_mutex> lock(m_DbMutex);
            ImGui::Text("  Loaded: %s", m_PriceDb.loaded ? "YES" : "NO");
            ImGui::Text("  Total items: %d", m_PriceDb.totalItems);
            ImGui::Text("  DivineInChaos: %.2f", m_PriceDb.divineInChaos);
            ImGui::Text("  ExaltedInChaos: %.2f", m_PriceDb.exaltedInChaos);
            ImGui::Text("  Map size: %d", (int)m_PriceDb.items.size());

            // Show first 10 items from DB
            if (m_PriceDb.loaded && ImGui::TreeNode("Sample DB entries (first 10)")) {
                int count = 0;
                for (auto& [key, item] : m_PriceDb.items) {
                    if (count++ >= 10) break;
                    ImGui::Text("  '%s' -> %.1f chaos (%.4f div)", key.c_str(), item.chaosValue,
                        (m_PriceDb.divineInChaos > 0) ? item.chaosValue / m_PriceDb.divineInChaos : 0.0f);
                }
                ImGui::TreePop();
            }
        }

        ImGui::Separator();

        if (!snap) {
            ImGui::TextDisabled("No snapshot available");
            return;
        }

        // --- Game State ---
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Game State:");
        ImGui::Text("  Attached: %s", snap->IsAttached ? "YES" : "NO");
        ImGui::Text("  State: %d (InGame=4)", (int)snap->CurrentState);
        ImGui::Text("  Area: %s (level %d)", snap->CurrentAreaName.c_str(), snap->CurrentAreaLevel);
        ImGui::Text("  IsHideout: %s, IsTown: %s", snap->IsHideout ? "YES" : "NO", snap->IsTown ? "YES" : "NO");

        ImGui::Separator();

        // --- Entity Items ---
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Ground Items (Entities):");
        int totalEntities = (int)snap->Entities.size();
        int worldItems = 0;
        for (auto& e : snap->Entities) {
            if (e.entityType == EntityTypes::Item && e.entitySubtype == EntitySubtypes::WorldItem)
                worldItems++;
        }
        ImGui::Text("  Total entities: %d", totalEntities);
        ImGui::Text("  WorldItem entities: %d", worldItems);
        ImGui::Text("  Name cache size: %d", (int)m_NameCache.size());

        if (worldItems > 0 && ImGui::TreeNode("WorldItem details (first 5)")) {
            int count = 0;
            for (auto& e : snap->Entities) {
                if (e.entityType != EntityTypes::Item || e.entitySubtype != EntitySubtypes::WorldItem) continue;
                if (count++ >= 5) break;

                std::string name = GetEntityLookupName(e);
                if (name.empty()) name = "(not read yet)";

                std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                auto price = LookupPrice(m_PriceDb, name);

                ImGui::Text("  ID:%u Addr:0x%llX Valid:%d", e.Id, (unsigned long long)e.Address, e.IsValid);
                ImGui::Text("    Name: '%s'", name.c_str());
                ImGui::Text("    Pos: (%.0f, %.0f, %.0f) Zone:%d",
                    e.WorldX, e.WorldY, e.WorldZ, (int)e.Zone);
                if (m_Context->WorldToScreen) {
                    float sx, sy;
                    bool vis = m_Context->WorldToScreen(e.WorldX, e.WorldY, e.WorldZ, &sx, &sy);
                    ImGui::Text("    Screen: (%.0f, %.0f) Visible:%s", sx, sy, vis ? "YES" : "NO");
                }
                ImGui::Text("    Price found: %s%s", price.found ? "YES" : "NO",
                    price.found ? (std::string(" -> ") + FormatPrice(GetDisplayValue(price, m_DisplayCurrency), m_DisplayCurrency)).c_str() : "");
                ImGui::Spacing();
            }
            ImGui::TreePop();
        }

        ImGui::Separator();

        // --- Inventory ---
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Inventory Data:");
        ImGui::Text("  Inventories in snapshot: %d", (int)snap->Inventories.size());
        ImGui::Text("  RequestInventoryScan: %s", m_Context->RequestInventoryScan ? "available" : "NULL");
        ImGui::Text("  GetInventoryName: %s", m_Context->GetInventoryName ? "available" : "NULL");

        // InventoryGrid info
        auto& grid = snap->InventoryGrid;
        ImGui::Text("  InventoryGrid.IsValid: %s", grid.IsValid ? "YES" : "NO");
        if (grid.IsValid) {
            ImGui::Text("    ScreenPos: (%.0f, %.0f)", grid.GridScreenX, grid.GridScreenY);
            ImGui::Text("    Size: (%.0f x %.0f)", grid.GridWidth, grid.GridHeight);
            ImGui::Text("    CellSize: %.1f", grid.CellSize);
            ImGui::Text("    UiAddress: 0x%llX", (unsigned long long)grid.UiAddress);
        }

        // Per-inventory details
        for (const auto& inv : snap->Inventories) {
            const char* invName = "(unknown)";
            if (m_Context->GetInventoryName) {
                const char* n = m_Context->GetInventoryName(inv.Id);
                if (n) invName = n;
            }

            if (ImGui::TreeNode((void*)(intptr_t)inv.Id, "Inventory #%d '%s' (%dx%d, %d items)",
                inv.Id, invName, inv.TotalBoxesX, inv.TotalBoxesY, (int)inv.Items.size()))
            {
                int matchCount = 0;
                for (const auto& item : inv.Items) {
                    std::string dn = GetItemLookupName(item);
                    std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                    auto price = LookupPrice(m_PriceDb, dn);
                    if (price.found) matchCount++;
                }
                ImGui::Text("  Items with price match: %d / %d", matchCount, (int)inv.Items.size());

                if (ImGui::TreeNode("Item list (first 10)")) {
                    int count = 0;
                    for (const auto& item : inv.Items) {
                        if (count++ >= 10) break;

                        std::string dn = GetItemLookupName(item);

                        std::shared_lock<std::shared_mutex> lock(m_DbMutex);
                        auto price = LookupPrice(m_PriceDb, dn);

                        ImGui::Text("  [%d,%d] Lookup:'%s' (stack:%d) path:'%s'",
                            item.SlotX, item.SlotY, dn.c_str(), item.StackCount, item.Path.c_str());
                        ImGui::Text("    Price: %s", price.found ? FormatPrice(
                            GetDisplayValue(price, m_DisplayCurrency), m_DisplayCurrency).c_str() : "NOT FOUND");
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
        }

        ImGui::Separator();

        // --- Rendering check ---
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Rendering:");
        ImGui::Text("  WorldToScreen: %s", m_Context->WorldToScreen ? "available" : "NULL");
        ImGui::Text("  ReadItemBaseTypeName: %s", m_Context->ReadItemBaseTypeName ? "available" : "NULL");
        ImGui::Text("  ReadItemStackCount: %s", m_Context->ReadItemStackCount ? "available" : "NULL");
        ImGui::Text("  IsOverlayMode: %s",
            (m_Context->IsOverlayMode && m_Context->IsOverlayMode()) ? "YES" : "NO");
        ImGui::Text("  Screen: %dx%d", snap->ScreenWidth, snap->ScreenHeight);
    }

    // ========================================================================
    // Ground Item Prices (via UI tree: GameUi[7][0][0] and [7][0][1])
    // ========================================================================

    void DrawGroundItemPrices(const std::shared_ptr<const PluginGameSnapshot>& snap) {
        uintptr_t gameUiAddr = GetGameUiAddr();
        if (gameUiAddr == 0) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        // GameUi[7][0] — item container base
        const int containerIndices[] = { 7, 0 };
        uintptr_t containerBase = m_Context->ReadUiChildChain(gameUiAddr, containerIndices, 2);
        if (containerBase == 0) return;

        // Both containers: [0]=normal, [1]=hovered
        for (int ci = 0; ci < 2; ci++) {
            uintptr_t containerAddr = m_Context->GetUiChildAt(containerBase, ci);
            if (containerAddr == 0) continue;

            auto children = m_Context->GetUiChildren(containerAddr);
            for (uintptr_t childAddr : children) {
                if (childAddr == 0) continue;
                if (!m_Context->IsUiElementVisible(childAddr)) continue;

                auto elemData = m_Context->ReadUiElement(childAddr);
                if (elemData.ElementType != 0x4084) continue;

                std::string itemName = m_Context->GetUiStringId(childAddr);
                if (itemName.empty()) continue;

                // Parse stack prefix: "2x Divine Orb" → multiplier=2, name="Divine Orb"
                int stackMultiplier = 1;
                std::string lookupName = ParseGroundItemName(itemName, stackMultiplier);
                if (lookupName.empty()) continue;

                PriceLookupResult price = LookupPrice(m_PriceDb, lookupName);
                if (!price.found) continue;

                // Unique category: ground item name must contain the unique item's name
                if (IsUniqueCategory(price.category)) {
                    std::string lowerLookup = ToLower(lookupName);
                    std::string lowerUnique = ToLower(price.itemName);
                    if (lowerLookup.find(lowerUnique) == std::string::npos)
                        continue;
                }

                float displayValue = GetDisplayValue(price, m_DisplayCurrency) * stackMultiplier;
                if (displayValue < 0.001f) continue;

                // Get screen position via full parent-chain computation
                float posX, posY, sizeW, sizeH;
                if (!m_Context->ComputeUiScreenRect(childAddr, &posX, &posY, &sizeW, &sizeH))
                    continue;
                if (sizeW < 1.0f) continue;

                std::string text = FormatPrice(displayValue, m_DisplayCurrency);
                ImVec2 ts = ImGui::CalcTextSize(text.c_str());
                float sw = ts.x * (baseFontSize / ImGui::GetFontSize());
                float sh = ts.y * (baseFontSize / ImGui::GetFontSize());

                // Position based on ground price position setting
                float textX, textY;
                CalcGroundPricePos(posX, posY, sizeW, sizeH, sw, sh, baseFontSize, textX, textY);

                DrawPriceLabelAt(dl, baseFontSize, textX, textY, sw, sh, text, price.chaosValue * stackMultiplier);
            }
        }
    }

    // ========================================================================
    // Inventory Prices (via InventoryGrid, only when panel visible)
    // ========================================================================

    void DrawInventoryPrices(const std::shared_ptr<const PluginGameSnapshot>& snap) {
        if (snap->Inventories.empty()) return;

        const auto& grid = snap->InventoryGrid;
        if (!grid.IsValid || grid.CellSize < 1.0f) return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        for (const auto& inv : snap->Inventories) {
            // Only main inventory (ID 1)
            if (inv.Id != 1) continue;

            for (const auto& item : inv.Items) {
                std::string displayName = GetItemLookupName(item);
                if (displayName.empty()) continue;

                PriceLookupResult price = LookupPrice(m_PriceDb, displayName);
                if (!price.found) continue;

                // Unique category: verify item is actually unique rarity
                if (IsUniqueCategory(price.category) && m_Context->ReadItemRarity && item.Address) {
                    if (m_Context->ReadItemRarity(item.Address) != 3)
                        continue;
                }

                int multiplier = (item.StackCount > 1) ? item.StackCount : 1;
                float displayValue = GetDisplayValue(price, m_DisplayCurrency) * multiplier;
                if (displayValue < 0.001f) continue;

                std::string text = FormatPrice(displayValue, m_DisplayCurrency);

                float cellX = grid.GridScreenX + item.SlotX * grid.CellSize;
                float cellY = grid.GridScreenY + item.SlotY * grid.CellSize;
                float cellW = item.Width * grid.CellSize;
                float cellH = item.Height * grid.CellSize;

                // Auto-adaptive text size for small cells
                float fontSize = ComputeAdaptiveFontSize(baseFontSize, cellW, cellH);

                ImVec2 ts = ImGui::CalcTextSize(text.c_str());
                float sw = ts.x * (fontSize / ImGui::GetFontSize());
                float sh = ts.y * (fontSize / ImGui::GetFontSize());

                float labelX, labelY;
                CalcUiPricePos(cellX, cellY, cellW, cellH, sw, sh, 2.0f, labelX, labelY);

                DrawPriceLabelAt(dl, fontSize, labelX, labelY, sw, sh, text, price.chaosValue * multiplier);
            }
        }
    }

    // ========================================================================
    // Stash Prices (inventory-data-driven, position-based rendering)
    // ========================================================================

    void DrawStashPrices() {
        uintptr_t gameUiAddr = GetGameUiAddr();
        if (gameUiAddr == 0) return;

        // Find stash root by brute-force (cached)
        if (m_StashRootIndex < 0) {
            DiscoverPanelIndex(gameUiAddr, "Stash", 25, 35, m_StashRootIndex);
        }
        if (m_StashRootIndex < 0) return;

        // Check stash root is visible
        uintptr_t stashRoot = m_Context->GetUiChildAt(gameUiAddr, m_StashRootIndex);
        if (stashRoot == 0 || !m_Context->IsUiElementVisible(stashRoot)) return;

        // Request stash inventory scan (IDs start from 133)
        auto now = std::chrono::steady_clock::now();
        if (now - m_LastStashScan > std::chrono::seconds(1)) {
            if (m_Context->RequestInventoryScan)
                m_Context->RequestInventoryScan(-1); // Scan all to include stash tabs
            m_LastStashScan = now;
        }

        // Navigate: root→2→0→0→0→1→1 → tab list
        const int tabListIndices[] = { 2, 0, 0, 0, 1, 1 };
        uintptr_t tabList = m_Context->ReadUiChildChain(stashRoot, tabListIndices, 6);
        if (tabList == 0) return;

        // Find active (visible) tab and its index
        auto tabs = m_Context->GetUiChildren(tabList);
        int activeTabIdx = -1;
        for (int i = 0; i < (int)tabs.size(); i++) {
            if (tabs[i] != 0 && m_Context->IsUiElementVisible(tabs[i])) {
                activeTabIdx = i;
                break;
            }
        }
        if (activeTabIdx < 0) return;

        // Get stash inventory for active tab: inventory IDs start from 133
        int stashInvId = 133 + activeTabIdx;
        auto snap = m_Context->GetSnapshot();
        if (!snap) return;

        // Find the matching stash inventory from snapshot
        const InventoryInfo* stashInv = nullptr;
        for (const auto& inv : snap->Inventories) {
            if (inv.Id == stashInvId) {
                stashInv = &inv;
                break;
            }
        }
        // No inventory data or empty — nothing to render
        if (!stashInv || stashInv->Items.empty()) return;
        if (stashInv->TotalBoxesX <= 0 || stashInv->TotalBoxesY <= 0) return;

        // Get the item container UI for screen position
        uintptr_t activeTabAddr = tabs[activeTabIdx];
        const int itemContainerIndices[] = { 0, 0 };
        uintptr_t itemContainer = m_Context->ReadUiChildChain(activeTabAddr, itemContainerIndices, 2);
        if (itemContainer == 0) return;

        float containerX, containerY, containerW, containerH;
        if (!m_Context->ComputeUiScreenRect(itemContainer, &containerX, &containerY, &containerW, &containerH))
            return;
        if (containerW < 1.0f || containerH < 1.0f) return;

        // Compute cell size from container and grid dimensions
        float cellW = containerW / static_cast<float>(stashInv->TotalBoxesX);
        float cellH = containerH / static_cast<float>(stashInv->TotalBoxesY);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float baseFontSize = ImGui::GetFontSize() * m_TextScale;

        // Iterate inventory items — only draw prices for items with a match in DB
        for (const auto& item : stashInv->Items) {
            std::string dn = GetItemLookupName(item);
            if (dn.empty()) continue;

            PriceLookupResult price = LookupPrice(m_PriceDb, dn);
            if (!price.found) continue;

            // Unique category: verify item is actually unique rarity
            if (IsUniqueCategory(price.category) && m_Context->ReadItemRarity && item.Address) {
                if (m_Context->ReadItemRarity(item.Address) != 3)
                    continue;
            }

            int multiplier = (item.StackCount > 1) ? item.StackCount : 1;
            float displayValue = GetDisplayValue(price, m_DisplayCurrency) * multiplier;
            if (displayValue < 0.001f) continue;

            std::string text = FormatPrice(displayValue, m_DisplayCurrency);

            // Compute item screen position from grid
            float posX = containerX + item.SlotX * cellW;
            float posY = containerY + item.SlotY * cellH;
            float itemW = item.Width * cellW;
            float itemH = item.Height * cellH;

            // Auto-adaptive text size for small cells
            float fontSize = ComputeAdaptiveFontSize(baseFontSize, itemW, itemH);

            ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            float sw = ts.x * (fontSize / ImGui::GetFontSize());
            float sh = ts.y * (fontSize / ImGui::GetFontSize());

            float labelX, labelY;
            CalcUiPricePos(posX, posY, itemW, itemH, sw, sh, 2.0f, labelX, labelY);

            DrawPriceLabelAt(dl, fontSize, labelX, labelY, sw, sh, text, price.chaosValue * multiplier);
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

    // Compute adaptive font size for small item cells
    float ComputeAdaptiveFontSize(float baseFontSize, float cellW, float cellH) {
        float minDim = (cellW < cellH) ? cellW : cellH;
        // Text should be at most 35% of the smaller dimension
        float maxTextH = minDim * 0.35f;
        if (baseFontSize > maxTextH && maxTextH > 6.0f) {
            return maxTextH;
        }
        return baseFontSize;
    }

    // Calculate price label position for UI items (inventory/stash)
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

    // Calculate price label position for ground items (relative to item label)
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
    // League Discovery (cached, fetched once on first access)
    // ========================================================================

    const std::vector<std::string>& GetLeagues() {
        if (!m_LeaguesFetched) {
            m_LeaguesFetched = true;
            m_Leagues = NinjaApi::FetchLeagues();
            if (m_Leagues.empty()) {
                // Fallback to hardcoded list
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
        // Match game window or overlay window
        return (wcsstr(title, L"Path of Exile") != nullptr ||
                wcsstr(title, L"POEFixer") != nullptr);
    }

    // ========================================================================
    // UI Tree Helpers (via SDK v5)
    // ========================================================================

    uintptr_t GetGameUiAddr() {
        if (!m_Context->GetGameUiRootAddress) return 0;
        return m_Context->GetGameUiRootAddress();
    }

    // Discover panel index by brute-force scanning GameUi children
    // Checks child[N]→child[1]→StringId == target
    void DiscoverPanelIndex(uintptr_t gameUiAddr, const std::string& targetId,
        int minIdx, int maxIdx, int& outIndex)
    {
        for (int idx = minIdx; idx <= maxIdx; idx++) {
            uintptr_t rootAddr = m_Context->GetUiChildAt(gameUiAddr, idx);
            if (rootAddr == 0) continue;
            uintptr_t headerAddr = m_Context->GetUiChildAt(rootAddr, 1);
            if (headerAddr == 0) continue;

            std::string stringId = m_Context->GetUiStringId(headerAddr);
            if (stringId == targetId) {
                outIndex = idx;
                return;
            }
        }
    }

    // Check if inventory panel is visible (brute-force find "Inventory")
    bool IsInventoryPanelVisible() {
        uintptr_t gameUiAddr = GetGameUiAddr();
        if (gameUiAddr == 0) return false;

        if (m_InvRootIndex < 0) {
            DiscoverPanelIndex(gameUiAddr, "Inventory", 25, 35, m_InvRootIndex);
        }
        if (m_InvRootIndex < 0) return false;

        uintptr_t invRoot = m_Context->GetUiChildAt(gameUiAddr, m_InvRootIndex);
        return invRoot != 0 && m_Context->IsUiElementVisible(invRoot);
    }

    // Parse "2x Divine Orb" → multiplier=2, return "Divine Orb"
    // Parse "Divine Orb" → multiplier=1, return "Divine Orb"
    static std::string ParseGroundItemName(const std::string& raw, int& outMultiplier) {
        outMultiplier = 1;
        if (raw.size() < 3) return raw;

        // Check for pattern: digit(s) + 'x' + ' ' at the start
        size_t i = 0;
        while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') i++;
        if (i > 0 && i < raw.size() && raw[i] == 'x' && i + 1 < raw.size() && raw[i + 1] == ' ') {
            outMultiplier = std::atoi(raw.c_str());
            if (outMultiplier < 1) outMultiplier = 1;
            return raw.substr(i + 2); // Skip "Nx "
        }
        return raw;
    }

    std::string GetItemBaseTypeName(const RadarEntity& entity) {
        // Check cache first
        auto it = m_NameCache.find(entity.Id);
        if (it != m_NameCache.end()) return it->second;

        // Read base type name (e.g., "Chaos Orb") via Base component
        std::string name;
        if (m_Context->ReadItemBaseTypeName) {
            name = m_Context->ReadItemBaseTypeName(entity.Address);
        }

        // Cache result (even if empty, to avoid repeated reads)
        m_NameCache[entity.Id] = name;
        return name;
    }

    // Returns the best name for price lookup: UniqueName for uniques, BaseTypeName for others.
    // For inventory items (have UniqueName/BaseTypeName fields).
    std::string GetItemLookupName(const PluginSDK::InventoryItemInfo& item) {
        // Prefer UniqueName for unique items (e.g., "Headhunter")
        if (!item.UniqueName.empty())
            return item.UniqueName;
        // Fallback: read UniqueName via function if field is empty but address available
        if (m_Context->ReadItemUniqueName && item.Address) {
            std::string un = m_Context->ReadItemUniqueName(item.Address);
            if (!un.empty()) return un;
        }
        // Non-unique: use BaseTypeName (e.g., "Divine Orb")
        if (!item.BaseTypeName.empty())
            return item.BaseTypeName;
        if (m_Context->ReadItemBaseTypeName && item.Address)
            return m_Context->ReadItemBaseTypeName(item.Address);
        return "";
    }

    // Returns the best name for price lookup for world/radar entities.
    std::string GetEntityLookupName(const RadarEntity& entity) {
        // Try unique name first
        if (m_Context->ReadItemUniqueName) {
            std::string un = m_Context->ReadItemUniqueName(entity.Address);
            if (!un.empty()) return un;
        }
        // Fallback to base type name
        return GetItemBaseTypeName(entity);
    }

    // ========================================================================
    // Fetch Thread
    // ========================================================================

    void StartFetchThread() {
        if (m_FetchThread.joinable()) return;
        m_Running.store(true);

        m_FetchThread = std::thread([this]() {
            while (m_Running.load()) {
                {
                    PriceDatabase tempDb;
                    IPriceSource& source = (m_DataSource == 0)
                        ? static_cast<IPriceSource&>(m_NinjaSource)
                        : static_cast<IPriceSource&>(m_ScoutSource);
                    source.FetchCategories(
                        m_League, m_Directory, m_CurrencyEnabled, m_UniqueEnabled,
                        tempDb, m_IsLoading, m_Running,
                        m_Context ? m_Context->Log : nullptr);

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
        fs::path settingsPath = fs::path(m_Directory) / "config" / "settings.json";
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
                m_DisplayCurrency = static_cast<DisplayCurrency>(j["displayCurrency"].get<int>());
            if (j.contains("textScale") && j["textScale"].is_number())
                m_TextScale = j["textScale"].get<float>();
            if (j.contains("showGroundPrices") && j["showGroundPrices"].is_boolean())
                m_ShowGroundPrices = j["showGroundPrices"].get<bool>();
            if (j.contains("showInventoryPrices") && j["showInventoryPrices"].is_boolean())
                m_ShowInventoryPrices = j["showInventoryPrices"].get<bool>();
            if (j.contains("showOtherInventoryPrices") && j["showOtherInventoryPrices"].is_boolean())
                m_ShowOtherInventoryPrices = j["showOtherInventoryPrices"].get<bool>();
            if (j.contains("refreshIntervalMin") && j["refreshIntervalMin"].is_number_integer())
                m_RefreshIntervalMin = std::clamp(j["refreshIntervalMin"].get<int>(), 15, 180);
            if (j.contains("hideWhenUnfocused") && j["hideWhenUnfocused"].is_boolean())
                m_HideWhenUnfocused = j["hideWhenUnfocused"].get<bool>();
            if (j.contains("hideHotkey") && j["hideHotkey"].is_number_integer())
                m_HideHotkey = j["hideHotkey"].get<int>();
            if (j.contains("uiPricePosition") && j["uiPricePosition"].is_number_integer())
                m_UiPricePosition = static_cast<UiPricePosition>(j["uiPricePosition"].get<int>());
            if (j.contains("groundPricePosition") && j["groundPricePosition"].is_number_integer())
                m_GroundPricePosition = static_cast<GroundPricePosition>(j["groundPricePosition"].get<int>());

            // New format: currencyEnabled + uniqueEnabled
            if (j.contains("currencyEnabled") && j["currencyEnabled"].is_array()) {
                auto& arr = j["currencyEnabled"];
                for (int i = 0; i < kMaxCurrencyCategories && i < (int)arr.size(); i++) {
                    if (arr[i].is_boolean())
                        m_CurrencyEnabled[i] = arr[i].get<bool>();
                }
            }
            // Backward compat: old format had "categoryEnabled" (13 bools)
            else if (j.contains("categoryEnabled") && j["categoryEnabled"].is_array()) {
                auto& cats = j["categoryEnabled"];
                for (int i = 0; i < 13 && i < (int)cats.size(); i++) {
                    if (cats[i].is_boolean())
                        m_CurrencyEnabled[i] = cats[i].get<bool>();
                }
                // Indices 13-15 default to true (already initialized)
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
            if (m_Context)
                m_Context->Log("Warning", "[NinjaPricer] Failed to load settings, using defaults");
        }
    }

    // ========================================================================
    // Members
    // ========================================================================

    PluginContext* m_Context = nullptr;
    std::string m_Directory;

    // Settings
    std::string m_League = "Fate of the Vaal";
    DisplayCurrency m_DisplayCurrency = DisplayCurrency::Divine;
    float m_TextScale = 1.0f;
    bool m_ShowGroundPrices = true;
    bool m_ShowInventoryPrices = true;
    bool m_ShowOtherInventoryPrices = false;
    int m_RefreshIntervalMin = 15;
    bool m_HideWhenUnfocused = true;
    int m_HideHotkey = 0;              // VK code, 0 = disabled
    UiPricePosition m_UiPricePosition = UiPricePosition::BottomRight;
    GroundPricePosition m_GroundPricePosition = GroundPricePosition::Top;
    int m_DataSource = 1;  // 0 = poe.ninja, 1 = poe2scout
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
    float m_CachedDivineInChaos = 1.0f; // Thread-local copy for rendering

    // Price source instances
    NinjaSource m_NinjaSource;
    ScoutSource m_ScoutSource;

    // Fetch thread
    std::thread m_FetchThread;
    std::atomic<bool> m_Running{ false };
    std::atomic<bool> m_ForceRefresh{ false };

    // Runtime caches
    std::unordered_map<uint32_t, std::string> m_NameCache; // entity ID -> item name
    uint64_t m_LastAreaChange = 0;
    std::chrono::steady_clock::time_point m_LastInventoryScan;

    // Cached UI panel indices (brute-force discovered, reset on area change)
    int m_InvRootIndex = -1;
    int m_StashRootIndex = -1;
    std::chrono::steady_clock::time_point m_LastStashScan;

    // League cache
    std::vector<std::string> m_Leagues;
    bool m_LeaguesFetched = false;

    // UI state
    bool m_CapturingHotkey = false;
};

// ============================================================================
// Factory exports
// ============================================================================

extern "C" PLUGIN_API IPlugin* CreatePlugin() {
    return new NinjaPricerPlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(IPlugin* plugin) {
    delete plugin;
}
