# Changelog

## 1.0.4

- Built with the current plugin SDK: a controller that is not XInput reports no port (-1) even when ready.

## 1.0.3

- Follows the program language (ten languages).
- Hotkey names show correctly with a non-English keyboard layout.
- Prices work in every game language: items in the inventory, the stash and on the ground are priced by the item itself, not by the name the game shows (POEFixer 371 and later; an older POEFixer prices by name as before).
- The Runeshape Combinations panel is found, and its reward quantities read, in every game language.
- The selected display currency is not priced on itself in a non-English game either, also in the Runeshape Combinations panel.
- The combination weights in the Runeshape Combinations panel appear in every game language: a row is matched to its reward by quantity, recipe size and price when the game shows the reward's name in another language.
- The Runeshape tooltip in Traditional Chinese uses the game's word for remnant, and the Debug panel keeps the SDK field name Zone in every language.

## 1.0.2

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.
- The Runeshape window now has the POEFixer gear menu (POEFixer 366): lock it, take it out of the game onto any screen, reset its position or close it; its place is remembered separately in and out of the game.
- Out of the game the Runeshape window does not hide on mouse hover (hide on hover applies in game only).

## 1.0.1

- Rebuilt with the updated plugin SDK headers (POEFixer 365 adds optional calls for UI control names, stash objects and tabs, wider entity scans and the host automation flag). No change in what the plugin does.

## 1.0.0

- First versioned release.
