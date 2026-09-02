# Favorites Banks: native favorites research

Target runtime: Starfield 1.16.244.0 / SFSE 0.2.21.

This document records the behavior verified from the shipped Scaleform files,
the runtime executable, the matching Address Library, CommonLibSF, and the
publicly released Favorites Menu Extended 5.0 package. It intentionally
separates confirmed behavior from compatibility policy.

## 1. Vanilla UI contract

- `FavoritesMenu.swf` contains exactly 12 `FavoritesEntry` clips,
  `Entry_0` through `Entry_11`. `FS_NONE` is 12.
- The menu subscribes to the `FavoritesData` shuttle. Its
  `aFavoriteItems` array is indexed by native quick-slot number.
- In assignment mode, choosing a slot dispatches
  `FavoritesMenu_AssignQuickkey` with `uQuickkeyIndex` and closes the menu.
- In normal mode, choosing a slot dispatches `FavoritesMenu_UseQuickkey` with
  the same field and closes the menu.
- `Quickkey1` through `Quickkey12` are not page keys. They select one of the
  12 native slots and immediately close/use the selection.
- The vanilla `onDataUpdate` reloads entries present in `aFavoriteItems`, but
  does not unload entries omitted from a later update. A page switch therefore
  has to clear stale `ImageFixture` state before publishing new data.
- Inventory favoriting dispatches both `uItemHandle` and `uFormID`. The handle
  is required to distinguish inventory rows that share a base form.
- Power favoriting dispatches only `uFormID`.

Consequence: overloading `Q+1` as a page selector conflicts with the native
meaning of `Quickkey1` unless the mod hooks and suppresses the game event. The
wheel, a separate page key, or a clickable page indicator do not alter the
native quick-slot contract.

## 2. Native favorite state

The `FavoritesManager` singleton for this runtime is Address Library ID
938946. The relevant verified fields are:

- pending inventory handle: `+0x420`
- pending form: `+0x428`
- form-slot array count: `+0x430`
- form-slot array mode: `+0x438`
- form-slot array storage: `+0x440`
- inventory-handle array mode: `+0x510`
- inventory-handle array storage: `+0x518`

Both active arrays contain 12 entries. An invalid inventory handle is
`0xFFFFFFFF`.

The base-form array alone cannot identify an exact weapon instance. A native
inventory favorite is represented by all of the following synchronized state:

- the slot's form pointer in `FavoritesManager`;
- the slot's native inventory handle in `FavoritesManager`;
- `BGSInventoryItem::unk24`, a signed byte at row offset `+0x24`;
- the engine-managed extra-data and inventory notifications performed by the
  native assignment path.

Changing only the form array, only `ExtraFavorite`, or only `unk24` produces a
partial favorite: the inventory can still show a heart while the menu cannot
resolve or use the item.

## 3. Inventory representation

`BGSInventoryItem` is 0x28 bytes:

- base object: `+0x00`
- smart pointer to `TBO_InstanceData`: `+0x08`
- stack array: `+0x10`
- flags: `+0x20`
- native quick-slot cache: `+0x24`

Each stack is 0x10 bytes and contains an `ExtraDataList` smart pointer and a
count. The inventory list stores its owner handle at `+0x38`.

The native slot resolver scans inventory rows and compares the signed byte at
`BGSInventoryItem + 0x24` to the requested slot. It then creates the game's
inventory handle from the exact row, not merely from its base FormID.

`ExtraFavorite` is 0x18 bytes and its slot byte is at `+0x18`. Its valid native
values are 0 through 11, while 0xFE means no favorite. Values 12 and above are
not persistent virtual slots and must never be used for inactive banks.

`ExtraUniqueID` is 0x20 bytes:

- unique ID: unsigned 16-bit value at `+0x18`
- origin/owner FormID: unsigned 32-bit value at `+0x1C`

The pair is suitable for resolving a non-stackable inventory instance after a
save load. Within the current process, the `TBO_InstanceData` pointer provides
an additional exact-row discriminator.

## 4. Verified native operations

Address Library IDs below are for runtime 1.16.244.0.

- ID 113919: clear one native favorite slot.
  - Signature: `(FavoritesManager*, uint8_t slot)`.
  - Clears the manager form and handle, finds the matching inventory row, and
    runs the native removal callback and notifications.
- ID 113920: assign one native favorite slot.
  - Signature: `(FavoritesManager*, uint32_t* inventoryHandle,
    TESForm* form, uint8_t slot)`.
  - Always clears the destination first.
  - A valid handle assigns the exact inventory row.
  - `0xFFFFFFFF` assigns a power/form without an inventory row.
- ID 48481: create a native inventory handle.
  - Signature: `(uint32_t* outHandle, uint32_t ownerHandle,
    BGSInventoryItem* row)`.
  - It locks and searches the owner's inventory internally. It must not be
  called while the plugin already holds the inventory lock.
- ID 48490: release the caller's inventory-handle reference.
  - Signature: `(handleManager, uint32_t* handle)`.
  - The global handle-manager singleton is Address Library ID 883301.
  - ID 113920 retains the reference needed by the favorite manager; the caller
    must release its local reference immediately afterward, matching the
    native caller. Omitting this release leaks a handle on every page switch.
- ID 113925: update a manager form slot.
  - It removes duplicate occurrences of the same form before writing the
    requested slot. It is a low-level helper, not a complete assignment API.
- ID 113930/113982: resolve the native handle associated with a slot by
  scanning `BGSInventoryItem::unk24`.
- ID 113935: rebuild the 12-slot `FavoritesData` shuttle from native state.
- ID 113999: clear the cached `FavoritesData` payload.

The complete clear/assign functions are the required mutation boundary. Direct
writes to manager arrays or extra data are appropriate only for diagnostics,
never for a page switch.

## 5. Persistence identity

Only one bank is native at a time. Inactive banks belong in a versioned
sidecar, not in invalid `ExtraFavorite` values.

Each stored inventory slot needs:

- source plugin filename;
- source-local FormID;
- raw FormID as a fast same-load-order path;
- editor ID when available;
- `ExtraUniqueID` unique/origin pair when available;
- row ordinal among equivalent base-form rows as a last-resort fallback;
- a retained `TBO_InstanceData` smart pointer for exact same-session matching.

Each stored power/form slot needs the form identity fields but no inventory
identity.

Form resolution order:

1. valid editor ID with source-file validation;
2. raw FormID with source-file validation;
3. source filename plus local FormID across loaded forms.

Local FormID masks:

- normal/full plugin: low 24 bits;
- light `FE` plugin: low 12 bits;
- medium `FD` plugin: low 16 bits.

Inventory-row resolution order:

1. same-session `TBO_InstanceData` pointer and base form;
2. matching `ExtraUniqueID` pair and base form;
3. the sole row for the resolved base form;
4. stored ordinal among same-base rows only when no persistent unique identity
   exists, with an explicit fallback warning.

If more than one ambiguous modified instance exists, silently choosing the
first row is unsafe. The slot should remain empty rather than equip the wrong
weapon.

The sidecar is scoped by `BGSSaveLoadManager::currentPlayerID`, stored under
the user's redirected `Documents/My Games/Starfield/SFSE/Plugins` tree, written
atomically through a same-directory temporary file and replacement, and keeps
the preceding file as `.bak`. `current.fbs` stores the live character state;
save-completion events create save-specific snapshots. The loaded save's
native active page is authoritative and is matched against the stored banks;
the sidecar supplies inactive pages.

## 6. Correct page-commit transaction

This section described the 0.4 page-switch transaction and was left stale
through 0.5, which abandoned native mutation entirely. It is accurate again as
of 0.6.0, with one change: **this runs on commit, not on every page switch.**
Scrolling the wheel only redraws. The transaction below runs when the player
selects a slot or closes the wheel, in `MaterializeBankLocked`.

All mutation runs on the game's task thread, or synchronously inside a
Scaleform callback that is already on it.

1. Capture all 12 slots of the page that currently occupies the native slots,
   and reconcile them back into that page's stored descriptors.
   - Scan rows with `unk24` in 0..11.
   - Use manager handles to distinguish inventory items from powers/forms.
2. Call native clear (ID 113919) for all 12 slots.
3. Resolve each target descriptor against the now-current inventory under a
   short read lock, then release the lock.
4. Immediately create that row's handle with ID 48481, assign it with ID
   113920, and release the caller's handle reference with ID 48490.
5. Assign each target power/form through ID 113920 with an invalid handle.
6. Rebuild and publish `FavoritesData` once.
7. Record the page now held by the native slots and atomically save the
   sidecar.

No engine function that takes the inventory lock may be called while the
plugin holds that lock. Violating this ordering can deadlock or crash before a
save loads.

Resolving all target row pointers before clearing is also unsafe: native clear
callbacks can mutate inventory bookkeeping and invalidate an earlier pointer.
Resolution therefore occurs one row at a time after the clear phase.

Drawing a page that does *not* occupy the native slots is a separate concern.
Vanilla `onDataUpdate` reloads the entries present in its array but never
unloads the ones absent from a later update, so the previous page's icon
fixtures must be unloaded and `selectedIndex` reset to `FS_NONE` (12) before
publishing a fabricated page. `Components.ImageFixture` numbers its fixture
types `FT_INVALID = -1` and `FT_INTERNAL = 0`, so an empty position must be
described as `-1`: a `0` sends it down the loading path and registers an
unnamed request against the shared `FavoritesIconBuffer`, corrupting the icons
of the positions that are genuinely occupied.

## 7. Save/load and event lifecycle

The SFSE fork used for runtime 1.16.244 declares only post-load/data messages
0 through 3. Its historical save/load messages 4 through 7 are inside disabled
`#if 0` hooks and are not a valid persistence mechanism.

The implementation uses engine events exposed by CommonLibSF:

- `SaveLoadEvent` begins a load transition, initializes after load success and
  snapshots state after save completion;
- `TESLoadGameEvent` provides a second post-load initialization boundary;
- `MenuOpenCloseEvent` captures changes when Favorites, Inventory or Powers
  menus close;
- `InventoryInterface::FavoriteChangedEvent` schedules an asynchronous capture
  on the main task queue.

The favorite callback must never inspect the inventory synchronously because
the engine may still own its inventory locks while notifying sinks.

## 8. Legacy prototype migration

Pre-0.4 prototypes wrote values beginning at 12 into `ExtraFavorite`. Those
values decode as `bank=(value-12)/12` and `slot=(value-12)%12`, but the native
resolver never treats them as real slots.

At first initialization, the plugin imports those descriptors into the
sidecar and calls the verified native extra-data setter with 0xFE. This removes
the invalid heart marker instead of leaving a half-favorited item in the save.

## 9. Assignment behavior

Assignment remains native:

- Select a bank with the wheel, selector, or a dedicated bank key.
- Favorite an inventory item or power normally.
- The vanilla assignment menu receives the exact inventory handle and assigns
  the selected slot in the currently active bank.
- The plugin captures that native result when switching bank or saving.

Only items on the page currently held by the native slots display the vanilla
inventory heart. Showing a heart for items parked on the other pages would
require replacing or patching the Inventory Menu and would make the heart
ambiguous because it has no page number. The mod should not fake that state.

Because assignment stays native, the wheel must dispatch
`FavoritesMenu_AssignQuickkey` and `FavoritesMenu_UseQuickkey` unconditionally.
Gating either dispatch on which page is showing — as 0.5 did — leaves the
engine with nothing to act on, which is what made items on other pages
unresponsive. The plugin's own selection callback runs first and commits the
shown page, so the vanilla event that follows always lands on the right slots.

## 10. Compatibility boundary

- UI mods that preserve `FavoritesData`, `FavoritesMenu_AssignQuickkey`,
  `FavoritesMenu_UseQuickkey`, and `Entry_0` through `Entry_11` are compatible
  in principle.
- Favorites Menu Extended replaces the menu with 24 entries and manages its
  own 12 extra items. Running both systems would create two independent owners
  of favorite state. The plugin must detect that implementation and disable
  itself with a clear log message rather than corrupt state.
- This mod does replace `favoritesmenu.swf`. An earlier draft of this document
  claimed it did not, which was never true of any shipped build: the wheel has
  to call back into the DLL to report a selection and to hand over its item
  cards, and there is no other hook for that. The replacement is a minimal
  diff against the vanilla decompile in `favoritesmenu-decompiled/`, so any
  other mod that replaces the same file conflicts at the file level.

## 11. Required verification matrix

Before publishing or declaring the implementation complete, test all of these:

- existing vanilla favorites import into bank 1;
- empty banks stay empty;
- one weapon persists after page away/back;
- two differently modified weapons with the same base form return to their
  original pages;
- consumable stack count changes and use from a favorite;
- apparel equip/unequip;
- power assignment and use;
- replacing and removing a slot from Inventory and Powers menus;
- direct keys `1-0`, `[` and `]` after each page switch;
- wheel, clickable indicator, and dedicated page keys;
- save/load with each bank active;
- quicksave, manual save, and loading an older save;
- moving an inactive favorite out of player inventory;
- dropping and reacquiring a favorite;
- changing plugin load order for a mod-added item;
- uninstall with the desired bank active;
- StarUI-compatible menu refresh with no stale or enlarged icons;
- runtime refusal on unsupported Starfield versions;
- deliberate incompatibility refusal when Favorites Menu Extended is loaded.
