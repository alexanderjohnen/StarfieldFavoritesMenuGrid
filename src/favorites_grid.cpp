#include "pch.h"
#include "favorites.h"

// An overview of every page at once, drawn next to the wheel: one row per
// page, one cell per slot, with externally managed slots lifted out into a
// separate pinned strip because they belong to no page.
//
// This is deliberately built out of display objects created from C++ rather
// than by replacing favoritesmenu.swf again. The wheel's own SWF is already
// a compatibility burden -- any other UI mod that ships that file conflicts
// with it -- and everything here is additive: a sprite on the stage that the
// vanilla menu neither knows nor cares about.
//
// The current build is a feasibility probe. Three things had to be proven
// before the real UI could be designed around them, and each is reported to
// the log on the first menu open:
//
//   1. whether Scaleform hands out flash.text.TextField at all,
//   2. whether the item icon classes can be instantiated from here, which
//      decides whether the grid can show icons or only names,
//   3. whether clicks on a self-made cell arrive with usable coordinates.
//
// Nothing here mutates favorite state. Clicks are logged, not acted on.
namespace FB
{
    // The whole draw. Called from one place only: the frame ticker below,
    // which the movie itself invokes. See the note on UpdateGridOverlay.
    void DrawGridOverlay(RE::IMenu* explicitMenu = nullptr);

    namespace
    {
        // Cells are sized for a readable label, not for an icon. The icon
        // mode looks cramped at this size and will want its own numbers, but
        // an unreadable grid is useless either way.
        constexpr double kCellSize = 66.0;
        constexpr double kCellGap = 4.0;
        constexpr double kLabelWidth = 30.0;
        constexpr double kPinGap = 24.0;
        constexpr double kPadding = 14.0;
        constexpr double kFontSize = 12.0;
        // The band below the cells that carries the status line.
        constexpr double kStatusHeight = 20.0;
        // And the band above them that carries the column headings. The two
        // are deliberately the same: measured from the cells outwards, the
        // border is then equal top and bottom, with each band holding text
        // rather than being empty space added to the margin. Making the top
        // its own extra allowance -- which it was, before there was anything
        // to put up there -- is exactly what made it look too deep.
        constexpr double kHeaderHeight = kStatusHeight;

        // How many icon clips may be created in one pass.
        //
        // Building all of them at once asks the engine to stream dozens of
        // symbols in a single frame, and that took the game down inside its
        // own asset job thread -- no plugin code in the backtrace at all.
        // The wheel never requests more than twelve. Creating a few per
        // pass and coming back for the rest keeps the peak near what the
        // engine already copes with, at the cost of icons fading in over a
        // moment rather than appearing together.
        constexpr std::size_t kIconsPerPass = 4;

        // Set while icons are still missing, so the polling loop knows to
        // ask for another pass.
        std::atomic_bool g_iconsIncomplete{ false };

        // Edit mode: rearranging favorites, which the wheel cannot do at all
        // because it only ever shows one page. Off by default so an ordinary
        // click keeps using the item rather than picking it up.
        std::atomic_bool g_editMode{ false };
        // The cell picked up first, waiting for somewhere to go. -1 = none.
        std::atomic_int g_editSourceBank{ -1 };
        std::atomic_int g_editSourceSlot{ -1 };

        // Big enough to aim at without covering the icon behind it.
        constexpr double kDeleteBoxSize = 20.0;

        // How much of a cell stays clear around its icon.
        constexpr double kIconInset = 5.0;

        // What each icon measured on the previous pass, by cell name.
        //
        // Only ever touched from UpdateGridOverlay, which already refuses to
        // run twice at once, so it needs no lock of its own.
        struct IconFit
        {
            // What the cell held when this fixture was built.
            std::string key;
            double width{ 0.0 };
            double height{ 0.0 };
            int passes{ 0 };
            bool reported{ false };
        };
        std::unordered_map<std::string, IconFit> g_iconFits;

        // Something changed and the grid should be redrawn on the next
        // frame the movie gives us.
        std::atomic_bool g_redrawRequested{ false };

        // Set the first time the frame ticker actually fires. Until then,
        // redraws still happen where they are asked for, because a grid
        // that never draws would be worse than one that draws unsafely.
        std::atomic_bool g_tickerAlive{ false };

        constexpr std::uint32_t kColorFrame = 0x6E7B8B;
        constexpr std::uint32_t kColorFill = 0x11161C;
        constexpr std::uint32_t kColorNativeRow = 0x1D2A36;
        constexpr std::uint32_t kColorPinned = 0x2A2113;
        constexpr std::uint32_t kColorButton = 0x14262A;
        constexpr std::uint32_t kColorEditOn = 0x3A2A12;
        constexpr std::uint32_t kColorSource = 0x2E4A2E;
        constexpr std::uint32_t kColorDelete = 0xE04A4A;
        constexpr std::uint32_t kColorText = 0xF4F4F2;

        [[nodiscard]] double ReadNumber(
            const RE::Scaleform::GFx::Value& object,
            const char* member,
            double fallback)
        {
            RE::Scaleform::GFx::Value value;
            if (!object.IsObject() || !object.GetMember(member, &value)) {
                return fallback;
            }
            if (value.IsNumber()) {
                return value.GetNumber();
            }
            if (value.IsInt()) {
                return static_cast<double>(value.GetInt());
            }
            if (value.IsUInt()) {
                return static_cast<double>(value.GetUInt());
            }
            return fallback;
        }

        [[nodiscard]] bool FindChild(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& parent,
            const char* name,
            RE::Scaleform::GFx::Value& child)
        {
            if (!root || !parent.IsObject()) {
                return false;
            }
            RE::Scaleform::GFx::Value nameValue;
            root->CreateString(&nameValue, name);
            return parent.Invoke("getChildByName", &child, &nameValue, 1) &&
                child.IsDisplayObject();
        }

        [[nodiscard]] bool AddChild(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& parent,
            const char* className,
            const char* instanceName,
            RE::Scaleform::GFx::Value& child)
        {
            if (!root || !parent.IsObject()) {
                return false;
            }
            root->CreateObject(&child, className);
            if (!child.IsDisplayObject()) {
                return false;
            }
            RE::Scaleform::GFx::Value nameValue;
            root->CreateString(&nameValue, instanceName);
            child.SetMember("name", nameValue);
            return parent.Invoke("addChild", nullptr, &child, 1);
        }

        // Defined further down, next to the rest of the text handling.
        // Declared here because the status line needs them before that.
        [[nodiscard]] bool EnsureLabel(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            const std::string& name,
            double x,
            double y,
            double width,
            std::string_view text,
            RE::Scaleform::GFx::Value& label,
            const char* align = nullptr);

        // Which display classes this Scaleform build will actually construct.
        // Reported once: the answer decides whether the grid can render real
        // item icons or has to fall back to names, and guessing it wrong
        // means designing a UI that cannot be built.
        void ProbeCreatableClasses(RE::Scaleform::GFx::ASMovieRootBase* root)
        {
            static std::atomic_bool probed{ false };
            if (!root || probed.exchange(true)) {
                return;
            }
            static constexpr std::array<const char*, 8> candidates{
                "flash.display.Sprite",
                "flash.display.MovieClip",
                "flash.text.TextField",
                "flash.text.TextFormat",
                "Components.ImageFixture",
                "FavoritesEntry",
                // If these exist, an icon could be loaded once, copied into
                // a bitmap and shown from there -- freeing the icon buffer
                // instead of holding dozens of live fixtures.
                "flash.display.BitmapData",
                "flash.display.Bitmap"
            };
            for (const auto* name : candidates) {
                RE::Scaleform::GFx::Value probe;
                root->CreateObject(&probe, name);
                REX::INFO(
                    "Grid probe: {:<26} object={} displayObject={}",
                    name,
                    probe.IsObject(),
                    probe.IsDisplayObject());
            }
        }

        struct GridGeometry
        {
            // One entry of the right-hand strip: either a pinned favorite
            // slot or a configured button, stacked in that order.
            enum class ExtraKind
            {
                kPinnedSlot,
                kEditToggle
            };

            struct Extra
            {
                ExtraKind kind{ ExtraKind::kPinnedSlot };
                // Slot index, for kPinnedSlot.
                std::size_t index{ 0 };
            };

            std::size_t rows{ 0 };
            std::vector<std::size_t> pageSlots;   // slot index per column
            std::vector<std::size_t> pinnedSlots; // slot index per pin cell
            std::vector<Extra> extras;            // the right-hand strip
            double gridWidth{ 0.0 };
            double pinWidth{ 0.0 };
            double totalWidth{ 0.0 };
            double totalHeight{ 0.0 };
        };

        [[nodiscard]] GridGeometry ComputeGeometry()
        {
            GridGeometry geometry;
            geometry.rows = g_settings.rowCount;
            for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                if (g_settings.externallyManagedSlots[slot]) {
                    geometry.pinnedSlots.push_back(slot);
                } else {
                    geometry.pageSlots.push_back(slot);
                }
            }
            // The right-hand strip is one column of ordinary cells stacked
            // downwards: the pinned slots first, then the configured
            // buttons. A pinned slot spanning every row said "this applies
            // to all pages", which is true, but it also made it the largest
            // thing on screen for what is one slot among twelve.
            for (const auto slot : geometry.pinnedSlots) {
                geometry.extras.push_back(GridGeometry::Extra{
                    GridGeometry::ExtraKind::kPinnedSlot, slot });
            }
            // The edit toggle always sits at the end of the strip.
            //
            // There were two more buttons here for a while, ITEMS and
            // POWERS, to open the menus where a favorite is created. They
            // are gone: see section 5 of HANDOFF.md. Showing InventoryMenu
            // through the UI message queue takes the game down even with
            // the wheel fully closed and committed first, so the menu needs
            // something the message does not carry. Both menus are one key
            // away in the game anyway.
            geometry.extras.push_back(GridGeometry::Extra{
                GridGeometry::ExtraKind::kEditToggle, 0 });
            const auto stride = kCellSize + kCellGap;
            // The trailing gap after the last cell is not part of the
            // content: counting it left a phantom column of empty space on
            // the right and an extra row's worth at the bottom.
            geometry.gridWidth = kLabelWidth +
                stride * static_cast<double>(geometry.pageSlots.size()) -
                kCellGap;
            // One column wide, however many entries it holds.
            geometry.pinWidth = geometry.extras.empty() ?
                0.0 : kPinGap + kCellSize;
            geometry.totalWidth = geometry.gridWidth + geometry.pinWidth;
            // The strip may be taller than the pages if there are more
            // extras than banks, so the backdrop follows whichever is taller.
            const auto rowsNeeded = std::max(
                geometry.rows, geometry.extras.size());
            geometry.totalHeight = kHeaderHeight +
                stride * static_cast<double>(rowsNeeded) - kCellGap;
            return geometry;
        }

        void DrawRect(
            RE::Scaleform::GFx::Value& graphics,
            double x,
            double y,
            double width,
            double height,
            std::uint32_t color,
            double alpha)
        {
            const std::array fill{
                RE::Scaleform::GFx::Value(static_cast<double>(color)),
                RE::Scaleform::GFx::Value(alpha)
            };
            graphics.Invoke("beginFill", nullptr, fill.data(), fill.size());
            const std::array rect{
                RE::Scaleform::GFx::Value(x),
                RE::Scaleform::GFx::Value(y),
                RE::Scaleform::GFx::Value(width),
                RE::Scaleform::GFx::Value(height)
            };
            graphics.Invoke("drawRect", nullptr, rect.data(), rect.size());
            graphics.Invoke("endFill");
        }

        void StrokeRect(
            RE::Scaleform::GFx::Value& graphics,
            double x,
            double y,
            double width,
            double height,
            std::uint32_t color,
            double alpha)
        {
            const std::array stroke{
                RE::Scaleform::GFx::Value(1.0),
                RE::Scaleform::GFx::Value(static_cast<double>(color)),
                RE::Scaleform::GFx::Value(alpha)
            };
            graphics.Invoke(
                "lineStyle", nullptr, stroke.data(), stroke.size());
            const std::array rect{
                RE::Scaleform::GFx::Value(x),
                RE::Scaleform::GFx::Value(y),
                RE::Scaleform::GFx::Value(width),
                RE::Scaleform::GFx::Value(height)
            };
            graphics.Invoke("drawRect", nullptr, rect.data(), rect.size());
            const std::array clear{ RE::Scaleform::GFx::Value(0.0) };
            graphics.Invoke("lineStyle", nullptr, clear.data(), clear.size());
        }

        // The delete corner of a cell in edit mode.
        //
        // It used to be a filled red square, which reads as a status light
        // rather than as "remove this". The hit area is unchanged; only what
        // is drawn inside it is.
        void DrawCross(
            RE::Scaleform::GFx::Value& graphics,
            double x,
            double y,
            double size,
            std::uint32_t color,
            double alpha)
        {
            const std::array stroke{
                RE::Scaleform::GFx::Value(2.0),
                RE::Scaleform::GFx::Value(static_cast<double>(color)),
                RE::Scaleform::GFx::Value(alpha)
            };
            graphics.Invoke(
                "lineStyle", nullptr, stroke.data(), stroke.size());
            constexpr double inset = 4.0;
            const auto left = x + inset;
            const auto right = x + size - inset;
            const auto top = y + inset;
            const auto bottom = y + size - inset;
            const auto line = [&graphics](
                double fromX, double fromY, double toX, double toY) {
                const std::array from{
                    RE::Scaleform::GFx::Value(fromX),
                    RE::Scaleform::GFx::Value(fromY)
                };
                graphics.Invoke("moveTo", nullptr, from.data(), from.size());
                const std::array to{
                    RE::Scaleform::GFx::Value(toX),
                    RE::Scaleform::GFx::Value(toY)
                };
                graphics.Invoke("lineTo", nullptr, to.data(), to.size());
            };
            line(left, top, right, bottom);
            line(right, top, left, bottom);
            const std::array clear{ RE::Scaleform::GFx::Value(0.0) };
            graphics.Invoke("lineStyle", nullptr, clear.data(), clear.size());
        }

        // Stroked shapes, for the symbols below.
        void BeginStroke(
            RE::Scaleform::GFx::Value& graphics,
            double thickness,
            std::uint32_t color,
            double alpha)
        {
            const std::array stroke{
                RE::Scaleform::GFx::Value(thickness),
                RE::Scaleform::GFx::Value(static_cast<double>(color)),
                RE::Scaleform::GFx::Value(alpha)
            };
            graphics.Invoke(
                "lineStyle", nullptr, stroke.data(), stroke.size());
        }

        void EndStroke(RE::Scaleform::GFx::Value& graphics)
        {
            const std::array clear{ RE::Scaleform::GFx::Value(0.0) };
            graphics.Invoke("lineStyle", nullptr, clear.data(), clear.size());
        }

        void PenTo(
            RE::Scaleform::GFx::Value& graphics,
            const char* verb,
            double x,
            double y)
        {
            const std::array point{
                RE::Scaleform::GFx::Value(x), RE::Scaleform::GFx::Value(y)
            };
            graphics.Invoke(verb, nullptr, point.data(), point.size());
        }

        void CurveTo(
            RE::Scaleform::GFx::Value& graphics,
            double controlX,
            double controlY,
            double toX,
            double toY)
        {
            const std::array arc{
                RE::Scaleform::GFx::Value(controlX),
                RE::Scaleform::GFx::Value(controlY),
                RE::Scaleform::GFx::Value(toX),
                RE::Scaleform::GFx::Value(toY)
            };
            graphics.Invoke("curveTo", nullptr, arc.data(), arc.size());
        }

        // A deployed turret: splayed legs, a body, a barrel.
        void DrawTurretSymbol(
            RE::Scaleform::GFx::Value& graphics,
            double x,
            double y,
            double size,
            std::uint32_t color)
        {
            const auto u = size / 44.0;      // the shapes are drawn at 44
            const auto px = [&](double v) { return x + v * u; };
            const auto py = [&](double v) { return y + v * u; };
            BeginStroke(graphics, 2.0, color, 1.0);
            // Legs.
            PenTo(graphics, "moveTo", px(8.0), py(40.0));
            PenTo(graphics, "lineTo", px(18.0), py(28.0));
            PenTo(graphics, "moveTo", px(36.0), py(40.0));
            PenTo(graphics, "lineTo", px(26.0), py(28.0));
            PenTo(graphics, "moveTo", px(22.0), py(40.0));
            PenTo(graphics, "lineTo", px(22.0), py(28.0));
            // Body.
            const std::array body{
                RE::Scaleform::GFx::Value(px(13.0)),
                RE::Scaleform::GFx::Value(py(14.0)),
                RE::Scaleform::GFx::Value(18.0 * u),
                RE::Scaleform::GFx::Value(14.0 * u)
            };
            graphics.Invoke("drawRect", nullptr, body.data(), body.size());
            // Barrel.
            PenTo(graphics, "moveTo", px(31.0), py(20.0));
            PenTo(graphics, "lineTo", px(42.0), py(20.0));
            // Sensor.
            PenTo(graphics, "moveTo", px(18.0), py(10.0));
            PenTo(graphics, "lineTo", px(18.0), py(14.0));
            PenTo(graphics, "moveTo", px(14.0), py(8.0));
            PenTo(graphics, "lineTo", px(22.0), py(8.0));
            EndStroke(graphics);
        }

        // The recall: a handheld transmitter with its signal going out.
        void DrawRecallSymbol(
            RE::Scaleform::GFx::Value& graphics,
            double x,
            double y,
            double size,
            std::uint32_t color)
        {
            const auto u = size / 44.0;
            const auto px = [&](double v) { return x + v * u; };
            const auto py = [&](double v) { return y + v * u; };
            BeginStroke(graphics, 2.0, color, 1.0);
            // The transmitter body.
            const std::array body{
                RE::Scaleform::GFx::Value(px(10.0)),
                RE::Scaleform::GFx::Value(py(18.0)),
                RE::Scaleform::GFx::Value(16.0 * u),
                RE::Scaleform::GFx::Value(24.0 * u)
            };
            graphics.Invoke("drawRect", nullptr, body.data(), body.size());
            // Its button.
            const std::array button{
                RE::Scaleform::GFx::Value(px(15.0)),
                RE::Scaleform::GFx::Value(py(23.0)),
                RE::Scaleform::GFx::Value(6.0 * u),
                RE::Scaleform::GFx::Value(5.0 * u)
            };
            graphics.Invoke("drawRect", nullptr, button.data(), button.size());
            // Antenna.
            PenTo(graphics, "moveTo", px(22.0), py(18.0));
            PenTo(graphics, "lineTo", px(28.0), py(8.0));
            // Two waves leaving it.
            PenTo(graphics, "moveTo", px(31.0), py(6.0));
            CurveTo(graphics, px(37.0), py(11.0), px(34.0), py(18.0));
            PenTo(graphics, "moveTo", px(35.0), py(2.0));
            CurveTo(graphics, px(44.0), py(11.0), px(39.0), py(21.0));
            EndStroke(graphics);
        }

        // Which symbol, if any, a slot's item asks for.
        //
        // Matched on the editor ID rather than configured: the pair that
        // shares such a slot names itself, and a mod that means something
        // else simply matches nothing and keeps the ordinary icon.
        enum class PinnedSymbol
        {
            kNone,
            kTurret,
            kRecall
        };

        [[nodiscard]] PinnedSymbol SymbolFor(const FavoriteSlot& slot)
        {
            if (!g_settings.gridPinnedSymbols || slot.Empty()) {
                return PinnedSymbol::kNone;
            }
            std::string id = slot.form.editorID;
            std::ranges::transform(
                id, id.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            if (id.find("recall") != std::string::npos) {
                return PinnedSymbol::kRecall;
            }
            if (id.find("turret") != std::string::npos) {
                return PinnedSymbol::kTurret;
            }
            return PinnedSymbol::kNone;
        }

        // Hands a cell to the wheel's own selection path.
        //
        // FavoritesMenu.ProcessUserEvent is public and is what the gameplay
        // "Quickkey1".."Quickkey12" events arrive through: it sets the
        // selection and calls SelectItem, which dispatches the vanilla
        // assign/use event exactly as a keypress would. Driving that instead
        // of reimplementing selection means the grid inherits every guard
        // already in place, including the plugin's own callback that refuses
        // a selection whose page is not the one in the real slots.
        //
        // The page has to be switched first and synchronously: the vanilla
        // event carries nothing but a slot number, so it acts on whichever
        // page occupies the native slots at that instant. This runs inside a
        // Scaleform callback on the game thread, which is where SwitchBank
        // expects to be called from.
        void SelectGridCell(std::size_t bank, std::size_t slot)
        {
            if (bank >= g_settings.rowCount ||
                slot >= kSlotsPerBank) {
                return;
            }

            bool onPage = false;
            {
                std::scoped_lock lock(g_stateMutex);
                onPage = g_activeBank == bank;
            }
            if (!onPage && !SwitchBank(bank)) {
                REX::WARN(
                    "Grid could not switch to page {}; selection ignored",
                    bank + 1);
                return;
            }

            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return;
            }
            static const RE::BSFixedString favoritesMenu("FavoritesMenu");
            auto menu = ui->GetMenu(favoritesMenu);
            if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot ||
                !menu->menuObj.IsObject()) {
                return;
            }

            auto* root = menu->uiMovie->asMovieRoot.get();
            RE::Scaleform::GFx::Value eventName;
            root->CreateString(
                &eventName, std::format("Quickkey{}", slot + 1).c_str());
            const std::array args{
                eventName,
                RE::Scaleform::GFx::Value(false)
            };
            RE::Scaleform::GFx::Value handled;
            if (!menu->menuObj.Invoke(
                    "ProcessUserEvent", &handled, args.data(), args.size())) {
                REX::WARN(
                    "Grid selection of page {} slot {} was refused by the wheel",
                    bank + 1,
                    slot + 1);
                return;
            }
            REX::INFO(
                "Grid selected page {} slot {}", bank + 1, slot + 1);
        }

        // Which cell the cursor is over, so the move handler can do nothing
        // on the overwhelming majority of events. mouseMove fires per frame
        // of cursor motion; rebuilding anything per event would be wasteful.
        std::atomic_int g_hoveredRow{ -1 };
        std::atomic_int g_hoveredSlot{ -1 };
        // Whether that cell is in the right-hand strip, where the row is
        // an entry index rather than a page.
        std::atomic_bool g_hoveredPinned{ false };

        // Resolves a point in overlay space to a cell. Returns false when
        // the point is between cells or outside the grid entirely.
        [[nodiscard]] bool CellAt(
            double localX,
            double localY,
            const GridGeometry& geometry,
            std::size_t& row,
            std::size_t& slot,
            bool& pinned)
        {
            const auto stride = kCellSize + kCellGap;
            if (localY < kHeaderHeight) {
                return false;
            }
            const auto rowIndex = static_cast<int>(
                (localY - kHeaderHeight) / stride);
            if (rowIndex < 0) {
                return false;
            }

            // The page rows end where the pages do; the strip does not. Two
            // pinned slots and three buttons on four pages already reach
            // past the last row, and a check against the page count alone
            // would leave the bottom of the strip unclickable.
            if (rowIndex < static_cast<int>(geometry.rows) &&
                localX >= kLabelWidth && localX < geometry.gridWidth) {
                const auto column = static_cast<std::size_t>(
                    (localX - kLabelWidth) / stride);
                if (column < geometry.pageSlots.size()) {
                    row = static_cast<std::size_t>(rowIndex);
                    slot = geometry.pageSlots[column];
                    pinned = false;
                    return true;
                }
            }
            // The strip is a single column, so the row decides which entry
            // was hit, not the column.
            if (!geometry.extras.empty() &&
                localX >= geometry.gridWidth + kPinGap &&
                localX < geometry.gridWidth + kPinGap + kCellSize &&
                static_cast<std::size_t>(rowIndex) < geometry.extras.size()) {
                row = static_cast<std::size_t>(rowIndex);
                slot = geometry.extras[row].index;
                pinned = true;
                return true;
            }
            return false;
        }

        // The grid's own status line, drawn inside the overlay just under
        // the cells.
        //
        // The wheel's ItemInfo_mc is still fed as well, but it cannot be
        // relied on alone: it belongs to the menu, the overlay is a later
        // child of the stage, so the overlay is always drawn on top of it.
        // Moving the panel out from under the grid put it somewhere its own
        // layout did not expect and it simply stopped appearing. Owning the
        // line means it is always where the grid says it is.
        void SetStatusText(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            std::string_view text)
        {
            const auto geometry = ComputeGeometry();
            RE::Scaleform::GFx::Value status;
            static_cast<void>(EnsureLabel(
                root, overlay, "GridStatus",
                0.0, geometry.totalHeight + kPadding * 0.5,
                geometry.totalWidth, text, status));
        }

        // Feeds the wheel's own info panel. Reusing ItemInfo_mc means the
        // grid describes an item exactly the way the wheel does, including
        // ammo, mods and elemental stats, without a second implementation.
        void ShowCellInfo(
            RE::IMenu* menu,
            RE::Scaleform::GFx::ASMovieRootBase* root,
            const FavoriteSlot* slot)
        {
            RE::Scaleform::GFx::Value info;
            if (!menu->menuObj.GetMember("ItemInfo_mc", &info) ||
                !info.IsDisplayObject()) {
                return;
            }
            if (!slot || slot->Empty()) {
                info.SetMember("visible", RE::Scaleform::GFx::Value(false));
                return;
            }
            RE::Scaleform::GFx::Value description;
            BuildFavoriteInfoForDisplay(root, *slot, description);
            static_cast<void>(info.Invoke(
                "UpdateDisplay", nullptr, &description, 1));
            info.SetMember("visible", RE::Scaleform::GFx::Value(true));
        }

        // The wheel's own selection, which is what a controller moves.
        //
        // FavoritesMenu.as exposes selectedIndex as a public property, so
        // the grid can simply read it -- no input has to be intercepted,
        // and the vanilla D-pad keeps writing it. FS_NONE is 12 and means
        // nothing is selected.
        constexpr std::size_t kNoSelection = 12;

        // Last selection the ticker saw, so a change can be noticed.
        std::atomic_int g_selectedIndex{ -1 };

        [[nodiscard]] std::size_t ReadSelectedIndex(RE::IMenu* menu)
        {
            if (!menu || !menu->menuObj.IsObject()) {
                return kNoSelection;
            }
            const auto raw = ReadNumber(
                menu->menuObj, "selectedIndex",
                static_cast<double>(kNoSelection));
            if (!(raw >= 0.0) || raw >= static_cast<double>(kNoSelection)) {
                return kNoSelection;
            }
            return static_cast<std::size_t>(raw);
        }

        // Deliberately a different shape from the mouse highlight: an outline
        // rather than a fill, so that with both on screen it stays obvious
        // which one the controller is driving.
        void MoveSelection(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            bool visible,
            double x = 0.0,
            double y = 0.0)
        {
            RE::Scaleform::GFx::Value marker;
            if (!FindChild(root, overlay, "GridSelection", marker)) {
                if (!AddChild(
                        root, overlay, "flash.display.Sprite",
                        "GridSelection", marker)) {
                    return;
                }
                marker.SetMember(
                    "mouseEnabled", RE::Scaleform::GFx::Value(false));
            }
            marker.SetMember("visible", RE::Scaleform::GFx::Value(visible));
            if (!visible) {
                return;
            }
            marker.SetMember("x", RE::Scaleform::GFx::Value(x));
            marker.SetMember("y", RE::Scaleform::GFx::Value(y));
            RE::Scaleform::GFx::Value graphics;
            if (!marker.GetMember("graphics", &graphics) ||
                !graphics.IsObject()) {
                return;
            }
            graphics.Invoke("clear");
            StrokeRect(
                graphics, -2.0, -2.0, kCellSize + 4.0, kCellSize + 4.0,
                kColorText, 0.95);
            StrokeRect(
                graphics, -1.0, -1.0, kCellSize + 2.0, kCellSize + 2.0,
                kColorText, 0.55);
        }

        void MoveHighlight(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            bool visible,
            double x = 0.0,
            double y = 0.0,
            double height = kCellSize)
        {
            RE::Scaleform::GFx::Value highlight;
            if (!FindChild(root, overlay, "GridHighlight", highlight)) {
                if (!AddChild(
                        root, overlay, "flash.display.Sprite",
                        "GridHighlight", highlight)) {
                    return;
                }
                highlight.SetMember(
                    "mouseEnabled", RE::Scaleform::GFx::Value(false));
            }
            highlight.SetMember("visible", RE::Scaleform::GFx::Value(visible));
            if (!visible) {
                return;
            }
            highlight.SetMember("x", RE::Scaleform::GFx::Value(x));
            highlight.SetMember("y", RE::Scaleform::GFx::Value(y));

            // Redrawn rather than merely moved, because the pinned strip is
            // taller than a page cell.
            RE::Scaleform::GFx::Value graphics;
            if (!highlight.GetMember("graphics", &graphics) ||
                !graphics.IsObject()) {
                return;
            }
            graphics.Invoke("clear");
            DrawRect(graphics, 0.0, 0.0, kCellSize, height, kColorText, 0.22);
            StrokeRect(graphics, 0.0, 0.0, kCellSize, height, kColorText, 0.9);
        }

        // Lights the delete corner while the cursor is actually inside it.
        //
        // A 20 pixel corner is easy to miss, and until it is hit nothing
        // happens -- which reads as the delete not working rather than as
        // the cursor being a few pixels off. This is the feedback that was
        // missing: hovering it fills the corner in, so the click that
        // deletes is the click that looks like it will.
        void MoveDeleteHint(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            bool visible,
            double x = 0.0,
            double y = 0.0)
        {
            RE::Scaleform::GFx::Value hint;
            if (!FindChild(root, overlay, "GridDeleteHint", hint)) {
                if (!AddChild(
                        root, overlay, "flash.display.Sprite",
                        "GridDeleteHint", hint)) {
                    return;
                }
                hint.SetMember(
                    "mouseEnabled", RE::Scaleform::GFx::Value(false));
            }
            hint.SetMember("visible", RE::Scaleform::GFx::Value(visible));
            if (!visible) {
                return;
            }
            hint.SetMember("x", RE::Scaleform::GFx::Value(x));
            hint.SetMember("y", RE::Scaleform::GFx::Value(y));
            RE::Scaleform::GFx::Value graphics;
            if (!hint.GetMember("graphics", &graphics) ||
                !graphics.IsObject()) {
                return;
            }
            graphics.Invoke("clear");
            DrawRect(
                graphics, 0.0, 0.0, kDeleteBoxSize, kDeleteBoxSize,
                kColorDelete, 0.85);
            DrawCross(
                graphics, 0.0, 0.0, kDeleteBoxSize, kColorText, 1.0);
        }

        // Where the delete corner of a page cell sits, in overlay space.
        [[nodiscard]] bool DeleteCornerOf(
            const GridGeometry& geometry,
            std::size_t row,
            std::size_t slot,
            double& outX,
            double& outY)
        {
            const auto found = std::ranges::find(geometry.pageSlots, slot);
            if (found == geometry.pageSlots.end()) {
                return false;
            }
            const auto column = static_cast<double>(
                std::distance(geometry.pageSlots.begin(), found));
            const auto stride = kCellSize + kCellGap;
            outX = kLabelWidth + stride * column + kCellSize - kDeleteBoxSize;
            outY = kHeaderHeight + stride * static_cast<double>(row);
            return true;
        }

        std::atomic_bool g_hoveredDelete{ false };

        class GridHoverHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1 || !params.args[0].IsObject()) {
                    return;
                }
                const auto geometry = ComputeGeometry();
                std::size_t row = 0;
                std::size_t slot = 0;
                bool pinned = false;
                const auto over = CellAt(
                    ReadNumber(params.args[0], "localX", -1.0),
                    ReadNumber(params.args[0], "localY", -1.0),
                    geometry, row, slot, pinned);

                const auto newRow = over ? static_cast<int>(row) : -1;
                const auto newSlot = over ? static_cast<int>(slot) : -1;

                // Whether the cursor is in the delete corner has to be part
                // of the change test: moving inside one cell is exactly the
                // movement that arms and disarms it, and the old test saw
                // that as "still the same cell, nothing to do".
                const auto localX = ReadNumber(params.args[0], "localX", -1.0);
                const auto localY = ReadNumber(params.args[0], "localY", -1.0);
                double cornerX = 0.0;
                double cornerY = 0.0;
                auto newDelete = false;
                if (over && !pinned && g_editMode.load() &&
                    DeleteCornerOf(geometry, row, slot, cornerX, cornerY)) {
                    std::scoped_lock lock(g_stateMutex);
                    newDelete = !g_banks[row][slot].Empty() &&
                        localX >= cornerX &&
                        localX < cornerX + kDeleteBoxSize &&
                        localY >= cornerY &&
                        localY < cornerY + kDeleteBoxSize;
                }
                const auto deleteChanged =
                    g_hoveredDelete.exchange(newDelete) != newDelete;
                if (!deleteChanged &&
                    g_hoveredRow.exchange(newRow) == newRow &&
                    g_hoveredSlot.exchange(newSlot) == newSlot) {
                    return;
                }
                g_hoveredRow.store(newRow);
                g_hoveredSlot.store(newSlot);
                g_hoveredPinned.store(over && pinned);

                auto* ui = RE::UI::GetSingleton();
                if (!ui) {
                    return;
                }
                static const RE::BSFixedString favoritesMenu("FavoritesMenu");
                auto menu = ui->GetMenu(favoritesMenu);
                if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot ||
                    !menu->menuObj.IsObject()) {
                    return;
                }
                auto* root = menu->uiMovie->asMovieRoot.get();
                RE::Scaleform::GFx::Value stage;
                RE::Scaleform::GFx::Value overlay;
                if (!menu->menuObj.GetMember("stage", &stage) ||
                    !FindChild(root, stage, "FavoritesBanksGrid", overlay)) {
                    return;
                }

                MoveDeleteHint(
                    root, overlay, newDelete, cornerX, cornerY);
                if (!over) {
                    MoveHighlight(root, overlay, false);
                    ShowCellInfo(menu.get(), root, nullptr);
                    SetStatusText(root, overlay, "");
                    return;
                }

                const auto stride = kCellSize + kCellGap;
                // A button's entry carries no slot index, so only a real
                // pinned entry may be looked up.
                const auto stripKind =
                    pinned && row < geometry.extras.size() ?
                        geometry.extras[row].kind :
                        GridGeometry::ExtraKind::kPinnedSlot;
                const auto realPinned =
                    pinned && stripKind == GridGeometry::ExtraKind::kPinnedSlot;
                FavoriteSlot hovered;
                {
                    std::scoped_lock lock(g_stateMutex);
                    if (realPinned) {
                        hovered = g_pinnedSlots[slot];
                    } else if (!pinned) {
                        hovered = g_banks[row][slot];
                    }
                }

                if (pinned) {
                    // The strip is one column of ordinary cells now, and the
                    // row is already its index, so no lookup is needed. The
                    // old code searched pinnedSlots for what could also be a
                    // button index and highlighted a full-height column.
                    MoveHighlight(
                        root, overlay, true,
                        geometry.gridWidth + kPinGap,
                        kHeaderHeight + stride * static_cast<double>(row),
                        kCellSize);
                } else {
                    const auto column = static_cast<std::size_t>(std::distance(
                        geometry.pageSlots.begin(),
                        std::ranges::find(geometry.pageSlots, slot)));
                    MoveHighlight(
                        root, overlay, true,
                        kLabelWidth + stride * static_cast<double>(column),
                        kHeaderHeight + stride * static_cast<double>(row));
                }
                ShowCellInfo(menu.get(), root, &hovered);

                // The full name, not the abbreviated cell label, plus what
                // acting on this cell would actually do.
                std::string status;
                if (pinned) {
                    const auto extraIndex = static_cast<std::size_t>(row);
                    const auto kind = extraIndex < geometry.extras.size() ?
                        geometry.extras[extraIndex].kind :
                        GridGeometry::ExtraKind::kPinnedSlot;
                    const auto editing = g_editMode.load();
                    if (kind == GridGeometry::ExtraKind::kEditToggle) {
                        // Reading gridButtons[extra.index] here was the
                        // crash: the edit toggle carried a sentinel index
                        // far outside the array.
                        status = editing ?
                            "Edit mode: click a favorite to pick it up, "
                            "then click where it should go; the red X "
                            "deletes" :
                            "Edit mode: rearrange and delete favorites";
                    } else if (!hovered.Empty()) {
                        status = hovered.visual.name +
                            "  -  managed by another mod, on every page";
                    }
                } else if (!hovered.Empty()) {
                    status = std::format(
                        "{}   (page {}, slot {}){}",
                        !hovered.visual.name.empty() ?
                            hovered.visual.name : hovered.form.editorID,
                        row + 1,
                        slot + 1,
                        hovered.unresolved ?
                            "   -  not in your inventory right now" : "");
                }
                if (newDelete) {
                    status = "Click to delete this favorite.   " + status;
                }
                // Picking something up is easy to miss -- the source cell
                // turns green and nothing else says so -- and then the
                // second click looks like it does nothing in particular.
                const auto heldBank = g_editSourceBank.load();
                const auto heldSlot = g_editSourceSlot.load();
                if (heldBank >= 0 && heldSlot >= 0) {
                    std::string held;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        const auto& source =
                            g_banks[static_cast<std::size_t>(heldBank)]
                                   [static_cast<std::size_t>(heldSlot)];
                        held = !source.visual.name.empty() ?
                            source.visual.name : source.form.editorID;
                    }
                    status = std::format(
                        "HOLDING {}  -  click any cell to put it there.   {}",
                        held,
                        status);
                }
                SetStatusText(root, overlay, status);
            }
        };

        void ToggleEditMode()
        {
            const auto now = !g_editMode.exchange(!g_editMode.load());
            g_editMode.store(now);
            g_editSourceBank.store(-1);
            g_editSourceSlot.store(-1);
            REX::INFO("Grid edit mode {}", now ? "on" : "off");
            if (const auto* tasks = SFSE::GetTaskInterface()) {
                tasks->AddTask([]() { UpdateGridOverlay(); });
            }
        }

        // Opens one of the game's own menus, closing the wheel first so it
        // does not sit underneath. An empty slot can only be filled from
        // there -- favorites are created by favoriting an item or a power,
        // never by the wheel itself -- so this saves leaving the grid,
        // finding the menu and coming back.
        // In edit mode a click either deletes (the corner box), picks a cell
        // up, or drops the held one onto this cell. Nothing is used, which
        // is the whole point: rearranging favorites in the wheel is not
        // possible at all, because it only ever shows one page.
        void HandleEditClick(
            std::size_t bank,
            std::size_t slot,
            double cellX,
            double cellY,
            double localX,
            double localY)
        {
            const auto inDeleteBox =
                localX >= cellX + kCellSize - kDeleteBoxSize &&
                localY <= cellY + kDeleteBoxSize;
            if (inDeleteBox) {
                const auto globalIndex = bank * kSlotsPerBank + slot;
                if (const auto* tasks = SFSE::GetTaskInterface()) {
                    tasks->AddTask([globalIndex]() {
                        ClearGridSlot(globalIndex);
                        UpdateGridOverlay();
                    });
                }
                return;
            }

            const auto sourceBank = g_editSourceBank.load();
            const auto sourceSlot = g_editSourceSlot.load();
            if (sourceBank < 0 || sourceSlot < 0) {
                // An empty cell with nothing held is a drop target waiting
                // for something to drop. Opening the inventory from here is
                // what the ITEMS and POWERS buttons are for: it needed a
                // right click the movie never reports, and nobody could see
                // that the option was there at all.
                bool empty = true;
                {
                    std::scoped_lock lock(g_stateMutex);
                    empty = g_banks[bank][slot].Empty();
                }
                if (empty) {
                    return;
                }
                g_editSourceBank.store(static_cast<int>(bank));
                g_editSourceSlot.store(static_cast<int>(slot));
                REX::INFO(
                    "Grid picked up page {} slot {}", bank + 1, slot + 1);
                if (const auto* tasks = SFSE::GetTaskInterface()) {
                    tasks->AddTask([]() { UpdateGridOverlay(); });
                }
                return;
            }

            g_editSourceBank.store(-1);
            g_editSourceSlot.store(-1);
            const auto fromBank = static_cast<std::size_t>(sourceBank);
            const auto fromSlot = static_cast<std::size_t>(sourceSlot);
            if (const auto* tasks = SFSE::GetTaskInterface()) {
                tasks->AddTask([fromBank, fromSlot, bank, slot]() {
                    static_cast<void>(
                        SwapStoredSlots(fromBank, fromSlot, bank, slot));
                    UpdateGridOverlay();
                });
            }
        }

        class GridClickHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1 || !params.args[0].IsObject()) {
                    return;
                }
                const auto localX = ReadNumber(params.args[0], "localX", -1.0);
                const auto localY = ReadNumber(params.args[0], "localY", -1.0);
                const auto geometry = ComputeGeometry();
                const auto stride = kCellSize + kCellGap;

                const auto row = static_cast<int>(
                    (localY - kHeaderHeight) / stride);
                if (localY < kHeaderHeight || row < 0) {
                    return;
                }

                if (row < static_cast<int>(geometry.rows) &&
                    localX >= kLabelWidth &&
                    localX < geometry.gridWidth) {
                    const auto column = static_cast<std::size_t>(
                        (localX - kLabelWidth) / stride);
                    if (column < geometry.pageSlots.size()) {
                        const auto bank = static_cast<std::size_t>(row);
                        const auto slot = geometry.pageSlots[column];
                        if (g_editMode.load()) {
                            HandleEditClick(
                                bank, slot,
                                kLabelWidth +
                                    stride * static_cast<double>(column),
                                kHeaderHeight +
                                    stride * static_cast<double>(row),
                                localX, localY);
                            return;
                        }
                        SelectGridCell(bank, slot);
                        return;
                    }
                }
                if (localX >= geometry.gridWidth + kPinGap &&
                    localX < geometry.gridWidth + kPinGap + kCellSize &&
                    static_cast<std::size_t>(row) < geometry.extras.size()) {
                    const auto& extra =
                        geometry.extras[static_cast<std::size_t>(row)];
                    if (extra.kind ==
                        GridGeometry::ExtraKind::kEditToggle) {
                        ToggleEditMode();
                        return;
                    }
                    // A pinned slot is the same on every page, so the page
                    // currently shown is as good as any.
                    std::size_t shown = 0;
                    {
                        std::scoped_lock lock(g_stateMutex);
                        shown = g_activeBank;
                    }
                    SelectGridCell(shown, extra.index);
                }
            }
        };

        // The movie's own heartbeat, and the only place the grid is drawn
        // once it is running.
        //
        // enterFrame is dispatched from inside the engine's advance of this
        // movie, which is by construction the thread and the moment at which
        // touching it is safe. Nothing here decides anything; it only asks
        // whether a redraw is owed.
        class GridTicker final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params&) override
            {
                if (!g_tickerAlive.exchange(true)) {
                    REX::INFO(
                        "Grid: the movie's frame ticker is running; drawing moved onto it");
                }
                // A controller moves the wheel's selection without emitting
                // anything the grid would otherwise hear: the D-pad writes
                // selectedIndex straight into the movie. Nothing asks for a
                // redraw, so the marker sat on whichever slot happened to be
                // selected when the overlay was last drawn. Watching the
                // property here is what makes it follow. One property read
                // per frame; the draw itself still only runs on a change.
                if (auto* ui = RE::UI::GetSingleton()) {
                    static const RE::BSFixedString favoritesMenu(
                        "FavoritesMenu");
                    if (auto menu = ui->GetMenu(favoritesMenu)) {
                        const auto selected =
                            static_cast<int>(ReadSelectedIndex(menu.get()));
                        if (g_selectedIndex.exchange(selected) != selected) {
                            g_redrawRequested.store(true);
                        }
                    }
                }
                if (!g_redrawRequested.load() &&
                    !g_iconsIncomplete.load()) {
                    return;
                }
                // The flag is cleared by the draw itself, once it holds the
                // draw guard. Clearing it here would lose the request
                // whenever a draw was already in flight.
                DrawGridOverlay();
            }
        };

        [[nodiscard]] bool BuildOverlay(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& stage,
            RE::Scaleform::GFx::Value& overlay)
        {
            // A fresh overlay means a fresh opening of the wheel, so the
            // icon measurements start over with it. The clips themselves
            // are gone with the old overlay.
            g_iconFits.clear();
            // And the remembered selection, or reopening on the same slot
            // would never look like a change and never draw the marker.
            g_selectedIndex.store(-1);
            // A new overlay carries a new ticker; the old one went with the
            // clip it was attached to.
            g_tickerAlive.store(false);
            // Edit mode belongs to one opening of the wheel. Coming back to
            // a grid that is still armed, with something still held from
            // minutes ago, is a good way to move a favorite by accident.
            g_editMode.store(false);
            g_editSourceBank.store(-1);
            g_editSourceSlot.store(-1);
            if (!AddChild(
                    root, stage, "flash.display.Sprite",
                    "FavoritesBanksGrid", overlay)) {
                REX::WARN("Favorites Menu Grid grid: stage refused the overlay sprite");
                return false;
            }
            RE::Scaleform::GFx::Value eventName;
            RE::Scaleform::GFx::Value callback;
            root->CreateString(&eventName, "click");
            root->CreateFunction(&callback, new GridClickHandler());
            const std::array args{ eventName, callback };
            if (!overlay.Invoke(
                    "addEventListener", nullptr, args.data(), args.size())) {
                REX::WARN("Favorites Menu Grid grid: click listener was refused");
            }

            RE::Scaleform::GFx::Value frameName;
            RE::Scaleform::GFx::Value frameCallback;
            root->CreateString(&frameName, "enterFrame");
            root->CreateFunction(&frameCallback, new GridTicker());
            const std::array frameArgs{ frameName, frameCallback };
            if (!overlay.Invoke(
                    "addEventListener", nullptr, frameArgs.data(),
                    frameArgs.size())) {
                REX::WARN(
                    "Favorites Menu Grid grid: the frame ticker was refused; drawing stays on the caller's thread");
            }

            RE::Scaleform::GFx::Value moveName;
            RE::Scaleform::GFx::Value moveCallback;
            root->CreateString(&moveName, "mouseMove");
            root->CreateFunction(&moveCallback, new GridHoverHandler());
            const std::array moveArgs{ moveName, moveCallback };
            if (!overlay.Invoke(
                    "addEventListener", nullptr, moveArgs.data(),
                    moveArgs.size())) {
                REX::WARN("Favorites Menu Grid grid: hover listener was refused");
            }

            overlay.SetMember("mouseEnabled", RE::Scaleform::GFx::Value(true));
            // Children stay out of the way so one listener sees every event:
            // dozens of entry clips each grabbing the cursor would only make
            // the same job harder.
            overlay.SetMember("mouseChildren", RE::Scaleform::GFx::Value(false));
            return true;
        }

        // Hides the wheel's twelve entries while the grid is up. The clips
        // stay in place and fully functional -- selection still runs through
        // them via ProcessUserEvent -- they simply are not drawn. ItemInfo_mc
        // is deliberately left visible, because the grid feeds it on hover.
        void SetWheelVisible(RE::IMenu* menu, bool visible)
        {
            static constexpr std::array<const char*, 2> extras{
                "CenterClip_mc", "SelectQuickslot_mc"
            };
            for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                RE::Scaleform::GFx::Value entry;
                const auto name = std::format("Entry_{}", index);
                RE::Scaleform::GFx::Value nameValue;
                if (!menu->uiMovie || !menu->uiMovie->asMovieRoot) {
                    return;
                }
                menu->uiMovie->asMovieRoot->CreateString(
                    &nameValue, name.c_str());
                if (menu->menuObj.Invoke(
                        "getChildByName", &entry, &nameValue, 1) &&
                    entry.IsDisplayObject()) {
                    entry.SetMember(
                        "visible", RE::Scaleform::GFx::Value(visible));
                }
            }
            for (const auto* name : extras) {
                RE::Scaleform::GFx::Value clip;
                if (menu->menuObj.GetMember(name, &clip) &&
                    clip.IsDisplayObject()) {
                    clip.SetMember(
                        "visible", RE::Scaleform::GFx::Value(visible));
                }
            }

            // The cross's backing plate and the rings around it are drawn
            // straight from the menu's timeline and have no names of their
            // own, so Flash gave them "instance1", "instance2" and so on.
            // There is nothing else to identify them by, which is why they
            // survived every attempt to hide the wheel by clip name. Walk
            // the children and hide exactly those.
            auto* root = menu->uiMovie ?
                menu->uiMovie->asMovieRoot.get() : nullptr;
            if (!root) {
                return;
            }
            const auto total = static_cast<int>(
                ReadNumber(menu->menuObj, "numChildren", 0.0));
            for (int index = 0; index < total; ++index) {
                RE::Scaleform::GFx::Value childIndex(
                    static_cast<double>(index));
                RE::Scaleform::GFx::Value child;
                if (!menu->menuObj.Invoke(
                        "getChildAt", &child, &childIndex, 1) ||
                    !child.IsDisplayObject()) {
                    continue;
                }
                RE::Scaleform::GFx::Value name;
                if (!child.GetMember("name", &name) || !name.IsString()) {
                    continue;
                }
                if (std::string_view(name.GetString())
                        .starts_with("instance")) {
                    child.SetMember(
                        "visible", RE::Scaleform::GFx::Value(visible));
                }
            }
        }

        // Defined below, next to the rest of the text handling.
        const char* PickFont(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay);

        // One text field per cell, created once and then only refilled.
        // Returns false as soon as Scaleform refuses a field, which is the
        // signal that the grid has to be built without text.
        bool EnsureLabel(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            const std::string& name,
            double x,
            double y,
            double width,
            std::string_view text,
            RE::Scaleform::GFx::Value& label,
            const char* align)
        {
            if (!FindChild(root, overlay, name.c_str(), label) &&
                !AddChild(
                    root, overlay, "flash.text.TextField", name.c_str(),
                    label)) {
                return false;
            }
            label.SetMember("x", RE::Scaleform::GFx::Value(x));
            label.SetMember("y", RE::Scaleform::GFx::Value(y));
            label.SetMember("width", RE::Scaleform::GFx::Value(width));
            label.SetMember("height", RE::Scaleform::GFx::Value(kCellSize));
            label.SetMember("selectable", RE::Scaleform::GFx::Value(false));
            label.SetMember("mouseEnabled", RE::Scaleform::GFx::Value(false));
            label.SetMember("multiline", RE::Scaleform::GFx::Value(true));
            label.SetMember("wordWrap", RE::Scaleform::GFx::Value(true));
            label.SetMember(
                "textColor",
                RE::Scaleform::GFx::Value(
                    static_cast<double>(kColorText)));

            // Without an explicit format the field renders at Scaleform's
            // default size, which is far too small to read. Only the size is
            // set: naming a font risks asking for one that is not embedded,
            // and an unavailable font draws nothing at all.
            RE::Scaleform::GFx::Value format;
            root->CreateObject(&format, "flash.text.TextFormat");
            if (format.IsObject()) {
                format.SetMember(
                    "size", RE::Scaleform::GFx::Value(kFontSize));
                format.SetMember(
                    "color",
                    RE::Scaleform::GFx::Value(
                        static_cast<double>(kColorText)));
                if (const auto* font = PickFont(root, overlay)) {
                    RE::Scaleform::GFx::Value fontName;
                    root->CreateString(&fontName, font);
                    format.SetMember("font", fontName);
                }
                if (align) {
                    RE::Scaleform::GFx::Value alignment;
                    root->CreateString(&alignment, align);
                    format.SetMember("align", alignment);
                }
                label.SetMember("defaultTextFormat", format);
            }

            RE::Scaleform::GFx::Value textValue;
            root->CreateString(&textValue, std::string(text).c_str());
            label.SetMember("text", textValue);

            // Applied again after the text, because defaultTextFormat only
            // governs text inserted afterwards in some Scaleform builds.
            if (format.IsObject()) {
                static_cast<void>(
                    label.Invoke("setTextFormat", nullptr, &format, 1));
            }
            return true;
        }

        // Item names are long ("Hitman Overcharged Calibrated AA BAR-32"),
        // and the front of one is rarely what tells it apart. The last two
        // words usually carry the actual weapon, so the label keeps the tail
        // and lets the hover panel show the full name.
        [[nodiscard]] std::string ShortLabel(const FavoriteSlot& slot)
        {
            if (slot.Empty()) {
                return {};
            }
            auto name = !slot.visual.name.empty() ?
                slot.visual.name : slot.form.editorID;
            if (name.size() <= 22) {
                return name;
            }
            const auto lastSpace = name.rfind(' ');
            if (lastSpace != std::string::npos && lastSpace > 0) {
                const auto secondLast = name.rfind(' ', lastSpace - 1);
                if (secondLast != std::string::npos) {
                    return ".." + name.substr(secondLast);
                }
                return ".." + name.substr(lastSpace);
            }
            return name.substr(0, 21) + ".";
        }

        // Reports every child of the menu once, so leftovers that are still
        // drawn under the grid can be named instead of guessed at. The
        // wheel's cross and the rings around it come from somewhere, and
        // "somewhere" is not something to hide by trial and error.
        void ReportMenuChildren(
            RE::IMenu* menu,
            RE::Scaleform::GFx::ASMovieRootBase* root)
        {
            static std::atomic_bool reported{ false };
            if (reported.exchange(true)) {
                return;
            }
            // numChildren comes back as an int, not a Number, so the old
            // IsNumber() check silently rejected it and the whole listing
            // never ran. ReadNumber accepts every numeric shape.
            const auto total = static_cast<int>(
                ReadNumber(menu->menuObj, "numChildren", -1.0));
            if (total < 0) {
                REX::WARN("Grid: could not read numChildren from the menu");
                return;
            }
            REX::INFO("Grid: favorites menu has {} child(ren)", total);
            for (int index = 0; index < total; ++index) {
                RE::Scaleform::GFx::Value childIndex(
                    static_cast<double>(index));
                RE::Scaleform::GFx::Value child;
                if (!menu->menuObj.Invoke(
                        "getChildAt", &child, &childIndex, 1) ||
                    !child.IsDisplayObject()) {
                    continue;
                }
                RE::Scaleform::GFx::Value name;
                RE::Scaleform::GFx::Value visible;
                child.GetMember("name", &name);
                child.GetMember("visible", &visible);
                REX::INFO(
                    "Grid: child[{}] '{}' visible={}",
                    index,
                    name.IsString() ? name.GetString() : "?",
                    visible.IsBoolean() ? visible.GetBoolean() : true);
            }
            static_cast<void>(root);
        }

        // Which font the labels use.
        //
        // Leaving the font unset falls back to whatever Scaleform picks,
        // and that fallback renders as a dot matrix here -- unreadable. So
        // the name has to be given, but naming a font the movie does not
        // have draws nothing at all, which is worse. The candidates are
        // therefore measured rather than assumed: a font that is present
        // reports a sensible textWidth for a known string, one that is
        // missing reports zero. Bethesda's UI fonts are registered with a
        // leading '$' through the font library.
        const char* PickFont(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay)
        {
            static const char* chosen = nullptr;
            static std::atomic_bool resolved{ false };
            if (resolved.load()) {
                return chosen;
            }

            // "$MAIN_Font_Bold" is not a guess: it is the name the vanilla
            // favoritesmenu.swf itself uses, read out of the decompressed
            // movie, which also names its font library as "fonts_en". The
            // rest are fallbacks in case a UI mod replaced the library.
            static constexpr std::array<const char*, 6> candidates{
                "$MAIN_Font_Bold",
                "$MAIN_Font",
                "MAIN_Font_Bold",
                "$EverywhereMediumFont",
                "Arial",
                nullptr   // the fallback, measured for comparison
            };

            RE::Scaleform::GFx::Value probe;
            if (!AddChild(
                    root, overlay, "flash.text.TextField", "GridFontProbe",
                    probe)) {
                resolved.store(true);
                return nullptr;
            }
            probe.SetMember("visible", RE::Scaleform::GFx::Value(false));
            probe.SetMember("autoSize", [&] {
                RE::Scaleform::GFx::Value left;
                root->CreateString(&left, "left");
                return left;
            }());

            double best = 0.0;
            for (const auto* candidate : candidates) {
                RE::Scaleform::GFx::Value format;
                root->CreateObject(&format, "flash.text.TextFormat");
                if (!format.IsObject()) {
                    continue;
                }
                format.SetMember(
                    "size", RE::Scaleform::GFx::Value(kFontSize));
                if (candidate) {
                    RE::Scaleform::GFx::Value fontName;
                    root->CreateString(&fontName, candidate);
                    format.SetMember("font", fontName);
                }
                RE::Scaleform::GFx::Value sample;
                root->CreateString(&sample, "Sample Text 123");
                probe.SetMember("text", sample);
                static_cast<void>(
                    probe.Invoke("setTextFormat", nullptr, &format, 1));

                const auto width = ReadNumber(probe, "textWidth", 0.0);
                const auto height = ReadNumber(probe, "textHeight", 0.0);
                REX::INFO(
                    "Grid font probe: {:<24} textWidth={:.1f} textHeight={:.1f}",
                    candidate ? candidate : "(default)",
                    width,
                    height);
                if (candidate && width > best) {
                    best = width;
                    chosen = candidate;
                }
            }

            RE::Scaleform::GFx::Value probeName;
            root->CreateString(&probeName, "GridFontProbe");
            RE::Scaleform::GFx::Value found;
            if (overlay.Invoke("getChildByName", &found, &probeName, 1) &&
                found.IsDisplayObject()) {
                static_cast<void>(
                    overlay.Invoke("removeChild", nullptr, &found, 1));
            }

            REX::INFO(
                "Grid: using font '{}'", chosen ? chosen : "(default)");
            resolved.store(true);
            return chosen;
        }

        // One wheel entry per cell. FavoritesEntry carries the whole icon
        // pipeline -- ImageFixture, the shared FavoritesIconBuffer, the
        // frame parking that keeps sizes stable -- so reusing the clip is
        // both far less code and automatically consistent with the wheel.
        //
        // The clip is built for the wheel's own size, so it is scaled into
        // the cell. Mouse input stays off: the overlay handles clicks itself
        // from one listener, and letting 40-odd clips fight over the cursor
        // would only produce a worse version of the same thing.
        // One icon per cell, loaded into a per-row image buffer.
        //
        // FavoritesEntry was the obvious choice -- it is the wheel's own
        // slot clip -- but it loads into "FavoritesIconBuffer", a single
        // buffer sized for the wheel's twelve slots. The grid asks for
        // dozens, and the crash landed reliably after sixteen to twenty:
        // the buffer overran, inside the engine's asset job thread and with
        // no plugin code in the backtrace.
        //
        // Driving Components.ImageFixture directly means the buffer name is
        // ours to choose, so each row gets its own and none of them is
        // asked to hold more than a wheel's worth. The trade is that the
        // count, ammo and equipped decorations of FavoritesEntry are lost;
        // for a grid of icons that is no loss.
        // The size of what a fixture actually draws.
        //
        // Measuring the fixture itself measures every layout helper inside
        // it as well. For a power that is catastrophic: the log said
        // "bounds 1250x600 at (-625,-300)" for every single power, always
        // the identical box, while the symbol inside it is a fraction of
        // that. Fitting the box to the cell therefore fit the box, and the
        // symbol came out at four percent scale.
        //
        // So the children are measured instead, and the ones that are only
        // there to hold a shape -- a sizer, a bound, a background, a hit
        // area -- are left out. Whatever remains is the artwork. If that
        // leaves nothing, the fixture's own box is still better than
        // refusing to draw.
        [[nodiscard]] bool LooksLikeLayoutHelper(std::string_view name)
        {
            std::string lowered(name);
            std::ranges::transform(
                lowered, lowered.begin(),
                [](unsigned char c) { return static_cast<char>(
                    std::tolower(c)); });
            for (const auto marker : {
                     "sizer"sv, "bound"sv, "background"sv, "_bg"sv,
                     "bg_"sv, "hit"sv, "frame"sv, "mask"sv }) {
                if (lowered.find(marker) != std::string_view::npos) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool BoundsOf(
            RE::Scaleform::GFx::Value& object,
            RE::Scaleform::GFx::Value& space,
            double& x,
            double& y,
            double& width,
            double& height)
        {
            const std::array args{ space };
            RE::Scaleform::GFx::Value bounds;
            if (!object.Invoke(
                    "getBounds", &bounds, args.data(), args.size()) ||
                !bounds.IsObject()) {
                return false;
            }
            x = ReadNumber(bounds, "x", 0.0);
            y = ReadNumber(bounds, "y", 0.0);
            width = ReadNumber(bounds, "width", 0.0);
            height = ReadNumber(bounds, "height", 0.0);
            return width > 1.0 && height > 1.0;
        }

        [[nodiscard]] bool MeasureArtwork(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& fixture,
            bool describe,
            bool& tight,
            double& outX,
            double& outY,
            double& outWidth,
            double& outHeight)
        {
            tight = false;
            double wholeX = 0.0;
            double wholeY = 0.0;
            double wholeWidth = 0.0;
            double wholeHeight = 0.0;
            const auto whole = BoundsOf(
                fixture, fixture, wholeX, wholeY, wholeWidth, wholeHeight);

            const auto children = static_cast<int>(
                ReadNumber(fixture, "numChildren", 0.0));
            bool found = false;
            double left = 0.0;
            double top = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            for (int index = 0; index < children; ++index) {
                RE::Scaleform::GFx::Value childIndex(
                    static_cast<double>(index));
                RE::Scaleform::GFx::Value child;
                if (!fixture.Invoke("getChildAt", &child, &childIndex, 1) ||
                    !child.IsDisplayObject()) {
                    continue;
                }
                RE::Scaleform::GFx::Value childName;
                std::string name;
                if (child.GetMember("name", &childName) &&
                    childName.IsString()) {
                    name = childName.GetString();
                }
                double cx = 0.0;
                double cy = 0.0;
                double cw = 0.0;
                double ch = 0.0;
                const auto sized = BoundsOf(child, fixture, cx, cy, cw, ch);
                RE::Scaleform::GFx::Value childVisible;
                const auto visible =
                    !child.GetMember("visible", &childVisible) ||
                    !childVisible.IsBoolean() || childVisible.GetBoolean();
                const auto helper = LooksLikeLayoutHelper(name);
                if (describe) {
                    REX::INFO(
                        "Grid icon child[{}] '{}': {:.0f}x{:.0f} at ({:.0f},{:.0f}), visible {}, treated as {}",
                        index,
                        name,
                        cw,
                        ch,
                        cx,
                        cy,
                        visible,
                        helper ? "layout" : "artwork");
                }
                if (!sized || !visible || helper) {
                    continue;
                }
                if (!found) {
                    left = cx;
                    top = cy;
                    right = cx + cw;
                    bottom = cy + ch;
                    found = true;
                } else {
                    left = std::min(left, cx);
                    top = std::min(top, cy);
                    right = std::max(right, cx + cw);
                    bottom = std::max(bottom, cy + ch);
                }
            }
            if (found) {
                tight = true;
                outX = left;
                outY = top;
                outWidth = right - left;
                outHeight = bottom - top;
                return true;
            }
            if (whole) {
                outX = wholeX;
                outY = wholeY;
                outWidth = wholeWidth;
                outHeight = wholeHeight;
                return true;
            }
            // Last resort: the clip's own box relative to its registration
            // point, which is right whenever the artwork starts there.
            outX = 0.0;
            outY = 0.0;
            outWidth = ReadNumber(fixture, "width", 0.0);
            outHeight = ReadNumber(fixture, "height", 0.0);
            static_cast<void>(root);
            return outWidth > 1.0 && outHeight > 1.0;
        }

        [[nodiscard]] bool EnsureEntry(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& overlay,
            const std::string& name,
            double x,
            double y,
            const FavoriteSlot& slot,
            std::size_t bufferRow,
            bool& created)
        {
            created = false;

            // An ImageFixture loads once and keeps what it loaded. After a
            // swap or a delete the cell holds something else but the clip
            // still shows the old icon, and the grid only looked right
            // again after closing and reopening the wheel -- which is what
            // made a successful move look like nothing had happened. So the
            // fixture is thrown away whenever the cell's contents change,
            // and built again from scratch below.
            const auto wanted = slot.Empty() ? std::string{} :
                std::format(
                    "{:08X}|{}", slot.form.rawFormID, slot.visual.imageName);
            auto& fit = g_iconFits[name];
            if (fit.key != wanted) {
                RE::Scaleform::GFx::Value stale;
                if (FindChild(root, overlay, name.c_str(), stale)) {
                    const std::array args{ stale };
                    static_cast<void>(overlay.Invoke(
                        "removeChild", nullptr, args.data(), args.size()));
                }
                fit = IconFit{};
                fit.key = wanted;
            }
            if (slot.Empty()) {
                // Nothing to draw, and asking the engine for an unnamed
                // image is exactly what corrupts a shared icon buffer.
                return true;
            }

            RE::Scaleform::GFx::Value fixture;
            if (!FindChild(root, overlay, name.c_str(), fixture)) {
                if (!AddChild(
                        root, overlay, "Components.ImageFixture",
                        name.c_str(), fixture)) {
                    return false;
                }
                created = true;
            }

            fixture.SetMember(
                "mouseEnabled", RE::Scaleform::GFx::Value(false));
            fixture.SetMember(
                "mouseChildren", RE::Scaleform::GFx::Value(false));
            // A favorite whose item is not in the inventory right now still
            // owns its place -- the wheel would simply show the slot as
            // empty, which explains nothing. Drawn faintly it keeps saying
            // what the slot is for and admits that pressing its key would do
            // nothing. The mod already knows: the commit records it on every
            // descriptor it could not write into a real slot.
            fixture.SetMember(
                "alpha",
                RE::Scaleform::GFx::Value(slot.unresolved ? 0.3 : 1.0));
            if (created) {
                // Hidden until it has been measured. A brand new fixture is
                // drawn at whatever size the artwork happens to be -- some
                // weapon icons are several hundred pixels wide -- and the
                // frames between creation and the first successful
                // measurement are exactly the icons that "flew in" across
                // the screen before settling into their cells.
                fixture.SetMember(
                    "visible", RE::Scaleform::GFx::Value(false));
                fixture.SetMember("x", RE::Scaleform::GFx::Value(x));
                fixture.SetMember("y", RE::Scaleform::GFx::Value(y));
            }

            // The size is corrected on every pass, not just when the clip
            // is made. Loading is asynchronous: measuring right after the
            // load call reads whatever is there at that instant, and for an
            // icon still in flight that is close to zero -- which turns
            // into an enormous scale factor. Rescaling until the icon has
            // really arrived is what keeps them sane.
            //
            // scale is reset first, so the measurement is of the icon's own
            // size rather than of the previous pass's result.
            fixture.SetMember("scaleX", RE::Scaleform::GFx::Value(1.0));
            fixture.SetMember("scaleY", RE::Scaleform::GFx::Value(1.0));

            // Measured with getBounds, not with width and height.
            //
            // width and height describe the clip's box relative to its
            // registration point, and an ImageFixture does not necessarily
            // put its artwork at that point: a centred clip hangs around it,
            // so its top left sits at a negative offset. Placing such a clip
            // by x and y alone lands it half its own size off, which is what
            // left icons sitting outside their cells. getBounds reports
            // where the artwork actually is in the clip's own space, so the
            // fit can account for it.
            double boundsX = 0.0;
            double boundsY = 0.0;
            double boundsWidth = 0.0;
            double boundsHeight = 0.0;
            bool tight = false;
            bool measured = MeasureArtwork(
                root, fixture, false, tight,
                boundsX, boundsY, boundsWidth, boundsHeight);
            double appliedScale = 0.0;
            if (measured) {
                const auto inner = kCellSize - kIconInset * 2.0;
                auto scale = std::min(
                    inner / boundsWidth, inner / boundsHeight);
                // Powers are drawn bigger than they measure, by however
                // much the INI says.
                //
                // Their fixture reports a constant 1250x600 box whatever
                // symbol it holds, and the symbol inside is a fraction of
                // that, so fitting the box leaves the power tiny. This was
                // conditional at first -- only when the measurement had
                // fallen back to the whole clip -- and that condition is
                // never true: the measurement finds two child objects and
                // takes their 1250x600 for the artwork. The setting
                // therefore did nothing at all, silently, which is the
                // worst thing a setting can do. It now applies to every
                // power; 100 means no change.
                if (slot.visual.isPower) {
                    scale *= static_cast<double>(
                        g_settings.gridPowerIconScalePercent) / 100.0;
                }
                appliedScale = scale;
                fixture.SetMember(
                    "scaleX", RE::Scaleform::GFx::Value(scale));
                fixture.SetMember(
                    "scaleY", RE::Scaleform::GFx::Value(scale));
                // Put the middle of the artwork in the middle of the cell.
                fixture.SetMember(
                    "x",
                    RE::Scaleform::GFx::Value(
                        x + kCellSize / 2.0 -
                        (boundsX + boundsWidth / 2.0) * scale));
                fixture.SetMember(
                    "y",
                    RE::Scaleform::GFx::Value(
                        y + kCellSize / 2.0 -
                        (boundsY + boundsHeight / 2.0) * scale));
                fixture.SetMember(
                    "visible", RE::Scaleform::GFx::Value(true));
            } else {
                // Not there yet: keep it out of sight rather than showing a
                // wrongly sized icon, and ask for another pass.
                fixture.SetMember(
                    "visible", RE::Scaleform::GFx::Value(false));
                g_iconsIncomplete.store(true);
            }

            // Fitted repeatedly, until two passes measure the same artwork.
            //
            // A power's symbol comes from an external library and arrives
            // well after the fixture that will hold it. Fitting once and
            // stopping meant a power was scaled to whatever placeholder the
            // clip had at that moment, and nothing ever came back to correct
            // it: that is why powers stayed visibly smaller than every item
            // icon beside them. The cap stops a clip that never settles from
            // asking for passes forever.
            constexpr int kMaxFitPasses = 40;
            const auto settled = measured && fit.passes > 0 &&
                std::abs(fit.width - boundsWidth) < 0.5 &&
                std::abs(fit.height - boundsHeight) < 0.5;
            fit.width = boundsWidth;
            fit.height = boundsHeight;
            ++fit.passes;
            if (!settled && fit.passes < kMaxFitPasses) {
                g_iconsIncomplete.store(true);
            }
            if (settled && !fit.reported &&
                slot.visual.isPower) {
                fit.reported = true;
                // The child listing has to happen now, not when the clip was
                // created: at creation every child measures nothing, which
                // is exactly what the first attempt at this reported.
                static std::atomic_int described{ 0 };
                if (slot.visual.isPower && described.fetch_add(1) < 2) {
                    bool ignoredTight = false;
                    double ignored = 0.0;
                    static_cast<void>(MeasureArtwork(
                        root, fixture, true, ignoredTight,
                        ignored, ignored, ignored, ignored));
                }
                REX::INFO(
                    "Grid icon '{}'{}: {} bounds {:.0f}x{:.0f} at ({:.0f},{:.0f}), scaled {:.2f} into a {:.0f} cell after {} pass(es)",
                    slot.visual.name,
                    slot.visual.isPower ? " (power)" : "",
                    tight ? "artwork" : "whole-clip",
                    boundsWidth,
                    boundsHeight,
                    boundsX,
                    boundsY,
                    appliedScale,
                    kCellSize,
                    fit.passes);
            }

            if (!created) {
                // Already loaded; never reload. Reloading an icon that is
                // already on screen is what turned the staged loading into
                // a continuous stream.
                return true;
            }

            // No clip sizer, for powers either.
            //
            // Sizer_mc is a bounding box the wheel hands the fixture so it
            // can fit a power's vector clip into a slot. It is larger than
            // the artwork inside it, so measuring a fixture that carries one
            // measures the box: the power ended up scaled to that, and the
            // symbol itself came out visibly smaller than every item icon
            // beside it. The grid does its own fitting from the artwork's
            // own bounds, so it wants the artwork and nothing else.
            RE::Scaleform::GFx::Value sizer;
            root->CreateString(&sizer, "");
            fixture.SetMember("clipSizer", sizer);
            fixture.SetMember(
                "centerClip", RE::Scaleform::GFx::Value(false));

            RE::Scaleform::GFx::Value info;
            BuildFavoriteInfoForDisplay(root, slot, info);
            RE::Scaleform::GFx::Value iconImage;
            if (!info.GetMember("iconImage", &iconImage) ||
                !iconImage.IsObject()) {
                return false;
            }

            RE::Scaleform::GFx::Value bufferName;
            root->CreateString(
                &bufferName,
                std::format("FavoritesBanksGrid{}", bufferRow).c_str());
            const std::array args{ iconImage, bufferName };
            if (!fixture.Invoke(
                    "LoadImageFixtureFromUIData", nullptr, args.data(),
                    args.size())) {
                return false;
            }

            return true;
        }
    }

    bool GridIconsPending()
    {
        return g_iconsIncomplete.load();
    }

    bool ClearHoveredGridCell()
    {
        const auto row = g_hoveredRow.load();
        const auto slot = g_hoveredSlot.load();
        if (row < 0 || slot < 0) {
            return false;
        }
        if (g_hoveredPinned.load()) {
            // Buttons have nothing to clear, and a pinned slot belongs
            // to another mod: emptying it only makes that mod put its
            // item straight back.
            return false;
        }

        const auto globalIndex =
            static_cast<std::size_t>(row) * kSlotsPerBank +
            static_cast<std::size_t>(slot);
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) {
            return false;
        }
        // Clearing touches native favorite state, so it belongs on the
        // main thread like every other mutation.
        tasks->AddTask([globalIndex]() {
            ClearGridSlot(globalIndex);
            UpdateGridOverlay();
        });
        return true;
    }

    // Asks for a redraw. Draws nothing itself.
    //
    // Everything this file does to the movie used to happen wherever the
    // caller happened to be: a task, a menu callback, a click. Our own log
    // shows those arriving on a different thread almost every time -- the
    // plugin starts on one thread and the grid's passes were logged from
    // half a dozen others -- and Scaleform is not thread-safe. The crashes
    // matched: an access violation inside the AS3 VM, once with the program
    // counter pointing at the bytes of a string instead of at code.
    //
    // So the work moved to where the engine itself is already touching this
    // movie: an enterFrame listener on our own overlay. Everyone else only
    // raises a flag. The one exception is the very first pass, which has to
    // create the overlay that carries the listener -- and it stays the
    // fallback if the movie turns out never to fire enterFrame at all, so a
    // grid that cannot tick is still a grid that draws.
    void UpdateGridOverlay(RE::IMenu* explicitMenu)
    {
        if (g_disabledForConflict.load()) {
            return;
        }
        if (!g_favoritesMenuVisible.load(std::memory_order_acquire)) {
            g_iconsIncomplete.store(false);
            return;
        }
        g_redrawRequested.store(true);
        if (!g_tickerAlive.load()) {
            DrawGridOverlay(explicitMenu);
        }
    }

    // Forgets everything that belongs to one opening of the wheel.
    void ResetGridSession()
    {
        g_tickerAlive.store(false);
        g_redrawRequested.store(false);
        g_iconsIncomplete.store(false);
        g_editMode.store(false);
        g_editSourceBank.store(-1);
        g_editSourceSlot.store(-1);
    }

    void DrawGridOverlay(RE::IMenu* explicitMenu)
    {
        if (g_disabledForConflict.load()) {
            return;
        }

        // Nothing at all once the wheel is closing.
        //
        // This is called from the polling loop for as long as icons are
        // outstanding, and clicking an empty cell in edit mode cancels the
        // wheel underneath it. The next pass then found no overlay, built a
        // fresh one on a stage the engine was already tearing down, and
        // faulted inside Scaleform -- with BuildOverlay right there in the
        // backtrace. The menu's own visibility flag is the authority: a menu
        // pointer stays retrievable well past the point of being usable.
        if (!g_favoritesMenuVisible.load(std::memory_order_acquire)) {
            g_iconsIncomplete.store(false);
            return;
        }

        // Only ever one draw at a time.
        //
        // This is reached from the menu callback, from every page switch,
        // from load initialisation and from the icon batch driver, and the
        // task queue happily holds several of those at once. Two passes
        // creating Scaleform display objects concurrently is what kept
        // crashing the engine's asset streaming: the log showed four passes
        // inside four milliseconds, from three different threads.
        static std::atomic_bool drawing{ false };
        if (drawing.exchange(true)) {
            return;
        }
        struct DrawGuard
        {
            std::atomic_bool& flag;
            ~DrawGuard() { flag.store(false); }
        } guard{ drawing };
        g_redrawRequested.store(false);

        RE::Scaleform::Ptr<RE::IMenu> ownedMenu;
        auto* menu = explicitMenu;
        if (!menu) {
            if (auto* ui = RE::UI::GetSingleton()) {
                static const RE::BSFixedString favoritesMenu("FavoritesMenu");
                ownedMenu = ui->GetMenu(favoritesMenu);
                menu = ownedMenu.get();
            }
        }
        if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot ||
            !menu->menuObj.IsObject()) {
            return;
        }

        auto* root = menu->uiMovie->asMovieRoot.get();
        ProbeCreatableClasses(root);

        RE::Scaleform::GFx::Value stage;
        if (!menu->menuObj.GetMember("stage", &stage) ||
            !stage.IsDisplayObject()) {
            return;
        }
        RE::Scaleform::GFx::Value overlay;
        bool freshlyOpened = false;
        if (!FindChild(root, stage, "FavoritesBanksGrid", overlay)) {
            if (!BuildOverlay(root, stage, overlay)) {
                return;
            }
            freshlyOpened = true;
        }
        if (freshlyOpened) {
            // Driven from here rather than from the movie's own data update,
            // because that update guards itself with IsDataInitialized and
            // that flag is never cleared: the selection was forced to slot 1
            // on the first opening of a session and never again. A fresh
            // overlay, on the other hand, means exactly one thing -- the
            // wheel has just been opened.
            menu->menuObj.SetMember(
                "selectedIndex", RE::Scaleform::GFx::Value(0.0));
        }
        // Unconditional. The grid exists so that the wheel does not have to
        // be looked at; leaving it drawn only put the two on top of each
        // other. It was an option, and there was no setting of it that made
        // sense -- a problem with hiding the wheel is a problem to fix, not
        // one to hand to the player as a switch.
        SetWheelVisible(menu, false);
        ReportMenuChildren(menu, root);

        const auto geometry = ComputeGeometry();
        const auto stageWidth = ReadNumber(stage, "stageWidth", 1280.0);
        const auto stageHeight = ReadNumber(stage, "stageHeight", 720.0);
        // The padding is symmetric, so centring the content also centres the
        // backdrop drawn around it.
        const auto originX = (stageWidth - geometry.totalWidth) / 2.0;
        const auto originY =
            (stageHeight - geometry.totalHeight - kStatusHeight) / 2.0;
        overlay.SetMember("x", RE::Scaleform::GFx::Value(originX));
        overlay.SetMember("y", RE::Scaleform::GFx::Value(originY));

        // Bring the wheel's info panel up against the grid instead of
        // leaving it stranded near the top of the screen. It goes above,
        // not below: below is where the grid's own status line lives, and
        // the earlier attempt to put it there simply lost it. Its height is
        // measured rather than assumed, and if there is no room the panel
        // keeps whatever position it already had.
        {
            RE::Scaleform::GFx::Value info;
            if (menu->menuObj.GetMember("ItemInfo_mc", &info) &&
                info.IsDisplayObject()) {
                // The panel's own registration point is not necessarily its
                // top left: setting y to "just above the grid" put it well
                // below instead, so the offset between the two is measured
                // rather than assumed. Moving it by a delta works whatever
                // the clip's anchor turns out to be.
                // The panel's y is in the menu's own space, not the
                // stage's: it reads -330 while sitting near the top of the
                // screen, because the menu's origin is its centre. Mixing
                // the two is why "just above the grid" landed well below
                // it. Convert through the menu's own offset instead.
                const auto currentY = ReadNumber(info, "y", 0.0);
                const auto height = ReadNumber(info, "height", 0.0);
                const auto menuY = ReadNumber(menu->menuObj, "y", 0.0);
                const auto wantedStageY = originY - kPadding - height;
                const auto target = wantedStageY - menuY;
                info.SetMember("y", RE::Scaleform::GFx::Value(target));

                static std::atomic_bool reportedInfo{ false };
                if (!reportedInfo.exchange(true)) {
                    REX::INFO(
                        "Grid: ItemInfo_mc y={:.0f} height={:.0f}, menu y={:.0f}; grid top {:.0f} -> panel y {:.0f}",
                        currentY,
                        height,
                        menuY,
                        originY,
                        target);
                }
            }
        }

        RE::Scaleform::GFx::Value graphics;
        if (!overlay.GetMember("graphics", &graphics) ||
            !graphics.IsObject()) {
            REX::WARN("Favorites Menu Grid grid: the overlay has no graphics API");
            return;
        }
        graphics.Invoke("clear");

        // Backdrop with an even margin on all four sides. It also catches
        // clicks anywhere inside the grid, including the gaps between cells.
        // The status line lives below the cells but inside the backdrop, so
        // the bottom gets its own allowance on top of the even padding.
        // totalHeight already contains the heading band, so the backdrop
        // adds only the status band below it and the even padding around
        // both.
        DrawRect(
            graphics, -kPadding, -kPadding,
            geometry.totalWidth + kPadding * 2.0,
            geometry.totalHeight + kPadding * 2.0 + kStatusHeight,
            kColorFill, 0.82);

        std::size_t activeBank = 0;
        std::size_t nativeBank = 0;
        FavoriteBanks banks;
        FavoriteBank pinned;
        std::array<std::string, kSlotsPerBank> quickKeys;
        {
            std::scoped_lock lock(g_stateMutex);
            quickKeys = g_settings.quickKeyNames;
            activeBank = g_activeBank;
            nativeBank = g_nativeBank;
            banks = g_banks;
            pinned = g_pinnedSlots;
        }

        const auto stride = kCellSize + kCellGap;
        bool textWorks = true;
        std::size_t entriesCreated = 0;
        std::size_t entryFailures = 0;
        bool iconsPending = false;
        // Cleared here and raised again by any icon that has not
        // finished loading, so the driver keeps coming back until every
        // one has a real size.
        g_iconsIncomplete.store(false);
        // The key that fires each column, read from the game's own
        // bindings. Blank where the slot has no key.
        for (std::size_t column = 0;
             textWorks && column < geometry.pageSlots.size();
             ++column) {
            const auto slot = geometry.pageSlots[column];
            const auto& key = quickKeys[slot];
            // The key and nothing else. The slot number was the first
            // attempt and it answered a question nobody was asking: which
            // cell is which is obvious from looking at it. What cannot be
            // seen is which key fires it, so a column with no binding is
            // left blank rather than labelled with a number that could be
            // mistaken for one.
            const auto& caption = key;
            RE::Scaleform::GFx::Value heading;
            textWorks = EnsureLabel(
                root, overlay, std::format("GridHead{}", slot),
                kLabelWidth + stride * static_cast<double>(column),
                1.0, kCellSize, caption, heading, "center");
        }

        for (std::size_t row = 0; row < geometry.rows; ++row) {
            const auto y = kHeaderHeight + stride * static_cast<double>(row);
            if (row == nativeBank) {
                // The page that actually occupies the twelve real slots.
                // Selecting anything in another row has to commit first,
                // so it is worth showing which one is already live.
                DrawRect(
                    graphics, 0.0, y - kCellGap / 2.0, geometry.gridWidth,
                    kCellSize + kCellGap, kColorNativeRow, 0.85);
            }

            if (textWorks) {
                RE::Scaleform::GFx::Value rowLabel;
                textWorks = EnsureLabel(
                    root, overlay, std::format("GridRow{}", row),
                    2.0, y + kCellSize / 3.0, kLabelWidth - 4.0,
                    std::format("{}", row + 1), rowLabel);
            }

            for (std::size_t column = 0;
                 column < geometry.pageSlots.size();
                 ++column) {
                const auto slot = geometry.pageSlots[column];
                const auto x = kLabelWidth +
                    stride * static_cast<double>(column);
                const auto isSource = g_editMode.load() &&
                    g_editSourceBank.load() == static_cast<int>(row) &&
                    g_editSourceSlot.load() == static_cast<int>(slot);
                if (isSource) {
                    DrawRect(
                        graphics, x, y, kCellSize, kCellSize,
                        kColorSource, 0.9);
                }
                StrokeRect(
                    graphics, x, y, kCellSize, kCellSize,
                    isSource ? kColorText : kColorFrame,
                    isSource ? 1.0 : (row == activeBank ? 0.95 : 0.45));
                // A delete box in the corner, only while editing and only
                // where there is something to delete.
                if (g_editMode.load() && !banks[row][slot].Empty()) {
                    DrawCross(
                        graphics, x + kCellSize - kDeleteBoxSize, y,
                        kDeleteBoxSize, kColorDelete, 1.0);
                }
                const auto cellName = std::format("GridCell{}_{}", row, slot);
                // A slot the wheel has never described has no image to draw,
                // and the icon path would leave the cell blank even though it
                // is occupied and works. That is what a save the plugin is
                // meeting for the first time looks like: real form IDs, no
                // cards. Fall back to the name for that cell alone until the
                // wheel hands one over.
                // No global switch any more: the fallback that mattered was
                // always the per-cell one. A cell whose card never arrived
                // shows its name; every other cell shows its icon, which is
                // the whole point of the grid.
                const auto drawIcon = banks[row][slot].Empty() ||
                    !banks[row][slot].visual.imageName.empty();
                if (drawIcon) {
                    // Existing clips are cheap to refresh; only brand new
                    // ones stream assets, so only those are rationed.
                    RE::Scaleform::GFx::Value existing;
                    const auto alreadyThere = FindChild(
                        root, overlay, cellName.c_str(), existing);
                    if (!alreadyThere && entriesCreated >= kIconsPerPass) {
                        iconsPending = true;
                        continue;
                    }
                    bool created = false;
                    if (!EnsureEntry(
                            root, overlay, cellName, x, y,
                            banks[row][slot], row, created)) {
                        ++entryFailures;
                    } else if (created) {
                        ++entriesCreated;
                    }
                } else if (textWorks) {
                    RE::Scaleform::GFx::Value cellLabel;
                    textWorks = EnsureLabel(
                        root, overlay, cellName, x + 2.0, y + 2.0,
                        kCellSize - 4.0, ShortLabel(banks[row][slot]),
                        cellLabel);
                    if (textWorks) {
                        cellLabel.SetMember(
                            "alpha",
                            RE::Scaleform::GFx::Value(
                                banks[row][slot].unresolved ? 0.3 : 1.0));
                    }
                }
            }
        }

        // Diagnose: was das Zeichnen tatsaechlich vorfindet, direkt neben
        // dem, was die Trageprüfung berechnet hat. Weichen die beiden ab,
        // setzt jemand dazwischen zurueck.
        {
            static std::atomic_int reports{ 0 };
            if (reports.fetch_add(1) < 20) {
                for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                    const auto& cell = banks[activeBank][slot];
                    if (cell.Empty()) {
                        continue;
                    }
                    REX::INFO(
                        "draw-probe r{}c{} '{}' unresolved={}",
                        activeBank + 1,
                        slot + 1,
                        cell.form.editorID,
                        cell.unresolved);
                }
            }
        }

        // Drawn after the cells so it sits on top of them, and on the active
        // row because that is the page the wheel is showing.
        const auto selected = ReadSelectedIndex(menu);
        if (selected == kNoSelection) {
            MoveSelection(root, overlay, false);
        } else {
            const auto found = std::ranges::find(geometry.pageSlots, selected);
            if (found == geometry.pageSlots.end()) {
                MoveSelection(root, overlay, false);
            } else {
                const auto column = static_cast<double>(
                    std::distance(geometry.pageSlots.begin(), found));
                MoveSelection(
                    root, overlay, true,
                    kLabelWidth + stride * column,
                    kHeaderHeight + stride * static_cast<double>(activeBank));
            }
        }

        const auto extrasX = geometry.gridWidth + kPinGap;
        for (std::size_t index = 0; index < geometry.extras.size(); ++index) {
            const auto& extra = geometry.extras[index];
            const auto y = kHeaderHeight + stride *
                static_cast<double>(index);
            const auto isEdit =
                extra.kind == GridGeometry::ExtraKind::kEditToggle;
            const auto isPinned =
                extra.kind == GridGeometry::ExtraKind::kPinnedSlot;
            const auto editing = g_editMode.load();
            DrawRect(
                graphics, extrasX, y, kCellSize, kCellSize,
                isPinned ? kColorPinned :
                    (isEdit && editing ? kColorEditOn : kColorButton),
                0.85);
            StrokeRect(
                graphics, extrasX, y, kCellSize, kCellSize, kColorFrame, 0.7);

            const auto symbol = isPinned ?
                SymbolFor(pinned[extra.index]) : PinnedSymbol::kNone;
            if (symbol != PinnedSymbol::kNone) {
                // Our own symbol, so this cell never depends on a card the
                // wheel may never hand over. Any icon clip left from before
                // has to go, or it would sit on top of the drawing.
                const auto pinName = std::format("GridPin{}", extra.index);
                RE::Scaleform::GFx::Value stale;
                if (FindChild(root, overlay, pinName.c_str(), stale)) {
                    const std::array args{ stale };
                    static_cast<void>(overlay.Invoke(
                        "removeChild", nullptr, args.data(), args.size()));
                    g_iconFits.erase(pinName);
                }
                constexpr double inset = 11.0;
                const auto size = kCellSize - inset * 2.0;
                if (symbol == PinnedSymbol::kTurret) {
                    DrawTurretSymbol(
                        graphics, extrasX + inset, y + inset, size,
                        kColorText);
                } else {
                    DrawRecallSymbol(
                        graphics, extrasX + inset, y + inset, size,
                        kColorText);
                }
            } else if (!isPinned) {
                if (textWorks) {
                    RE::Scaleform::GFx::Value buttonLabel;
                    textWorks = EnsureLabel(
                        root, overlay,
                        std::format("GridButton{}", index),
                        extrasX + 2.0, y + 2.0, kCellSize - 4.0,
                        editing ? "EDIT: ON" : "EDIT", buttonLabel);
                }
            } else {
                const auto pinName = std::format("GridPin{}", extra.index);
                // Same rule as a page cell: the icon when there is one to
                // draw, the name when the card has never arrived.
                const auto drawPinIcon = pinned[extra.index].Empty() ||
                    !pinned[extra.index].visual.imageName.empty();
                if (drawPinIcon) {
                    RE::Scaleform::GFx::Value existing;
                    const auto alreadyThere = FindChild(
                        root, overlay, pinName.c_str(), existing);
                    if (!alreadyThere && entriesCreated >= kIconsPerPass) {
                        iconsPending = true;
                        continue;
                    }
                    bool created = false;
                    if (!EnsureEntry(
                            root, overlay, pinName, extrasX, y,
                            pinned[extra.index], geometry.rows,
                            created)) {
                        ++entryFailures;
                    } else if (created) {
                        ++entriesCreated;
                    }
                } else if (textWorks) {
                    RE::Scaleform::GFx::Value pinLabel;
                    textWorks = EnsureLabel(
                        root, overlay, pinName, extrasX + 2.0, y + 2.0,
                        kCellSize - 4.0, ShortLabel(pinned[extra.index]),
                        pinLabel);
                }
            }
        }

        // What edit mode is holding, said as soon as it is picked up.
        //
        // The status line was only ever written by the hover handler, and a
        // click does not move the mouse: you picked a favorite up and the
        // grid told you nothing until you nudged the cursor. That is
        // indistinguishable from a click that did not register, which is
        // exactly how it was read.
        if (textWorks && g_editMode.load()) {
            const auto heldBank = g_editSourceBank.load();
            const auto heldSlot = g_editSourceSlot.load();
            if (heldBank >= 0 && heldSlot >= 0) {
                std::string held;
                {
                    std::scoped_lock lock(g_stateMutex);
                    const auto& source =
                        g_banks[static_cast<std::size_t>(heldBank)]
                               [static_cast<std::size_t>(heldSlot)];
                    held = !source.visual.name.empty() ?
                        source.visual.name : source.form.editorID;
                }
                SetStatusText(
                    root, overlay,
                    std::format(
                        "HOLDING {}  -  click any cell to put it there.",
                        held));
            }
        }

        static std::atomic_bool reportedText{ false };
        if (!textWorks && !reportedText.exchange(true)) {
            REX::WARN(
                "Favorites Menu Grid grid: Scaleform refused a TextField, so rows cannot be numbered.");
        }
        // Either there are clips left to create, or some already exist
        // but have not reported a size yet.
        g_iconsIncomplete.store(
            iconsPending || g_iconsIncomplete.load());
        if (entriesCreated != 0 || entryFailures != 0) {
            REX::INFO(
                "Favorites Menu Grid grid: {} icon clip(s) created this pass, {} refused, more pending: {}",
                entriesCreated,
                entryFailures,
                iconsPending);
        }
    }
}
