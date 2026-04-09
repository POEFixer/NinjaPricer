#pragma once

#include "PluginGameData.h"
#include <memory>

// ============================================================================
// PluginContext — Bridge between host and plugin
// ============================================================================
// The host populates this struct with function pointers that plugins call
// to access game data. All complex types are in PluginSDK namespace.
// ============================================================================

struct PluginContext {
    // --- Game Data Access ---

    // Get a snapshot of all game data (thread-safe, immutable).
    // The returned snapshot is valid until next frame.
    std::shared_ptr<const PluginSDK::PluginGameSnapshot> (*GetSnapshot)() = nullptr;

    // Get player vitals (HP/ES/MP + buffs). Convenience shortcut.
    PluginSDK::PlayerVitals (*GetPlayerVitals)() = nullptr;

    // Get current game state (InGameState, AreaLoadingState, etc.)
    PluginSDK::GameStateTypes (*GetCurrentState)() = nullptr;

    // Check if game process is attached
    bool (*IsAttached)() = nullptr;

    // Check if currently in game (not loading, not login screen)
    bool (*IsInGame)() = nullptr;

    // Check if game window is in foreground
    bool (*IsGameForeground)() = nullptr;

    // Get game process ID
    DWORD (*GetProcessId)() = nullptr;

    // --- Item Data Access ---

    // Read extended item mods for an entity by its address.
    PluginSDK::ExtendedItemModInfo (*ReadExtendedItemMods)(uintptr_t entityAddress) = nullptr;

    // Read item rarity: 0=Normal, 1=Magic, 2=Rare, 3=Unique
    int (*ReadItemRarity)(uintptr_t entityAddress) = nullptr;

    // Read item stack count (for currency/stackable items)
    int (*ReadItemStackCount)(uintptr_t entityAddress) = nullptr;

    // Read item base type name
    std::string (*ReadItemName)(uintptr_t entityAddress) = nullptr;

    // Read item metadata path
    std::string (*ReadItemPath)(uintptr_t entityAddress) = nullptr;

    // Read item base type name (e.g., "Chaos Orb") via Base component.
    // Unlike ReadItemName which returns the metadata path, this reads the actual
    // base type name from BaseItemTypeData.BaseTypeName (std::wstring at +0x30).
    std::string (*ReadItemBaseTypeName)(uintptr_t entityAddress) = nullptr;

    // Read unique item name (e.g., "Headhunter", "Brimstone Call") from Words.dat.
    // Returns empty string for non-unique items.
    std::string (*ReadItemUniqueName)(uintptr_t entityAddress) = nullptr;

    // --- Host Services ---

    // Log a message to the host's logger.
    // level: "Debug", "Info", "Warning", "Error"
    void (*Log)(const char* level, const char* message) = nullptr;

    // ImGui context pointer — call ImGui::SetCurrentContext() with this.
    void* ImGuiContext = nullptr;

    // D3D11 device pointer (ID3D11Device*) — for loading textures.
    void* D3DDevice = nullptr;

    // SDK version.
    int SDKVersion = 0;

    // --- Overlay Mode (SDK v2) ---

    // Returns true if the host is currently in overlay mode (transparent overlay on game).
    bool (*IsOverlayMode)() = nullptr;

    // --- Memory Reading (SDK v2) ---
    // Direct access to game process memory. All reads are safe (return 0/empty on failure).

    // Get the game module base address. Returns 0 if not attached.
    uintptr_t (*GetBaseAddress)() = nullptr;

    // Get the game module size. Returns 0 if not attached.
    uintptr_t (*GetModuleSize)() = nullptr;

    // Read a block of memory from the game process.
    // Returns true if the read succeeded. buffer must have at least 'size' bytes allocated.
    bool (*ReadProcessMemory)(uintptr_t address, void* buffer, size_t size) = nullptr;

    // Read a null-terminated ASCII string from game memory (max 128 chars).
    std::string (*ReadString)(uintptr_t address) = nullptr;

    // Read a null-terminated Unicode (wide) string from game memory (max 128 wchars).
    std::wstring (*ReadUnicodeString)(uintptr_t address) = nullptr;

    // Get a resolved pattern scan address by name.
    // Standard names: "Game States", "File Root", "AreaChangeCounter",
    //                 "Terrain Rotator Helper", "Terrain Rotation Selector", "GameCullSize"
    // Returns 0 if pattern not found.
    uintptr_t (*GetPatternAddress)(const char* patternName) = nullptr;

    // --- World-to-Screen Projection (SDK v2) ---

    // Convert a world-space position (x,y,z) to screen coordinates (outX, outY).
    // Returns true if the position is in front of the camera (visible).
    bool (*WorldToScreen)(float worldX, float worldY, float worldZ, float* outX, float* outY) = nullptr;

    // --- Inventory (SDK v2) ---

    // Request the host to scan inventories. Inventory data in the snapshot
    // is only populated after this call completes (next frame).
    // Pass -1 to scan all inventories, or a specific inventory ID.
    void (*RequestInventoryScan)(int inventoryId) = nullptr;

    // --- Terrain Data (SDK v2) ---

    // Get walkable grid data. Returns pointer to grid bytes and sets outWidth/outHeight.
    // The grid is a 2D array where 0 = not walkable, non-zero = walkable.
    // Returns nullptr if data not available. Data valid until next GetSnapshot() call.
    const uint8_t* (*GetWalkableGrid)(int* outWidth, int* outHeight) = nullptr;

    // Get terrain height at a grid position. Returns 0 if out of bounds.
    float (*GetTerrainHeight)(int gridX, int gridY) = nullptr;

    // --- Advanced Memory Reading (SDK v3) ---
    // Read native C++ containers from game memory using the same logic as the host.
    // These mirror GameLibrary's Core::Process methods.

    // Read a StdVector<T> from game memory. The 'containerAddress' points to a 24-byte
    // StdVector struct {First, Last, End}. 'elementSize' is sizeof(T).
    // Returns a copy of the data. outCount receives the number of elements.
    // Returns nullptr on failure; caller must free() the returned buffer.
    void* (*ReadStdVector)(uintptr_t containerAddress, int elementSize, int* outCount) = nullptr;

    // Read a StdList<T> from game memory. 'containerAddress' points to a 16-byte
    // StdList struct {Head, Size}. 'nodeDataOffset' is the offset of Data in StdListNodeGeneric<T> (usually 16).
    // 'elementSize' is sizeof(T). Returns malloc'd buffer; caller must free().
    void* (*ReadStdList)(uintptr_t containerAddress, int elementSize, int* outCount) = nullptr;

    // Read a StdBucket<T> from game memory. 'containerAddress' points to a StdBucket struct.
    // Internally reads the embedded StdVector. Returns malloc'd buffer; caller must free().
    void* (*ReadStdBucket)(uintptr_t containerAddress, int elementSize, int* outCount) = nullptr;

    // Traverse a StdMap and call 'callback' for each key-value pair.
    // 'containerAddress' points to a 16-byte StdMap struct {Head, Size}.
    // 'keySize' and 'valueSize' are sizeof(TKey) and sizeof(TValue).
    // 'callback' receives pointers to key and value data for each node.
    // Returns the number of nodes visited.
    int (*ReadStdMap)(uintptr_t containerAddress, int keySize, int valueSize,
        void (*callback)(const void* key, const void* value, void* userData), void* userData) = nullptr;

    // Read a StdWString from game memory. 'containerAddress' points to a 32-byte StdWString struct.
    // Returns the string content. Empty string on failure.
    std::wstring (*ReadStdWString)(uintptr_t containerAddress) = nullptr;

    // Get inventory name by ID (e.g., 1 -> "MainInventory1", 3 -> "Weapon1").
    const char* (*GetInventoryName)(int inventoryId) = nullptr;

    // --- Debug Data Access (SDK v4) ---
    // Entity debug list, watch mechanism, inventory watch, and UI root addresses.

    // Get the full entity debug list (Id, Address, Path, Type, Components).
    // Returns a vector of DebugEntityInfo structs. Thread-safe copy from worker.
    std::vector<PluginSDK::DebugEntityInfo> (*GetEntityDebugList)() = nullptr;

    // Watch/unwatch an entity for detailed component data.
    // When watched, the worker thread reads full component data each frame.
    void (*WatchEntity)(uint32_t entityId) = nullptr;
    void (*UnwatchEntity)(uint32_t entityId) = nullptr;

    // Get watched entity component data. Returns empty if not watched or not yet loaded.
    PluginSDK::DebugEntityComponents (*GetWatchedEntityData)(uint32_t entityId) = nullptr;

    // Watch an inventory by ID for detailed debug data (slot grid, items, mods).
    void (*WatchInventory)(int inventoryId) = nullptr;

    // Get watched inventory debug data. Returns empty if not watched yet.
    PluginSDK::DebugInventoryData (*GetWatchedInventoryData)() = nullptr;

    // Get ServerData address from debug data.
    uintptr_t (*GetServerDataAddress)() = nullptr;

    // Get player inventory list: vector of (inventoryId, address) pairs.
    std::vector<std::pair<int, uintptr_t>> (*GetPlayerInventoryList)() = nullptr;

    // Get the Game UI root element address (InGameState UI root).
    uintptr_t (*GetGameUiRootAddress)() = nullptr;

    // Get the top-level UI root address.
    uintptr_t (*GetUiRootAddress)() = nullptr;

    // Get the game cull value (used for UI scale calculation).
    int (*GetGameCullValue)() = nullptr;

    // --- UI State (SDK v4 addition) ---

    // Returns true when the host's settings menu is visible (overlay is interactive).
    // Plugins can use this to show/hide draggable title bars in overlay mode.
    bool (*IsMenuVisible)() = nullptr;

    // --- UI Element API (SDK v5) ---

    PluginSDK::UiElementData (*ReadUiElement)(uintptr_t addr) = nullptr;
    std::vector<uintptr_t> (*GetUiChildren)(uintptr_t addr) = nullptr;
    uintptr_t (*GetUiChildAt)(uintptr_t addr, int index) = nullptr;
    uintptr_t (*ReadUiChildChain)(uintptr_t root, const int* indices, int count) = nullptr;
    bool (*IsUiElementVisible)(uintptr_t addr) = nullptr;
    std::string (*GetUiStringId)(uintptr_t addr) = nullptr;
    bool (*ComputeUiScreenRect)(uintptr_t addr, float* outX, float* outY,
                                float* outW, float* outH) = nullptr;
    std::string (*GetUiText)(uintptr_t addr) = nullptr;

    // --- Component Reader API (SDK v5) ---

    PluginSDK::PluginLifeData (*ReadLifeComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginRenderData (*ReadRenderComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginPositionedData (*ReadPositionedComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginTargetableData (*ReadTargetableComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginChestData (*ReadChestComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginShrineData (*ReadShrineComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginStackData (*ReadStackComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginChargesData (*ReadChargesComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginPlayerData (*ReadPlayerComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginAnimatedData (*ReadAnimatedComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginTransitionableData (*ReadTransitionableComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginTriggerableBlockageData (*ReadTriggerableBlockageComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginMinimapIconData (*ReadMinimapIconComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginStateMachineData (*ReadStateMachineComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginBaseData (*ReadBaseComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginModsData (*ReadModsComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginStatsData (*ReadStatsComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginBuffsData (*ReadBuffsComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginActorData (*ReadActorComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginNpcData (*ReadNpcComponent)(uintptr_t componentAddr) = nullptr;
    PluginSDK::PluginDiesAfterTimeData (*ReadDiesAfterTimeComponent)(uintptr_t componentAddr) = nullptr;

    // --- Inline Convenience Helpers (SDK v5) ---

    float GetHealthPercent(uintptr_t lifeAddr) const {
        if (!ReadLifeComponent) return 0;
        auto life = ReadLifeComponent(lifeAddr);
        if (!life.Valid || life.Health.Total <= 0) return 0;
        return static_cast<float>(life.Health.Current) * 100.f / static_cast<float>(life.Health.Total);
    }

    float GetEsPercent(uintptr_t lifeAddr) const {
        if (!ReadLifeComponent) return 0;
        auto life = ReadLifeComponent(lifeAddr);
        if (!life.Valid || life.EnergyShield.Total <= 0) return 0;
        return static_cast<float>(life.EnergyShield.Current) * 100.f / static_cast<float>(life.EnergyShield.Total);
    }

    float GetManaPercent(uintptr_t lifeAddr) const {
        if (!ReadLifeComponent) return 0;
        auto life = ReadLifeComponent(lifeAddr);
        if (!life.Valid || life.Mana.Total <= 0) return 0;
        return static_cast<float>(life.Mana.Current) * 100.f / static_cast<float>(life.Mana.Total);
    }

    bool IsAlive(uintptr_t lifeAddr) const {
        if (!ReadLifeComponent) return false;
        auto life = ReadLifeComponent(lifeAddr);
        return life.Valid && life.Health.Current > 0;
    }

    bool IsChestOpenedHelper(uintptr_t chestAddr) const {
        if (!ReadChestComponent) return false;
        auto chest = ReadChestComponent(chestAddr);
        return chest.Valid && chest.IsOpened;
    }

    bool GetWorldPosition(uintptr_t renderAddr, float* outX, float* outY, float* outZ) const {
        if (!ReadRenderComponent || !outX || !outY || !outZ) return false;
        auto render = ReadRenderComponent(renderAddr);
        if (!render.Valid) return false;
        *outX = render.WorldX; *outY = render.WorldY; *outZ = render.WorldZ;
        return true;
    }

    int GetItemRarityFromMods(uintptr_t modsAddr) const {
        if (!ReadModsComponent) return 0;
        auto mods = ReadModsComponent(modsAddr);
        return mods.Valid ? mods.Rarity : 0;
    }

    bool IsItemIdentifiedHelper(uintptr_t modsAddr) const {
        if (!ReadModsComponent) return false;
        auto mods = ReadModsComponent(modsAddr);
        return mods.Valid && mods.IsIdentified;
    }

    int GetStackCountHelper(uintptr_t stackAddr) const {
        if (!ReadStackComponent) return 0;
        auto stack = ReadStackComponent(stackAddr);
        return stack.Valid ? stack.CurrentSize : 0;
    }

    std::string GetPlayerNameHelper(uintptr_t playerAddr) const {
        if (!ReadPlayerComponent) return "";
        auto player = ReadPlayerComponent(playerAddr);
        return player.Valid ? player.Name : "";
    }
};
