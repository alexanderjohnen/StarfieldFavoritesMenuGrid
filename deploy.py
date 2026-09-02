"""Copy a built Favorites Menu Grid straight into the game.

Skips the archive/mod-manager round trip while the mod is being worked on.

The INI is never overwritten once it exists, because it holds the player's
own settings. Keys a newer build introduced are inserted into their proper
section with their default and their explanatory comment, so a new option
appears without resetting anything. Windows reads INIs strictly by section,
so appending stray keys at the end of the file would silently do nothing --
placement matters.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILT_DLL = ROOT / "build" / "windows" / "x64" / "releasedbg" / "FavoritesMenuGrid.dll"
SWF = ROOT.parent / "FavoriteBanksCompiled" / "Interface" / "favoritesmenu.swf"
REFERENCE_INI = ROOT / "FavoritesMenuGrid.ini"

DEFAULT_DATA = Path(
    os.environ.get(
        "STARFIELD_DATA",
        r"G:\Program Files (x86)\Steam\steamapps\common\Starfield\Data",
    )
)


def parse_ini(text: str) -> dict[str, dict[str, str]]:
    """Section -> key -> value. Comments and order are not needed here."""
    sections: dict[str, dict[str, str]] = {}
    current = ""
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1]
            sections.setdefault(current, {})
        elif "=" in stripped and not stripped.startswith(";"):
            key = stripped.split("=", 1)[0].strip()
            sections.setdefault(current, {})[key.lower()] = stripped
    return sections


def blocks_for(text: str) -> dict[str, dict[str, list[str]]]:
    """Section -> key -> the key's line plus the comment lines above it."""
    blocks: dict[str, dict[str, list[str]]] = {}
    current = ""
    pending: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = stripped[1:-1]
            blocks.setdefault(current, {})
            pending = []
        elif stripped.startswith(";"):
            pending.append(line)
        elif "=" in stripped:
            key = stripped.split("=", 1)[0].strip().lower()
            blocks.setdefault(current, {})[key] = pending + [line]
            pending = []
        else:
            pending = []
    return blocks


def merge_ini(reference: str, existing: str) -> tuple[str, list[str]]:
    """Insert keys the target lacks, each into its own section."""
    have = parse_ini(existing)
    want = blocks_for(reference)

    lines = existing.splitlines()
    added: list[str] = []

    # Walk sections back to front so earlier insertion points stay valid.
    for section in reversed(list(want)):
        missing = [
            block
            for key, block in want[section].items()
            if key not in have.get(section, {})
        ]
        if not missing:
            continue
        flat = [line for block in missing for line in block]
        added.extend(
            block[-1] for block in missing
        )

        if section not in have:
            lines.extend(["", f"[{section}]", *flat])
            continue

        # Find the section, then its last line, and insert there.
        start = next(
            index
            for index, line in enumerate(lines)
            if line.strip() == f"[{section}]"
        )
        end = len(lines)
        for index in range(start + 1, len(lines)):
            stripped = lines[index].strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                end = index
                break
        while end > start + 1 and not lines[end - 1].strip():
            end -= 1
        lines[end:end] = ["", *flat]

    return "\n".join(lines) + "\n", added


def main() -> int:
    data = DEFAULT_DATA
    if len(sys.argv) > 1:
        data = Path(sys.argv[1])
    if not data.is_dir():
        print(f"FEHLER: Data-Ordner nicht gefunden: {data}")
        return 1
    if not BUILT_DLL.is_file():
        print("FEHLER: Kein Build vorhanden. Erst 'xmake' laufen lassen.")
        return 1

    plugins = data / "SFSE" / "Plugins"
    interface = data / "Interface"
    plugins.mkdir(parents=True, exist_ok=True)
    interface.mkdir(parents=True, exist_ok=True)

    try:
        shutil.copy2(BUILT_DLL, plugins / "FavoritesMenuGrid.dll")
    except PermissionError:
        print("FEHLER: DLL ist gesperrt - laeuft Starfield noch?")
        return 1
    print(f"DLL  -> {(plugins / 'FavoritesMenuGrid.dll').stat().st_size} Bytes")

    # The UI patch. Without it the wheel cannot call back into the plugin,
    # so pages draw but nothing on them can be selected.
    target_swf = interface / "favoritesmenu.swf"
    if not SWF.is_file():
        print(f"SWF  -> WARNUNG: {SWF} fehlt")
    elif not target_swf.is_file() or target_swf.read_bytes() != SWF.read_bytes():
        shutil.copy2(SWF, target_swf)
        print("SWF  -> aktualisiert")
    else:
        print("SWF  -> unveraendert")

    target_ini = plugins / "FavoritesMenuGrid.ini"
    reference = REFERENCE_INI.read_text(encoding="utf-8")
    if not target_ini.is_file():
        shutil.copy2(REFERENCE_INI, target_ini)
        print("INI  -> neu angelegt")
    else:
        merged, added = merge_ini(reference, target_ini.read_text(encoding="utf-8"))
        if added:
            target_ini.write_text(merged, encoding="utf-8")
            print("INI  -> ergaenzt (bestehende Werte unangetastet):")
            for line in added:
                print(f"          {line}")
        else:
            print("INI  -> vollstaendig, unveraendert")

    print("Fertig.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
