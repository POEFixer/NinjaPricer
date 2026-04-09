# NinjaPricer Plugin

Multi-source price overlay plugin for POE2Fixer. Displays item prices from poe.ninja and poe2scout on dropped items, inventory, and stash.

## Features

- Real-time price data from poe.ninja Exchange API and poe2scout
- Overlay prices on ground items, inventory, and stash tabs
- Configurable display currency (Divine/Exalted/Chaos)
- Category toggles for currency and unique item types
- Auto-refresh with configurable interval
- Hold-to-hide hotkey support

## Build

Requires Visual Studio 2022 with MSVC v143 toolset.

```
MSBuild NinjaPricer.sln -p:Configuration=Release -p:Platform=x64
```

Output: `bin/Release/NinjaPricer.dll`

## Dependencies

- **nlohmann/json** -- bundled in `lib/nlohmann/json.hpp`
- **wininet.lib** -- Windows SDK (linked automatically)
- **Plugin SDK** -- bundled in `sdk/`
- **ImGui** -- bundled in `imgui/`

## Installation

Copy the built `NinjaPricer.dll` into your POE2Fixer `Plugins/NinjaPricer/` directory.
