# Favorites Menu Grid - SFSE

Starfield gives you twelve favorite slots. This gives you several rows of
twelve, and draws them all at once as a grid you can click.

This is a fork of [Favorites Banks](https://www.nexusmods.com/starfield/mods/17906)
by Sator, which added the extra rows. This build adds the grid and makes it the
point: the wheel still runs underneath, but the grid is what you look at and
what you edit in. See [What is different from Favorites Banks](#what-is-different-from-favorites-banks).

Targets Starfield 1.16.244.0. Treat it as a beta: it is tested on one save and
one load order.

## The grid

One row per set of twelve, one cell per slot. The column headings show which key fires
each column, read from your own key bindings, so a glance tells you what `3`
will do on the row you are looking at.

- **Click a cell** to switch to that row and use the slot. This goes through
  the wheel's own selection path, so every guard the wheel has still applies.
- **Hover a cell** for the full item name, its row and slot, and the game's
  own item card.
- **Favorites you are not carrying** are drawn faint, and the hover says so.
  The wheel would just show those slots as empty, which explains nothing.
- **`DELETE`** empties the cell under the cursor. Starfield itself cannot
  remove a favorite at all - it can only overwrite one.

### Edit mode

The `EDIT` button in the right-hand strip turns it on.

- **Click a favorite** to pick it up, then **click any cell** to put it there.
  Across rows, which the wheel cannot do at all.
- **The red X** in a cell's corner deletes that favorite. It lights up when the
  cursor is on it.
- The status line says what you are holding, so a click that did nothing is
  distinguishable from a click that did not register.

Creating a favorite still happens where it always did: favorite an item in
your inventory or a power in the powers menu.

## Reserved slots

Some mods pick a favorite slot in their own options and write their item into
it whenever they like. Such a slot belongs to no row, and letting the rows
capture it is what used to copy that item onto every one you visited.

`ExternallyManagedSlots` in the INI leaves such a slot alone entirely and
shows it once, in the strip on the right, instead of in the rows. It costs one
column on every row - twelve is all the game has.

This was built for
[UC Classified Gear Sets - AEGIS Prototype Armor](https://www.nexusmods.com/starfield/mods/17579)
by the same author,
whose Technician set puts a deployable turret and its recall item into one
slot. Two items sharing one slot is exactly why it has to be reserved: both
need to know where they live. For that case the grid draws its own symbols -
a turret, or a handheld transmitter for the recall - because a slot like this
is the one place the game's icon data cannot be relied on.

## Keys

All of these work only while the favorites menu is open.

`DELETE` empties the slot under the cursor. That is the only key the mod
binds, and only while the favorites menu is open.

The normal `1-0`, `[` and `]` quickkeys keep their vanilla behaviour and
always act on your **default row**, which goes back into the game's real
slots every time the menu closes. Set which one with `DefaultRow` in the
INI.

**No controller support.** The grid is mouse-driven, and rather than ship a
half-working gamepad path there is none at all. The design is worked out - the
wheel's selection is a property the plugin can read and set, so the grid could
follow a gamepad without intercepting any input - but it needs someone who
plays that way to test it. Say so on the mod page if that is you.

## How the rows work

Every row is stored in a file beside your save, and one row at a time
occupies the game's real twelve slots. Browsing rows changes nothing; it only
draws. The row you settle on is written into the real slots when you use a
slot or close the menu. From then on the game owns it, which is why the
quickkeys and the inventory heart behave exactly as they do unmodded.

Which row is live is not something the grid shows you - it draws all of
them - so leaving whichever row you last clicked in the real slots would turn
the gameplay quickkeys into invisible state. Instead your `DefaultRow` goes
back in every time the menu closes. Those keys then mean one fixed thing.

The rows live beside the save, the twelve real slots live *in* it, so each
save keeps its own copy of the rows and they rewind with it. Load a save the
mod has never seen - one made before you installed it - and it starts from
that save's own twelve favorites, placed on your default row, with the other
rows empty. It never carries the rows of the save you just left into it.

## Requirements

- Starfield 1.16.244.0 (Steam)
- SFSE matching runtime 1.16.244.0
- Address Library for SFSE Plugins matching runtime 1.16.244.0

The DLL refuses a different runtime rather than using unverified addresses.

## Installation

Install with Vortex or Mod Organizer 2, or copy the `SFSE` and `Interface`
folders into the game's `Data` folder by hand, then launch through
`sfse_loader.exe`:

```
Starfield/Data/SFSE/Plugins/FavoritesMenuGrid.dll
Starfield/Data/SFSE/Plugins/FavoritesMenuGrid.ini
Starfield/Data/Interface/favoritesmenu.swf
```

The archive root is the content of `Data`, not a `Data` folder. Mod Organizer 2
rejects an archive wrapped in `Data` with "The content of &lt;data&gt; does not
look valid".

The file names are deliberately the same as Favorites Banks. Both mods patch
the same menu, so they must not run at once - identical names make your mod
manager say so instead of letting two plugins fight over the same wheel.

## Compatibility

- **Replaces Favorites Banks.** Do not run both.
- Any other mod that replaces `favoritesmenu.swf` conflicts at the file level;
  whichever file wins deployment controls the wheel.
- Intentionally incompatible with Favorites Menu Extended. If its DLL is
  detected, this plugin disables itself without changing favorite state.

## Known limits

- Locked to Starfield 1.16.244.0. A game update needs a matching build.
- No controller support for the grid.
- The inventory heart can only represent the row currently in the real slots.
- Item icons come from the wheel's own item cards, which the game hands over
  only when the favorites change while the menu is open. A slot that has never
  been described shows its editor ID until it is.
- A favorited item you no longer carry leaves its slot faint but assigned. The
  descriptor is kept and works again when the item returns.
- Tested on one setup, not on every load order, language or UI mod. Keep a
  save backup.

## What is different from Favorites Banks

Added:

- the grid, with icons, column headings from your key bindings, hover
  information and the dimming of items you are not carrying
- edit mode: moving favorites between slots and rows, deleting from a cell
- `ExternallyManagedSlots`, and drawn symbols for a reserved slot
- `DefaultRow`, restored on every close, and `ToggleEquipOnSelect`

Removed, because the grid replaces them or they belong to the wheel:

- mouse-wheel switching, and the low-level mouse hook behind it
- controller shoulder-button switching
- the small indicator drawn under the wheel
- the keys `F1`-`F8`, `PrevPageKey`/`NextPageKey` and `ModifierKey`: with one
  row always restored on close, they had nothing left to do
- `IconSize`, `IconTelemetry`, `PollIntervalMs`

## Building from source

Place CommonLibSF beside this project as `CommonLibSF-libxse`, at the exact
commit in `THIRD_PARTY.md`. Then:

```powershell
xmake f -m releasedbg
xmake
python -m unittest discover -s tests -v
```

The tests are source invariants, not a substitute for playing: engine ABI and
Scaleform behaviour cannot be proven offline.

`favoritesmenu.swf` is Bethesda's file with patched ActionScript. It is not
rebuilt by this project; `swf-src/` holds the decompiled scripts for reference
only.

## Licence and credits

Source code is GPL-3.0-or-later, with the Modding Exception and the GPL-3.0
Linking Exception in `EXCEPTIONS`. See `LICENSE`.

- Original mod: **Favorites Banks** by **Sator** -
  https://www.nexusmods.com/starfield/mods/17906
- This is an unofficial fork. It is not endorsed by Sator.
- Starfield and Bethesda assets, including `favoritesmenu.swf`, are not
  covered by that licence and remain the property of Bethesda. See
  `THIRD_PARTY.md`.
- SFSE Team and CommonLibSF contributors for the plugin ecosystem.

Binary distributions must include or offer the corresponding source under the
included GPL terms.
