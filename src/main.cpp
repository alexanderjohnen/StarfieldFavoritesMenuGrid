#include "pch.h"
#include "favorites.h"

namespace
{
    std::atomic_bool g_runtimeEventsRegistered{ false };
    std::mutex g_registrationMutex;
    bool g_menuEventsRegistered{ false };
    bool g_saveLoadEventsRegistered{ false };
    bool g_loadGameEventsRegistered{ false };
    bool g_favoriteEventsRegistered{ false };

    [[nodiscard]] std::wstring ReadProfileString(
        const std::filesystem::path& path,
        const wchar_t* section,
        const wchar_t* key,
        const wchar_t* fallback)
    {
        std::wstring value(256, L'\0');
        const auto length = GetPrivateProfileStringW(
            section,
            key,
            fallback,
            value.data(),
            static_cast<DWORD>(value.size()),
            path.c_str());
        value.resize(length);
        return value;
    }

    [[nodiscard]] std::wstring NormalizeKeyName(std::wstring value)
    {
        const auto first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(L" \t\r\n");
        value = value.substr(first, last - first + 1);

        std::wstring normalized;
        normalized.reserve(value.size());
        for (const auto character : value) {
            if (!std::iswspace(character) && character != L'_') {
                normalized.push_back(
                    static_cast<wchar_t>(std::towupper(character)));
            }
        }
        return normalized;
    }

    [[nodiscard]] std::optional<int> ParseVirtualKey(
        const std::wstring& rawValue)
    {
        const auto value = NormalizeKeyName(rawValue);
        if (value.empty()) {
            return std::nullopt;
        }
        if (value == L"NONE" || value == L"DISABLED") {
            return 0;
        }
        if (value.size() == 1) {
            const auto character = value.front();
            if ((character >= L'A' && character <= L'Z') ||
                (character >= L'0' && character <= L'9')) {
                return static_cast<int>(character);
            }
        }
        if (value.starts_with(L"F") && value.size() <= 3) {
            const auto number = std::wcstol(value.c_str() + 1, nullptr, 10);
            if (number >= 1 && number <= 24) {
                return VK_F1 + static_cast<int>(number - 1);
            }
        }

        const std::array<std::pair<std::wstring_view, int>, 29> names{
            std::pair{ L"PAGEUP"sv, VK_PRIOR },
            std::pair{ L"PGUP"sv, VK_PRIOR },
            std::pair{ L"PAGEDOWN"sv, VK_NEXT },
            std::pair{ L"PGDN"sv, VK_NEXT },
            std::pair{ L"HOME"sv, VK_HOME },
            std::pair{ L"END"sv, VK_END },
            std::pair{ L"INSERT"sv, VK_INSERT },
            std::pair{ L"DELETE"sv, VK_DELETE },
            std::pair{ L"SPACE"sv, VK_SPACE },
            std::pair{ L"TAB"sv, VK_TAB },
            std::pair{ L"ALT"sv, VK_MENU },
            std::pair{ L"CTRL"sv, VK_CONTROL },
            std::pair{ L"CONTROL"sv, VK_CONTROL },
            std::pair{ L"SHIFT"sv, VK_SHIFT },
            std::pair{ L"NUMPAD0"sv, VK_NUMPAD0 },
            std::pair{ L"NUMPAD1"sv, VK_NUMPAD1 },
            std::pair{ L"NUMPAD2"sv, VK_NUMPAD2 },
            std::pair{ L"NUMPAD3"sv, VK_NUMPAD3 },
            std::pair{ L"NUMPAD4"sv, VK_NUMPAD4 },
            std::pair{ L"NUMPAD5"sv, VK_NUMPAD5 },
            std::pair{ L"NUMPAD6"sv, VK_NUMPAD6 },
            std::pair{ L"NUMPAD7"sv, VK_NUMPAD7 },
            std::pair{ L"NUMPAD8"sv, VK_NUMPAD8 },
            std::pair{ L"NUMPAD9"sv, VK_NUMPAD9 },
            std::pair{ L"NUMPADPLUS"sv, VK_ADD },
            std::pair{ L"NUMPADMINUS"sv, VK_SUBTRACT },
            std::pair{ L"NUMPADMULTIPLY"sv, VK_MULTIPLY },
            std::pair{ L"NUMPADDIVIDE"sv, VK_DIVIDE },
            std::pair{ L"ESCAPE"sv, VK_ESCAPE }
        };
        for (const auto& [name, code] : names) {
            if (value == name) {
                return code;
            }
        }

        wchar_t* end = nullptr;
        const auto numeric = std::wcstol(value.c_str(), &end, 0);
        if (end && *end == L'\0' && numeric >= 0 && numeric <= 0xFF) {
            return static_cast<int>(numeric);
        }
        return std::nullopt;
    }

    [[nodiscard]] int ReadVirtualKey(
        const std::filesystem::path& path,
        const wchar_t* key,
        int fallback)
    {
        const auto raw = ReadProfileString(path, L"Controls", key, L"");
        if (const auto parsed = ParseVirtualKey(raw)) {
            return *parsed;
        }
        if (!raw.empty()) {
            REX::WARN(
                L"Favorites Menu Grid ignored unknown key name '{}' for {}",
                raw,
                std::wstring(key));
        }
        return fallback;
    }

    // "6" or "6,9" or "NONE". Slot numbers are the ones the wheel shows,
    // so they are 1-based here and stored 0-based.
    void ParseExternallyManagedSlots(const std::wstring& rawValue)
    {
        FB::g_settings.externallyManagedSlots.fill(false);
        const auto value = NormalizeKeyName(rawValue);
        if (value.empty() || value == L"NONE" || value == L"0") {
            return;
        }

        std::wstring listed;
        for (std::size_t start = 0; start < value.size();) {
            auto end = value.find(L',', start);
            if (end == std::wstring::npos) {
                end = value.size();
            }
            const auto token = value.substr(start, end - start);
            start = end + 1;
            if (token.empty()) {
                continue;
            }
            wchar_t* parseEnd = nullptr;
            const auto number = std::wcstol(token.c_str(), &parseEnd, 10);
            if (parseEnd && *parseEnd == L'\0' && number >= 1 &&
                number <= static_cast<long>(FB::kSlotsPerBank)) {
                FB::g_settings.externallyManagedSlots[
                    static_cast<std::size_t>(number - 1)] = true;
                if (!listed.empty()) {
                    listed += L", ";
                }
                listed += std::to_wstring(number);
            } else {
                REX::WARN(
                    L"Favorites Menu Grid ignored '{}' in ExternallyManagedSlots; expected slot numbers 1-12",
                    token);
            }
        }
        if (!listed.empty()) {
            REX::INFO(
                L"Favorites Menu Grid leaves slot(s) {} to whichever mod owns them: they are never captured into a page and never rewritten by a page commit",
                listed);
        }
    }

    [[nodiscard]] std::string VirtualKeyName(std::uint8_t code)
    {
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z')) {
            return std::string(1, static_cast<char>(code));
        }
        if (code >= VK_F1 && code <= VK_F24) {
            return std::format("F{}", code - VK_F1 + 1);
        }
        if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) {
            return std::format("N{}", code - VK_NUMPAD0);
        }
        switch (code) {
        case VK_SPACE:    return "SPC";
        case VK_TAB:      return "TAB";
        case VK_RETURN:   return "RET";
        case VK_ESCAPE:   return "ESC";
        case VK_LSHIFT:   return "LSH";
        case VK_RSHIFT:   return "RSH";
        case VK_LCONTROL: return "LCT";
        case VK_RCONTROL: return "RCT";
        case VK_LMENU:    return "LAL";
        case VK_RMENU:    return "RAL";
        case VK_INSERT:   return "INS";
        case VK_DELETE:   return "DEL";
        case VK_HOME:     return "HOM";
        case VK_END:      return "END";
        case VK_PRIOR:    return "PGU";
        case VK_NEXT:     return "PGD";
        default:          break;
        }
        return std::format("{:02X}", code);
    }

    // Which key each quick slot carries, straight out of the game's own
    // ControlMap_Custom.txt.
    //
    // The format is undocumented but plainly regular: a null-terminated
    // context, a null-terminated action, then eight bytes whose fifth is the
    // virtual key code and 0xFF where nothing is bound. Reading it is a file
    // read and nothing more -- no engine call, no address, nothing that can
    // move under a game update.
    //
    // Starfield writes the file when the bindings screen is left, not when
    // the game exits, so it is current while playing; the grid re-reads it
    // every time the wheel opens. Its age goes into the log all the same,
    // because a copy that predates the bindings it claims to describe looks
    // exactly like a bug in here.
    void LoadQuickKeyNames()
    {
        // Under the state lock, because the grid reads these while drawing
        // and the drawing does not happen on this thread.
        std::scoped_lock lock(FB::g_stateMutex);
        for (auto& name : FB::g_settings.quickKeyNames) {
            name.clear();
        }

        // SFSE already resolves the redirected Documents folder for its
        // log directory, and the control map sits two levels above it.
        const auto logs = SFSE::log::log_directory();
        if (!logs) {
            return;
        }
        const auto path = logs->parent_path().parent_path() /
            L"ControlMap_Custom.txt";

        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            REX::INFO(
                "Favorites Menu Grid found no ControlMap_Custom.txt; grid headings stay blank");
            return;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return;
        }
        const std::string data(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());

        // Built rather than counted. Written as a literal with an embedded
        // NUL it needs an explicit length, and getting that length wrong by
        // one made the marker end in a second NUL, which matches nothing:
        // what follows "Quickkey" is a digit.
        static const std::string kMarker =
            std::string("MainGameplay") + std::string(1, char{}) + "Quickkey";
        std::size_t found = 0;
        for (std::size_t at = data.find(kMarker, 0);
             at != std::string::npos;
             at = data.find(kMarker, at + 1)) {
            auto cursor = at + kMarker.size();
            std::size_t slot = 0;
            std::size_t digits = 0;
            while (cursor < data.size() && data[cursor] >= '0' &&
                   data[cursor] <= '9') {
                slot = slot * 10 +
                    static_cast<std::size_t>(data[cursor] - '0');
                ++cursor;
                ++digits;
            }
            if (digits == 0 || cursor >= data.size() || data[cursor] != 0 ||
                slot < 1 || slot > FB::kSlotsPerBank) {
                continue;
            }
            ++cursor;
            if (cursor + 8 > data.size()) {
                continue;
            }
            const auto code = static_cast<std::uint8_t>(data[cursor + 4]);
            if (code == 0xFF || code == 0x00) {
                continue;
            }
            auto& name = FB::g_settings.quickKeyNames[slot - 1];
            if (name.empty()) {
                name = VirtualKeyName(code);
                ++found;
            }
        }

        const auto written = std::filesystem::last_write_time(path, error);
        const auto age = error ?
            std::chrono::hours(0) :
            std::chrono::duration_cast<std::chrono::hours>(
                decltype(written)::clock::now() - written);
        std::string listing;
        for (std::size_t slot = 0; slot < FB::kSlotsPerBank; ++slot) {
            if (!FB::g_settings.quickKeyNames[slot].empty()) {
                if (!listing.empty()) {
                    listing += ", ";
                }
                listing += std::format(
                    "{}={}", slot + 1, FB::g_settings.quickKeyNames[slot]);
            }
        }
        REX::INFO(
            "Favorites Menu Grid read {} quick key binding(s) from ControlMap_Custom.txt, last written {} hour(s) ago: {}",
            found,
            age.count(),
            listing.empty() ? "none bound" : listing);
    }

    // A key that changes section silently reverts to its default for
    // everyone who already has an INI: GetPrivateProfileInt cannot tell
    // "the key is absent" from "the value happens to equal the default", so
    // the move looks like a fresh install. It is read with a sentinel
    // instead, and the old section is consulted before giving up. That
    // failure has already cost this mod once -- ExternallyManagedSlots
    // stopped pinning slot 6 because its read was quietly lost.
    [[nodiscard]] int ReadMovedInt(
        const std::filesystem::path& path,
        const wchar_t* section,
        const wchar_t* previousSection,
        const wchar_t* key,
        const char* label,
        int fallback)
    {
        constexpr int kAbsent = 0x7FFFFFFF;
        auto value = static_cast<int>(GetPrivateProfileIntW(
            section, key, kAbsent, path.c_str()));
        if (value != kAbsent) {
            return value;
        }
        value = static_cast<int>(GetPrivateProfileIntW(
            previousSection, key, kAbsent, path.c_str()));
        if (value == kAbsent) {
            return fallback;
        }
        REX::INFO(
            "Favorites Menu Grid took '{}' from its old INI section; the file still works, but the key has moved",
            label);
        return value;
    }

    void LoadSettings()
    {
        const auto path = FB::GetConfigPath();
        FB::g_settings.rowCount = static_cast<std::size_t>(std::clamp(
            ReadMovedInt(path, L"Grid", L"Settings", L"RowCount", "RowCount", 4),
            2,
            static_cast<int>(FB::kMaxBanks)));
        LoadQuickKeyNames();
        FB::g_settings.gridPinnedSymbols =
            ReadMovedInt(path, L"Controls", L"Grid", L"PinnedSymbols", "PinnedSymbols", 1) != 0;
        FB::g_settings.gridPowerIconScalePercent = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"Grid", L"PowerIconScalePercent", 300, path.c_str())),
            50,
            800);

        // Everything below reads the rest of the file. It went missing when
        // the hotkey buttons were removed: the deletion took the surrounding
        // block with it, and every one of these settings silently fell back
        // to its default. That is why ExternallyManagedSlots stopped pinning
        // slot 6, and why the mouse wheel and the shoulder buttons kept
        // working after being switched off in the INI.
        FB::g_settings.defaultRow = static_cast<std::size_t>(std::clamp(
            ReadMovedInt(path, L"Grid", L"Settings", L"DefaultRow", "DefaultRow", 1),
            1,
            static_cast<int>(FB::g_settings.rowCount)));
        FB::g_settings.toggleEquipOnSelect = ReadMovedInt(
            path, L"Controls", L"Settings", L"ToggleEquipOnSelect", "ToggleEquipOnSelect", 1) != 0;
        FB::g_settings.clearSlotKey = ReadVirtualKey(
            path, L"ClearSlotKey", VK_DELETE);
        ParseExternallyManagedSlots(
            ReadProfileString(
                path, L"Controls", L"ExternallyManagedSlots", L"NONE"));

        REX::INFO(
            "Favorites Menu Grid settings: rows={}, defaultRow={}, clearKey=0x{:02X}, toggleEquipOnSelect={}, pinnedSymbols={}, powerIconScale={}%",
            FB::g_settings.rowCount,
            FB::g_settings.defaultRow,
            FB::g_settings.clearSlotKey,
            FB::g_settings.toggleEquipOnSelect,
            FB::g_settings.gridPinnedSymbols,
            FB::g_settings.gridPowerIconScalePercent);
    }

    [[nodiscard]] bool IsGameForeground()
    {
        const auto foreground = GetForegroundWindow();
        if (!foreground) {
            return false;
        }
        DWORD processID = 0;
        GetWindowThreadProcessId(foreground, &processID);
        return processID == GetCurrentProcessId();
    }

    // How often the plugin looks at the keyboard while the favorites
    // menu is open. Responsive and inexpensive; not worth a setting.
    constexpr int kPollIntervalMs = 10;

    [[nodiscard]] bool IsKeyDown(int key)
    {
        return key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    [[nodiscard]] bool EqualsMenuName(
        const RE::BSFixedString& menuName,
        std::string_view expected)
    {
        const auto* name = menuName.c_str();
        return name && std::string_view(name) == expected;
    }

    [[nodiscard]] bool HasFavoritesMenuExtendedConflict()
    {
        std::error_code error;
        const auto pluginDirectory = FB::GetPluginDirectory();
        if (!std::filesystem::exists(pluginDirectory, error)) {
            return false;
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(pluginDirectory, error)) {
            if (error || !entry.is_regular_file()) {
                continue;
            }
            auto name = entry.path().filename().wstring();
            std::ranges::transform(name, name.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (name == L"favoritesmenuextended.dll") {
                return true;
            }
        }
        return false;
    }

    void KeyboardPollingLoop()
    {
        bool previousClearKey = false;
        bool previousMenuReady = false;
        while (true) {
            const auto menuReady =
                !FB::g_disabledForConflict.load(std::memory_order_acquire) &&
                IsGameForeground() && FB::IsFavoritesMenuOpen();
            FB::g_favoritesMenuVisible.store(
                menuReady, std::memory_order_release);
            if (!previousMenuReady && menuReady) {
                // Starfield writes ControlMap_Custom.txt when the bindings
                // screen is left, not when the game exits -- so a binding
                // changed this session is on disk long before the next
                // start. Re-reading here keeps the grid's headings honest
                // without waiting for one.
                LoadQuickKeyNames();
            }
            if (previousMenuReady && !menuReady) {
                // The wheel just closed. Its overlay went with it, and so
                // did the frame ticker that was drawing the grid.
                FB::ResetGridSession();
            }
            previousMenuReady = menuReady;

            // A grid button's key waits here until the favorites menu has
            // finished closing, because a hotkey mod ignores keys while a
            // menu is up. This loop already runs often enough to be prompt.
            // The wheel's clear key acts on its highlighted slot, which
            // the grid never sets. Polled here so it can act on the cell
            // under the cursor instead.
            const auto clearDown =
                menuReady && IsKeyDown(FB::g_settings.clearSlotKey);
            if (clearDown && !previousClearKey) {
                static_cast<void>(FB::ClearHoveredGridCell());
            }
            previousClearKey = clearDown;

            // Icons are created a few at a time; keep asking for the
            // next batch until the grid reports itself complete.
            if (menuReady && FB::GridIconsPending()) {
                // One outstanding request at a time. Queueing another while
                // the previous has not run yet is how several passes ended
                // up overlapping.
                // A function-local static, so the task can clear it again
                // without capturing anything.
                static std::atomic_bool queued{ false };
                if (!queued.exchange(true)) {
                    if (const auto* tasks = SFSE::GetTaskInterface()) {
                        tasks->AddTask([]() {
                            FB::UpdateGridOverlay();
                            queued.store(false);
                        });
                    } else {
                        queued.store(false);
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(80));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(
                kPollIntervalMs));
        }
    }

    void OnMenuMovieCreated(RE::IMenu* menu)
    {
        if (!menu || !EqualsMenuName(menu->menuName, "FavoritesMenu")) {
            return;
        }
        FB::g_favoritesMenuVisible.store(true, std::memory_order_release);
        FB::AttachFavoritesMenuBridge(menu);
        FB::QueueLoadedGameInitialization();
        // Deliberately not called directly: this callback does not run on
        // the game's main thread, and Scaleform is not thread safe. The
        // indicator above has always been built from here and gets away
        // with it because it is a single sprite; the grid creates dozens of
        // display objects and loads icons, which corrupted Scaleform state
        // and crashed the next ordinary render four seconds later. The task
        // queue runs on the main thread, and the overlay looks the menu up
        // again itself, so a menu closed in the meantime is simply skipped.
        if (const auto* tasks = SFSE::GetTaskInterface()) {
            tasks->AddTask([]() { FB::UpdateGridOverlay(); });
        }
    }

    [[nodiscard]] bool IsLoadOperation(RE::SaveLoadEvent::OpType operation)
    {
        using Op = RE::SaveLoadEvent::OpType;
        return operation == Op::kLoadMostRecent ||
            operation == Op::kQuickload ||
            operation == Op::kLoad ||
            operation == Op::kLoadNamedFile;
    }

    // Measured, not inferred: a quicksave reports opType=3 with status=0
    // then status=1. Status 4 (kSaveCompleted) never arrives at all, so the
    // status alone cannot say whether a save or a load just finished -- the
    // operation has to.
    [[nodiscard]] bool IsSaveOperation(RE::SaveLoadEvent::OpType operation)
    {
        using Op = RE::SaveLoadEvent::OpType;
        return operation == Op::kAutosave ||
            operation == Op::kQuicksave ||
            operation == Op::kManualSave ||
            operation == Op::kExitSaveToMainMenu ||
            operation == Op::kExitSaveToDesktop;
    }

    class RuntimeEventSink final :
        public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
        public RE::BSTEventSink<RE::SaveLoadEvent>,
        public RE::BSTEventSink<RE::TESLoadGameEvent>,
        public RE::BSTEventSink<RE::InventoryInterface::FavoriteChangedEvent>
    {
    public:
        static RuntimeEventSink& GetSingleton()
        {
            static RuntimeEventSink singleton;
            return singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (EqualsMenuName(event.menuName, "FavoritesMenu")) {
                FB::g_favoritesMenuVisible.store(
                    event.opening, std::memory_order_release);
                if (event.opening) {
                    FB::QueueLoadedGameInitialization();
                } else {
                    // Closing the wheel is the player committing to the page
                    // they left it on. Writing that page into the real slots
                    // is what lets the gameplay quickkeys 1-0, [ and ] reach
                    // it: the engine only ever reads the twelve real slots.
                    // The commit captures the outgoing page first, so this
                    // also persists any change made while the menu was open.
                    FB::QueueCommitActiveBank();
                }
            } else if (!event.opening &&
                       (EqualsMenuName(event.menuName, "InventoryMenu") ||
                        EqualsMenuName(event.menuName, "PowersMenu"))) {
                // Capturing on close also covers powers, whose favorite event
                // does not come from BGSInventoryInterface.
                FB::QueueCaptureCurrentState(false);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::SaveLoadEvent& event,
            RE::BSTEventSource<RE::SaveLoadEvent>*) override
        {
            using Status = RE::SaveLoadEvent::Status;
            // Diagnose: der Snapshot-Zweig hat nachweislich nie gelaufen,
            // obwohl gespeichert wurde. Bevor irgendetwas daran umgebaut
            // wird, muss belegt sein, welche Ereignisse ueberhaupt kommen.
            REX::INFO(
                "SaveLoadEvent: opType={} status={}",
                static_cast<unsigned>(event.opType),
                static_cast<unsigned>(event.status));
            if (event.status == Status::kBegin &&
                IsLoadOperation(event.opType)) {
                // Which save is coming in has to be read now: the manager
                // only holds the queued entry until the load consumes it.
                FB::NoteIncomingSave(event.opType);
                FB::BeginLoadTransition();
            } else if (event.status == Status::kLoadSucceeded) {
                // Despite the name, status 1 means "the operation succeeded".
                // Treating it as a load regardless is what re-ran session
                // initialisation after every quicksave, and what left the
                // save itself unnoticed. The exit-save operations matter
                // most here: they are the last chance to record state before
                // the game closes.
                if (IsSaveOperation(event.opType)) {
                    FB::QueueCaptureCurrentState(true);
                } else {
                    FB::QueueLoadedGameInitialization();
                }
            } else if (event.status == Status::kSaveCompleted) {
                // Never observed in the wild. Kept because acting on it is
                // correct if it ever does arrive.
                FB::QueueCaptureCurrentState(true);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESLoadGameEvent&,
            RE::BSTEventSource<RE::TESLoadGameEvent>*) override
        {
            FB::QueueLoadedGameInitialization();
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::InventoryInterface::FavoriteChangedEvent&,
            RE::BSTEventSource<
                RE::InventoryInterface::FavoriteChangedEvent>*) override
        {
            if (!FB::g_switchInProgress.load(std::memory_order_acquire)) {
                // The event may be emitted while the engine owns inventory
                // locks. Defer all inspection to the main task queue.
                FB::QueueCaptureCurrentState(false);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        RuntimeEventSink() = default;
    };

    void RegisterRuntimeEvents()
    {
        if (g_runtimeEventsRegistered.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock registrationLock(g_registrationMutex);
        if (g_runtimeEventsRegistered.load(std::memory_order_relaxed)) {
            return;
        }
        auto& sink = RuntimeEventSink::GetSingleton();

        if (!g_menuEventsRegistered) {
            if (auto* ui = RE::UI::GetSingleton()) {
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&sink);
                g_menuEventsRegistered = true;
            }
        }
        if (!g_saveLoadEventsRegistered) {
            if (auto* source = RE::SaveLoadEvent::GetEventSource()) {
                source->RegisterSink(&sink);
                g_saveLoadEventsRegistered = true;
            }
        }
        if (!g_loadGameEventsRegistered) {
            if (auto* source = RE::TESLoadGameEvent::GetEventSource()) {
                source->RegisterSink(&sink);
                g_loadGameEventsRegistered = true;
            }
        }
        if (!g_favoriteEventsRegistered) {
            if (auto* inventory = RE::BGSInventoryInterface::GetSingleton()) {
                // BGSInventoryInterface inherits this source privately at
                // +0x10. CommonLib exposes the verified layout but cannot
                // static_cast to the private base from plugin code.
                using FavoriteEvent =
                    RE::InventoryInterface::FavoriteChangedEvent;
                auto* source =
                    reinterpret_cast<RE::BSTEventSource<FavoriteEvent>*>(
                        reinterpret_cast<std::byte*>(inventory) + 0x10);
                source->RegisterSink(&sink);
                g_favoriteEventsRegistered = true;
            }
        }

        const auto registered =
            static_cast<std::size_t>(g_menuEventsRegistered) +
            static_cast<std::size_t>(g_saveLoadEventsRegistered) +
            static_cast<std::size_t>(g_loadGameEventsRegistered) +
            static_cast<std::size_t>(g_favoriteEventsRegistered);
        g_runtimeEventsRegistered.store(
            registered == 4, std::memory_order_release);
        REX::INFO(
            "Favorites Menu Grid registered {}/4 native runtime event sources",
            registered);
        if (registered != 4) {
            REX::WARN(
                "Favorites Menu Grid could not register every runtime event source; save/load or favorite capture may be incomplete");
        }
        FB::QueueLoadedGameInitialization();
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (!message ||
            FB::g_disabledForConflict.load(std::memory_order_acquire)) {
            return;
        }
        if (message->type == SFSE::MessagingInterface::kPostDataLoad ||
            message->type == SFSE::MessagingInterface::kPostPostDataLoad) {
            RegisterRuntimeEvents();
        }
    }

    void StartPlugin()
    {
        if (FB::g_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        LoadSettings();

        if (HasFavoritesMenuExtendedConflict()) {
            FB::g_disabledForConflict.store(true, std::memory_order_release);
            REX::CRITICAL(
                "Favorites Menu Grid disabled: FavoritesMenuExtended.dll owns a different expanded favorites state and cannot safely run at the same time");
            return;
        }

        if (const auto* messages = SFSE::GetMessagingInterface()) {
            if (!messages->RegisterListener(&OnSFSEMessage)) {
                throw std::runtime_error(
                    "could not register the SFSE post-data-load listener");
            }
        } else {
            throw std::runtime_error("SFSE messaging interface is unavailable");
        }
        if (const auto* menus = SFSE::GetMenuInterface()) {
            menus->Register(&OnMenuMovieCreated);
        } else {
            throw std::runtime_error("SFSE menu interface is unavailable");
        }

        std::thread(KeyboardPollingLoop).detach();
        REX::INFO(
            "Favorites Menu Grid started: {} pages of {} slots; one page at a time occupies the native slots and is committed when the wheel closes",
            FB::g_settings.rowCount,
            FB::kSlotsPerBank);
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse);
    if (sfse->RuntimeVersion() != SFSE::RUNTIME_SF_1_16_244) {
        REX::CRITICAL(
            "Favorites Menu Grid refused unsupported Starfield runtime {} (requires 1.16.244.0)",
            sfse->RuntimeVersion().string());
        return false;
    }
    try {
        StartPlugin();
    } catch (const std::exception& exception) {
        REX::CRITICAL(
            "Favorites Menu Grid failed to start: {}", exception.what());
        return false;
    }
    return true;
}
