#pragma once

// Plugin SDK Version - increment when breaking changes are made to the API
#define PLUGIN_SDK_VERSION 5

// Export/Import macros for plugin DLLs
#ifdef PLUGIN_EXPORTS
    #define PLUGIN_API __declspec(dllexport)
#else
    #define PLUGIN_API __declspec(dllimport)
#endif

// Forward declaration
struct PluginContext;

/// Abstract interface that every plugin must implement.
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// Called once after loading to set the plugin's directory path.
    /// @param dllDirectory Relative path like "Plugins/PluginName"
    virtual void SetPluginDirectory(const char* dllDirectory) = 0;

    /// Called when the plugin is enabled (by user toggle or on startup if previously enabled).
    /// @param isGameOpened true if game process is currently attached
    virtual void OnEnable(bool isGameOpened) = 0;

    /// Called when the plugin is disabled by the user.
    virtual void OnDisable() = 0;

    /// Called every frame to draw plugin settings in the Plugins settings tab.
    /// Use ImGui calls here.
    virtual void DrawSettings() = 0;

    /// Called every frame to draw plugin overlay/UI. Only called when plugin is enabled.
    /// Use ImGui calls here.
    virtual void DrawUI() = 0;

    /// Called periodically to save plugin settings to disk.
    virtual void SaveSettings() = 0;

    /// Return the plugin's display name for the settings UI.
    virtual const char* GetName() = 0;

    /// Return the SDK version this plugin was built against.
    virtual int GetSDKVersion() { return PLUGIN_SDK_VERSION; }

    /// Called once after creation to provide host services context.
    /// @param context Pointer to PluginContext (valid for the plugin's lifetime)
    virtual void SetContext(PluginContext* context) = 0;

    /// Return true if this plugin wants to render in overlay mode (transparent overlay on top of game).
    /// When true, the host enters overlay mode even if no built-in features require it.
    /// Default: false (plugin only renders in normal settings window).
    virtual bool WantsOverlay() { return false; }
};

// Factory function signatures that each plugin DLL must export.
// The host calls CreatePlugin() to instantiate and DestroyPlugin() to clean up.
extern "C" {
    typedef IPlugin* (*CreatePluginFunc)();
    typedef void (*DestroyPluginFunc)(IPlugin*);
}

// Plugin DLLs must export these two functions:
//   extern "C" PLUGIN_API IPlugin* CreatePlugin();
//   extern "C" PLUGIN_API void DestroyPlugin(IPlugin* plugin);
