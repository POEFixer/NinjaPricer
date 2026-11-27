# Changelog

## 1.0.5

- The hold hotkey (Overlay Toggles tab) has a mode: **Hold to hide** (as before: prices and the Runeshape window disappear while the key is held) or **Hold to show** (they stay hidden and appear only while the key is held). Without a hotkey the mode does nothing; settings saved by an older version keep Hold to hide.
- Hotkeys look like the other plugin settings: the key on a button you click to change it, a quiet Clear beside it. The key code is no longer shown. Keyboard keys carry the plugin kit's English names, as in the other plugins (the arrow keys, Insert, Delete, Home, End, Page Up / Down and Num Lock showed as numpad keys or Pause before); punctuation keys show the character your keyboard layout puts on them (Ü, Ж, ...). A key the plugin cannot name is not bound.
- While a hotkey waits for a key, a left click elsewhere cancels, as Esc does; the left mouse button is never bound (one bound by an older version keeps working).
- The Runeshape window's show/hide hotkey also works while the hold hotkey hides the overlay.
- In a narrow settings tab or with a large font the hotkey rows put their controls under their labels instead of past the tab's edge.

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
