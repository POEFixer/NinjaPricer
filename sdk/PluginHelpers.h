#pragma once
// ============================================================================
// PluginHelpers.h — Convenience wrappers for plugin SDK
// ============================================================================
// Include this header in your plugin code for type-safe memory reading,
// inventory name lookup, and other utilities built on top of PluginContext.
// ============================================================================

#include "PluginAPI.h"
#include "PluginContext.h"
#include "PluginGameData.h"
#include "../imgui/imgui.h"

#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace PluginSDK {

// ============================================================================
// MemoryReader — typed wrapper around PluginContext memory functions
// ============================================================================
class MemoryReader {
public:
    explicit MemoryReader(PluginContext* ctx) : m_Ctx(ctx) {}

    // Read a single POD type from game memory
    template<typename T>
    T Read(uintptr_t address) const {
        T result{};
        if (m_Ctx && m_Ctx->ReadProcessMemory && address != 0) {
            m_Ctx->ReadProcessMemory(address, &result, sizeof(T));
        }
        return result;
    }

    // Read an array of POD types from game memory
    template<typename T>
    std::vector<T> ReadArray(uintptr_t address, int count) const {
        if (!m_Ctx || !m_Ctx->ReadProcessMemory || address == 0 || count <= 0)
            return {};
        std::vector<T> result(count);
        if (!m_Ctx->ReadProcessMemory(address, result.data(), count * sizeof(T)))
            return {};
        return result;
    }

    // Read a StdVector<T> from a container address in game memory
    template<typename T>
    std::vector<T> ReadStdVector(uintptr_t containerAddress) const {
        if (!m_Ctx || !m_Ctx->ReadStdVector || containerAddress == 0)
            return {};
        int count = 0;
        void* data = m_Ctx->ReadStdVector(containerAddress, sizeof(T), &count);
        if (!data || count <= 0) return {};
        std::vector<T> result(count);
        std::memcpy(result.data(), data, count * sizeof(T));
        free(data);
        return result;
    }

    // Read a StdList<T> from a container address in game memory
    template<typename T>
    std::vector<T> ReadStdList(uintptr_t containerAddress) const {
        if (!m_Ctx || !m_Ctx->ReadStdList || containerAddress == 0)
            return {};
        int count = 0;
        void* data = m_Ctx->ReadStdList(containerAddress, sizeof(T), &count);
        if (!data || count <= 0) return {};
        std::vector<T> result(count);
        std::memcpy(result.data(), data, count * sizeof(T));
        free(data);
        return result;
    }

    // Read a StdBucket<T> from a container address in game memory
    template<typename T>
    std::vector<T> ReadStdBucket(uintptr_t containerAddress) const {
        if (!m_Ctx || !m_Ctx->ReadStdBucket || containerAddress == 0)
            return {};
        int count = 0;
        void* data = m_Ctx->ReadStdBucket(containerAddress, sizeof(T), &count);
        if (!data || count <= 0) return {};
        std::vector<T> result(count);
        std::memcpy(result.data(), data, count * sizeof(T));
        free(data);
        return result;
    }

    // Traverse a StdMap and collect key-value pairs
    template<typename TKey, typename TValue>
    std::vector<std::pair<TKey, TValue>> ReadStdMap(uintptr_t containerAddress) const {
        if (!m_Ctx || !m_Ctx->ReadStdMap || containerAddress == 0)
            return {};
        std::vector<std::pair<TKey, TValue>> result;
        m_Ctx->ReadStdMap(containerAddress, sizeof(TKey), sizeof(TValue),
            [](const void* key, const void* value, void* userData) {
                auto* vec = static_cast<std::vector<std::pair<TKey, TValue>>*>(userData);
                TKey k; TValue v;
                std::memcpy(&k, key, sizeof(TKey));
                std::memcpy(&v, value, sizeof(TValue));
                vec->push_back({ k, v });
            }, &result);
        return result;
    }

    // Read a StdWString from a container address
    std::wstring ReadStdWString(uintptr_t containerAddress) const {
        if (!m_Ctx || !m_Ctx->ReadStdWString || containerAddress == 0)
            return L"";
        return m_Ctx->ReadStdWString(containerAddress);
    }

    // Read null-terminated ASCII string
    std::string ReadString(uintptr_t address) const {
        if (!m_Ctx || !m_Ctx->ReadString || address == 0) return "";
        return m_Ctx->ReadString(address);
    }

    // Read null-terminated Unicode string
    std::wstring ReadUnicodeString(uintptr_t address) const {
        if (!m_Ctx || !m_Ctx->ReadUnicodeString || address == 0) return L"";
        return m_Ctx->ReadUnicodeString(address);
    }

    uintptr_t GetBaseAddress() const {
        return (m_Ctx && m_Ctx->GetBaseAddress) ? m_Ctx->GetBaseAddress() : 0;
    }

    uintptr_t GetModuleSize() const {
        return (m_Ctx && m_Ctx->GetModuleSize) ? m_Ctx->GetModuleSize() : 0;
    }

    uintptr_t GetPatternAddress(const char* name) const {
        return (m_Ctx && m_Ctx->GetPatternAddress) ? m_Ctx->GetPatternAddress(name) : 0;
    }

private:
    PluginContext* m_Ctx;
};

// ============================================================================
// Utility: wide string to narrow string conversion
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
// Utility: get entity type name string
// ============================================================================
inline const char* GetEntityTypeName(EntityTypes type) {
    switch (type) {
    case EntityTypes::Chest: return "Chest";
    case EntityTypes::NPC: return "NPC";
    case EntityTypes::Player: return "Player";
    case EntityTypes::Shrine: return "Shrine";
    case EntityTypes::Monster: return "Monster";
    case EntityTypes::DeliriumBomb: return "DeliriumBomb";
    case EntityTypes::DeliriumSpawner: return "DeliriumSpawner";
    case EntityTypes::OtherImportantObjects: return "Important";
    case EntityTypes::Item: return "Item";
    case EntityTypes::Renderable: return "Renderable";
    case EntityTypes::AreaTransition: return "Transition";
    case EntityTypes::ExpeditionMarker: return "Expedition";
    case EntityTypes::ExpeditionRemnant: return "Remnant";
    default: return "Unknown";
    }
}

// ============================================================================
// Utility: get nearby zone name
// ============================================================================
inline const char* GetNearbyZoneName(NearbyZone zone) {
    switch (zone) {
    case NearbyZone::InnerCircle: return "Inner";
    case NearbyZone::OuterCircle: return "Outer";
    case NearbyZone::Far: return "Far";
    default: return "None";
    }
}

// ============================================================================
// Utility: get rarity name
// ============================================================================
inline const char* GetRarityName(int rarity) {
    switch (rarity) {
    case 0: return "Normal";
    case 1: return "Magic";
    case 2: return "Rare";
    case 3: return "Unique";
    default: return "Unknown";
    }
}

// ============================================================================
// Utility: rarity color
// ============================================================================
inline ImVec4 GetRarityColor(int rarity) {
    switch (rarity) {
    case 1: return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);   // Magic - blue
    case 2: return ImVec4(1.0f, 1.0f, 0.3f, 1.0f);    // Rare - yellow
    case 3: return ImVec4(0.8f, 0.4f, 0.0f, 1.0f);    // Unique - orange
    default: return ImVec4(0.9f, 0.9f, 0.9f, 1.0f);   // Normal - white
    }
}

} // namespace PluginSDK
