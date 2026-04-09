# NinjaPricer — Item Price Overlay Plugin

A real-time item price overlay for POE2Fixer that fetches prices from **poe2scout** and **poe.ninja**, displaying them directly on ground items, inventory, and stash tabs.

## What It Does

NinjaPricer automatically prices items you encounter in Path of Exile 2 by querying online price databases and rendering the prices as an overlay on top of the game.

### Dual Price Sources
- **poe2scout** (default) — comprehensive PoE 2 coverage with unique item pricing
- **poe.ninja** — alternative/supplementary data source

You can switch between sources in the plugin settings at any time.

### Price Categories
Fetches prices across **15 currency categories**: Currency, Fragments, Abyssal Bones, Uncut Gems, Essences, Soul Cores, Idols, Runes, Ritual Omens, Expedition, Delirium, Breach, Lineage Support Gems, Reliquary Keys (poe2scout only), Incursion (poe2scout only).

Additionally supports **7 unique item categories** (poe2scout only): Accessories, Armour, Flasks, Jewels, Maps, Weapons, Sanctum Research.

Each category can be individually toggled on or off.

## How It Works

### Three Display Locations

**Ground Items** — prices appear on dropped items in the game world. Detects stack counts (e.g., "2x Divine Orb" shows doubled price). Position is configurable: Top, Bottom, Left, or Right of the item label.

**Inventory** — prices appear on items in the player inventory grid. Font size adapts automatically for small cells. Position configurable: any corner of each cell.

**Stash Tabs** — prices appear on items in the currently open stash tab. The plugin auto-detects the stash UI and active tab index.

### Price Display
- **Three currency modes**: Divine (D), Exalted (E), Chaos (C)
- **Color-coded values**: gold (>= 1 Divine), white (>= 0.1 Divine), gray (< 0.1 Divine)
- **Adjustable text size**: 0.5x to 2.0x

### Auto-Refresh
A background thread fetches prices every 5-60 minutes (configurable interval). A manual refresh button is available. Prices are cached locally as JSON files for offline fallback.

### League Detection
Automatically fetches the current league list from poe2scout.com API. Fallback leagues: Fate of the Vaal, HC Fate of the Vaal, Standard, Hardcore.

### Visibility Controls
- **Auto-hide** when the game window is not in focus
- **Hold-to-hide hotkey** for temporarily hiding the overlay during gameplay
- **Per-category toggles** to enable/disable individual price categories

### Settings
League selector, data source selection, currency display mode, text scale, display position per location, refresh interval, category toggles, hide hotkey configuration.

## Build

**Requirements:** Visual Studio 2022 (MSVC v143), Windows SDK 10.0, C++20

Open `NinjaPricer.sln` and build **Release | x64**.

Or from command line:
```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" NinjaPricer.sln -p:Configuration=Release -p:Platform=x64
```

Output: `bin\Release\NinjaPricer.dll`

## Dependencies

All dependencies are bundled — no external installs needed:
- **nlohmann/json** (`lib/nlohmann/json.hpp`) — JSON parser for API responses
- **wininet.lib** — Windows SDK HTTP library (linked automatically, no install needed)
- **ImGui** (`imgui/`) — immediate-mode GUI library
- **Plugin SDK** (`sdk/`) — POE2Fixer plugin interface headers

## Install

Copy `NinjaPricer.dll` to `Plugins/NinjaPricer/` in your POE2Fixer directory, then enable it in the Plugins tab.
