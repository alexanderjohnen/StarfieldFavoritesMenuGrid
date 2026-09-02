from __future__ import annotations

import re
import shlex
import unittest
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SLOTS = 12
MAX_BANKS = 8


def strip_line_comments(source: str) -> str:
    """Drop // comments so prose about removed code cannot satisfy a check."""
    return "\n".join(line.split("//", 1)[0] for line in source.splitlines())


def decode_legacy(value: int) -> tuple[int, int] | None:
    if value < SLOTS:
        return None
    offset = value - SLOTS
    bank, slot = divmod(offset, SLOTS)
    return (bank, slot) if bank < MAX_BANKS else None


@dataclass(frozen=True)
class Candidate:
    instance: str
    unique_ids: frozenset[tuple[int, int]]
    ordinal: int


def resolve_candidate(
    candidates: list[Candidate],
    session_instance: str | None,
    unique_ids: frozenset[tuple[int, int]],
    ordinal: int,
) -> Candidate | None:
    if session_instance:
        matches = [c for c in candidates if c.instance == session_instance]
        if len(matches) == 1:
            return matches[0]
    if unique_ids:
        matches = [c for c in candidates if c.unique_ids & unique_ids]
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            return None
    if len(candidates) == 1:
        return candidates[0]
    if not unique_ids and 0 <= ordinal < len(candidates):
        return candidates[ordinal]
    return None


def parse_state(text: str) -> dict:
    lines = [line for line in text.splitlines() if line]
    header = shlex.split(lines[0])
    if len(header) != 2 or header[0] != "FAVORITES_BANKS_STATE" or int(header[1]) not in (3, 4, 5):
        raise ValueError("unsupported header")
    # Files older than version 5 always kept page 1 in the real favorite
    # slots, so zero is the correct value to assume for them.
    result: dict = {"version": int(header[1]), "slots": {}, "native_bank": 0}
    saw_end = False
    for line in lines[1:]:
        fields = shlex.split(line)
        if not fields:
            continue
        tag = fields[0]
        if tag == "END":
            saw_end = True
            break
        if tag in {"CHARACTER", "BANK_COUNT", "ACTIVE_BANK", "NATIVE_BANK"}:
            result[tag.lower()] = int(fields[1])
        elif tag == "SAVE_NAME":
            result["save_name"] = fields[1]
        elif tag == "SLOT":
            bank, slot, kind = map(int, fields[1:4])
            if not (0 <= bank < MAX_BANKS and 0 <= slot < SLOTS):
                raise ValueError("slot out of range")
            unique_count = int(fields[10])
            unique_tokens = fields[11:]
            if len(unique_tokens) != unique_count * 2:
                raise ValueError("invalid unique ID count")
            result["slots"][(bank, slot)] = {
                "kind": kind,
                "raw_form_id": int(fields[4]),
                "form_type": int(fields[5]),
                "local_form_id": int(fields[6]),
                "ordinal": int(fields[7]),
                "source": fields[8],
                "editor_id": fields[9],
                "unique_ids": tuple(
                    (int(unique_tokens[i]), int(unique_tokens[i + 1]))
                    for i in range(0, len(unique_tokens), 2)
                ),
            }
        elif tag == "VISUAL":
            bank, slot = map(int, fields[1:3])
            if (bank, slot) not in result["slots"]:
                raise ValueError("visual without slot")
            elemental_count = int(fields[13])
            elemental_tokens = fields[14:]
            if len(elemental_tokens) != elemental_count * 2:
                raise ValueError("invalid elemental stat count")
            result["slots"][(bank, slot)]["visual"] = {
                "count": int(fields[3]),
                "is_power": bool(int(fields[4])),
                "is_equippable": bool(int(fields[5])),
                "is_equipped": bool(int(fields[6])),
                "ammo_count": int(fields[7]),
                "fixture_type": int(fields[8]),
                "name": fields[9],
                "ammo_name": fields[10],
                "image_directory": fields[11],
                "image_name": fields[12],
                "elemental_stats": tuple(
                    (int(elemental_tokens[i]), float(elemental_tokens[i + 1]))
                    for i in range(0, len(elemental_tokens), 2)
                ),
            }
    if not saw_end:
        raise ValueError("missing END")
    return result


class PersistenceModelTests(unittest.TestCase):
    def test_legacy_slot_migration_map(self) -> None:
        self.assertIsNone(decode_legacy(11))
        self.assertEqual(decode_legacy(12), (0, 0))
        self.assertEqual(decode_legacy(23), (0, 11))
        self.assertEqual(decode_legacy(24), (1, 0))
        self.assertEqual(decode_legacy(107), (7, 11))
        self.assertIsNone(decode_legacy(108))

    def test_instance_resolution_priority_and_ambiguity(self) -> None:
        first = Candidate("ptr-a", frozenset({(12, 0x14)}), 0)
        second = Candidate("ptr-b", frozenset({(13, 0x14)}), 1)
        candidates = [first, second]
        self.assertIs(
            resolve_candidate(candidates, "ptr-b", frozenset(), 0), second
        )
        self.assertIs(
            resolve_candidate(candidates, None, frozenset({(12, 0x14)}), 1),
            first,
        )
        self.assertIs(
            resolve_candidate(candidates, None, frozenset(), 1), second
        )
        ambiguous = [
            Candidate("ptr-a", frozenset({(12, 0x14)}), 0),
            Candidate("ptr-b", frozenset({(12, 0x14)}), 1),
        ]
        self.assertIsNone(
            resolve_candidate(
                ambiguous, None, frozenset({(12, 0x14)}), 0
            )
        )

    def test_state_format_keeps_save_and_exact_unique_ids(self) -> None:
        state = "\n".join(
            [
                "FAVORITES_BANKS_STATE 3",
                "CHARACTER 123456",
                "BANK_COUNT 4",
                "ACTIVE_BANK 2",
                'SAVE_NAME "Save 42 - Sator"',
                'SLOT 2 7 1 16777232 41 16 1 "Starfield.esm" "" 2 12 20 13 20',
                "END",
            ]
        )
        parsed = parse_state(state)
        self.assertEqual(parsed["character"], 123456)
        self.assertEqual(parsed["active_bank"], 2)
        self.assertEqual(parsed["save_name"], "Save 42 - Sator")
        self.assertEqual(
            parsed["slots"][(2, 7)]["unique_ids"], ((12, 20), (13, 20))
        )

    def test_state_parser_rejects_damage(self) -> None:
        with self.assertRaises(ValueError):
            parse_state("FAVORITES_BANKS_STATE 2\nEND\n")
        with self.assertRaises(ValueError):
            parse_state("FAVORITES_BANKS_STATE 3\nCHARACTER 1\n")

    def test_version_four_persists_virtual_icon_and_item_card(self) -> None:
        state = "\n".join(
            [
                "FAVORITES_BANKS_STATE 4",
                "CHARACTER 123456",
                "BANK_COUNT 4",
                "ACTIVE_BANK 1",
                'SAVE_NAME "Save 43"',
                'SLOT 1 2 1 291180 48 291180 0 "Starfield.esm" "Beowulf" 0',
                'VISUAL 1 2 1 0 1 0 321 3 "Beowulf" "7.77MM" "Interface/" "Weapon_Generic" 2 0 99.5 2 14',
                "END",
            ]
        )
        parsed = parse_state(state)
        visual = parsed["slots"][(1, 2)]["visual"]
        self.assertEqual(visual["name"], "Beowulf")
        self.assertEqual(visual["image_name"], "Weapon_Generic")
        self.assertEqual(visual["elemental_stats"], ((0, 99.5), (2, 14.0)))

    def test_version_five_records_which_page_holds_the_native_slots(self) -> None:
        state = "\n".join(
            [
                "FAVORITES_BANKS_STATE 5",
                "CHARACTER 123456",
                "BANK_COUNT 4",
                "ACTIVE_BANK 2",
                "NATIVE_BANK 2",
                'SAVE_NAME "Save 44"',
                'SLOT 2 0 1 44025 54 44025 0 "Starfield.esm" "" 0',
                "END",
            ]
        )
        parsed = parse_state(state)
        self.assertEqual(parsed["native_bank"], 2)
        self.assertEqual(parsed["active_bank"], 2)

    def test_pre_v5_state_is_read_as_page_one_being_native(self) -> None:
        state = "\n".join(
            [
                "FAVORITES_BANKS_STATE 4",
                "CHARACTER 123456",
                "BANK_COUNT 4",
                "ACTIVE_BANK 3",
                'SAVE_NAME "Old save"',
                'SLOT 0 0 1 291180 48 291180 0 "Starfield.esm" "Beowulf" 0',
                "END",
            ]
        )
        parsed = parse_state(state)
        self.assertEqual(parsed["native_bank"], 0)
        self.assertEqual(parsed["active_bank"], 3)


class SourceInvariantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.core = (ROOT / "src" / "favorites_core.cpp").read_text(
            encoding="utf-8"
        )
        cls.main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cls.ui = (ROOT / "src" / "favorites_ui.cpp").read_text(
            encoding="utf-8"
        )

    def test_no_virtual_slot_writer_remains(self) -> None:
        joined = self.core + self.main
        self.assertNotIn("EncodeVirtualSlot", joined)
        self.assertNotIn("WriteFavoriteSlot", joined)
        self.assertNotRegex(joined, r"unk24\s*=")

    def test_browsing_pages_never_mutates_native_slots(self) -> None:
        """Scrolling the wheel must stay free of side effects.

        Only an explicit commit may touch the twelve real slots, so the
        switch path is allowed to read and reconcile but never to clear or
        assign.
        """
        for address_id in (113919, 113920, 48481, 48490, 883301):
            self.assertIn(f"REL::ID({address_id})", self.core)
        switch = self.core.split("bool SwitchBank", 1)[1].split(
            "void QueueBankSwitch", 1
        )[0]
        self.assertNotIn("NativeClearSlot", switch)
        self.assertNotIn("NativeAssignInventory", switch)
        self.assertNotIn("NativeAssignForm", switch)
        self.assertNotIn("MaterializeBankLocked", switch)
        self.assertIn("ReconcileNativePageWithBank(g_nativeBank", switch)
        self.assertIn("RenderActiveBank", switch)

    def test_commit_proves_it_can_write_before_it_clears_anything(self) -> None:
        """Regression guard for the 0.6.0 data loss.

        0.6.0 cleared all twelve native slots and only then discovered that
        inventory rows could not be assigned, which emptied the player's real
        favorites. Nothing may be cleared before every occupied position of
        the target page has been proven writable.
        """
        commit = self.core.split("bool MaterializeBankLocked(", 1)[1].split(
            "void CaptureCurrentBankLocked", 1
        )[0]
        self.assertLess(
            commit.index("CanAssignDescriptor"),
            commit.index("NativeClearSlot"),
            "the writability check must run before the first clear",
        )
        # The refusal path has to return before reaching the clear loop.
        refusal = commit[: commit.index("NativeClearSlot")]
        self.assertIn("Refusing to commit page", refusal)
        self.assertIn("return false;", refusal)

    def test_a_page_is_never_drawn_showing_items_the_player_lost(self) -> None:
        """Regression guard for the ghost icon.

        A page that does not occupy the real slots is drawn from stored
        descriptors, which say what the player put there rather than what they
        still carry. Without a check against the live inventory a sold or
        stored weapon keeps its icon until the page is committed.
        """
        self.assertIn("BuildRenderablePage", self.core)
        self.assertIn("BuildRenderablePage", self.ui)
        build = self.core.split("FavoriteBank BuildRenderablePage", 1)[1].split(
            "void LogFromMenu", 1
        )[0]
        self.assertIn("inventoryList.LockRead", build)
        self.assertIn("carried.contains", build)
        # The native page is the engine's own, and must not be filtered.
        self.assertLess(
            build.index("showingNativePage"), build.index("inventoryList")
        )
        # Only the copy may be blanked; the stored descriptor has to survive.
        self.assertIn("slot = FavoriteSlot{}", build)
        self.assertNotIn("g_banks[activeBank][", build)

    def test_a_lost_item_does_not_block_its_whole_page(self) -> None:
        """Regression guard: storing one favorited weapon broke all of page 1.

        Refusing the entire commit because a single item left the inventory
        made the page unusable and, worse, blocked re-favouriting, because the
        selection handler reports the page as not committed. Only a failure
        that happens while the item is present may block a commit.
        """
        core = self.core
        self.assertIn("enum class SlotReadiness", core)
        for state in ("kWritable", "kItemAbsent", "kBlocked"):
            self.assertIn(state, core)
        commit = core.split("bool MaterializeBankLocked", 1)[1].split(
            "void CaptureCurrentBankLocked", 1
        )[0]
        refusal = commit[: commit.index("NativeClearSlot")]
        # The refusal must be driven by blocked slots alone, never by absent
        # ones, or losing an item locks the page again.
        self.assertIn("if (blocked != 0)", refusal)
        self.assertNotIn("if (absent != 0)", refusal)

    def test_handle_creation_failure_is_reported_not_swallowed(self) -> None:
        probe = self.core.split("SlotReadiness CanAssignDescriptor", 1)[1].split(
            "bool MaterializeBankLocked", 1
        )[0]
        # A check that does not exercise handle creation would not have caught
        # the failure that emptied page 1.
        self.assertIn("CreateRowHandle", probe)
        self.assertIn("ReleaseLocalInventoryHandle", probe)
        factory = self.core.split("bool CreateRowHandle", 1)[1].split(
            "bool NativeAssignInventory", 1
        )[0]
        self.assertIn("REL::ID(48481)", factory)
        self.assertIn("REX::WARN", factory)

    def test_commit_is_the_only_writer_of_native_slots(self) -> None:
        commit = self.core.split("bool MaterializeBankLocked", 1)[1].split(
            "void CaptureCurrentBankLocked", 1
        )[0]
        # Capture the outgoing page before overwriting it, clear every slot,
        # and only then resolve and assign the incoming page.
        self.assertLess(
            commit.index("CaptureNativePage"), commit.index("NativeClearSlot")
        )
        self.assertLess(
            commit.index("NativeClearSlot"),
            commit.index("AssignDescriptorToNativeSlot"),
        )
        self.assertIn("g_nativeBank = targetBank", commit)
        self.assertIn("RebuildFavoritesData", commit)

    def test_a_missing_item_does_not_delete_its_favorite(self) -> None:
        """Regression guard: a favourite must survive its item being away.

        The reconcile cleared any stored descriptor whose native slot read
        empty unless it was already flagged unresolved, and only a commit
        ever set that flag. A favourite whose item left the inventory while
        its page was the native one was therefore deleted outright, which
        contradicts the sidecar's whole purpose and is also what destroyed
        the record a re-slotted item has to be recognised by.
        """
        core = strip_line_comments(self.core)
        self.assertIn("bool DescriptorItemIsCarried", core)
        carried = core.split("bool DescriptorItemIsCarried", 1)[1].split(
            "void ReconcileNativePageWithBank", 1
        )[0]
        # Powers have no inventory row and must never be judged missing.
        # The exemption is for powers, which are never in the inventory
        # and always available. Writing it as "not an inventory slot"
        # let three real items through: the Technician turret, frag
        # grenade and field kit are stored as kForm, so they counted as
        # always-present and stayed bright after leaving the inventory.
        self.assertIn("stored.visual.isPower", carried)
        self.assertNotIn("kind != FavoriteKind::kInventory", carried)
        self.assertIn("carriedForms", carried)

        reconcile = core.split("void ReconcileNativePageWithBank", 1)[1].split(
            "int ScoreStoredBank", 1
        )[0]
        # Everything up to the final clear, which is the one that deletes a
        # descriptor outright. Earlier clears belong to other branches.
        preserve = reconcile.rsplit("stored = FavoriteSlot{};", 1)[0]
        self.assertIn("!DescriptorItemIsCarried(stored, nativePage)", preserve)
        # The set is filled from the pass that already walks the inventory,
        # for every row rather than only favorited ones.
        capture = core.split("NativePage CaptureNativePage", 1)[1].split(
            "bool DescriptorItemIsCarried", 1
        )[0]
        self.assertIn("carriedForms.insert(formID)", capture)
        self.assertLess(
            capture.index("carriedForms.insert(formID)"),
            capture.index("item.unk24"),
        )

    def test_engine_reslotting_does_not_move_a_stored_favorite(self) -> None:
        """A favourite the game re-slotted must stay where the page put it.

        Script-owned items -- follower command tokens, mod toggle items --
        are removed and handed back by their own scripts with no player
        action, and the engine then drops them into whichever native slot is
        free. Adopting that rewrote the stored page, so favourites migrated
        to the far end of the wheel on their own and the move persisted.
        A deliberate reassignment is indistinguishable from the native slots
        alone, so it is gated on the pending-assignment marker instead.
        """
        core = strip_line_comments(self.core)
        reconcile = core.split("void ReconcileNativePageWithBank", 1)[1].split(
            "int ScoreStoredBank", 1
        )[0]
        detection = reconcile.split("for (std::size_t slot", 1)[0]
        self.assertIn("!g_pendingVisual.valid", detection)

        self.assertIn("ignoreNative", reconcile)
        self.assertIn("keepStored", reconcile)
        # The move is only a move when the item's own slot is now empty and
        # the slot it turned up in holds nothing of the page's own.
        self.assertIn("!nativePage.slots[home].Empty()", reconcile)
        self.assertIn("!g_banks[bank][slot].Empty()", reconcile)
        self.assertIn("SameFormIdentity(stored.form, native.form)", reconcile)
        # Both halves are left alone: the slot moved into and the real home.
        self.assertIn("ignoreNative[slot] || keepStored[slot]", reconcile)
        # It must be visible in the log, not silently corrected.
        self.assertIn("was re-slotted by the game", reconcile)
        # Detection only, no native writes from the reconcile.
        for writer in ("NativeClearSlot", "AssignDescriptorToNativeSlot"):
            self.assertNotIn(writer, reconcile)

    def test_externally_managed_slots_are_left_alone(self) -> None:
        """A slot another mod owns belongs to no page.

        Mods that let you pick a favorite slot in their own options write
        their item into it whenever they like. Capturing that put the item
        into every page the player visited, and the owning mod pulling it
        out and handing it back is what makes the engine hand out fresh
        slots and move other favourites. Such a slot has to be excluded from
        capture, from the commit's clear and write, and from removal.
        """
        core = strip_line_comments(self.core)
        self.assertIn("externallyManagedSlots", strip_line_comments(self.main))
        self.assertIn(
            "externallyManagedSlots",
            strip_line_comments((ROOT / "src" / "favorites.h").read_text(encoding="utf-8")),
        )

        # Never captured into a page; recorded once, shared by every page.
        reconcile = core.split("void ReconcileNativePageWithBank", 1)[1].split(
            "int ScoreStoredBank", 1
        )[0]
        self.assertIn("g_settings.externallyManagedSlots[slot]", reconcile)
        self.assertIn("g_pinnedSlots[slot] = native", reconcile)

        # The commit must neither clear nor rewrite it.
        commit = core.split("bool MaterializeBankLocked", 1)[1].split(
            "bool SwitchBank", 1
        )[0]
        clear = commit.split("NativeClearSlot", 1)[0]
        self.assertIn("g_settings.externallyManagedSlots[index]", clear)
        assign = commit.split("AssignDescriptorToNativeSlot", 1)[0]
        self.assertIn("g_settings.externallyManagedSlots[index]", assign)

        # It shows on every page rather than blinking in and out.
        render = core.split("FavoriteBank BuildRenderablePage", 1)[1].split(
            "void ClearSlotFromMenu", 1
        )[0]
        self.assertIn("page[slot] = g_pinnedSlots[slot]", render)

        # And the clear key may not fight the mod that owns it.
        removal = core.split("void ClearSlotAt", 1)[1].split(
            "void LogFromMenu", 1
        )[0]
        self.assertIn("g_settings.externallyManagedSlots[index]", removal)

        self.assertIn(
            "ExternallyManagedSlots",
            (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8"),
        )

    def test_grid_is_never_built_off_the_main_thread(self) -> None:
        """Regression guard for the crash the grid caused.

        The SFSE menu callback does not run on the game's main thread, and
        Scaleform is not thread safe. Creating the grid's display objects
        from there corrupted Scaleform state and took the next ordinary
        render down with it, four seconds later and in unrelated code. The
        overlay may only be built from the task queue, which runs on the
        main thread, and it looks the menu up itself so a stale pointer
        cannot be handed across the queue.
        """
        main = strip_line_comments(self.main)
        created = main.split("void OnMenuMovieCreated", 1)[1].split(
            "bool IsLoadOperation", 1
        )[0]
        self.assertIn("UpdateGridOverlay", created)
        self.assertIn("AddTask", created)
        # It must be queued, never called straight from the callback, and
        # never handed the menu pointer the callback happens to hold.
        self.assertNotIn("UpdateGridOverlay(menu)", created)
        queued = created.split("AddTask", 1)[1]
        self.assertIn("UpdateGridOverlay()", queued)

        # Icons have no switch. The fallback that ever mattered is the
        # per-cell one -- a cell whose card never arrived shows its name --
        # and a global "turn the icons off" contradicted the point of the
        # build the same way an "Enabled" switch did.
        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        self.assertNotIn("gridIcons", main)
        self.assertNotIn("gridIcons", grid)
        ini = (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8")
        self.assertNotIn("Icons=", ini)
        # Both places that draw a cell keep the per-cell fallback.
        self.assertEqual(2, grid.count("visual.imageName.empty()"))

    def test_plugin_no_longer_reimplements_use(self) -> None:
        """Starfield's own quickslot handler must perform ordinary use.

        The 0.5 reimplementation equipped everything through EquipObject,
        which silently did nothing for aid items because they are consumed
        rather than equipped. EquipObject itself must stay gone; only the
        narrow, additive UnequipObject toggle covered by
        test_toggle_unequip_is_narrow_and_additive is permitted to touch the
        equip manager.
        """
        code = strip_line_comments(self.core)
        for removed in (
            "UseStoredFavorite",
            "UsePowerForm",
            "EquipObject",
        ):
            self.assertNotIn(
                removed,
                code,
                f"{removed} still appears outside a comment",
            )
        selection = self.core.split("bool ProcessMenuSelection", 1)[1].split(
            "void CaptureNativePageVisuals", 1
        )[0]
        self.assertIn("GetPendingAssignedForm", selection)
        self.assertIn("MaterializeBankLocked", selection)

    def test_toggle_unequip_is_narrow_and_additive(self) -> None:
        """The equip-toggle must stay an addition, not a reimplementation.

        It may fire only for an inventory slot that is equippable, is not a
        power, and -- checked against the live inventory row, never the
        wheel's cached visual, which goes stale the moment the player equips
        something else on another page -- is actually equipped right now. It
        must be possible to disable, and it must never replace the vanilla
        assign/use dispatch for any other slot.
        """
        core = strip_line_comments(self.core)
        self.assertIn(
            "bool TryToggleUnequipLocked", core, "toggle helper is missing"
        )
        toggle = core.split("bool TryToggleUnequipLocked", 1)[1].split(
            "bool ProcessMenuSelection", 1
        )[0]
        self.assertIn("FavoriteKind::kInventory", toggle)
        self.assertIn("visual.isPower", toggle)
        self.assertIn("visual.isEquippable", toggle)
        self.assertIn("ResolveInventoryRow", toggle)
        self.assertIn("resolved.row->IsEquipped()", toggle)
        self.assertIn("ActorEquipManager", toggle)
        self.assertIn("UnequipObject", toggle)
        self.assertNotIn("EquipObject(", toggle.replace("UnequipObject(", ""))

        selection = core.split("bool ProcessMenuSelection", 1)[1].split(
            "void CaptureNativePageVisuals", 1
        )[0]
        self.assertIn("g_settings.toggleEquipOnSelect", selection)
        self.assertIn("!assigning", selection.split("TryToggleUnequipLocked")[0][-80:])
        self.assertIn(
            "ToggleEquipOnSelect",
            (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8"),
        )

    def test_engine_may_only_act_on_the_page_it_actually_holds(self) -> None:
        """Regression guard: clicking page 2 must not use page 1's item.

        The vanilla quickkey event carries only a slot number. Dispatching it
        while the real slots hold a different page equips that other page's
        item, which is what happened when the commit was refused.
        """
        selection = self.core.split("bool ProcessMenuSelection", 1)[1].split(
            "void CaptureNativePageVisuals", 1
        )[0]
        self.assertIn("return false;", selection)
        self.assertIn("Ignoring the selection of page", selection)

        swf_source = (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
            encoding="utf-8"
        )
        select = swf_source.split("private function SelectItem", 1)[1].split(
            "private function onFavEntryMouseover", 1
        )[0]
        # Both dispatches must sit inside the branch guarded by the DLL's answer.
        guard = select.index("FavoritesBanksNativeSelection")
        self.assertLess(guard, select.index("FavoritesMenu_AssignQuickkey"))
        self.assertLess(guard, select.index("FavoritesMenu_UseQuickkey"))
        self.assertIn("_loc2_ = this.FavoritesBanksNativeSelection", select)
        self.assertIn("if(_loc2_)", select)

    def test_bound_objects_are_not_obtained_through_As(self) -> None:
        """Regression guard for the bug that made every item unassignable.

        TESForm::As<T>() expands to an exact `GetFormType() == T::FORMTYPE`
        test, not a polymorphic cast. TESBoundObject inherits TESForm's
        SF_FORMTYPE(NONE), so As<TESBoundObject>() compares against kNONE and
        returns null for every weapon, grenade and aid item in the game. That
        single call is why no inventory favorite could ever be written to a
        native slot, and why version 0.4 emptied page 1.
        """
        code = strip_line_comments(self.core)
        self.assertNotIn("As<RE::TESBoundObject>", code)
        self.assertIn("IsBoundObject()", code)
        helper = self.core.split("TESBoundObject* AsBoundObject", 1)[1].split(
            "bool SameFormIdentity", 1
        )[0]
        self.assertIn("static_cast<RE::TESBoundObject*>", helper)

    def test_menu_close_commits_the_shown_page(self) -> None:
        self.assertIn("QueueCommitActiveBank", self.main)
        closing = self.main.split("EqualsMenuName(event.menuName, \"FavoritesMenu\")", 1)[1]
        self.assertIn("QueueCommitActiveBank", closing.split("else if", 1)[0])

    def test_empty_wheel_positions_report_an_invalid_fixture(self) -> None:
        """FT_INTERNAL is 0 and FT_INVALID is -1 in Components.ImageFixture.

        A default of 0 made every empty position stream an unnamed bitmap
        into the shared FavoritesIconBuffer, which corrupted the icons that
        were really there.
        """
        header = (ROOT / "src" / "favorites.h").read_text(encoding="utf-8")
        self.assertIn("kInvalidFixtureType = -1", header)
        self.assertIn("fixtureType{ kInvalidFixtureType }", header)
        self.assertNotIn("fixtureType{ 0 }", header)
        self.assertIn("BuildEmptyFavoriteInfo", self.ui)
        entry = (ROOT / "swf-src" / "scripts" / "FavoritesEntry.as").read_text(
            encoding="utf-8"
        )
        invalid_branch = entry.split("FT_INVALID", 1)[1].split("else", 1)[0]
        self.assertIn('clipSizer = ""', invalid_branch)
        self.assertIn("centerClip = false", invalid_branch)

    def test_real_game_events_replace_disabled_sfse_save_messages(self) -> None:
        for event_name in (
            "RE::SaveLoadEvent",
            "RE::TESLoadGameEvent",
            "FavoriteChangedEvent",
            "RE::MenuOpenCloseEvent",
        ):
            self.assertIn(event_name, self.main)
        self.assertNotIn("kPreSaveGameMessage", self.main)
        self.assertNotIn("kPostLoadGameMessage", self.main)

    def test_ui_uses_a_scaleform_virtual_page_bridge(self) -> None:
        self.assertIn('"FavoritesBanksNativeSelection"', self.ui)
        self.assertIn('"FavoritesBanksRenderPage"', self.ui)
        self.assertIn("BuildFavoriteInfo", self.ui)
        swf_source = (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
            encoding="utf-8"
        )
        self.assertIn("FavoritesBanksRenderPage", swf_source)
        self.assertIn("FavoritesBanksNativeSelection", swf_source)
        # The wheel no longer decides for itself whether a page is native;
        # the DLL tells it, because "page 1" and "the page in the real slots"
        # are no longer the same thing.
        self.assertNotIn("FavoritesBanksActiveBank == 0", swf_source)
        self.assertIn("FavoritesBanksNativePage", swf_source)

    def test_native_mutation_marks_itself_in_progress(self) -> None:
        """g_switchInProgress was read three times and set nowhere.

        Committing a page runs up to twelve native clears and twelve native
        assigns, and the engine emits a FavoriteChangedEvent for each one.
        The three guards that were supposed to stop those self-inflicted
        events from queueing captures of half-written state were all dead
        because nothing ever stored true into the flag.
        """
        core = strip_line_comments(self.core)
        self.assertIn("g_switchInProgress.store(true", core)
        self.assertIn("g_switchInProgress.store(false", core)
        self.assertIn("NativeMutationGuard", core)
        # The guard has to cover the native writes, not just be declared.
        commit = core.split("bool MaterializeBankLocked", 1)[1]
        commit = commit.split("bool SwitchBank", 1)[0]
        self.assertIn("NativeMutationGuard mutating", commit)

    def test_render_keeps_the_starting_selection(self) -> None:
        """The wheel opens with the slot the D-pad direction chose.

        The engine sends it as uStartingSelection and onDataUpdate applies it.
        Clearing selectedIndex on every render threw that away, so opening the
        wheel with a D-pad direction stopped selecting anything in that
        direction and needed a second press.
        """
        swf_source = (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
            encoding="utf-8"
        )
        self.assertIn("uStartingSelection", swf_source)
        render = swf_source.split(
            "public function FavoritesBanksRenderPage", 1
        )[1].split("public function isAssigningItem", 1)[0]
        render = strip_line_comments(render)
        self.assertIn("selectedIndex = FS_NONE", render)
        # It may only clear when the page really changed.
        self.assertIn("_RenderedBank", render)
        cleared = render.split("selectedIndex = FS_NONE", 1)[0]
        self.assertIn("this._RenderedBank != param2", cleared)

    def test_per_save_state_uses_the_save_being_loaded(self) -> None:
        """Every save must restore its own pages.

        The snapshot lookup used to key on mostRecentSaveGame, which names the
        newest save on disk so Continue can find it. Loading a deliberately
        older save therefore looked up the newest save's snapshot, so all
        saves of one character behaved as if they shared one set of pages.
        queuedEntryToLoad is the entry the player actually chose.
        """
        core = strip_line_comments(self.core)
        self.assertIn("queuedEntryToLoad", core)
        self.assertIn("g_incomingSaveName", core)
        self.assertIn("NoteIncomingSave", core)
        # Quickload never goes through the queued entry.
        self.assertIn("quickSaveFileName", core)
        load = core.split("bool LoadBestStateLocked", 1)[1].split(
            "return false;", 1
        )[0]
        self.assertIn("g_incomingSaveName", load)
        # The hook has to run before the load tears the session down.
        main = strip_line_comments(self.main)
        self.assertLess(
            main.index("NoteIncomingSave"), main.index("BeginLoadTransition")
        )
        # Unbounded snapshots would fill the folder: Starfield autosaves a lot.
        self.assertIn("PruneSnapshotsLocked", core)

    def test_page_change_forces_icons_to_attach_again(self) -> None:
        """Regression guard for "the page is empty until you reopen it".

        Skipping an identical icon key stops a page change from restarting
        every asynchronous load, and that has to stay for repeated renders of
        the same page. Across a real page change it is wrong: an entry whose
        symbol lost its attach race still holds that icon's key, so nothing
        would ever ask for it again and only closing and reopening the wheel
        brought it back.
        """
        entry = (ROOT / "swf-src" / "scripts" / "FavoritesEntry.as").read_text(
            encoding="utf-8"
        )
        menu = (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
            encoding="utf-8"
        )
        load = strip_line_comments(
            entry.split("public function LoadIcon", 1)[1]
        )
        # The early return survives, but only when no reload was forced.
        self.assertIn("!param2 && _loc3_ == this._LoadedKey", load)
        render = strip_line_comments(
            menu.split("public function FavoritesBanksRenderPage", 1)[1].split(
                "public function isAssigningItem", 1
            )[0]
        )
        self.assertIn("RefreshFavoriteEntries(_loc4_)", render)

    def test_emptied_slot_drops_its_icon(self) -> None:
        """A slot that goes from occupied to empty must lose its picture.

        The pending item was staged and then applied only when it was not
        null, so clearing a slot never reached ApplyIcon and the entry kept
        showing the item that used to be there.
        """
        entry = strip_line_comments(
            (ROOT / "swf-src" / "scripts" / "FavoritesEntry.as").read_text(
                encoding="utf-8"
            )
        )
        self.assertIn("_HasPendingItem", entry)
        frame = entry.split("private function onIconLoadFrame", 1)[1]
        self.assertIn("if(this._HasPendingItem)", frame)
        self.assertNotIn("if(this._PendingItem != null)", frame)

    def test_one_playthrough_cannot_overwrite_another(self) -> None:
        """State is per character, and must never be written under a guess.

        The session used to initialise while the character ID was still zero,
        which happens on the main menu: it adopted whatever the twelve native
        slots held as page 1 and marked itself ready. The next write then
        re-read the character, picked up whichever playthrough had meanwhile
        become current, and saved those adopted pages over that character's
        real file. Two campaigns keep separate directories only if neither of
        those two steps can happen.
        """
        core = strip_line_comments(self.core)
        init = core.split("bool InitializeSessionIfNeeded", 1)[1]
        init = init.split("NativeFavoritesReady", 1)[0]
        self.assertIn("currentCharacterID == 0", init)
        self.assertIn("return false;", init)
        # A save must refuse rather than adopt a character that appeared later.
        save = core.split("bool SaveCurrentStateLocked", 1)[1]
        save = save.split("bool LoadBestStateLocked", 1)[0]
        self.assertIn("g_characterID == 0", save)
        self.assertNotIn("g_characterID = ReadCharacterID();", save)
        # State lives under a per-character directory keyed on the player ID.
        self.assertIn("GetCharacterStateDirectory", core)
        self.assertIn("currentPlayerID", core)

    def test_a_slot_can_be_emptied(self) -> None:
        """Starfield cannot remove a favorite, only overwrite one.

        Its single slot-clearing routine is called from exactly one place,
        inside the assign routine, to make room before a write. So the wheel
        has to offer removal itself, on the page being drawn, and it has to
        work for an item that is no longer in the inventory too - that case
        is precisely the one the reconcile deliberately keeps.
        """
        core = strip_line_comments(self.core)
        self.assertIn("void ClearSlotAt", core)
        clear = core.split("void ClearSlotAt", 1)[1].split(
            "void LogFromMenu", 1
        )[0]
        # It clears the stored descriptor...
        self.assertIn("g_banks[bank][index] = FavoriteSlot{}", clear)
        # ...and the native slot too, but only when this page owns them.
        self.assertIn("bank == g_nativeBank", clear)
        self.assertIn("NativeClearSlot", clear)
        self.assertIn("NativeMutationGuard", clear)
        # The wheel may only clear the page it is actually showing - but
        # the grid shows every page at once, so its delete corner has to be
        # allowed to empty a slot on any of them. Refusing that silently is
        # what made the corner do nothing on three rows out of four.
        self.assertIn("onlyActiveBank && bank != g_activeBank", clear)
        self.assertIn("void ClearSlotFromMenu", core)
        self.assertIn("void ClearGridSlot", core)
        self.assertIn("ClearSlotAt(globalIndex, true)", core)
        self.assertIn("ClearSlotAt(globalIndex, false)", core)
        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        self.assertNotIn("ClearSlotFromMenu", grid)

        ui = strip_line_comments(self.ui)
        self.assertIn("ClearSlotHandler", ui)
        self.assertIn("FavoritesBanksClearSlot", ui)
        menu = strip_line_comments(
            (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
                encoding="utf-8"
            )
        )
        self.assertIn("FavoritesBanksClearSlotKey", menu)
        self.assertIn("this.selectedIndex != FS_NONE", menu)
        self.assertIn(
            "ClearSlotKey=DELETE",
            (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8"),
        )

    def test_wheel_always_dispatches_the_vanilla_quickkey_event(self) -> None:
        swf_source = (ROOT / "swf-src" / "scripts" / "FavoritesMenu.as").read_text(
            encoding="utf-8"
        )
        select = swf_source.split("private function SelectItem", 1)[1].split(
            "private function onFavEntryMouseover", 1
        )[0]
        self.assertIn("FavoritesMenu_AssignQuickkey", select)
        self.assertIn("FavoritesMenu_UseQuickkey", select)
        # Neither dispatch may be gated on which page is showing, or items on
        # every page but the native one stop responding.
        self.assertNotIn("FavoritesBanksActiveBank == 0", select)

    def test_every_documented_ini_key_is_actually_read(self) -> None:
        """A setting nobody reads is a setting that silently does nothing.

        Removing the hotkey buttons once took the surrounding block of
        LoadSettings with it, and two thirds of the file stopped being read
        at all: ExternallyManagedSlots no longer pinned its slot, and the
        mouse wheel and shoulder buttons kept working after being switched
        off. Nothing failed, nothing warned - the values simply were their
        defaults. This walks the shipped INI and insists on a reader.
        """
        ini = (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8")
        documented = {
            line.split("=", 1)[0].strip()
            for line in ini.splitlines()
            if "=" in line and not line.lstrip().startswith(";")
        }
        self.assertIn("ExternallyManagedSlots", documented)
        source = strip_line_comments(
            (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        )
        # The eight page keys are read in a loop, by a built-up name.
        numbered = re.compile(r"^Bank[1-8]Key$")
        looped = 'L"Bank%zuKey"' in source
        for key in sorted(documented):
            if numbered.match(key) and looped:
                continue
            with self.subTest(key=key):
                self.assertIn(
                    f'L"{key}"',
                    source,
                    f"{key} is documented in the INI but nothing reads it",
                )


    def test_the_grid_draws_only_from_the_movies_own_frame_tick(self) -> None:
        """Scaleform is not thread-safe, and we do not pick the thread.

        Every entry point into the grid used to draw where the caller stood:
        an SFSE task, a menu callback, a click. The log shows those arriving
        on a different thread almost every time, and the crashes followed -
        access violations inside the AS3 VM, once with the program counter
        pointing at the bytes of a string rather than at code. The draw now
        happens on an enterFrame listener, which the engine dispatches from
        inside its own advance of that movie. Everyone else raises a flag.
        """
        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("class GridTicker", grid)
        self.assertIn('"enterFrame"', grid)

        request = grid.split("void UpdateGridOverlay(", 1)[1].split(
            "void ResetGridSession", 1
        )[0]
        self.assertIn("g_redrawRequested.store(true)", request)
        # It may still draw when no tick has ever arrived - a grid that
        # cannot tick must not be a grid that never appears - but it may not
        # draw anything itself.
        self.assertIn("if (!g_tickerAlive.load())", request)
        for drawing in ("DrawRect(", "EnsureEntry(", "EnsureLabel("):
            with self.subTest(call=drawing):
                self.assertNotIn(drawing, request)


    def test_publishing_a_page_does_not_read_the_old_one_back_over_it(self) -> None:
        """An edit to the live page must survive being published.

        MaterializeBankLocked begins by taking back whatever the real slots
        hold, which is right when moving between pages: the outgoing page
        owns that arrangement. Applied to the page being written it is
        exactly wrong. That call is only reached with force, which means an
        edit has just changed the stored page and wants it pushed out - and
        the real slots still hold what came before the edit. Reading them
        back in overwrote the edit, and the old order was then written out
        again and reported as a success. Swapping two favorites on the page
        that was live looked like nothing happening at all.
        """
        core = strip_line_comments(self.core)
        materialize = core.split("bool MaterializeBankLocked", 1)[1].split(
            "void CaptureCurrentBankLocked", 1
        )[0]
        self.assertIn("if (targetBank != previousBank) {", materialize)
        guarded = materialize.split("if (targetBank != previousBank) {", 1)[1]
        self.assertIn("ReconcileNativePageWithBank(previousBank", guarded)
        # And nowhere outside that guard.
        before = materialize.split("if (targetBank != previousBank) {", 1)[0]
        self.assertNotIn("ReconcileNativePageWithBank", before)


    def test_a_save_the_plugin_has_named_never_inherits_another_saves_pages(self) -> None:
        """Per-save state must not leak between saves.

        Pages live beside the save, not in it, so they do not rewind when the
        player loads an older save. Reading the character's shared state for
        any save that lacks a snapshot is what showed a player the favorites
        of the run they had just left. But the shared file records the save
        it was written for, so the two cases can be told apart: a matching
        name is that save's own state, a different one is the leak.
        """
        core = strip_line_comments(self.core)
        load = core.split("bool LoadBestStateLocked", 1)[1].split(
            "std::size_t CountNativeForms", 1
        )[0]
        # The name is only trusted when it came from the player's own choice.
        self.assertIn("identified = !g_incomingSaveName.empty();", load)
        # The shared file is only adopted unnamed, or when it names this save.
        self.assertIn(
            "if (!identified || storedSaveName == g_loadedSaveName) {", load)
        # A mismatch must undo the read: ReadStateFile has already filled the
        # pages in, and leaving them there is exactly the leak.
        mismatch = load.split(
            "if (!identified || storedSaveName == g_loadedSaveName) {", 1)[1]
        mismatch = mismatch.split("if (identified) {", 1)[0]
        self.assertIn("g_banks = FavoriteBanks{};", mismatch)
        # And a named save still stops before the legacy fallbacks.
        guarded, rest = load.split("if (identified) {", 1)
        self.assertIn("return false;", rest.split("GetLegacyStateRoot", 1)[0])
        self.assertNotIn("GetLegacyStateRoot", guarded)

    def test_the_shared_state_is_stamped_with_the_save_being_played(self) -> None:
        """The stamp the load side trusts must name this session's save.

        Reading the newest save on disk is only right in the moment the game
        has just written one. Load an older save and play without saving, and
        that name belongs to a save this session has nothing to do with -
        stamping current.fbs with it would hand these pages to that save on
        its next load, which is the leak the load side now relies on the
        stamp to prevent.
        """
        core = strip_line_comments(self.core)
        save = core.split("bool SaveCurrentStateLocked", 1)[1].split(
            "bool LoadBestStateLocked", 1
        )[0]
        self.assertIn("g_loadedSaveName.empty() ?", save)
        self.assertIn("recent : g_loadedSaveName;", save)
        # A new save is noticed by mostRecentSaveGame moving away from what it
        # was when the session began - it only moves when the game writes a
        # save. Comparing it against g_loadedSaveName instead would misfire
        # the moment an older save is loaded, which is most of the point.
        self.assertIn("recent != g_recentSaveAtSessionStart", save)
        # Not a new save, merely the value settling onto the one just loaded.
        self.assertIn("recent != g_loadedSaveName", save)
        self.assertIn("g_recentSaveAtSessionStart = recent;", save)

    def test_a_finished_save_is_recognised_by_its_operation(self) -> None:
        """Status 1 means "succeeded", not "a load succeeded".

        Measured in game: a quicksave emits opType=3 status=0 then
        opType=3 status=1, and status 4 (kSaveCompleted) never arrives at all.
        Branching on the status alone therefore re-ran session initialisation
        after every quicksave and never noticed the save. The operation is
        what separates the two, and the exit-save operations are the last
        chance to record state before the game closes.
        """
        main = strip_line_comments(self.main)
        self.assertIn("bool IsSaveOperation", main)
        for op in ("kAutosave", "kQuicksave", "kManualSave",
                   "kExitSaveToMainMenu", "kExitSaveToDesktop"):
            with self.subTest(op=op):
                self.assertIn(op, main)
        branch = main.split("Status::kLoadSucceeded", 1)[1]
        branch = branch.split("kSaveCompleted", 1)[0]
        self.assertIn("IsSaveOperation(event.opType)", branch)
        self.assertIn("QueueCaptureCurrentState(true)", branch)
        self.assertIn("QueueLoadedGameInitialization()", branch)

    def test_the_d_pad_is_rewritten_rather_than_intercepted(self) -> None:
        """The movie ships with this mod, so the keys are redefined in it.

        The earlier plan reached for the shoulder buttons and XInput because
        the vanilla D-pad writes selectedIndex itself and that reaction would
        have had to be suppressed. It does not have to be: up and down change
        the row, left and right step through the slots, and both branches
        stand down when the callback is absent so a new movie with an old DLL
        keeps its vanilla meaning.
        """
        as_source = (
            ROOT / "swf-src" / "scripts" / "FavoritesMenu.as"
        ).read_text(encoding="utf-8", errors="replace")
        self.assertIn("if(this.FavoritesBanksSwitchRow != null)", as_source)
        self.assertIn("this.FavoritesBanksSwitchRow(-1);", as_source)
        self.assertIn("this.FavoritesBanksSwitchRow(1);", as_source)
        self.assertIn("FavoritesBanksStepSlot", as_source)
        # The direction that opens the wheel does NOT arrive here -- proven
        # by measurement: swallowing a first press cost the player an extra
        # one before anything moved. The selection is put on slot 1 from the
        # C++ side instead, once per opening, when the overlay is built.
        self.assertNotIn("FavoritesBanksFreshOpen", as_source)
        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        fresh = grid.split("freshlyOpened = true;", 1)[1].split("geometry", 1)[0]
        self.assertIn('"selectedIndex"', fresh)
        # A row change keeps the slot; the wheel used to blank the selection.
        self.assertIn("_loc5_ != FS_NONE", as_source)
        # Ends of a row: walls or doors, and that one is the player's call.
        self.assertIn("FavoritesBanksWrapNavigation", as_source)

        ui = strip_line_comments(
            (ROOT / "src" / "favorites_ui.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("class SwitchRowHandler", ui)
        self.assertIn('"FavoritesBanksSwitchRow"', ui)
        self.assertIn('"FavoritesBanksWrapNavigation"', ui)
        self.assertIn("g_settings.wrapNavigation", ui)
        ini = (ROOT / "FavoritesMenuGrid.ini").read_text(encoding="utf-8")
        self.assertIn("WrapNavigation=", ini)

    def test_the_grid_shows_the_wheel_s_own_selection(self) -> None:
        """Controller support starts with seeing where you are.

        FavoritesMenu.as exposes selectedIndex as a public property, so the
        grid reads it instead of intercepting input - which is what such
        attempts normally founder on. Measured in game: the vanilla D-pad
        walks slots 1-6 left/right and 7-12 up/down, writing that same
        property. FS_NONE is 12 and means nothing is selected.
        """
        as_source = (
            ROOT / "swf-src" / "scripts" / "FavoritesMenu.as"
        ).read_text(encoding="utf-8", errors="replace")
        self.assertIn("public function get selectedIndex()", as_source)
        self.assertIn("public function set selectedIndex(", as_source)
        self.assertIn("FS_NONE:uint = 12", as_source)

        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("kNoSelection = 12", grid)
        self.assertIn('menu->menuObj, "selectedIndex"', grid)
        # Its own marker: the existing highlight follows the mouse and is
        # driven by cursor events, which a controller never sends.
        self.assertIn("void MoveSelection(", grid)
        self.assertIn('"GridSelection"', grid)
        # Shown on the active row, since that is the page the wheel draws.
        draw = grid.split("const auto selected = ReadSelectedIndex(menu);", 1)[1]
        draw = draw.split("const auto extrasX", 1)[0]
        self.assertIn("activeBank", draw)
        self.assertIn("geometry.pageSlots", draw)
        # The D-pad writes selectedIndex straight into the movie and asks for
        # nothing, so the ticker has to watch it or the marker only ever
        # shows where the selection was when the overlay was last drawn.
        ticker = grid.split("class GridTicker", 1)[1].split("};", 1)[0]
        self.assertIn("ReadSelectedIndex", ticker)
        self.assertIn("g_selectedIndex.exchange(selected)", ticker)
        self.assertIn("g_redrawRequested.store(true);", ticker)
        # Forgotten with the overlay, or reopening on the same slot would not
        # count as a change and the marker would stay hidden.
        build = grid.split("bool BuildOverlay", 1)[1]
        self.assertIn("g_selectedIndex.store(-1);", build)

    def test_committing_a_page_cannot_clear_the_carried_flag(self) -> None:
        """One flag must not carry two meanings.

        The carried test asks whether the item is in the inventory; the
        commit asks whether it could be written into a real slot. Those
        disagree exactly when an item has left the inventory while its
        favourite stays assignable, and the commit was clearing the flag
        there - so a correctly faint cell went bright the moment the wheel
        closed, which looked like saving had done it.
        """
        core = strip_line_comments(self.core)
        commit = core.split("const auto success = AssignDescriptorToNativeSlot",
                            1)[1]
        commit = commit.split("std::size_t MigrateLegacyVirtualFavorites", 1)[0]
        commit = commit.split("}", 40)[0] if False else commit[:1200]
        # Raised on failure, never lowered.
        self.assertNotIn("descriptor.unresolved = !success;", commit)
        self.assertIn("descriptor.unresolved = true;", commit)
        # And the carried test still gets the last word on the page.
        self.assertIn("DescriptorItemIsCarried(descriptor, committed)", commit)

    def test_the_quantity_comes_from_the_inventory(self) -> None:
        """A consumable used with the menu shut must still count down.

        The bracketed quantity is fed into the vanilla ItemInfo_mc from the
        stored visual, and that visual is only ever refreshed from the
        wheel's own item cards - which are not redrawn while the wheel is
        hidden. So the number froze at whatever it was when the card was last
        seen. The inventory knows the real figure without any card: the
        item's stacks carry it.
        """
        core = strip_line_comments(self.core)
        self.assertIn("carriedCounts", core)
        # Summed over the stacks, not the number of entries.
        self.assertIn("for (const auto& stack : item.stacks)", core)
        self.assertIn("stacked += stack.count;", core)
        # And applied wherever the carried flag is brought up to date, which
        # is what runs when the menu opens.
        refresh = core.split("void RefreshCarriedFlagsLocked", 1)[1].split(
            "void CaptureCurrentBankLocked", 1
        )[0]
        self.assertIn("descriptor.visual.count = found->second;", refresh)
        self.assertIn("carriedCounts.find", refresh)

    def test_a_page_change_leaves_the_faint_flags_correct(self) -> None:
        """Every point that rewrites the rows re-asks the inventory.

        The flag went wrong in a different place each time it was chased: the
        commit cleared it, then the reconcile did, then a page change did.
        Trusting each writer to leave it alone does not hold, so the rule is
        applied at the end of the rewrite instead. Assigning favorites does
        not change what the player carries, so the page read before the
        writes still describes the inventory.
        """
        core = strip_line_comments(self.core)
        materialize = core.split("bool MaterializeBankLocked", 1)[1].split(
            "void RefreshCarriedFlagsLocked", 1
        )[0]
        self.assertIn("RefreshCarriedFlagsLocked(committed);", materialize)
        # Declared before use, since the definition sits further down.
        self.assertIn(
            "void RefreshCarriedFlagsLocked(const NativePage& nativePage);",
            core)

    def test_a_capture_keeps_the_faint_flag_honest(self) -> None:
        """Saving must not make an item the player lost look carried.

        The faint flag was only ever set after a load, while every capture
        ran a reconcile against the native slots - and those keep holding an
        item that is no longer in the inventory. So a slot correctly drawn
        faint went bright again for no better reason than the game having
        been saved.
        """
        core = strip_line_comments(self.core)
        capture = core.split("void CaptureCurrentBankLocked", 1)[1].split(
            "void RefreshCarriedFlags(", 1
        )[0]
        self.assertIn("RefreshCarriedFlagsLocked(nativePage);", capture)
        # And only after the reconcile, which is what clears the flag.
        reconcile = capture.index("ReconcileNativePageWithBank")
        self.assertLess(reconcile, capture.index("RefreshCarriedFlagsLocked"))

    def test_the_per_save_file_does_not_depend_on_a_save_event(self) -> None:
        """The per-save file is the primary store, so it cannot be rare.

        Hanging it on RE::SaveLoadEvent::Status::kSaveCompleted made a whole
        design depend on one event, and that event never arrived: across a
        full play session the log carried neither the success line nor its
        warning, and not one snapshot existed on disk. Every load then fell
        back to the shared file, which by then named a different save. So the
        file is written wherever state is written, and the save event is at
        most an early nudge.
        """
        core = strip_line_comments(self.core)
        save = core.split("bool SaveCurrentStateLocked", 1)[1].split(
            "bool LoadBestStateLocked", 1
        )[0]
        # Writing it must not sit behind the parameter any more.
        self.assertNotIn("if (saveSnapshot) {", save)
        self.assertIn("GetSnapshotStatePath(g_characterID, saveName)", save)
        # And the baseline has to be re-armed per session, or a stale one
        # would announce a new save on the first capture after every load.
        # Armed at the first capture, not while loading: mostRecentSaveGame
        # moves on load too and has not settled on the incoming save yet.
        save = core.split("bool SaveCurrentStateLocked", 1)[1].split(
            "bool LoadBestStateLocked", 1)[0]
        self.assertIn("if (g_recentSaveAtSessionStart.empty()) {", save)
        load = core.split("bool LoadBestStateLocked", 1)[1].split(
            "std::size_t CountNativeForms", 1)[0]
        self.assertNotIn("g_recentSaveAtSessionStart", load)
        reset = core.split("void BeginLoadTransition", 1)[1]
        self.assertIn("g_recentSaveAtSessionStart.clear();", reset)

    def test_a_card_without_an_icon_does_not_erase_one(self) -> None:
        """An item the player is not carrying still has an icon.

        The wheel hands over a card for such an item with its name but no
        iconImage. Assigning that wholesale dropped the icon captured while
        the item was carried, and the harvest writes state, so the cell
        stayed blank for good - one look at the wheel without the item was
        enough to lose it permanently.
        """
        core = strip_line_comments(self.core)
        harvest = core.split("bool HarvestMenuVisuals", 1)[-1]
        if "auto merged = harvested[index];" not in harvest:
            harvest = core.split("harvested[index].HasData()", 1)[1]
        harvest = harvest.split("FavoriteBank BuildRenderablePage", 1)[0]
        # Nothing is assigned straight from the card any more.
        self.assertNotIn("slot.visual = harvested[index];", harvest)
        self.assertIn("auto merged = harvested[index];", harvest)
        # Icon fields survive a card that carries none of them.
        self.assertIn("merged.fixtureType == kInvalidFixtureType", harvest)
        self.assertIn("merged.imageName.empty()", harvest)
        self.assertIn("merged.fixtureType = slot.visual.fixtureType;", harvest)
        self.assertIn("merged.imageName = slot.visual.imageName;", harvest)
        # The skip-if-unchanged test has to compare the merged result, or a
        # recovered icon would be compared away and never stored.
        self.assertIn("slot.visual.imageName == merged.imageName", harvest)

    def test_a_first_seen_save_lands_on_the_default_page(self) -> None:
        """Adopted favorites belong where the quickkeys point.

        The default page is restored into the real slots on every close, so
        it is the one the gameplay quickkeys mean. Favorites the mod is
        meeting for the first time have to land there, not on whichever page
        happened to score best against them - that is what once dropped a
        player's pre-install favorites onto page 4.
        """
        core = strip_line_comments(self.core)
        init = core.split("bool InitializeSessionIfNeeded", 1)[1].split(
            "bool SwitchBank", 1
        )[0]
        adopt = init.split("} else {", 1)[1]
        self.assertIn("g_settings.defaultRow - 1", adopt)
        self.assertIn("g_banks[g_nativeBank] = nativePage.slots;", adopt)
        self.assertNotIn("g_nativeBank = 0;", adopt)

    def test_a_cell_without_an_icon_still_says_what_it_holds(self) -> None:
        """An occupied cell must never look empty.

        Icons come from the wheel's own item cards, which only arrive when
        favorites change while the menu is open. A save the mod has just
        adopted has form IDs and no cards, and the icon path drew nothing at
        all for those cells - occupied, selectable, and apparently blank.
        The name is always available, so it stands in per cell.
        """
        grid = strip_line_comments(
            (ROOT / "src" / "favorites_grid.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("const auto drawIcon = banks[row][slot].Empty() ||", grid)
        self.assertIn("!banks[row][slot].visual.imageName.empty();", grid)
        self.assertIn("if (drawIcon) {", grid)
        # The reserved-slot strip draws cells too, and needs the same rule.
        self.assertIn("const auto drawPinIcon", grid)
        self.assertIn("if (drawPinIcon) {", grid)


if __name__ == "__main__":
    unittest.main(verbosity=2)
