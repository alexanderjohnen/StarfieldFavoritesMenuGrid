"""Rebuild favoritesmenu.swf from swf-src/scripts.

The SWF ships with this mod, so what the D-pad *means* is changed in
ActionScript rather than by intercepting input. FFDec does the compiling; a
round trip with unchanged sources was verified to decompile back byte for
byte, so the tool itself does not alter the movie.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FFDEC = Path(r"C:\Program Files (x86)\FFDec\ffdec-cli.exe")
PACKAGE = ROOT.parent / "FavoriteBanksCompiled" / "Interface" / "favoritesmenu.swf"
EDITED = ROOT / "swf-src" / "scripts"


def main() -> int:
    if not FFDEC.is_file():
        print(f"FFDec fehlt: {FFDEC}")
        return 1
    if not PACKAGE.is_file():
        print(f"SWF fehlt: {PACKAGE}")
        return 1

    with tempfile.TemporaryDirectory() as raw:
        work = Path(raw)
        exported = work / "scripts"
        subprocess.run(
            [str(FFDEC), "-export", "script", str(exported), str(PACKAGE)],
            check=True, capture_output=True)

        # Only the files we actually maintain are swapped in; everything else
        # goes back exactly as it came out.
        replaced = []
        for source in sorted(EDITED.glob("*.as")):
            target = exported / "scripts" / source.name
            if not target.is_file():
                print(f"unbekanntes Skript, uebersprungen: {source.name}")
                continue
            shutil.copyfile(source, target)
            replaced.append(source.name)

        backup = PACKAGE.with_suffix(".swf.bak")
        shutil.copyfile(PACKAGE, backup)
        built = work / "built.swf"
        subprocess.run(
            [str(FFDEC), "-importScript", str(PACKAGE), str(built),
             str(exported)],
            check=True, capture_output=True)
        shutil.copyfile(built, PACKAGE)

    print(f"SWF neu gebaut, ersetzt: {', '.join(replaced)}")
    print(f"Sicherung: {backup.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
