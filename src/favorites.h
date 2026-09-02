#pragma once

namespace FB
{
    inline constexpr wchar_t kConfigName[] = L"FavoritesMenuGrid.ini";
    inline constexpr wchar_t kStateFolderName[] = L"FavoritesMenuGrid";
    inline constexpr std::size_t kSlotsPerBank = 12;
    inline constexpr std::size_t kMaxBanks = 8;
    inline constexpr std::uint8_t kNoFavoriteSlot = 0xFE;
    inline constexpr std::uint8_t kLegacyFirstVirtualSlot = 12;
    inline constexpr std::uint32_t kInvalidFavoriteHandle = 0xFFFFFFFF;
    inline constexpr std::uint32_t kStateVersion = 5;

    // Components.ImageFixture numbers its fixture types with EnumHelper:
    // FT_INVALID is -1 and FT_INTERNAL is 0. A default-constructed visual
    // therefore must not use 0, or every empty slot on a rendered page asks
    // the engine to stream an unnamed bitmap into the shared favorites icon
    // buffer, which corrupts the icons that really are present.
    inline constexpr std::int32_t kInvalidFixtureType = -1;

    struct Settings
    {
        std::size_t rowCount{ 4 };
        // Emptying a slot has no vanilla equivalent: the engine's only
        // slot-clearing routine is called from inside its assign routine,
        // purely to make room before a write, so the game can overwrite a
        // favorite but never remove one. This key does it from the wheel.
        int clearSlotKey{ VK_DELETE };
        // Slots another mod writes into on its own, by index. Such a slot
        // belongs to no page: the mod that owns it puts its item back
        // whenever it likes, and adopting that used to copy the item into
        // every page the player visited. These are left alone entirely --
        // never captured into a page, never cleared or rewritten by a
        // commit -- so the owning mod keeps them and the pages stay clean.
        std::array<bool, kSlotsPerBank> externallyManagedSlots{};

        [[nodiscard]] bool HasExternallyManagedSlots() const noexcept
        {
            return std::ranges::any_of(
                externallyManagedSlots, [](bool pinned) { return pinned; });
        }

        // The page that goes back into the game's twelve real slots every
        // time the favorites menu closes, counted from 1.
        //
        // The grid shows every page at once, so "the page you are on" is not
        // a thing the player can see. Leaving whichever page was last
        // touched in the real slots therefore turns the gameplay quickkeys
        // into invisible state: the same key does something different
        // depending on what you clicked ten minutes ago. Restoring one
        // chosen page on every close makes those keys mean one fixed thing.
        std::size_t defaultRow{ 1 };
        // When set, selecting a favorite that is already equipped unequips
        // it instead of replaying the same equip, so a second click toggles
        // it off the way holstering already works everywhere else.
        bool toggleEquipOnSelect{ true };

        // The all-pages overview drawn next to the wheel. Off by default:
        // it is still a feasibility probe, not a finished interface.

        // Whether grid cells use the wheel's own entry clips, and with them
        // real item icons. Off by default because it is the expensive half:
        // dozens of clips streaming into the icon buffer the twelve wheel
        // entries already share. Names alone are cheap and safe.

        // How much bigger a power icon is drawn than its measured box,
        // in percent.
        //
        // A power's fixture reports a constant 1250x600 box whatever symbol
        // it holds, and the symbol inside is a fraction of that; fitting the
        // box to a cell therefore leaves the symbol tiny. Until the symbol
        // itself can be measured, this multiplies the result. It applies
        // only when the measurement did fall back to that box, so it stops
        // having any effect the moment the artwork can be measured properly.
        int gridPowerIconScalePercent{ 300 };

        // Hides the wheel's twelve entries while the grid is shown. They
        // stay functional -- selection still runs through them -- and only
        // stop being drawn, so the two do not sit on top of each other.

        // Draw our own symbol in an externally managed slot instead of the
        // game's icon, when the item's name says what it is.
        //
        // Such a slot is the one place the icon pipeline cannot be relied
        // on: its card only arrives when the favorites happen to change
        // while the menu is open, and the two forms that share the slot take
        // turns, so one of them is usually undescribed. A symbol drawn from
        // the item's editor ID needs no card at all.
        bool gridPinnedSymbols{ true };

        // The key each quick slot is bound to, as the game's own bindings
        // screen shows it, or empty where nothing is bound. Read once at
        // startup from ControlMap_Custom.txt.
        std::array<std::string, kSlotsPerBank> quickKeyNames{};

    };

    enum class FavoriteKind : std::uint8_t
    {
        kEmpty = 0,
        kInventory = 1,
        kForm = 2
    };

    struct FormIdentity
    {
        RE::TESFormID rawFormID{ 0 };
        std::uint32_t formType{ 0 };
        RE::TESFormID localFormID{ 0 };
        std::string sourceFile;
        std::string editorID;
    };

    struct UniqueIdentity
    {
        std::uint16_t uniqueID{ 0 };
        RE::TESFormID baseID{ 0 };

        auto operator<=>(const UniqueIdentity&) const = default;
    };

    struct ElementalStat
    {
        std::int32_t type{ 0 };
        double value{ 0.0 };
    };

    struct FavoriteVisual
    {
        std::string name;
        std::uint32_t count{ 1 };
        bool isPower{ false };
        bool isEquippable{ true };
        bool isEquipped{ false };
        std::string ammoName;
        std::uint32_t ammoCount{ 0 };
        std::int32_t fixtureType{ kInvalidFixtureType };
        std::string imageDirectory;
        std::string imageName;
        std::vector<ElementalStat> elementalStats;

        [[nodiscard]] bool HasData() const noexcept
        {
            return !name.empty() || fixtureType != kInvalidFixtureType ||
                !imageName.empty() || !elementalStats.empty();
        }
    };

    struct FavoriteSlot
    {
        FavoriteKind kind{ FavoriteKind::kEmpty };
        FormIdentity form;
        std::vector<UniqueIdentity> uniqueIDs;
        std::uint32_t rowOrdinal{ 0 };
        RE::BSTSmartPointer<RE::TBO_InstanceData> sessionInstanceData;
        FavoriteVisual visual;
        bool unresolved{ false };

        [[nodiscard]] bool Empty() const noexcept
        {
            return kind == FavoriteKind::kEmpty;
        }
    };

    using FavoriteBank = std::array<FavoriteSlot, kSlotsPerBank>;
    using FavoriteBanks = std::array<FavoriteBank, kMaxBanks>;

    extern Settings g_settings;
    extern std::mutex g_stateMutex;
    extern FavoriteBanks g_banks;

    // What the externally managed slots currently hold. Kept once rather
    // than per page, because that is what they are: one slot shared by
    // every page, owned by another mod. Not persisted -- the owning mod
    // re-establishes it, and the native slots are read on load anyway.
    extern FavoriteBank g_pinnedSlots;

    // The page the wheel is currently showing. Browsing only changes this.
    extern std::size_t g_activeBank;

    // The page whose descriptors are currently written into the twelve real
    // Starfield favorite slots. Gameplay quickkeys, the inventory heart and
    // the engine's own "use quickslot" handler can only ever see this page,
    // so it is committed to match g_activeBank when the player commits to a
    // page: on a slot selection, and when the favorites menu closes.
    extern std::size_t g_nativeBank;
    extern std::atomic_bool g_started;
    extern std::atomic_bool g_switchQueued;
    extern std::atomic_bool g_captureQueued;
    extern std::atomic_bool g_favoritesMenuVisible;
    extern std::atomic_bool g_switchInProgress;
    extern std::atomic_bool g_disabledForConflict;
    extern std::atomic_uint64_t g_sessionGeneration;

    [[nodiscard]] std::filesystem::path GetPluginDirectory();
    [[nodiscard]] std::filesystem::path GetConfigPath();
    [[nodiscard]] bool IsFavoritesMenuOpen();

    bool InitializeSessionIfNeeded();
    bool SwitchBank(std::size_t targetBank);
    void QueueBankSwitch(std::size_t targetBank);
    void QueueCaptureCurrentState(bool saveSnapshot = false);
    void QueueCommitActiveBank();
    // Records which save a starting load will bring in, so the state for
    // that exact save can be restored instead of the character's shared
    // state. Must be called before BeginLoadTransition.
    void NoteIncomingSave(RE::SaveLoadEvent::OpType operation);
    void BeginLoadTransition();
    void QueueLoadedGameInitialization();

    // Empties one stored slot of the page the wheel is showing, and the
    // matching native slot when that page is the one in the real slots.
    void ClearSlotFromMenu(std::size_t globalIndex);
    // The same, for the grid, which shows every page and may empty a slot
    // on any of them.
    void ClearGridSlot(std::size_t globalIndex);

    // Exchanges two stored slots, across pages if needed, and rewrites
    // the native slots when the live page is involved. Refuses slots
    // that belong to another mod.
    bool SwapStoredSlots(
        std::size_t bankA,
        std::size_t slotA,
        std::size_t bankB,
        std::size_t slotB);


    // Draws the all-pages overview: one row per page, one cell per slot,
    // with externally managed slots in a separate pinned strip.
    void UpdateGridOverlay(RE::IMenu* explicitMenu = nullptr);
    // Called when the favorites menu closes: edit mode, the frame ticker and
    // the icon bookkeeping all belong to one opening of the wheel.
    void ResetGridSession();

    // True while the grid still has icon clips to create. They are
    // built a few per pass so the engine's asset streaming is never
    // asked for dozens at once, which crashed it in its own job thread.
    [[nodiscard]] bool GridIconsPending();

    // Empties the grid cell under the cursor. The wheel's own clear key
    // acts on its highlighted slot, and the grid never sets one, so
    // hovering had no way to remove anything.
    bool ClearHoveredGridCell();

    // Describes one slot the way the wheel's own entries expect it. Shared
    // so the grid can render with FavoritesEntry clips instead of a second
    // implementation that would have to be kept in step.
    void BuildFavoriteInfoForDisplay(
        RE::Scaleform::GFx::ASMovieRootBase* root,
        const FavoriteSlot& slot,
        RE::Scaleform::GFx::Value& result);
    void AttachFavoritesMenuBridge(RE::IMenu* menu);
    void RenderActiveBank(RE::IMenu* explicitMenu = nullptr);
    // Returns whether the engine may now act on the selected wheel position.
    // It may only do so once the shown page occupies the real favorite slots,
    // because the vanilla quickkey event carries nothing but a slot number:
    // dispatching it while the real slots still hold a different page uses
    // that other page's item.
    [[nodiscard]] bool ProcessMenuSelection(
        std::size_t globalIndex,
        bool assigning,
        const RE::Scaleform::GFx::Value* assignedItem);

    // Harvests names and icons from the wheel's own item cards for the page
    // that currently occupies the real favorite slots. Those cards are the
    // only place this data exists: a native capture reads inventory rows and
    // manager arrays, which carry no presentation at all. Without harvesting,
    // a page could not be drawn once some other page took over the real slots.
    void CaptureNativePageVisuals(const RE::Scaleform::GFx::Value* items);

    // Copies the page the wheel is about to draw, blanking any favourite whose
    // item is not in the player's inventory right now. A page that does not
    // occupy the real slots is drawn from stored descriptors, and those
    // describe what the player put there, not what they still carry: without
    // this filter a weapon that was sold or stored keeps a ghost icon until
    // the page is committed. The stored descriptor itself is left untouched,
    // so the favourite returns if the item does.
    [[nodiscard]] FavoriteBank BuildRenderablePage(
        std::size_t& activeBank,
        bool& showingNativePage);

    // Lets the wheel report into the plugin log. The icon scale is computed
    // inside Scaleform at the instant a symbol is attached, and nothing on the
    // C++ side can observe that moment, so the measurement has to come from
    // the ActionScript that lives through it.
    void LogFromMenu(std::string_view text);
    void RebuildFavoritesData(void* manager);
}
