# Third-party source and licenses

Favorites Menu Grid is a fork of Favorites Banks by Sator
(https://www.nexusmods.com/starfield/mods/17906), whose source is licensed
GPL-3.0-or-later with the exceptions in EXCEPTIONS. This build was made
against:

- CommonLibSF commit `b5817cfbc2459661760e9b03ada702aa706369a8`
  - Source: https://github.com/libxse/CommonLibSF/tree/b5817cfbc2459661760e9b03ada702aa706369a8
  - License: GPL-3.0-or-later with the Modding Exception and GPL-3.0
    Linking Exception included in this source package.
- SFSE source examined at commit
  `3f4bbe83be403aaedbfd1c2ba281df2e4e03cc05`
  - Source: https://github.com/gazzamc/sfse/tree/3f4bbe83be403aaedbfd1c2ba281df2e4e03cc05
  - SFSE is a runtime requirement and is not bundled in the binary archive.
- spdlog 1.16.0, obtained by CommonLibSF's xmake dependency declaration.
  - Source: https://github.com/gabime/spdlog/tree/v1.16.0
  - License: MIT.

The matching project source, build script, tests and reverse-engineering notes
are distributed beside the binary archive.

## Bethesda material

Two parts of this project are not the authors' work and are not covered by the
GPL. They remain the property of Bethesda Softworks.

- `Interface/favoritesmenu.swf` in the binary archive is Starfield's own
  favorites menu with additional ActionScript members, which are the hooks the
  plugin needs. The file is redistributed as built by the original author of
  Favorites Banks and is not modified further here; this project has no Flash
  compiler and cannot rebuild it.
- `swf-src/scripts/` in the source archive holds decompiled ActionScript from
  that same file, included so the hooks can be read and reasoned about.

Neither is licensed to anyone by this project. They are present because a UI
plugin cannot work without the menu it hooks into.
