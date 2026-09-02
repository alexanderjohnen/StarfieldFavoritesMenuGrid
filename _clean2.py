import io


def load(p):
    s = io.open(p, encoding='utf-8', newline='').read()
    return s.replace('\r\n', '\n'), ('\r\n' in s)


def save(p, s, crlf):
    if crlf:
        s = s.replace('\n', '\r\n')
    io.open(p, 'w', encoding='utf-8', newline='').write(s)


def cut(s, start, end, label):
    i = s.find(start)
    assert i >= 0, 'START MISSING: ' + label
    j = s.find(end, i)
    assert j >= 0, 'END MISSING: ' + label
    return s[:i] + s[j:]


def rep(s, old, new, label):
    assert old in s, 'MISSING: ' + label
    return s.replace(old, new, 1)


# ===========================================================================
# favorites_ui.cpp
# ===========================================================================
p = 'src/favorites_ui.cpp'
s, crlf = load(p)
s = cut(s, "        void DrawBankIndicatorGraphics(",
        "        void SetStringMember(", 'indicator helpers')
s = cut(s, "    void UpdateBankIndicator(RE::IMenu* explicitMenu)",
        "    void AttachFavoritesMenuBridge(RE::IMenu* menu)", 'indicator entry')
s = cut(s, '        static_cast<void>(menu->menuObj.SetMember(\n            "FavoritesBanksIconFrame",',
        "        RE::Scaleform::GFx::Value visualsCallback;", 'icon frame and telemetry')
save(p, s, crlf)

# ===========================================================================
# favorites.h
# ===========================================================================
p = 'src/favorites.h'
s, crlf = load(p)
s = cut(s, "        bool mouseWheelEnabled{ true };",
        "        // When set, closing the Favorites menu always commits page 1",
        'wheel and controller fields')
s = cut(s, "        // Power and item icons are symbols streamed from external",
        "        bool showIndicator{ true };", 'settle and icon frame fields')
s = cut(s, "        bool showIndicator{ true };",
        "        // The all-pages overview drawn next to the wheel.",
        'indicator and poll fields')
s = rep(s, "    void UpdateBankIndicator(RE::IMenu* explicitMenu = nullptr);\n",
        "", 'indicator declaration')
save(p, s, crlf)

# ===========================================================================
# favorites_core.cpp
# ===========================================================================
p = 'src/favorites_core.cpp'
s, crlf = load(p)
s = s.replace("        UpdateBankIndicator();\n", "")
s = s.replace("                UpdateBankIndicator();\n", "")
s = rep(s, "    std::atomic_int g_pendingWheelSteps{ 0 };\n", "", 'wheel steps decl')
s = rep(s, "        g_pendingWheelSteps.store(0, std::memory_order_release);\n",
        "", 'wheel steps reset')
save(p, s, crlf)

# ===========================================================================
# favorites_grid.cpp
# ===========================================================================
p = 'src/favorites_grid.cpp'
s, crlf = load(p)
s = rep(s, "                (slot.visual.isPower || g_settings.iconTelemetry)) {",
        "                slot.visual.isPower) {", 'telemetry use')
save(p, s, crlf)
print('cleaned')
