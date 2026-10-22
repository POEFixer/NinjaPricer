# NinjaPricer — Item Price Overlay Plugin

Current version: 1.0.1

Requires POE2Fixer 364 or newer.

An item price overlay for POE2Fixer: prices appear directly on dropped items, in your
inventory and in the open stash tab, plus Ritual shop and Runeshape rewards.

## Where the prices come from

NinjaPricer does no downloading of its own. It shows the prices POE2Fixer already
loads: the price source (poe.ninja or poe2scout) and the league are chosen in POEFixer
**Settings -> Prices**, and the plugin follows that choice. The **Data Source** tab shows
whether prices are loaded, how many items are known and the current Divine / Exalted
exchange rates.

## What It Does

- **Dropped items** — a price next to each item label on the ground, multiplied by the
  stack size. The position (top, bottom, left or right of the label) is configurable.
- **Inventory and stash** — a price in the corner of each occupied cell (corner
  configurable), with an optional colour square or item icon.
- **Ritual shop** — prices on the Favours window.
- **Runeshape** — reward prices for Runeshape devices, rune sockets and propagating runes,
  and a Runeshape window with the best reward's name, quantity, icon and price (text or
  currency icon, adjustable opacity, a show/hide hotkey, hides on mouse hover if wanted).
- **Currency display** — values in Divine (D), Exalted (E) or Chaos (C).
- **Minimum price per currency** — hide anything worth less than your Divine / Exalted /
  Chaos threshold (the stack or reward total counts; 0 turns a minimum off). Prices are
  not drawn on the selected currency itself.

## Settings

- **Display Settings** — currency mode, text size, label positions for ground items and for
  inventory/stash, minimum prices, inventory scan interval.
- **Overlay Toggles** — dropped items, inventory, stash, Ritual, Runeshape (and its window
  options), item icons, colour square, hide when the game is not focused, and a
  hold-to-hide hotkey (any key or mouse button).
- **Runeshape weights** — show or hide weights in both the game's Combinations panel
  and the movable Runeshape window. **Radar -> RuneShape -> Show weights on map**
  controls map labels separately; edit the rune weights in that Radar tab.
- **Debug** — diagnostics for troubleshooting.

## Disclaimer

Community third-party plugin for POE2Fixer. Use at your own risk. Not affiliated with
Grinding Gear Games.

## Install

Install it from POE2Fixer → Plugins → Plugin Manager (one click, updates included), or download `NinjaPricer.zip` from [Releases](https://github.com/POEFixer/NinjaPricer/releases/latest) and extract it into the POE2Fixer folder (it creates `Plugins\NinjaPricer\`). Then enable the plugin in the Plugins tab.

## Build from source

This repository is the plugin folder from the POE2Fixer source tree (`Plugins/NinjaPricer/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POE2Fixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `NinjaPricer.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\NinjaPricer\NinjaPricer.dll` (Release builds carry no debug information, see `Directory.Build.targets`).
4. Copy `NinjaPricer.dll` into `Plugins\NinjaPricer\` next to the POE2Fixer executable.
