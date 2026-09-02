#include "pch.h"
#include "favorites.h"

namespace FB
{
    namespace
    {
        using MarkUIDataDirty_t = void (*)(void*);
        using ClearFavoritesData_t = void (*)(void*);
        using RebuildFavoritesData_t = void (*)(void**, void*);

        double ReadScaleformNumber(
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

        bool FindDisplayChild(
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

        bool AddNamedDisplayObject(
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

        class FavoritesSelectionHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 2) {
                    return;
                }

                std::size_t globalIndex = std::numeric_limits<std::size_t>::max();
                if (params.args[0].IsNumber()) {
                    globalIndex = static_cast<std::size_t>(
                        params.args[0].GetNumber());
                } else if (params.args[0].IsInt()) {
                    globalIndex = static_cast<std::size_t>(
                        params.args[0].GetInt());
                } else if (params.args[0].IsUInt()) {
                    globalIndex = params.args[0].GetUInt();
                }
                if (globalIndex >= g_settings.rowCount * kSlotsPerBank ||
                    !params.args[1].IsBoolean()) {
                    return;
                }

                const auto* assignedItem = params.argCount >= 3 &&
                    params.args[2].IsObject() ?
                    std::addressof(params.args[2]) : nullptr;
                const auto mayDispatch = ProcessMenuSelection(
                    globalIndex,
                    params.args[1].GetBoolean(),
                    assignedItem);
                if (params.ret) {
                    *params.ret = RE::Scaleform::GFx::Value(mayDispatch);
                }
            }
        };

        class ClearSlotHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1) {
                    return;
                }
                std::size_t globalIndex =
                    std::numeric_limits<std::size_t>::max();
                if (params.args[0].IsNumber()) {
                    globalIndex = static_cast<std::size_t>(
                        params.args[0].GetNumber());
                } else if (params.args[0].IsInt()) {
                    globalIndex = static_cast<std::size_t>(
                        params.args[0].GetInt());
                } else if (params.args[0].IsUInt()) {
                    globalIndex = params.args[0].GetUInt();
                } else {
                    return;
                }
                ClearSlotFromMenu(globalIndex);
                if (params.ret) {
                    *params.ret = RE::Scaleform::GFx::Value(true);
                }
            }
        };

        // Moves the shown row, called from the wheel's own key handler.
        //
        // The D-pad is not intercepted anywhere: FavoritesMenu.as is shipped
        // with this mod, so what up and down *mean* is simply rewritten
        // there. Vertical belongs to the rows because the grid stacks them
        // vertically; the vanilla mapping walks the wheel's rings instead,
        // which says nothing about a grid.
        class SwitchRowHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1) {
                    return;
                }
                double raw = 0.0;
                if (params.args[0].IsNumber()) {
                    raw = params.args[0].GetNumber();
                } else if (params.args[0].IsInt()) {
                    raw = static_cast<double>(params.args[0].GetInt());
                } else if (params.args[0].IsUInt()) {
                    raw = static_cast<double>(params.args[0].GetUInt());
                } else {
                    return;
                }
                const auto delta = raw < 0.0 ? -1 : (raw > 0.0 ? 1 : 0);
                if (delta == 0) {
                    return;
                }
                std::size_t target = 0;
                {
                    std::scoped_lock lock(g_stateMutex);
                    const auto rows = static_cast<int>(g_settings.rowCount);
                    auto next = static_cast<int>(g_activeBank) + delta;
                    if (g_settings.wrapNavigation) {
                        next = ((next % rows) + rows) % rows;
                    } else {
                        next = std::clamp(next, 0, rows - 1);
                    }
                    target = static_cast<std::size_t>(next);
                    if (target == g_activeBank) {
                        return;
                    }
                }
                QueueBankSwitch(target);
                if (params.ret) {
                    *params.ret = RE::Scaleform::GFx::Value(true);
                }
            }
        };

        class MenuLogHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1 || !params.args[0].IsString()) {
                    return;
                }
                if (const auto* text = params.args[0].GetString()) {
                    LogFromMenu(text);
                }
            }
        };

        class NativeVisualsHandler final :
            public RE::Scaleform::GFx::FunctionHandler
        {
        public:
            void Call(const Params& params) override
            {
                if (params.argCount < 1 || !params.args[0].IsArray()) {
                    return;
                }
                CaptureNativePageVisuals(std::addressof(params.args[0]));
                if (params.ret) {
                    *params.ret = RE::Scaleform::GFx::Value(true);
                }
            }
        };

        void SetStringMember(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& object,
            const char* name,
            std::string_view text)
        {
            RE::Scaleform::GFx::Value value;
            root->CreateString(&value, std::string(text).c_str());
            object.SetMember(name, value);
        }

        // An empty wheel position must describe itself as FT_INVALID with no
        // name. FavoritesEntry.LoadIcon only hides its ImageFixture for that
        // exact shape; any other fixture type sends the slot down the loading
        // path and registers an unnamed request against the shared
        // "FavoritesIconBuffer", which is what made icons from the previous
        // page survive on top of the new one.
        void BuildEmptyFavoriteInfo(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            RE::Scaleform::GFx::Value& result)
        {
            root->CreateObject(&result);
            SetStringMember(root, result, "sName", "");
            result.SetMember("uCount", RE::Scaleform::GFx::Value(0.0));
            result.SetMember("bIsPower", RE::Scaleform::GFx::Value(false));
            result.SetMember("bIsEquippable", RE::Scaleform::GFx::Value(false));
            result.SetMember("bIsEquipped", RE::Scaleform::GFx::Value(false));
            SetStringMember(root, result, "sAmmoName", "");
            result.SetMember("uAmmoCount", RE::Scaleform::GFx::Value(0.0));

            RE::Scaleform::GFx::Value icon;
            root->CreateObject(&icon);
            icon.SetMember(
                "iFixtureType",
                RE::Scaleform::GFx::Value(
                    static_cast<double>(kInvalidFixtureType)));
            SetStringMember(root, icon, "sDirectory", "");
            SetStringMember(root, icon, "sImageName", "");
            result.SetMember("iconImage", icon);

            RE::Scaleform::GFx::Value stats;
            root->CreateArray(&stats);
            result.SetMember("aElementalStats", stats);
        }

        void BuildFavoriteInfo(
            RE::Scaleform::GFx::ASMovieRootBase* root,
            const FavoriteSlot& slot,
            RE::Scaleform::GFx::Value& result)
        {
            if (slot.Empty()) {
                BuildEmptyFavoriteInfo(root, result);
                return;
            }

            root->CreateObject(&result);
            const auto& visual = slot.visual;
            const auto fallbackName = !slot.form.editorID.empty() ?
                slot.form.editorID :
                (!slot.Empty() ? std::format("{:08X}", slot.form.rawFormID) : "");
            SetStringMember(
                root,
                result,
                "sName",
                !visual.name.empty() ? visual.name : fallbackName);
            result.SetMember(
                "uCount",
                RE::Scaleform::GFx::Value(static_cast<double>(visual.count)));
            result.SetMember(
                "bIsPower",
                RE::Scaleform::GFx::Value(visual.isPower));
            result.SetMember(
                "bIsEquippable",
                RE::Scaleform::GFx::Value(visual.isEquippable));
            result.SetMember(
                "bIsEquipped",
                RE::Scaleform::GFx::Value(visual.isEquipped));
            SetStringMember(root, result, "sAmmoName", visual.ammoName);
            result.SetMember(
                "uAmmoCount",
                RE::Scaleform::GFx::Value(
                    static_cast<double>(visual.ammoCount)));

            // An occupied slot whose item card was never harvested still shows
            // its name and stays selectable, but it must not ask the engine
            // for an image it cannot name. Only a fixture type with a real
            // image name is allowed through.
            const auto drawable = visual.fixtureType != kInvalidFixtureType &&
                !visual.imageName.empty();
            RE::Scaleform::GFx::Value icon;
            root->CreateObject(&icon);
            icon.SetMember(
                "iFixtureType",
                RE::Scaleform::GFx::Value(static_cast<double>(
                    drawable ? visual.fixtureType : kInvalidFixtureType)));
            SetStringMember(
                root,
                icon,
                "sDirectory",
                drawable ? visual.imageDirectory : "");
            SetStringMember(
                root, icon, "sImageName", drawable ? visual.imageName : "");
            result.SetMember("iconImage", icon);

            RE::Scaleform::GFx::Value stats;
            root->CreateArray(&stats);
            for (const auto& elemental : visual.elementalStats) {
                RE::Scaleform::GFx::Value stat;
                root->CreateObject(&stat);
                stat.SetMember(
                    "iElementalType",
                    RE::Scaleform::GFx::Value(
                        static_cast<double>(elemental.type)));
                stat.SetMember(
                    "fValue",
                    RE::Scaleform::GFx::Value(elemental.value));
                stats.PushBack(stat);
            }
            result.SetMember("aElementalStats", stats);
        }

    }

    // The grid renders its cells with the wheel's own FavoritesEntry clips,
    // so it needs the same item description the wheel is handed. Building a
    // second one would be two things to keep in step for no reason.
    void BuildFavoriteInfoForDisplay(
        RE::Scaleform::GFx::ASMovieRootBase* root,
        const FavoriteSlot& slot,
        RE::Scaleform::GFx::Value& result)
    {
        BuildFavoriteInfo(root, slot, result);
    }

    void AttachFavoritesMenuBridge(RE::IMenu* menu)
    {
        if (!menu || !menu->uiMovie || !menu->uiMovie->asMovieRoot ||
            !menu->menuObj.IsObject()) {
            return;
        }
        auto* root = menu->uiMovie->asMovieRoot.get();
        RE::Scaleform::GFx::Value callback;
        root->CreateFunction(&callback, new FavoritesSelectionHandler());
        if (!menu->menuObj.SetMember(
                "FavoritesBanksNativeSelection", callback)) {
            REX::WARN(
                "Favorites Menu Grid could not attach the selection callback; verify Data/Interface/favoritesmenu.swf");
        }

        RE::Scaleform::GFx::Value clearCallback;
        root->CreateFunction(&clearCallback, new ClearSlotHandler());
        static_cast<void>(menu->menuObj.SetMember(
            "FavoritesBanksClearSlot", clearCallback));
        static_cast<void>(menu->menuObj.SetMember(
            "FavoritesBanksWrapNavigation",
            RE::Scaleform::GFx::Value(g_settings.wrapNavigation)));
        static_cast<void>(menu->menuObj.SetMember(
            "FavoritesBanksClearSlotKey",
            RE::Scaleform::GFx::Value(
                static_cast<double>(g_settings.clearSlotKey))));

        RE::Scaleform::GFx::Value rowCallback;
        root->CreateFunction(&rowCallback, new SwitchRowHandler());
        if (!menu->menuObj.SetMember(
                "FavoritesBanksSwitchRow", rowCallback)) {
            REX::WARN(
                "Favorites Menu Grid could not attach the row callback; the D-pad will keep its vanilla meaning");
        }

        RE::Scaleform::GFx::Value visualsCallback;
        root->CreateFunction(&visualsCallback, new NativeVisualsHandler());
        if (!menu->menuObj.SetMember(
                "FavoritesBanksCaptureVisuals", visualsCallback)) {
            REX::WARN(
                "Favorites Menu Grid could not attach the item-card callback; pages will lose their icons once another page takes the native slots");
        }
    }

    void RenderActiveBank(RE::IMenu* explicitMenu)
    {
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
        if (!menu->menuObj.HasMember("FavoritesBanksRenderPage")) {
            REX::WARN(
                "Favorites Menu Grid UI bridge is absent; the custom favoritesmenu.swf is not active");
            return;
        }

        std::size_t activeBank = 0;
        bool showingNativePage = false;
        const auto page = BuildRenderablePage(activeBank, showingNativePage);

        // The wheel unloads and reloads each position itself, and only when
        // that position's icon actually changes. Unloading all twelve here
        // first would defeat that and restart every icon's asynchronous load
        // on every page change, which is what made icons overlap.
        auto* root = menu->uiMovie->asMovieRoot.get();
        RE::Scaleform::GFx::Value items;
        root->CreateArray(&items);
        for (const auto& slot : page) {
            RE::Scaleform::GFx::Value item;
            BuildFavoriteInfo(root, slot, item);
            items.PushBack(item);
        }
        const std::array args{
            items,
            RE::Scaleform::GFx::Value(static_cast<double>(activeBank)),
            RE::Scaleform::GFx::Value(showingNativePage)
        };
        if (!menu->menuObj.Invoke(
                "FavoritesBanksRenderPage",
                nullptr,
                args.data(),
                args.size())) {
            REX::WARN(
                "Favorites Menu Grid could not render page {}",
                activeBank + 1);
        }
    }

    void RebuildFavoritesData(void* manager)
    {
        if (!manager) {
            return;
        }

        static REL::Relocation<ClearFavoritesData_t> clearData{
            REL::ID(113999)
        };
        auto* shuttlePointer = reinterpret_cast<std::byte*>(manager) + 0x08;
        if (!*reinterpret_cast<void**>(shuttlePointer)) {
            REX::WARN(
                "FavoritesData rebuild skipped: native UI event shuttle is unavailable");
            return;
        }
        clearData(shuttlePointer);

        static REL::Relocation<RebuildFavoritesData_t> rebuildData{
            REL::ID(113935)
        };
        auto* managerReference = manager;
        rebuildData(std::addressof(managerReference), manager);

        static REL::Relocation<MarkUIDataDirty_t> markDirty{ REL::ID(38843) };
        auto* favoritesData = reinterpret_cast<std::byte*>(manager) + 0xB0;
        markDirty(favoritesData);
    }
}
