#pragma once

// ============================================================================
// Plugin Game Data Structures (namespace PluginSDK)
// ============================================================================
// Self-contained header for plugin DLLs. All types are in the PluginSDK
// namespace to avoid conflicts with host-internal types.
//
// Enum values match the host's GameLibrary definitions exactly.
// Struct layouts match the host's GameClient.h definitions.
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <Windows.h>
#include <DirectXMath.h>

namespace PluginSDK {

// --- Enums (values match GameLibrary originals) -----------------------------

// Matches GameOffsets::Entity::EntityTypes (unscoped in host, sequential from 0)
enum class EntityTypes : int {
    Unidentified = 0,
    Chest = 1,
    NPC = 2,
    Player = 3,
    Shrine = 4,
    Monster = 5,
    DeliriumBomb = 6,
    DeliriumSpawner = 7,
    OtherImportantObjects = 8,
    Item = 9,
    Renderable = 10,
    AreaTransition = 11,
    ExpeditionMarker = 12,
    ExpeditionRemnant = 13,
};

// Matches GameOffsets::Entity::EntitySubtypes (unscoped in host)
enum class EntitySubtypes : int {
    _Unidentified = 0,
    _None = 1,
    PlayerSelf = 2,
    PlayerOther = 3,
    ChestWithMagicRarity = 4,
    ChestWithRareRarity = 5,
    ExpeditionChest = 6,
    BreachChest = 7,
    Strongbox = 8,
    SpecialNPC = 9,
    POIMonster = 10,
    PinnacleBoss = 11,
    WorldItem = 12,
    InventoryItem = 13,
};

// Matches GameOffsets::Entity::EntityStates (unscoped in host)
enum class EntityStates : int {
    None = 0,
    Useless = 1,
    PlayerLeader = 2,
    MonsterFriendly = 3,
    PinnacleBossHidden = 4,
};

// Matches GameClient.h NearbyZone
enum class NearbyZone : uint8_t {
    None = 0,
    InnerCircle = 1,  // within ~60 grid units from player
    OuterCircle = 2,  // within ~120 grid units from player
    Far = 3           // beyond outer radius
};

// Matches GameOffsets::Objects::States::GameStateTypes (scoped enum class)
enum class GameStateTypes : int {
    AreaLoadingState = 0,
    ChangePasswordState = 1,
    CreditsState = 2,
    EscapeState = 3,
    InGameState = 4,
    PreGameState = 5,
    LoginState = 6,
    WaitingState = 7,
    CreateCharacterState = 8,
    SelectCharacterState = 9,
    DeleteCharacterState = 10,
    LoadingState = 11,
    GameNotLoaded = 12,
};

// --- Structs ----------------------------------------------------------------

struct EntityComponentCache {
    uintptr_t RenderAddr = 0;
    uintptr_t PositionedAddr = 0;
    uintptr_t ChestAddr = 0;
    uintptr_t PlayerAddr = 0;
    uintptr_t ShrineAddr = 0;
    uintptr_t LifeAddr = 0;
    uintptr_t TargetableAddr = 0;
    uintptr_t OMPAddr = 0;
    uintptr_t NPCAddr = 0;
    uintptr_t TriggerableBlockageAddr = 0;
    uintptr_t DiesAfterTimeAddr = 0;
    uintptr_t BuffsAddr = 0;
    uintptr_t WorldItemAddr = 0;
    uintptr_t AreaTransitionAddr = 0;
    uintptr_t MinimapIconAddr = 0;
    uintptr_t StatsAddr = 0;
    uintptr_t ActorAddr = 0;
    uintptr_t AnimatedAddr = 0;
    uintptr_t BaseAddr = 0;
    uintptr_t ChargesAddr = 0;
    uintptr_t ModsAddr = 0;
    uintptr_t StackAddr = 0;
    uintptr_t TransitionableAddr = 0;
    uintptr_t StateMachineAddr = 0;

    bool HasRender() const { return RenderAddr != 0; }
    bool HasPositioned() const { return PositionedAddr != 0; }
    bool HasChest() const { return ChestAddr != 0; }
    bool HasPlayer() const { return PlayerAddr != 0; }
    bool HasShrine() const { return ShrineAddr != 0; }
    bool HasLife() const { return LifeAddr != 0; }
    bool HasTargetable() const { return TargetableAddr != 0; }
    bool HasOMP() const { return OMPAddr != 0; }
    bool HasNPC() const { return NPCAddr != 0; }
    bool HasTriggerableBlockage() const { return TriggerableBlockageAddr != 0; }
    bool HasDiesAfterTime() const { return DiesAfterTimeAddr != 0; }
    bool HasBuffs() const { return BuffsAddr != 0; }
    bool HasWorldItem() const { return WorldItemAddr != 0; }
    bool HasAreaTransition() const { return AreaTransitionAddr != 0; }
    bool HasMinimapIcon() const { return MinimapIconAddr != 0; }
    bool HasStats() const { return StatsAddr != 0; }
    bool HasActor() const { return ActorAddr != 0; }
    bool HasAnimated() const { return AnimatedAddr != 0; }
    bool HasBase() const { return BaseAddr != 0; }
    bool HasCharges() const { return ChargesAddr != 0; }
    bool HasMods() const { return ModsAddr != 0; }
    bool HasStack() const { return StackAddr != 0; }
    bool HasTransitionable() const { return TransitionableAddr != 0; }
    bool HasStateMachine() const { return StateMachineAddr != 0; }
};

struct RadarEntity {
    uint32_t Id = 0;
    uintptr_t Address = 0;
    uintptr_t EntityDetailsAddress = 0;
    uintptr_t RenderComponentAddress = 0;
    bool IsValid = false;

    EntityTypes entityType = EntityTypes::Unidentified;
    EntitySubtypes entitySubtype = EntitySubtypes::_Unidentified;
    EntityStates entityState = EntityStates::None;

    int Rarity = 0;
    uint8_t Reaction = 0;

    float GridPositionX = 0;
    float GridPositionY = 0;
    float TerrainHeight = 0;
    float WorldX = 0;
    float WorldY = 0;
    float WorldZ = 0;

    float ModelBoundsZ = 0;

    std::wstring Path;
    std::wstring PlayerName;
    std::string TgtPath;

    int CurrentHP = 0;
    int MaxHP = 0;
    int CurrentES = 0;
    int MaxES = 0;

    bool IsSleeping = false;
    bool IsChestOpened = false;

    NearbyZone Zone = NearbyZone::None;
    EntityComponentCache ComponentCache;
};

struct MapData {
    float CenterX = 0;
    float CenterY = 0;
    float SizeX = 0;
    float SizeY = 0;
    float ShiftX = 0;
    float ShiftY = 0;
    float DefaultShiftX = 0;
    float DefaultShiftY = 0;
    float Zoom = 0;
    float Scale = 0;
    bool IsVisible = false;
};

struct Buff {
    std::string Name;
    float TimeLeft = 0.0f;
    short Charges = 0;
    float TotalTime = 0.0f;
};

struct PlayerVitals {
    int CurrentHP = 0;
    int MaxHP = 0;
    int HPPercent = 0;

    int CurrentES = 0;
    int MaxES = 0;
    int ESPercent = 0;

    int CurrentMP = 0;
    int MaxMP = 0;
    int MPPercent = 0;

    bool IsTownOrHideout = false;
    bool IsPaused = false;
    bool IsValid = false;

    std::vector<Buff> Buffs;
};

struct InventoryItemInfo {
    uintptr_t Address = 0;
    std::string Name;         // Metadata path (legacy)
    std::string Path;         // Same as Name
    std::string BaseTypeName;  // Base type name (e.g., "Divine Orb")
    std::string UniqueName;   // Unique item name from Words.dat (e.g., "Headhunter"), empty for non-uniques
    int SlotX = 0;
    int SlotY = 0;
    int Width = 1;
    int Height = 1;
    int StackCount = 1;
    bool IsCurrency = false;
};

struct InventoryInfo {
    int Id = 0;
    int TotalBoxesX = 0;
    int TotalBoxesY = 0;
    uintptr_t Ptr = 0;
    std::vector<InventoryItemInfo> Items;
};

struct ItemModData {
    std::string Key;
    std::vector<float> Values;
};

struct ExtendedItemModInfo {
    std::vector<ItemModData> ImplicitMods;
    std::vector<ItemModData> ExplicitMods;
    std::vector<ItemModData> EnchantMods;
    std::vector<ItemModData> HellscapeMods;
    std::vector<ItemModData> CrucibleMods;
    int Rarity = 0;
};

struct InventoryGridInfo {
    uintptr_t UiAddress = 0;
    float GridScreenX = 0;
    float GridScreenY = 0;
    float GridWidth = 0;
    float GridHeight = 0;
    float CellSize = 0;
    bool IsValid = false;
};

// --- Debug Data Structs (SDK v4) -------------------------------------------
// Mirror GameClient.h debug structs for entity/inventory inspection in plugins.

struct DebugVital {
    float Regeneration = 0;
    int Total = 0;
    int ReservedFlat = 0;
    int Current = 0;
    int ReservedPercent = 0;
};

struct DebugLifeComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    DebugVital Health, EnergyShield, Mana;
};

struct DebugRenderComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    float WorldX = 0, WorldY = 0, WorldZ = 0;
    float GridX = 0, GridY = 0;
    float TerrainHeight = 0;
    float ModelBoundsX = 0, ModelBoundsY = 0, ModelBoundsZ = 0;
};

struct DebugPositionedComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    uint8_t Reaction = 0;
    bool IsFriendly = false;
};

struct DebugTargetableComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    bool IsTargetable = false;
    bool IsHighlightable = false;
    bool IsTargettedByPlayer = false;
    bool HiddenFromPlayer = false;
    bool MeetsQuestState = false;
    bool MeetsItemRequirements = false;
};

struct DebugAnimatedComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    std::string Path;
    uint32_t Id = 0;
};

struct DebugActiveSkill {
    std::string Name;
    int UseStage = 0;
    int CastType = 0;
    int TotalUses = 0;
    int TotalCooldownTimeInMs = 0;
    bool CanBeUsed = true;
};

struct DebugStatsComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    int CurrentWeaponIndex = 0;
    bool IsShapeshifted = false;
    std::vector<std::pair<int, int>> StatsItems;
    std::vector<std::pair<int, int>> StatsBuff;
};

struct DebugActorComp {
    uintptr_t Address = 0;
    uintptr_t OwnerAddress = 0;
    int AnimationId = 0;
    std::string AnimationName;
    std::vector<DebugActiveSkill> ActiveSkills;
    int DeployedCounts[256] = {};
};

struct DebugBuff {
    std::string Name;
    float TotalTime = 0;
    float TimeLeft = 0;
    short Charges = 0;
    short FlaskSlot = 0;
    short Effectiveness = 0;
    uint32_t SourceEntityId = 0;
};

struct DebugEntityComponents {
    uint32_t EntityId = 0;
    bool Valid = false;
    DebugLifeComp Life;
    DebugRenderComp Render;
    DebugPositionedComp Positioned;
    DebugTargetableComp Targetable;
    DebugAnimatedComp Animated;
    DebugStatsComp Stats;
    DebugActorComp Actor;
    std::vector<DebugBuff> Buffs;
    bool HasLife = false;
    bool HasRender = false;
    bool HasPositioned = false;
    bool HasTargetable = false;
    bool HasAnimated = false;
    bool HasStats = false;
    bool HasActor = false;
    bool HasBuffs = false;
};

struct DebugEntityInfo {
    uint32_t Id = 0;
    uintptr_t Address = 0;
    std::string Path;
    int EntityType = 0;
    int EntitySubType = 0;
    int EntityState = 0;
    int Rarity = 0;
    NearbyZone Zone = NearbyZone::None;
    std::vector<std::pair<std::string, uintptr_t>> ComponentAddresses;
};

struct DebugModInfo {
    std::string Name;
    std::string StatKey;
    std::string AffixName;
    int GenerationType = 0; // 1=Prefix, 2=Suffix, 3=Implicit
    float Value0 = std::numeric_limits<float>::quiet_NaN();
    float Value1 = std::numeric_limits<float>::quiet_NaN();
};

struct DebugInventoryItem {
    uintptr_t Address = 0;
    std::string Path;
    std::string BaseTypeName;
    std::string UniqueName;
    int SlotX = 0;
    int SlotY = 0;
    int Width = 1;
    int Height = 1;
    int Rarity = 0;
    int ItemLevel = 0;
    int RequiredLevel = 0;
    bool IsIdentified = false;
    bool IsCorrupted = false;
    int CraftedModCount = 0;
    std::vector<DebugModInfo> ImplicitMods;
    std::vector<DebugModInfo> ExplicitMods;
    std::vector<DebugModInfo> EnchantMods;
    std::vector<DebugModInfo> HellscapeMods;
};

struct DebugInventoryData {
    int InventoryId = -1;
    uintptr_t Address = 0;
    int TotalBoxesX = 0;
    int TotalBoxesY = 0;
    int ServerRequestCounter = 0;
    float GridScreenX = 0;
    float GridScreenY = 0;
    float CellSize = 0;
    bool GridValid = false;
    std::vector<bool> SlotOccupied;
    std::vector<DebugInventoryItem> Items;
};

// Simplified game snapshot for plugins.
// Populated by the host from GameClientData each frame.
struct PluginGameSnapshot {
    GameStateTypes CurrentState = GameStateTypes::GameNotLoaded;

    std::string CurrentAreaName;
    std::string CurrentAreaHash;
    uint8_t CurrentAreaLevel = 0;
    bool IsTown = false;
    bool IsHideout = false;
    bool IsPaused = false;
    bool IsSkillTreeVisible = false;

    float WorldToGridConvertor = 0;

    RadarEntity Player;
    std::vector<RadarEntity> Entities;

    MapData LargeMap;
    MapData MiniMap;

    PlayerVitals Vitals;

    int ScreenWidth = 0;
    int ScreenHeight = 0;

    DWORD ProcessId = 0;
    HWND GameWindow = nullptr;
    bool GameWindowForeground = true;

    bool IsAttached = false;
    bool IsWindowValid = false;

    uint64_t LastUpdateTime = 0;
    uint64_t AreaChangeCounter = 0;

    std::vector<InventoryInfo> Inventories;
    std::map<std::string, int> CurrencyTotals;
    InventoryGridInfo InventoryGrid;

    DirectX::XMFLOAT4X4 WorldToScreenMatrix = {};
};

// --- UI Element Data (SDK v5) -------------------------------------------------

struct UiElementData {
    uintptr_t ParentAddr = 0;
    int ChildCount = 0;

    float RelativeX = 0, RelativeY = 0;
    float PositionModX = 0, PositionModY = 0;
    float UnscaledWidth = 0, UnscaledHeight = 0;
    float LocalScaleMultiplier = 1.0f;

    uint32_t Flags = 0;
    uint16_t ElementType = 0;
    uint8_t ScaleIndex = 0;
    bool IsVisible = false;
    bool HasPositionModifier = false;

    std::string StringId;

    bool Valid = false;
};

// --- Component Data Structs (SDK v5) ------------------------------------------

struct PluginVitalData {
    float Regeneration = 0;
    int Total = 0;
    int Current = 0;
    int ReservedFlat = 0;
    int ReservedPercent = 0;
};

struct PluginLifeData {
    PluginVitalData Health;
    PluginVitalData Mana;
    PluginVitalData EnergyShield;
    bool Valid = false;
};

struct PluginRenderData {
    float WorldX = 0, WorldY = 0, WorldZ = 0;
    float ModelBoundsX = 0, ModelBoundsY = 0, ModelBoundsZ = 0;
    float TerrainHeight = 0;
    bool Valid = false;
};

struct PluginPositionedData {
    uint8_t Reaction = 0;
    bool IsFriendly = false;
    bool Valid = false;
};

struct PluginTargetableData {
    bool IsTargetable = false;
    bool IsHighlightable = false;
    bool IsTargettedByPlayer = false;
    bool HiddenFromPlayer = false;
    bool MeetsQuestState = false;
    bool MeetsItemRequirements = false;
    bool Valid = false;
};

struct PluginChestData {
    bool IsOpened = false;
    bool IsLabelVisible = false;
    bool Valid = false;
};

struct PluginShrineData {
    bool IsUsed = false;
    bool Valid = false;
};

struct PluginStackData {
    int CurrentSize = 0;
    int MaxSize = 0;
    bool Valid = false;
};

struct PluginChargesData {
    int Current = 0;
    int PerUseCharges = 0;
    bool Valid = false;
};

struct PluginPlayerData {
    std::string Name;
    uint32_t Xp = 0;
    uint8_t Level = 0;
    bool Valid = false;
};

struct PluginAnimatedData {
    std::string Path;
    uint32_t Id = 0;
    bool Valid = false;
};

struct PluginTransitionableData {
    short CurrentState = 0;
    bool Valid = false;
};

struct PluginTriggerableBlockageData {
    bool IsClosed = false;
    bool IsBlocked = false;
    bool Valid = false;
};

struct PluginMinimapIconData {
    uintptr_t DatRowAddr = 0;
    bool Valid = false;
};

struct PluginStateMachineData {
    uintptr_t StatesPtr = 0;
    int StatesCount = 0;
    bool Valid = false;
};

struct PluginBaseData {
    std::string BaseTypeName;
    uint8_t Width = 0;
    uint8_t Height = 0;
    bool Valid = false;
};

struct PluginModsData {
    bool IsIdentified = false;
    bool IsCorrupted = false;
    bool IsSplit = false;
    bool IsMirrored = false;
    bool IsRelic = false;
    bool IsSynthesised = false;
    int Rarity = 0;
    int ItemLevel = 0;
    int RequiredLevel = 0;
    int CraftedModCount = 0;
    std::vector<ItemModData> ImplicitMods;
    std::vector<ItemModData> ExplicitMods;
    std::vector<ItemModData> EnchantMods;
    std::vector<ItemModData> HellscapeMods;
    std::vector<ItemModData> CrucibleMods;
    bool Valid = false;
};

struct PluginStatsData {
    int CurrentWeaponIndex = 0;
    bool IsShapeshifted = false;
    std::vector<std::pair<int, int>> StatsByItems;
    std::vector<std::pair<int, int>> StatsByBuffs;
    bool Valid = false;
};

struct PluginBuffData {
    std::string Name;
    float TotalTime = 0;
    float TimeLeft = 0;
    short Charges = 0;
    short FlaskSlot = 0;
    short Effectiveness = 0;
    uint32_t SourceEntityId = 0;
};

struct PluginBuffsData {
    std::vector<PluginBuffData> Buffs;
    bool Valid = false;
};

struct PluginActiveSkillData {
    std::string Name;
    int UseStage = 0;
    int CastType = 0;
    int TotalUses = 0;
    int TotalCooldownTimeInMs = 0;
    bool CanBeUsed = true;
};

struct PluginActorData {
    int AnimationId = 0;
    std::string AnimationName;
    std::vector<PluginActiveSkillData> ActiveSkills;
    bool Valid = false;
};

struct PluginNpcData {
    uintptr_t EntityOwnerAddr = 0;
    bool Valid = false;
};

struct PluginDiesAfterTimeData {
    uintptr_t EntityOwnerAddr = 0;
    bool Valid = false;
};

} // namespace PluginSDK
