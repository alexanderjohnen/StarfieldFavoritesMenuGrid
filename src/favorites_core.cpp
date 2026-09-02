#include "pch.h"
#include "favorites.h"

namespace FB
{
    Settings g_settings;
    std::mutex g_stateMutex;
    FavoriteBanks g_banks{};
    FavoriteBank g_pinnedSlots{};

    // Cards seen for the forms that take turns in a pinned slot, by form ID.
    //
    // A slot another mod owns can hold more than one thing: the deployed
    // turret and its recall item replace each other there. Each is described
    // only when the wheel happens to hand its card over, so without this the
    // slot falls back to an editor ID every time the pair swaps and stays
    // that way until the next card arrives. Remembering each form the first
    // time it is described means the second turn is already known.
    // Guarded by g_stateMutex; rebuilt each session.
    std::unordered_map<std::uint32_t, FavoriteVisual> g_pinnedVisuals;

    // When each pinned slot last really held something. Guarded by
    // g_stateMutex.
    std::array<std::chrono::steady_clock::time_point, kSlotsPerBank>
        g_pinnedSeenAt{};
    std::size_t g_activeBank{ 0 };
    std::size_t g_nativeBank{ 0 };
    std::atomic_bool g_started{ false };
    std::atomic_bool g_switchQueued{ false };
    std::atomic_bool g_captureQueued{ false };
    std::atomic_bool g_favoritesMenuVisible{ false };

    std::atomic_bool g_switchInProgress{ false };
    std::atomic_bool g_disabledForConflict{ false };
    std::atomic_uint64_t g_sessionGeneration{ 1 };

    namespace
    {
        using ClearNativeFavorite_t = void (*)(void*, std::uint8_t);
        using AssignNativeFavorite_t = void (*)(
            void*, std::uint32_t*, RE::TESForm*, std::uint8_t);
        using CreateInventoryHandle_t = void (*)(
            std::uint32_t*, std::uint32_t, RE::BGSInventoryItem*);
        using ReleaseInventoryHandle_t = void (*)(void*, std::uint32_t*);
        using GetExtraFavoriteSlot_t = std::uint8_t (*)(RE::ExtraDataList*);
        using SetExtraFavoriteSlot_t = void (*)(
            RE::ExtraDataList*, std::uint8_t);

        struct SourceIdentity
        {
            std::string fileName;
            RE::TESFormID localID{ 0 };
        };

        struct NativePage
        {
            FavoriteBank slots{};
            std::array<bool, kSlotsPerBank> orphanedHandles{};

            // Every base form the player is carrying, collected during the
            // same inventory pass that reads the slot bytes. It lets the
            // reconcile tell "this favourite's item is not in the inventory
            // right now" apart from "the player removed this favourite",
            // which look identical from the native slots alone.
            std::unordered_set<RE::TESFormID> carriedForms;

            // How many of each the player is carrying, summed over the
            // item's stacks. The wheel's card carries a count too, but that
            // card is only refreshed when the wheel itself is drawn -- so a
            // consumable used with the menu shut kept showing the number it
            // had when the card was last seen. The inventory always knows.
            std::unordered_map<RE::TESFormID, std::uint32_t> carriedCounts;
        };

        struct ResolvedInventoryRow
        {
            RE::BGSInventoryItem* row{ nullptr };
            RE::TESBoundObject* object{ nullptr };
            std::uint32_t ownerHandle{ 0 };
            RE::BSTSmartPointer<RE::TBO_InstanceData> keepAlive;
            std::string resolution;
        };

        bool g_sessionInitialized{ false };
        bool g_stateLoaded{ false };
        std::uint64_t g_characterID{ 0 };
        std::string g_loadedSaveName;

        // The save the player actually asked for, captured the moment the
        // load starts. mostRecentSaveGame cannot answer this: it names the
        // newest save on disk so "Continue" can find it, which is still the
        // newest one when the player deliberately loads an older save. Keying
        // per-save state on it therefore always resolved to the wrong file,
        // which is why every save of a character shared one set of pages.
        std::mutex g_incomingSaveMutex;
        std::string g_incomingSaveName;
        // What the game called the newest save on disk when this session
        // began. mostRecentSaveGame only changes when the game writes a
        // save, so any later difference means the player has saved since --
        // which is how a newly written save is noticed without depending on
        // an event whose delivery cannot be verified.
        std::string g_recentSaveAtSessionStart;
        std::string g_stateSource;
        std::atomic_bool g_snapshotRequested{ false };
        std::atomic_bool g_commitQueued{ false };

        // The wheel hands the DLL an item card at the moment the player picks
        // a slot, but the engine's own assignment runs immediately afterwards.
        // The descriptor therefore only exists after the next capture, so the
        // card is parked here and applied once the capture produces a slot
        // with a matching form.
        struct PendingVisual
        {
            bool valid{ false };
            std::size_t bank{ 0 };
            std::size_t slot{ 0 };
            FormIdentity form;
            FavoriteVisual visual;
        };

        PendingVisual g_pendingVisual;

        [[nodiscard]] bool EqualsIgnoreCase(
            std::string_view left,
            std::string_view right)
        {
            if (left.size() != right.size()) {
                return false;
            }
            return std::equal(
                left.begin(),
                left.end(),
                right.begin(),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                        std::tolower(static_cast<unsigned char>(b));
                });
        }

        [[nodiscard]] std::string SanitizeFileComponent(std::string_view raw)
        {
            std::string result;
            result.reserve(raw.size());
            for (const auto character : raw) {
                const auto byte = static_cast<unsigned char>(character);
                if (std::isalnum(byte) || character == '-' || character == '_') {
                    result.push_back(character);
                } else if (character == '.' || character == ' ' ||
                           character == '/' || character == '\\' ||
                           character == ':') {
                    result.push_back('_');
                }
            }
            if (result.empty()) {
                result = "unspecified";
            }
            if (result.size() > 120) {
                result.resize(120);
            }
            return result;
        }

        [[nodiscard]] std::filesystem::path GetStateRoot()
        {
            // Writable state belongs with the user's Starfield data, not in
            // the mod-manager deployment tree. SFSE already resolves the
            // redirected Windows Documents folder for its log directory.
            if (const auto logs = SFSE::log::log_directory()) {
                return logs->parent_path() / L"Plugins" /
                    kStateFolderName / L"States";
            }
            return GetPluginDirectory() / kStateFolderName / L"States";
        }

        [[nodiscard]] std::filesystem::path GetLegacyStateRoot()
        {
            return GetPluginDirectory() / kStateFolderName / L"States";
        }

        [[nodiscard]] std::filesystem::path GetCharacterStateDirectoryAt(
            const std::filesystem::path& root,
            std::uint64_t characterID)
        {
            return root / std::format(L"character-{:016X}", characterID);
        }

        [[nodiscard]] std::filesystem::path GetCharacterStateDirectory(
            std::uint64_t characterID)
        {
            return GetCharacterStateDirectoryAt(
                GetStateRoot(), characterID);
        }

        [[nodiscard]] std::filesystem::path GetCurrentStatePath(
            std::uint64_t characterID)
        {
            return GetCharacterStateDirectory(characterID) / L"current.fbs";
        }

        [[nodiscard]] std::filesystem::path GetSnapshotStatePath(
            std::uint64_t characterID,
            std::string_view saveName)
        {
            const auto safe = SanitizeFileComponent(saveName);
            return GetCharacterStateDirectory(characterID) /
                (std::filesystem::path(safe).wstring() + L".fbs");
        }

        [[nodiscard]] std::uint64_t ReadCharacterID()
        {
            const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager) {
                return 0;
            }
            return manager->currentPlayerID != 0 ?
                manager->currentPlayerID : manager->displayPlayerID;
        }

        // The name of the save a load is about to bring in. QueueLoadGame
        // parks the chosen entry in queuedEntryToLoad, and Continue routes
        // through the same call, so this is set for every ordinary load.
        // Quickload goes through its own task and leaves the entry alone, so
        // the manager's own quicksave name answers that case.
        [[nodiscard]] std::string ReadIncomingSaveName(
            RE::SaveLoadEvent::OpType operation)
        {
            const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager) {
                return {};
            }
            if (operation == RE::SaveLoadEvent::OpType::kQuickload) {
                const auto* quick = manager->quickSaveFileName.c_str();
                if (quick && *quick) {
                    return quick;
                }
            }
            if (manager->queuedEntryToLoad) {
                const auto* name = manager->queuedEntryToLoad->GetFileName();
                if (name && *name) {
                    return name;
                }
            }
            return {};
        }

        [[nodiscard]] std::string ReadMostRecentSaveName()
        {
            const auto* manager = RE::BGSSaveLoadManager::GetSingleton();
            if (!manager || !manager->mostRecentSaveGame) {
                return {};
            }
            return manager->mostRecentSaveGame;
        }

        [[nodiscard]] SourceIdentity GetSourceIdentity(RE::TESFormID formID)
        {
            SourceIdentity result;
            const auto* data = RE::TESDataHandler::GetSingleton();
            if (!data || formID == 0) {
                return result;
            }

            const auto high = static_cast<std::uint8_t>(formID >> 24);
            if (high == 0xFF) {
                return result;
            }

            const RE::TESFile* file = nullptr;
            if (high == 0xFE) {
                const auto index = static_cast<std::size_t>((formID >> 12) & 0xFFF);
                result.localID = formID & 0xFFF;
                if (index < data->compiledFileCollection.smallFiles.size()) {
                    file = data->compiledFileCollection.smallFiles[
                        static_cast<std::uint32_t>(index)];
                }
            } else if (high == 0xFD) {
                const auto index = static_cast<std::size_t>((formID >> 16) & 0xFF);
                result.localID = formID & 0xFFFF;
                if (index < data->compiledFileCollection.mediumFiles.size()) {
                    file = data->compiledFileCollection.mediumFiles[
                        static_cast<std::uint32_t>(index)];
                }
            } else {
                result.localID = formID & 0xFFFFFF;
                for (const auto* candidate :
                     data->compiledFileCollection.files) {
                    if (candidate && candidate->compileIndex == high) {
                        file = candidate;
                        break;
                    }
                }
            }

            if (file && file->fileName[0] != '\0') {
                result.fileName = file->fileName;
            }
            return result;
        }

        [[nodiscard]] FormIdentity MakeFormIdentity(const RE::TESForm* form)
        {
            FormIdentity identity;
            if (!form) {
                return identity;
            }
            identity.rawFormID = form->GetFormID();
            identity.formType = static_cast<std::uint32_t>(
                std::to_underlying(form->GetFormType()));
            const auto source = GetSourceIdentity(identity.rawFormID);
            identity.sourceFile = source.fileName;
            identity.localFormID = source.localID;
            if (const auto* editorID = form->GetFormEditorID();
                editorID && editorID[0] != '\0') {
                identity.editorID = editorID;
            }
            return identity;
        }

        [[nodiscard]] bool FormSourceMatches(
            const RE::TESForm* form,
            const FormIdentity& identity)
        {
            if (!form) {
                return false;
            }
            if (identity.formType != 0 &&
                identity.formType != static_cast<std::uint32_t>(
                    std::to_underlying(form->GetFormType()))) {
                return false;
            }
            if (identity.sourceFile.empty()) {
                return true;
            }
            const auto source = GetSourceIdentity(form->GetFormID());
            return !source.fileName.empty() &&
                EqualsIgnoreCase(source.fileName, identity.sourceFile) &&
                source.localID == identity.localFormID;
        }

        [[nodiscard]] RE::TESFormID ReconstructFormID(
            const FormIdentity& identity)
        {
            if (identity.sourceFile.empty()) {
                return 0;
            }
            const auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return 0;
            }

            for (const auto* file : data->compiledFileCollection.files) {
                if (file && EqualsIgnoreCase(file->fileName, identity.sourceFile)) {
                    return (static_cast<RE::TESFormID>(file->compileIndex) << 24) |
                        (identity.localFormID & 0xFFFFFF);
                }
            }
            for (std::size_t index = 0;
                 index < data->compiledFileCollection.mediumFiles.size();
                 ++index) {
                const auto* file = data->compiledFileCollection.mediumFiles[
                    static_cast<std::uint32_t>(index)];
                if (file && EqualsIgnoreCase(file->fileName, identity.sourceFile)) {
                    return 0xFD000000 |
                        (static_cast<RE::TESFormID>(index) << 16) |
                        (identity.localFormID & 0xFFFF);
                }
            }
            for (std::size_t index = 0;
                 index < data->compiledFileCollection.smallFiles.size();
                 ++index) {
                const auto* file = data->compiledFileCollection.smallFiles[
                    static_cast<std::uint32_t>(index)];
                if (file && EqualsIgnoreCase(file->fileName, identity.sourceFile)) {
                    return 0xFE000000 |
                        (static_cast<RE::TESFormID>(index) << 12) |
                        (identity.localFormID & 0xFFF);
                }
            }
            return 0;
        }

        [[nodiscard]] RE::TESForm* ResolveForm(const FormIdentity& identity)
        {
            if (!identity.editorID.empty()) {
                if (auto* form = RE::TESForm::LookupByEditorID(
                        RE::BSFixedString(identity.editorID.c_str()));
                    FormSourceMatches(form, identity)) {
                    return form;
                }
            }
            if (identity.rawFormID != 0) {
                if (auto* form = RE::TESForm::LookupByID(identity.rawFormID);
                    FormSourceMatches(form, identity)) {
                    return form;
                }
            }
            const auto reconstructed = ReconstructFormID(identity);
            if (reconstructed != 0) {
                if (auto* form = RE::TESForm::LookupByID(reconstructed);
                    FormSourceMatches(form, identity)) {
                    return form;
                }
            }
            return nullptr;
        }

        // TESForm::As<T>() is not a polymorphic cast. It expands to
        // `Is(T::FORMTYPE) ? static_cast<T*>(this) : nullptr`, an exact
        // form-type equality test. TESBoundObject declares no form type of its
        // own, so it inherits TESForm's SF_FORMTYPE(NONE) and the comparison
        // becomes `GetFormType() == kNONE`, which is false for every real
        // object: a weapon is kWEAP, a medkit is kALCH. As<TESBoundObject>()
        // therefore returned null for every single inventory item ever passed
        // to it, which is why no item could be written to a native slot and
        // why version 0.4 emptied page 1.
        //
        // IsBoundObject is the engine's own virtual test, and
        // TESForm <- TESObject <- TESBoundObject is a single inheritance
        // chain, so the cast is well defined once that test passes.
        [[nodiscard]] RE::TESBoundObject* AsBoundObject(RE::TESForm* form)
        {
            return form && form->IsBoundObject() ?
                static_cast<RE::TESBoundObject*>(form) :
                nullptr;
        }

        [[nodiscard]] bool SameFormIdentity(
            const FormIdentity& left,
            const FormIdentity& right)
        {
            if (!left.sourceFile.empty() && !right.sourceFile.empty()) {
                return EqualsIgnoreCase(left.sourceFile, right.sourceFile) &&
                    left.localFormID == right.localFormID &&
                    (left.formType == 0 || right.formType == 0 ||
                     left.formType == right.formType);
            }
            return left.rawFormID != 0 && left.rawFormID == right.rawFormID &&
                (left.formType == 0 || right.formType == 0 ||
                 left.formType == right.formType);
        }

        [[nodiscard]] std::vector<UniqueIdentity> ReadUniqueIdentities(
            const RE::BGSInventoryItem& item)
        {
            std::vector<UniqueIdentity> identities;
            for (const auto& stack : item.stacks) {
                if (!stack.extra) {
                    continue;
                }
                const auto* extra = stack.extra->GetByType(
                    RE::ExtraDataType::kUniqueID);
                if (!extra) {
                    continue;
                }
                const auto* bytes = reinterpret_cast<const std::byte*>(extra);
                UniqueIdentity identity;
                std::memcpy(
                    std::addressof(identity.uniqueID),
                    bytes + 0x18,
                    sizeof(identity.uniqueID));
                std::memcpy(
                    std::addressof(identity.baseID),
                    bytes + 0x1C,
                    sizeof(identity.baseID));
                identities.push_back(identity);
            }
            std::ranges::sort(identities);
            identities.erase(
                std::unique(identities.begin(), identities.end()),
                identities.end());
            return identities;
        }

        [[nodiscard]] bool UniqueIdentitiesIntersect(
            const std::vector<UniqueIdentity>& left,
            const std::vector<UniqueIdentity>& right)
        {
            for (const auto& a : left) {
                if (std::ranges::find(right, a) != right.end()) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] FavoriteSlot MakeInventorySlot(
            const RE::BGSInventoryItem& item,
            std::uint32_t ordinal)
        {
            FavoriteSlot slot;
            slot.kind = FavoriteKind::kInventory;
            slot.form = MakeFormIdentity(item.object);
            slot.uniqueIDs = ReadUniqueIdentities(item);
            slot.rowOrdinal = ordinal;
            slot.sessionInstanceData = item.instanceData;
            slot.unresolved = false;
            return slot;
        }

        [[nodiscard]] FavoriteSlot MakeFormSlot(const RE::TESForm* form)
        {
            FavoriteSlot slot;
            slot.kind = FavoriteKind::kForm;
            slot.form = MakeFormIdentity(form);
            slot.unresolved = false;
            return slot;
        }

        [[nodiscard]] std::string ReadGFxString(
            const RE::Scaleform::GFx::Value& object,
            std::string_view member)
        {
            RE::Scaleform::GFx::Value value;
            if (!object.IsObject() || !object.GetMember(member, &value) ||
                !value.IsString()) {
                return {};
            }
            const auto* string = value.GetString();
            return string ? string : "";
        }

        [[nodiscard]] double ReadGFxNumber(
            const RE::Scaleform::GFx::Value& object,
            std::string_view member,
            double fallback = 0.0)
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

        [[nodiscard]] bool ReadGFxBoolean(
            const RE::Scaleform::GFx::Value& object,
            std::string_view member,
            bool fallback)
        {
            RE::Scaleform::GFx::Value value;
            if (!object.IsObject() || !object.GetMember(member, &value)) {
                return fallback;
            }
            if (value.IsBoolean()) {
                return value.GetBoolean();
            }
            return fallback;
        }

        [[nodiscard]] FavoriteVisual CaptureVisual(
            const RE::Scaleform::GFx::Value* item)
        {
            FavoriteVisual visual;
            if (!item || !item->IsObject()) {
                return visual;
            }

            visual.name = ReadGFxString(*item, "sName");
            visual.count = static_cast<std::uint32_t>(std::max(
                1.0, ReadGFxNumber(*item, "uCount", 1.0)));
            visual.isPower = ReadGFxBoolean(*item, "bIsPower", false);
            visual.isEquippable = ReadGFxBoolean(
                *item, "bIsEquippable", true);
            visual.isEquipped = ReadGFxBoolean(
                *item, "bIsEquipped", false);
            visual.ammoName = ReadGFxString(*item, "sAmmoName");
            visual.ammoCount = static_cast<std::uint32_t>(std::max(
                0.0, ReadGFxNumber(*item, "uAmmoCount", 0.0)));

            RE::Scaleform::GFx::Value icon;
            if (item->GetMember("iconImage", &icon) && icon.IsObject()) {
                visual.fixtureType = static_cast<std::int32_t>(
                    ReadGFxNumber(icon, "iFixtureType", 0.0));
                visual.imageDirectory = ReadGFxString(icon, "sDirectory");
                visual.imageName = ReadGFxString(icon, "sImageName");
            }

            RE::Scaleform::GFx::Value stats;
            if (item->GetMember("aElementalStats", &stats) && stats.IsArray()) {
                class ElementalVisitor final :
                    public RE::Scaleform::GFx::Value::ArrayVisitor
                {
                public:
                    explicit ElementalVisitor(std::vector<ElementalStat>& output) :
                        output_(output)
                    {}

                    void Visit(
                        std::uint32_t,
                        const RE::Scaleform::GFx::Value& entry) override
                    {
                        if (output_.size() >= 16 || !entry.IsObject()) {
                            return;
                        }
                        output_.push_back(ElementalStat{
                            static_cast<std::int32_t>(
                                ReadGFxNumber(
                                    entry, "iElementalType", 0.0)),
                            ReadGFxNumber(entry, "fValue", 0.0)
                        });
                    }

                private:
                    std::vector<ElementalStat>& output_;
                } visitor(visual.elementalStats);
                stats.VisitElements(&visitor);
            }
            return visual;
        }

        [[nodiscard]] RE::TESForm* GetPendingAssignedForm(void* manager)
        {
            if (!manager) {
                return nullptr;
            }
            return *reinterpret_cast<RE::TESForm**>(
                reinterpret_cast<std::byte*>(manager) + 0x428);
        }

        [[nodiscard]] std::uint8_t ReadExtraFavoriteSlot(
            RE::ExtraDataList* extra)
        {
            if (!extra) {
                return kNoFavoriteSlot;
            }
            static REL::Relocation<GetExtraFavoriteSlot_t> getSlot{
                REL::ID(45008)
            };
            return getSlot(extra);
        }

        void RemoveExtraFavorite(RE::ExtraDataList* extra)
        {
            if (!extra) {
                return;
            }
            static REL::Relocation<SetExtraFavoriteSlot_t> setSlot{
                REL::ID(45007)
            };
            setSlot(extra, kNoFavoriteSlot);
        }

        [[nodiscard]] void* GetFavoritesManager()
        {
            static REL::Relocation<void**> singleton{ REL::ID(938946) };
            return *singleton;
        }

        [[nodiscard]] RE::TESForm** GetManagerFavoriteForms(void* manager)
        {
            if (!manager) {
                return nullptr;
            }
            auto* bytes = reinterpret_cast<std::byte*>(manager);
            const auto count = *reinterpret_cast<std::uint32_t*>(bytes + 0x430);
            if (count < kSlotsPerBank) {
                return nullptr;
            }
            const auto mode = *reinterpret_cast<std::int32_t*>(bytes + 0x438);
            if (mode < 0) {
                return reinterpret_cast<RE::TESForm**>(bytes + 0x440);
            }
            return *reinterpret_cast<RE::TESForm***>(bytes + 0x440);
        }

        [[nodiscard]] std::uint32_t* GetManagerFavoriteHandles(void* manager)
        {
            if (!manager) {
                return nullptr;
            }
            auto* bytes = reinterpret_cast<std::byte*>(manager);
            const auto mode = *reinterpret_cast<std::int32_t*>(bytes + 0x510);
            if (mode < 0) {
                return reinterpret_cast<std::uint32_t*>(bytes + 0x518);
            }
            return *reinterpret_cast<std::uint32_t**>(bytes + 0x518);
        }

        [[nodiscard]] bool NativeFavoritesReady()
        {
            auto* manager = GetFavoritesManager();
            return manager && GetManagerFavoriteForms(manager) &&
                GetManagerFavoriteHandles(manager) &&
                RE::PlayerCharacter::GetSingleton();
        }

        void NativeClearSlot(void* manager, std::uint8_t slot)
        {
            static REL::Relocation<ClearNativeFavorite_t> clear{
                REL::ID(113919)
            };
            clear(manager, slot);
        }

        void ReleaseLocalInventoryHandle(std::uint32_t& handle)
        {
            if (handle == kInvalidFavoriteHandle) {
                return;
            }
            static REL::Relocation<void**> handleManagerSingleton{
                REL::ID(883301)
            };
            static REL::Relocation<ReleaseInventoryHandle_t> release{
                REL::ID(48490)
            };
            if (auto* handleManager = *handleManagerSingleton) {
                release(handleManager, std::addressof(handle));
            }
            handle = kInvalidFavoriteHandle;
        }

        // Asks the engine for an inventory handle referring to one exact row.
        // This is the single operation that has never been observed to succeed
        // in this project: version 0.4 emptied page 1 because of it, and 0.6.0
        // repeated the mistake by trusting it. Every failure is now logged
        // with the values that went in, and no caller may destroy favorite
        // state before proving this works.
        [[nodiscard]] bool CreateRowHandle(
            std::uint32_t& handle,
            std::uint32_t ownerHandle,
            RE::BGSInventoryItem* row)
        {
            handle = kInvalidFavoriteHandle;
            if (!row) {
                return false;
            }
            static REL::Relocation<CreateInventoryHandle_t> createHandle{
                REL::ID(48481)
            };
            createHandle(std::addressof(handle), ownerHandle, row);
            if (handle == kInvalidFavoriteHandle) {
                REX::WARN(
                    "Inventory handle creation failed: owner={:08X} row={} object={:08X} stacks={}",
                    ownerHandle,
                    static_cast<const void*>(row),
                    row->object ? row->object->GetFormID() : 0,
                    row->stacks.size());
                return false;
            }
            return true;
        }

        [[nodiscard]] bool NativeAssignInventory(
            void* manager,
            RE::TESBoundObject* object,
            RE::BGSInventoryItem* row,
            std::uint32_t ownerHandle,
            std::uint8_t slot)
        {
            if (!manager || !object || !row) {
                return false;
            }
            static REL::Relocation<AssignNativeFavorite_t> assign{
                REL::ID(113920)
            };
            auto handle = kInvalidFavoriteHandle;
            if (!CreateRowHandle(handle, ownerHandle, row)) {
                return false;
            }
            assign(manager, std::addressof(handle), object, slot);
            ReleaseLocalInventoryHandle(handle);
            return true;
        }

        [[nodiscard]] bool NativeAssignForm(
            void* manager,
            RE::TESForm* form,
            std::uint8_t slot)
        {
            if (!manager || !form) {
                return false;
            }
            static REL::Relocation<AssignNativeFavorite_t> assign{
                REL::ID(113920)
            };
            auto invalidHandle = kInvalidFavoriteHandle;
            assign(manager, std::addressof(invalidHandle), form, slot);
            return true;
        }

        [[nodiscard]] bool WriteStateFile(
            const std::filesystem::path& path,
            std::uint64_t characterID,
            std::string_view saveName)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) {
                REX::WARN(
                    "Could not create Favorites Menu Grid state directory '{}': {}",
                    path.parent_path().string(),
                    error.message());
                return false;
            }

            auto temporary = path;
            temporary += std::format(L".tmp-{}", GetCurrentProcessId());
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output) {
                    REX::WARN(
                        "Could not open Favorites Menu Grid temporary state '{}'",
                        temporary.string());
                    return false;
                }
                output << "FAVORITES_BANKS_STATE " << kStateVersion << '\n';
                output << "CHARACTER " << characterID << '\n';
                output << "BANK_COUNT " << g_settings.rowCount << '\n';
                output << "ACTIVE_BANK " << g_activeBank << '\n';
                // Which page the real twelve slots currently hold. Without
                // this the next session cannot tell whether the save's native
                // favorites belong to page 1 or to any other page.
                output << "NATIVE_BANK " << g_nativeBank << '\n';
                output << "SAVE_NAME " << std::quoted(std::string(saveName)) << '\n';
                // Preserve banks that are temporarily disabled by lowering
                // BankCount. Raising it again must not silently erase them.
                for (std::size_t bank = 0; bank < kMaxBanks; ++bank) {
                    for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                        const auto& slot = g_banks[bank][index];
                        if (slot.Empty()) {
                            continue;
                        }
                        output << "SLOT " << bank << ' ' << index << ' '
                               << static_cast<unsigned>(slot.kind) << ' '
                               << slot.form.rawFormID << ' '
                               << slot.form.formType << ' '
                               << slot.form.localFormID << ' '
                               << slot.rowOrdinal << ' '
                               << std::quoted(slot.form.sourceFile) << ' '
                               << std::quoted(slot.form.editorID) << ' '
                               << slot.uniqueIDs.size();
                        for (const auto& unique : slot.uniqueIDs) {
                            output << ' ' << unique.uniqueID << ' ' << unique.baseID;
                        }
                        output << '\n';
                        if (slot.visual.HasData()) {
                            output << "VISUAL " << bank << ' ' << index << ' '
                                   << slot.visual.count << ' '
                                   << static_cast<unsigned>(slot.visual.isPower) << ' '
                                   << static_cast<unsigned>(slot.visual.isEquippable) << ' '
                                   << static_cast<unsigned>(slot.visual.isEquipped) << ' '
                                   << slot.visual.ammoCount << ' '
                                   << slot.visual.fixtureType << ' '
                                   << std::quoted(slot.visual.name) << ' '
                                   << std::quoted(slot.visual.ammoName) << ' '
                                   << std::quoted(slot.visual.imageDirectory) << ' '
                                   << std::quoted(slot.visual.imageName) << ' '
                                   << slot.visual.elementalStats.size();
                            output << std::setprecision(
                                std::numeric_limits<double>::max_digits10);
                            for (const auto& stat : slot.visual.elementalStats) {
                                output << ' ' << stat.type << ' ' << stat.value;
                            }
                            output << '\n';
                        }
                    }
                }
                output << "END\n";
                output.flush();
                if (!output) {
                    output.close();
                    std::filesystem::remove(temporary, error);
                    REX::WARN(
                        "Writing Favorites Menu Grid state '{}' failed",
                        temporary.string());
                    return false;
                }
            }

            const auto backup = path.wstring() + L".bak";
            if (std::filesystem::exists(path)) {
                CopyFileW(path.c_str(), backup.c_str(), FALSE);
            }
            if (!MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const auto windowsError = GetLastError();
                std::filesystem::remove(temporary, error);
                REX::WARN(
                    "Atomic replacement of Favorites Menu Grid state '{}' failed (Windows error {})",
                    path.string(),
                    windowsError);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool ReadStateFile(
            const std::filesystem::path& path,
            std::uint64_t expectedCharacterID,
            std::string& storedSaveName)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                return false;
            }

            std::string header;
            std::uint32_t version = 0;
            if (!(input >> header >> version) ||
                header != "FAVORITES_BANKS_STATE" ||
                version < 3 || version > kStateVersion) {
                REX::WARN(
                    "Ignoring unsupported or damaged Favorites Menu Grid state '{}'",
                    path.string());
                return false;
            }

            FavoriteBanks loadedBanks{};
            std::uint64_t storedCharacterID = 0;
            std::size_t storedBankCount = 0;
            std::size_t storedActiveBank = 0;
            // Files written before version 5 always kept page 1 in the real
            // slots, so zero is the correct migration default.
            std::size_t storedNativeBank = 0;
            std::string tag;
            bool sawEnd = false;
            while (input >> tag) {
                if (tag == "CHARACTER") {
                    input >> storedCharacterID;
                } else if (tag == "BANK_COUNT") {
                    input >> storedBankCount;
                } else if (tag == "ACTIVE_BANK") {
                    input >> storedActiveBank;
                } else if (tag == "NATIVE_BANK") {
                    input >> storedNativeBank;
                } else if (tag == "SAVE_NAME") {
                    input >> std::quoted(storedSaveName);
                } else if (tag == "SLOT") {
                    std::size_t bank = 0;
                    std::size_t index = 0;
                    unsigned kind = 0;
                    FavoriteSlot slot;
                    std::size_t uniqueCount = 0;
                    if (!(input >> bank >> index >> kind >>
                          slot.form.rawFormID >> slot.form.formType >>
                          slot.form.localFormID >> slot.rowOrdinal >>
                          std::quoted(slot.form.sourceFile) >>
                          std::quoted(slot.form.editorID) >> uniqueCount)) {
                        return false;
                    }
                    if (bank >= kMaxBanks || index >= kSlotsPerBank ||
                        kind < static_cast<unsigned>(FavoriteKind::kInventory) ||
                        kind > static_cast<unsigned>(FavoriteKind::kForm) ||
                        uniqueCount > 64) {
                        REX::WARN(
                            "Ignoring invalid slot record in Favorites Menu Grid state '{}'",
                            path.string());
                        return false;
                    }
                    slot.kind = static_cast<FavoriteKind>(kind);
                    slot.unresolved = true;
                    slot.uniqueIDs.reserve(uniqueCount);
                    for (std::size_t i = 0; i < uniqueCount; ++i) {
                        UniqueIdentity unique;
                        unsigned uniqueID = 0;
                        if (!(input >> uniqueID >> unique.baseID) ||
                            uniqueID > std::numeric_limits<std::uint16_t>::max()) {
                            return false;
                        }
                        unique.uniqueID = static_cast<std::uint16_t>(uniqueID);
                        slot.uniqueIDs.push_back(unique);
                    }
                    loadedBanks[bank][index] = std::move(slot);
                } else if (tag == "VISUAL") {
                    std::size_t bank = 0;
                    std::size_t index = 0;
                    unsigned isPower = 0;
                    unsigned isEquippable = 0;
                    unsigned isEquipped = 0;
                    FavoriteVisual visual;
                    std::size_t elementalCount = 0;
                    if (!(input >> bank >> index >> visual.count >> isPower >>
                          isEquippable >> isEquipped >> visual.ammoCount >>
                          visual.fixtureType >> std::quoted(visual.name) >>
                          std::quoted(visual.ammoName) >>
                          std::quoted(visual.imageDirectory) >>
                          std::quoted(visual.imageName) >> elementalCount) ||
                        bank >= kMaxBanks || index >= kSlotsPerBank ||
                        isPower > 1 || isEquippable > 1 || isEquipped > 1 ||
                        elementalCount > 16) {
                        REX::WARN(
                            "Ignoring invalid visual record in Favorites Menu Grid state '{}'",
                            path.string());
                        return false;
                    }
                    visual.isPower = isPower != 0;
                    visual.isEquippable = isEquippable != 0;
                    visual.isEquipped = isEquipped != 0;
                    visual.elementalStats.reserve(elementalCount);
                    for (std::size_t i = 0; i < elementalCount; ++i) {
                        ElementalStat stat;
                        if (!(input >> stat.type >> stat.value)) {
                            return false;
                        }
                        visual.elementalStats.push_back(stat);
                    }
                    loadedBanks[bank][index].visual = std::move(visual);
                } else if (tag == "END") {
                    sawEnd = true;
                    break;
                } else {
                    REX::WARN(
                        "Ignoring unknown token '{}' in Favorites Menu Grid state '{}'",
                        tag,
                        path.string());
                    return false;
                }
                if (!input) {
                    return false;
                }
            }

            if (!sawEnd ||
                (expectedCharacterID != 0 && storedCharacterID != 0 &&
                 expectedCharacterID != storedCharacterID)) {
                REX::WARN(
                    "Favorites Menu Grid state '{}' does not belong to the current character",
                    path.string());
                return false;
            }

            g_banks = std::move(loadedBanks);
            g_activeBank = std::min(
                storedActiveBank,
                g_settings.rowCount - 1);
            g_nativeBank = std::min(
                storedNativeBank,
                g_settings.rowCount - 1);
            REX::INFO(
                "Favorites Menu Grid state loaded from '{}': {} stored bank(s), page {} shown, page {} recorded in the native slots",
                path.string(),
                storedBankCount,
                g_activeBank + 1,
                g_nativeBank + 1);
            return true;
        }

        [[nodiscard]] NativePage CaptureNativePage(
            void* manager,
            RE::PlayerCharacter* player)
        {
            NativePage page;
            if (!manager || !player) {
                return page;
            }

            {
                const auto inventoryGuard = player->inventoryList.LockRead();
                const auto* inventory = *inventoryGuard;
                if (!inventory) {
                    return page;
                }
                std::unordered_map<RE::TESFormID, std::uint32_t> ordinals;
                for (const auto& item : inventory->data) {
                    if (!item.object) {
                        continue;
                    }
                    const auto formID = item.object->GetFormID();
                    // Recorded for every row, not just favorited ones: the
                    // reconcile needs to know the item exists at all.
                    page.carriedForms.insert(formID);
                    std::uint32_t stacked = 0;
                    for (const auto& stack : item.stacks) {
                        stacked += stack.count;
                    }
                    page.carriedCounts[formID] += std::max(stacked, 1u);
                    const auto ordinal = ordinals[formID]++;
                    const auto nativeSlot = static_cast<int>(item.unk24);
                    if (nativeSlot < 0 ||
                        nativeSlot >= static_cast<int>(kSlotsPerBank)) {
                        continue;
                    }
                    auto& destination =
                        page.slots[static_cast<std::size_t>(nativeSlot)];
                    if (!destination.Empty()) {
                        REX::WARN(
                            "Native inventory has two rows claiming favorite slot {}; keeping the first",
                            nativeSlot + 1);
                        continue;
                    }
                    destination = MakeInventorySlot(item, ordinal);
                }
            }

            const auto* forms = GetManagerFavoriteForms(manager);
            const auto* handles = GetManagerFavoriteHandles(manager);
            if (!forms || !handles) {
                return page;
            }
            for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                if (!page.slots[slot].Empty()) {
                    continue;
                }
                if (handles[slot] != kInvalidFavoriteHandle) {
                    page.orphanedHandles[slot] = true;
                    REX::WARN(
                        "Native favorite slot {} has handle {:08X} but no matching inventory row",
                        slot + 1,
                        handles[slot]);
                    continue;
                }
                if (forms[slot]) {
                    page.slots[slot] = MakeFormSlot(forms[slot]);
                }
            }
            return page;
        }

        // Whether the item behind a stored descriptor is in the player's
        // inventory at this moment. Powers and plain forms have no inventory
        // row to lose, so they always count as present.
        [[nodiscard]] bool DescriptorItemIsCarried(
            const FavoriteSlot& stored,
            const NativePage& nativePage)
        {
            if (stored.kind != FavoriteKind::kInventory) {
                return true;
            }
            const auto* form = ResolveForm(stored.form);
            return form && nativePage.carriedForms.contains(form->GetFormID());
        }

        void ReconcileNativePageWithBank(
            std::size_t bank,
            const NativePage& nativePage,
            bool preserveUnresolved)
        {
            if (bank >= kMaxBanks) {
                return;
            }

            // Favourites the engine re-slotted on its own must not be allowed
            // to rewrite the stored page. Script-owned items -- follower
            // command tokens, mod toggle items -- are pulled out of the
            // inventory and handed back by their own scripts without the
            // player doing anything, and the engine gives such an item
            // whichever native slot happens to be free when it returns. The
            // stored page is the only record of where it actually belongs,
            // so the move is ignored here and the next commit puts the native
            // slots back in line with the page.
            //
            // A deliberate reassignment looks the same from the native slots,
            // which is why g_pendingVisual gates this: it is set by
            // ProcessMenuSelection for exactly the case where the player just
            // put an item on a slot themselves, and is still set when the
            // reconcile that follows runs.
            std::array<bool, kSlotsPerBank> ignoreNative{};
            std::array<bool, kSlotsPerBank> keepStored{};
            if (preserveUnresolved && !g_pendingVisual.valid) {
                for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                    const auto& native = nativePage.slots[slot];
                    if (native.Empty() || !g_banks[bank][slot].Empty()) {
                        continue;
                    }
                    for (std::size_t home = 0; home < kSlotsPerBank; ++home) {
                        if (home == slot ||
                            !nativePage.slots[home].Empty() ||
                            keepStored[home]) {
                            continue;
                        }
                        const auto& stored = g_banks[bank][home];
                        if (stored.Empty() || stored.kind != native.kind ||
                            !SameFormIdentity(stored.form, native.form)) {
                            continue;
                        }
                        ignoreNative[slot] = true;
                        keepStored[home] = true;
                        REX::WARN(
                            "'{}' was re-slotted by the game from page {} slot {} to slot {} without the wheel being used; keeping it on slot {}. The mod that owns this item most likely took it out of the inventory and handed it back.",
                            !stored.visual.name.empty() ?
                                stored.visual.name : stored.form.editorID,
                            bank + 1,
                            home + 1,
                            slot + 1,
                            home + 1);
                        break;
                    }
                }
            }

            for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                const auto& native = nativePage.slots[slot];
                auto& stored = g_banks[bank][slot];
                if (g_settings.externallyManagedSlots[slot]) {
                    // Owned by another mod. Whatever it holds is recorded
                    // once, for drawing, and never enters a page: doing so
                    // copied the item into every page the player visited.
                    //
                    // An empty read is not news, though. The owning mod
                    // swaps its item out and back: the whole reason such a
                    // slot is reserved is that two forms share it -- the
                    // deployed turret and its recall item, one replacing
                    // the other -- and unequipping the gear set takes both
                    // away for a while. Between the two writes the slot
                    // reads empty for a moment, and recording that gap is
                    // what made the icon blink out and stay out until
                    // something happened to bring it back. The last thing
                    // really seen there is kept instead, and a real item --
                    // either of the two -- replaces it as soon as it
                    // appears.
                    if (!native.Empty()) {
                        // The native read knows the form and nothing about
                        // how to draw it; the card harvested from the wheel
                        // knows how to draw it and arrives at its own pace.
                        // Whichever lands second must not erase the first.
                        auto known = std::move(g_pinnedSlots[slot].visual);
                        const auto sameItem =
                            SameFormIdentity(g_pinnedSlots[slot].form,
                                             native.form);
                        g_pinnedSlots[slot] = native;
                        if (!g_pinnedSlots[slot].visual.HasData()) {
                            if (sameItem && known.HasData()) {
                                g_pinnedSlots[slot].visual = std::move(known);
                            } else if (const auto seen = g_pinnedVisuals.find(
                                           native.form.rawFormID);
                                       seen != g_pinnedVisuals.end()) {
                                // The other form of the pair, described on
                                // one of its earlier turns.
                                g_pinnedSlots[slot].visual = seen->second;
                            }
                        }
                        g_pinnedSeenAt[slot] =
                            std::chrono::steady_clock::now();
                    } else if (!g_pinnedSlots[slot].Empty()) {
                        // The grace for an empty read, and no more than a
                        // grace. Between the two forms that share this slot
                        // swapping places it reads empty for a moment, and
                        // holding the last one across that gap is what stops
                        // the icon blinking out. But an item that is really
                        // gone -- turret destroyed, recall item used -- must
                        // not sit there for the rest of the session, so the
                        // hold expires.
                        if (std::chrono::steady_clock::now() -
                                g_pinnedSeenAt[slot] > 3s) {
                            g_pinnedSlots[slot] = FavoriteSlot{};
                        }
                    }
                    stored = FavoriteSlot{};
                    continue;
                }
                if (ignoreNative[slot] || keepStored[slot]) {
                    // One half of a move the engine made on its own: either
                    // the slot it moved the item into, which must not be
                    // adopted, or the slot it belongs on, which must not be
                    // cleared. Both keep whatever the page already holds.
                    continue;
                }
                if (!native.Empty()) {
                    // The native capture reads inventory rows and manager
                    // arrays; it has no access to the Scaleform item card.
                    // Overwriting blindly would erase the icon and name that
                    // let this page be drawn while some other page is the one
                    // written into the real slots.
                    auto carriedVisual = SameFormIdentity(stored.form, native.form) &&
                            stored.visual.HasData() ?
                        stored.visual :
                        FavoriteVisual{};
                    stored = native;
                    if (carriedVisual.HasData()) {
                        stored.visual = std::move(carriedVisual);
                    }
                } else if (preserveUnresolved && !stored.Empty() &&
                           (stored.unresolved ||
                            nativePage.orphanedHandles[slot] ||
                            !DescriptorItemIsCarried(stored, nativePage))) {
                    // An item that is not in the inventory right now is the
                    // case the sidecar exists for: the descriptor stays and
                    // becomes usable again if the item returns. Only a commit
                    // used to set that marker, so a favourite whose item left
                    // while its page was the native one was simply deleted --
                    // and with it the only record of where a script-owned
                    // item belongs once the engine hands it a new slot.
                    stored.unresolved = true;
                } else {
                    stored = FavoriteSlot{};
                }
            }
        }

        [[nodiscard]] int ScoreStoredBank(
            const FavoriteBank& stored,
            const NativePage& native,
            std::size_t& matches,
            std::size_t& conflicts)
        {
            int score = 0;
            matches = 0;
            conflicts = 0;
            for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                const auto& current = native.slots[slot];
                const auto& candidate = stored[slot];
                if (current.Empty() && candidate.Empty()) {
                    ++score;
                    continue;
                }
                if (current.Empty()) {
                    continue;
                }
                if (candidate.Empty()) {
                    score -= 5;
                    continue;
                }
                if (current.kind == candidate.kind &&
                    SameFormIdentity(current.form, candidate.form)) {
                    score += 20;
                    ++matches;
                    if (current.kind == FavoriteKind::kInventory &&
                        !current.uniqueIDs.empty() &&
                        !candidate.uniqueIDs.empty() &&
                        UniqueIdentitiesIntersect(
                            current.uniqueIDs, candidate.uniqueIDs)) {
                        score += 10;
                    }
                } else {
                    score -= 25;
                    ++conflicts;
                }
            }
            return score;
        }

        [[nodiscard]] std::size_t ChooseBankForNativePage(
            const NativePage& nativePage,
            std::size_t preferredBank)
        {
            const auto nonEmpty = static_cast<std::size_t>(std::count_if(
                nativePage.slots.begin(),
                nativePage.slots.end(),
                [](const FavoriteSlot& slot) { return !slot.Empty(); }));
            if (nonEmpty == 0) {
                return std::min(preferredBank, g_settings.rowCount - 1);
            }

            std::size_t bestBank = std::min(
                preferredBank,
                g_settings.rowCount - 1);
            int bestScore = std::numeric_limits<int>::min();
            std::size_t bestMatches = 0;
            std::size_t bestConflicts = std::numeric_limits<std::size_t>::max();
            for (std::size_t bank = 0; bank < g_settings.rowCount; ++bank) {
                std::size_t matches = 0;
                std::size_t conflicts = 0;
                const auto score = ScoreStoredBank(
                    g_banks[bank], nativePage, matches, conflicts);
                const auto better =
                    score > bestScore ||
                    (score == bestScore && matches > bestMatches) ||
                    (score == bestScore && matches == bestMatches &&
                     conflicts < bestConflicts) ||
                    (score == bestScore && matches == bestMatches &&
                     conflicts == bestConflicts && bank == preferredBank);
                if (better) {
                    bestBank = bank;
                    bestScore = score;
                    bestMatches = matches;
                    bestConflicts = conflicts;
                }
            }

            if (bestMatches == 0 || bestConflicts != 0) {
                for (std::size_t bank = 0; bank < g_settings.rowCount; ++bank) {
                    const auto empty = std::ranges::all_of(
                        g_banks[bank],
                        [](const FavoriteSlot& slot) { return slot.Empty(); });
                    if (empty) {
                        REX::INFO(
                            "Loaded native page did not exactly match the sidecar; importing it into empty bank {}",
                            bank + 1);
                        return bank;
                    }
                }
                REX::WARN(
                    "Loaded native page did not exactly match any stored bank; reconciling it with bank {}",
                    bestBank + 1);
            }
            return bestBank;
        }

        [[nodiscard]] ResolvedInventoryRow ResolveInventoryRow(
            RE::PlayerCharacter* player,
            const FavoriteSlot& descriptor)
        {
            ResolvedInventoryRow result;
            if (!player || descriptor.kind != FavoriteKind::kInventory) {
                return result;
            }
            auto* form = ResolveForm(descriptor.form);
            auto* object = AsBoundObject(form);
            if (!object) {
                return result;
            }

            struct Candidate
            {
                RE::BGSInventoryItem* row{ nullptr };
                RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
                std::vector<UniqueIdentity> uniqueIDs;
                std::uint32_t ordinal{ 0 };
            };
            std::vector<Candidate> candidates;
            {
                const auto inventoryGuard = player->inventoryList.LockRead();
                auto* inventory = *inventoryGuard;
                if (!inventory) {
                    return result;
                }
                result.ownerHandle = inventory->ownerHandle;
                std::uint32_t ordinal = 0;
                for (auto& item : inventory->data) {
                    if (!item.object ||
                        item.object->GetFormID() != object->GetFormID()) {
                        continue;
                    }
                    candidates.push_back(Candidate{
                        std::addressof(item),
                        item.instanceData,
                        ReadUniqueIdentities(item),
                        ordinal++
                    });
                }
            }

            if (candidates.empty()) {
                return result;
            }
            auto choose = [&](Candidate& candidate, std::string_view reason) {
                result.row = candidate.row;
                result.object = candidate.row ? candidate.row->object : object;
                result.keepAlive = candidate.instanceData;
                result.resolution = reason;
            };

            if (descriptor.sessionInstanceData) {
                std::vector<Candidate*> pointerMatches;
                for (auto& candidate : candidates) {
                    if (candidate.instanceData.get() ==
                        descriptor.sessionInstanceData.get()) {
                        pointerMatches.push_back(std::addressof(candidate));
                    }
                }
                if (pointerMatches.size() == 1) {
                    choose(*pointerMatches.front(), "same-session instance data");
                    return result;
                }
            }

            if (!descriptor.uniqueIDs.empty()) {
                std::vector<Candidate*> uniqueMatches;
                for (auto& candidate : candidates) {
                    if (UniqueIdentitiesIntersect(
                            descriptor.uniqueIDs, candidate.uniqueIDs)) {
                        uniqueMatches.push_back(std::addressof(candidate));
                    }
                }
                if (uniqueMatches.size() == 1) {
                    choose(*uniqueMatches.front(), "persistent ExtraUniqueID");
                    return result;
                }
                if (uniqueMatches.size() > 1) {
                    REX::WARN(
                        "More than one inventory row matched ExtraUniqueID for form {:08X}; refusing an ambiguous restore",
                        object->GetFormID());
                    return result;
                }
            }

            if (candidates.size() == 1) {
                choose(candidates.front(), "sole matching base form row");
                return result;
            }

            if (descriptor.uniqueIDs.empty() &&
                descriptor.rowOrdinal < candidates.size()) {
                auto& candidate = candidates[descriptor.rowOrdinal];
                choose(candidate, "same-base row ordinal fallback");
                REX::WARN(
                    "Restored form {:08X} by row ordinal because no persistent unique ID was available",
                    object->GetFormID());
                return result;
            }

            REX::WARN(
                "Could not safely distinguish {} inventory rows for form {:08X}; slot remains unresolved",
                candidates.size(),
                object->GetFormID());
            return result;
        }

        // Versions up to 0.5 re-implemented "use this favorite" inside the
        // plugin: a hand-rolled power-equip sequence built on raw executable
        // offsets, and ActorEquipManager::EquipObject for everything else.
        // That reimplementation is gone. It could never be complete, because
        // the wheel's own item cards report bIsEquippable = 0 for aid items,
        // which are consumed rather than equipped; medkits therefore did
        // nothing at all. The active page is now committed into the real
        // favorite slots instead, so Starfield's own quickslot handler
        // performs every use, for every item category, exactly as it does on
        // an unmodded wheel.
        //
        // FavoritesMenu_UseQuickkey has no case of its own for "this is
        // already equipped": selecting an equipped weapon's slot again just
        // replays the same equip, unlike holstering. TryToggleUnequipLocked
        // is a narrow, additive exception to the paragraph above, not a
        // second reimplementation of use: it never calls EquipObject, it
        // only ever unequips, and only for the one case the vanilla handler
        // has no answer for. Every other slot -- aid, powers, ammo,
        // assignment, or a slot that is not currently worn or wielded --
        // still goes through the vanilla dispatch exactly as before.
        [[nodiscard]] bool TryToggleUnequipLocked(
            const FavoriteSlot& slot,
            std::size_t bank,
            std::size_t index)
        {
            if (slot.kind != FavoriteKind::kInventory ||
                slot.visual.isPower || !slot.visual.isEquippable) {
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }

            // The wheel's cached visual only reflects the last time this
            // slot's page was native and its item card was harvested, so it
            // goes stale the moment the player equips something else on a
            // different page. The live inventory row is the only honest
            // source for whether this exact item is worn or wielded now.
            const auto resolved = ResolveInventoryRow(player, slot);
            if (!resolved.row || !resolved.row->IsEquipped()) {
                return false;
            }

            auto* equipManager = RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                return false;
            }

            const RE::BGSObjectInstance instance(
                resolved.object, resolved.keepAlive.get());
            equipManager->UnequipObject(
                player, instance, nullptr, false, false, true, true,
                nullptr);
            REX::INFO(
                "Favorites Menu Grid unequipped {} (page {} slot {}) because it "
                "was already equipped; the vanilla use event was suppressed "
                "for this click",
                slot.visual.name,
                bank + 1,
                index + 1);
            return true;
        }

        [[nodiscard]] std::size_t MigrateLegacyVirtualFavorites(
            RE::PlayerCharacter* player,
            std::array<bool, kMaxBanks>& legacyBankUsed)
        {
            if (!player) {
                return 0;
            }
            std::size_t migrated = 0;
            const auto inventoryGuard = player->inventoryList.LockWrite();
            auto* inventory = *inventoryGuard;
            if (!inventory) {
                return 0;
            }

            std::unordered_map<RE::TESFormID, std::uint32_t> ordinals;
            for (auto& item : inventory->data) {
                if (!item.object) {
                    continue;
                }
                const auto ordinal = ordinals[item.object->GetFormID()]++;
                std::optional<FavoriteSlot> rowDescriptor;
                for (auto& stack : item.stacks) {
                    if (!stack.extra) {
                        continue;
                    }
                    const auto value = ReadExtraFavoriteSlot(stack.extra.get());
                    if (value < kLegacyFirstVirtualSlot) {
                        continue;
                    }
                    const auto offset = static_cast<std::size_t>(
                        value - kLegacyFirstVirtualSlot);
                    const auto bank = offset / kSlotsPerBank;
                    const auto slot = offset % kSlotsPerBank;
                    if (bank >= kMaxBanks) {
                        continue;
                    }
                    if (!rowDescriptor) {
                        rowDescriptor = MakeInventorySlot(item, ordinal);
                        rowDescriptor->unresolved = true;
                    }
                    legacyBankUsed[bank] = true;
                    if (g_banks[bank][slot].Empty()) {
                        g_banks[bank][slot] = *rowDescriptor;
                    } else if (!SameFormIdentity(
                                   g_banks[bank][slot].form,
                                   rowDescriptor->form)) {
                        REX::WARN(
                            "Legacy favorite collision in bank {} slot {}; kept sidecar entry and removed the invalid marker",
                            bank + 1,
                            slot + 1);
                    }
                    RemoveExtraFavorite(stack.extra.get());
                    ++migrated;
                }
            }
            return migrated;
        }

        // One snapshot per save file means the directory grows for as long as
        // the player keeps saving, and Starfield autosaves often. Keep the
        // newest ones and drop the rest; current.fbs is never a candidate.
        void PruneSnapshotsLocked(std::uint64_t characterID)
        {
            constexpr std::size_t kKeptSnapshots = 96;
            std::error_code error;
            const auto directory = GetCharacterStateDirectory(characterID);
            std::vector<std::pair<std::filesystem::file_time_type,
                                  std::filesystem::path>> snapshots;
            for (const auto& entry :
                 std::filesystem::directory_iterator(directory, error)) {
                if (error || !entry.is_regular_file()) {
                    continue;
                }
                const auto& path = entry.path();
                if (path.extension() != L".fbs" ||
                    path.filename() == L"current.fbs") {
                    continue;
                }
                std::error_code timeError;
                const auto written =
                    std::filesystem::last_write_time(path, timeError);
                if (!timeError) {
                    snapshots.emplace_back(written, path);
                }
            }
            if (snapshots.size() <= kKeptSnapshots) {
                return;
            }
            std::ranges::sort(snapshots, [](const auto& a, const auto& b) {
                return a.first > b.first;
            });
            for (std::size_t index = kKeptSnapshots; index < snapshots.size();
                 ++index) {
                std::error_code removeError;
                std::filesystem::remove(snapshots[index].second, removeError);
            }
            REX::INFO(
                "Favorites Menu Grid pruned {} old per-save state file(s), keeping the newest {}",
                snapshots.size() - kKeptSnapshots,
                kKeptSnapshots);
        }

        [[nodiscard]] bool SaveCurrentStateLocked(bool saveSnapshot)
        {
            // Deliberately not re-reading the character here. Adopting a
            // character that became current after the session started is what
            // let main-menu state be written into a real playthrough's file.
            if (g_characterID == 0) {
                REX::DEBUG(
                    "Favorites Menu Grid state save skipped: this session has no character");
                return false;
            }
            // The newest save on disk identifies this session only in the
            // moment the game has just written one. At any other time it
            // names some other save -- an older one deliberately loaded,
            // above all -- and stamping current.fbs with it would hand this
            // session's pages to a save they do not belong to, which is
            // precisely what the load side now trusts that stamp not to do.
            // mostRecentSaveGame changes only when the game writes a save.
            // Comparing it against what it was when this session began is
            // therefore a reliable "the player has saved since" test, and it
            // needs no event: the previous design hung the whole per-save
            // file on kSaveCompleted, which never arrived, so not one
            // snapshot was ever written and every save fell back to the
            // shared file -- the file that by then named a different save.
            const auto recent = ReadMostRecentSaveName();
            if (!recent.empty()) {
                if (g_recentSaveAtSessionStart.empty()) {
                    // Armed here rather than while loading: mostRecentSaveGame
                    // also moves when a save is *loaded*, and it has not
                    // settled on the incoming one yet at load time. Reading it
                    // there made the first capture of every session report a
                    // new save that had never been written.
                    g_recentSaveAtSessionStart = recent;
                } else if (recent != g_recentSaveAtSessionStart &&
                           recent != g_loadedSaveName) {
                    // Both conditions earn their place. mostRecentSaveGame
                    // settles onto the save that was just *loaded* a few
                    // seconds after the load, which is not a new save and
                    // must not be announced as one -- that is the second
                    // test. But dropping the baseline and comparing only
                    // against g_loadedSaveName would adopt the newest save
                    // on disk in the window before it settles, which is
                    // exactly wrong when an older save was loaded on
                    // purpose.
                    REX::INFO(
                        "Favorites Menu Grid noticed a new save '{}'; this session's pages now belong to it",
                        recent);
                    g_loadedSaveName = recent;
                    g_recentSaveAtSessionStart = recent;
                }
            }
            const auto saveName = g_loadedSaveName.empty() ?
                recent : g_loadedSaveName;

            const auto currentPath = GetCurrentStatePath(g_characterID);
            const auto currentSaved = WriteStateFile(
                currentPath, g_characterID, saveName);

            // The per-save file is written at every capture point, not only
            // after a save. It is the primary store; the shared file is only
            // the fallback for installs that predate it. Writing it here is
            // what makes a snapshot exist at all.
            bool snapshotSaved = true;
            if (saveName.empty()) {
                REX::WARN(
                    "Favorites Menu Grid cannot name the save being played, so only the character's shared state was written");
            } else {
                const auto snapshotPath =
                    GetSnapshotStatePath(g_characterID, saveName);
                snapshotSaved = WriteStateFile(
                    snapshotPath, g_characterID, saveName);
                static std::string lastSnapshotName;
                if (snapshotSaved && lastSnapshotName != saveName) {
                    REX::INFO(
                        "Favorites Menu Grid wrote per-save state for '{}'",
                        saveName);
                    lastSnapshotName = saveName;
                    PruneSnapshotsLocked(g_characterID);
                }
            }
            if (currentSaved) {
                g_loadedSaveName = saveName;
            }
            static_cast<void>(saveSnapshot);
            return currentSaved && snapshotSaved;
        }

        [[nodiscard]] bool LoadBestStateLocked()
        {
            g_banks = FavoriteBanks{};
            g_stateLoaded = false;
            g_stateSource.clear();
            g_characterID = ReadCharacterID();

            // Whether the save being loaded is positively identified.
            // g_incomingSaveName is the file name the player actually chose;
            // ReadMostRecentSaveName is a guess that names the newest save on
            // disk, which is the wrong answer whenever an older one is loaded.
            bool identified = false;
            {
                std::scoped_lock incoming(g_incomingSaveMutex);
                identified = !g_incomingSaveName.empty();
                g_loadedSaveName = identified ?
                    g_incomingSaveName : ReadMostRecentSaveName();
            }
            if (g_characterID == 0) {
                return false;
            }

            std::string storedSaveName;
            if (!g_loadedSaveName.empty()) {
                const auto snapshot = GetSnapshotStatePath(
                    g_characterID, g_loadedSaveName);
                if (std::filesystem::exists(snapshot) &&
                    ReadStateFile(snapshot, g_characterID, storedSaveName)) {
                    g_stateLoaded = true;
                    g_stateSource = snapshot.string();
                    return true;
                }
            }
            // No snapshot for a save we can name means the plugin has never
            // written state for it: it was made before the mod was installed,
            // or belongs to a run the mod never touched. Falling through to
            // current.fbs is what used to carry another save's pages into it.
            // That state lives outside the save file, so it does not rewind
            // when the save does, and the player sees favorites from a run
            // they just left. Start this save clean instead -- every save the
            // mod has seen keeps its own snapshot, so nothing is lost by
            // refusing to guess here.
            // current.fbs records the save it was last written for. When
            // that is the save now being loaded, it holds that save's own
            // pages -- the snapshot is merely missing, because the state was
            // last written outside a save (closing the wheel does it), or
            // because the save predates snapshots altogether. Refusing it
            // here is what made every save that existed before this
            // mechanism start empty, which is every save every player has.
            const auto current = GetCurrentStatePath(g_characterID);
            if (std::filesystem::exists(current) &&
                ReadStateFile(current, g_characterID, storedSaveName)) {
                if (!identified || storedSaveName == g_loadedSaveName) {
                    g_stateLoaded = true;
                    g_stateSource = current.string();
                    return true;
                }
                // Written for a different save of this character. Reading it
                // has already filled g_banks, so undo that before falling
                // through: carrying those pages in is the very bug this
                // guard exists for.
                g_banks = FavoriteBanks{};
                g_activeBank = 0;
                g_nativeBank = 0;
            }

            if (identified) {
                REX::INFO(
                    "No stored pages for save '{}' (the shared state belongs to '{}'); treating it as a save this plugin has not seen and starting from its own favorites",
                    g_loadedSaveName,
                    storedSaveName);
                return false;
            }

            // Pre-0.4 development builds wrote beside the DLL. Read that
            // location once as a migration fallback; all later writes go to
            // Documents so Vortex/MO2 cannot purge character state.
            const auto legacyCharacter = GetCharacterStateDirectoryAt(
                GetLegacyStateRoot(), g_characterID);
            if (!g_loadedSaveName.empty()) {
                const auto legacySnapshot = legacyCharacter /
                    (std::filesystem::path(
                         SanitizeFileComponent(g_loadedSaveName)).wstring() +
                     L".fbs");
                if (std::filesystem::exists(legacySnapshot) &&
                    ReadStateFile(
                        legacySnapshot, g_characterID, storedSaveName)) {
                    g_stateLoaded = true;
                    g_stateSource = legacySnapshot.string();
                    return true;
                }
            }
            const auto legacyCurrent = legacyCharacter / L"current.fbs";
            if (std::filesystem::exists(legacyCurrent) &&
                ReadStateFile(
                    legacyCurrent, g_characterID, storedSaveName)) {
                g_stateLoaded = true;
                g_stateSource = legacyCurrent.string();
                return true;
            }
            return false;
        }

        [[nodiscard]] std::size_t CountNativeForms(void* manager)
        {
            const auto* forms = GetManagerFavoriteForms(manager);
            if (!forms) {
                return 0;
            }
            return static_cast<std::size_t>(std::count_if(
                forms,
                forms + kSlotsPerBank,
                [](const RE::TESForm* form) { return form != nullptr; }));
        }

        [[nodiscard]] std::size_t CountNativeHandles(void* manager)
        {
            const auto* handles = GetManagerFavoriteHandles(manager);
            if (!handles) {
                return 0;
            }
            return static_cast<std::size_t>(std::count_if(
                handles,
                handles + kSlotsPerBank,
                [](std::uint32_t handle) {
                    return handle != kInvalidFavoriteHandle;
                }));
        }

        [[nodiscard]] std::size_t RecoverNativeBankZero(
            void* manager,
            RE::PlayerCharacter* player)
        {
            if (!manager || !player ||
                CountNativeForms(manager) != 0 ||
                CountNativeHandles(manager) != 0) {
                return 0;
            }

            std::size_t restored = 0;
            for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                auto& descriptor = g_banks[0][index];
                if (descriptor.Empty()) {
                    continue;
                }
                auto* form = ResolveForm(descriptor.form);
                if (!form) {
                    descriptor.unresolved = true;
                    continue;
                }

                bool success = false;
                if (descriptor.kind == FavoriteKind::kInventory) {
                    auto resolved = ResolveInventoryRow(player, descriptor);
                    if (resolved.row && resolved.object) {
                        success = NativeAssignInventory(
                            manager,
                            resolved.object,
                            resolved.row,
                            resolved.ownerHandle,
                            static_cast<std::uint8_t>(index));
                        if (success) {
                            descriptor.sessionInstanceData = resolved.keepAlive;
                            descriptor.form = MakeFormIdentity(resolved.object);
                        }
                    }
                } else {
                    success = NativeAssignForm(
                        manager,
                        form,
                        static_cast<std::uint8_t>(index));
                }
                descriptor.unresolved = !success;
                restored += static_cast<std::size_t>(success);
                if (!success) {
                    REX::WARN(
                        "Migration could not restore native page-1 slot {}; its virtual fallback was preserved",
                        index + 1);
                }
            }
            if (restored != 0) {
                RebuildFavoritesData(manager);
            }
            return restored;
        }

        void ApplyPendingVisualLocked()
        {
            if (!g_pendingVisual.valid) {
                return;
            }
            auto& slot = g_banks[g_pendingVisual.bank][g_pendingVisual.slot];
            if (slot.Empty() ||
                !SameFormIdentity(slot.form, g_pendingVisual.form)) {
                return;
            }
            slot.visual = g_pendingVisual.visual;
            g_pendingVisual = PendingVisual{};
        }

        // Writes one stored descriptor into a real favorite slot. Resolution
        // happens here, after the clear phase, because the native clear
        // callbacks mutate inventory bookkeeping and can invalidate a row
        // pointer taken before them.
        [[nodiscard]] bool AssignDescriptorToNativeSlot(
            void* manager,
            RE::PlayerCharacter* player,
            FavoriteSlot& descriptor,
            std::size_t index)
        {
            auto* form = ResolveForm(descriptor.form);
            if (!form) {
                return false;
            }
            if (descriptor.kind != FavoriteKind::kInventory) {
                return NativeAssignForm(
                    manager, form, static_cast<std::uint8_t>(index));
            }

            auto resolved = ResolveInventoryRow(player, descriptor);
            if (!resolved.row || !resolved.object) {
                return false;
            }
            if (!NativeAssignInventory(
                    manager,
                    resolved.object,
                    resolved.row,
                    resolved.ownerHandle,
                    static_cast<std::uint8_t>(index))) {
                return false;
            }
            descriptor.sessionInstanceData = resolved.keepAlive;
            auto visual = std::move(descriptor.visual);
            descriptor.form = MakeFormIdentity(resolved.object);
            descriptor.visual = std::move(visual);
            return true;
        }

        // Why one stored favorite cannot be written right now.
        enum class SlotReadiness
        {
            // It can be written.
            kWritable,
            // The item is simply not in the player's inventory any more: it
            // was stored, sold, consumed, or its plugin is gone. This is
            // ordinary attrition and exactly what the vanilla wheel shows as
            // an empty slot. It must never block the rest of the page.
            kItemAbsent,
            // The item is right there and the plugin still could not write it.
            // That means the mechanism is broken, which is the situation that
            // emptied the player's favorites in 0.6.0, so nothing may be
            // cleared while this is true.
            kBlocked
        };

        // Checks whether one descriptor could be written, without touching any
        // favorite state. For an inventory row this deliberately performs the
        // real handle creation and immediately releases it, because that is
        // the step that fails, and a check that does not exercise it would be
        // worthless.
        [[nodiscard]] SlotReadiness CanAssignDescriptor(
            RE::PlayerCharacter* player,
            const FavoriteSlot& descriptor,
            std::size_t bank,
            std::size_t index)
        {
            if (!ResolveForm(descriptor.form)) {
                REX::WARN(
                    "Page {} slot {} cannot be committed: form {:08X} ('{}' from '{}') is not present in the current load order",
                    bank + 1,
                    index + 1,
                    descriptor.form.rawFormID,
                    descriptor.form.editorID,
                    descriptor.form.sourceFile);
                // A form that no longer exists is gone for good, the same as
                // an item the player sold. It is not a broken mechanism.
                return SlotReadiness::kItemAbsent;
            }
            if (descriptor.kind != FavoriteKind::kInventory) {
                return SlotReadiness::kWritable;
            }

            auto resolved = ResolveInventoryRow(player, descriptor);
            if (!resolved.row || !resolved.object) {
                // "No row" has several very different causes and they need
                // different fixes, so report which one this is instead of one
                // message that covers all of them.
                auto* form = ResolveForm(descriptor.form);
                auto* object = AsBoundObject(form);
                std::size_t rows = 0;
                std::size_t matching = 0;
                std::uint32_t stacked = 0;
                if (object) {
                    const auto guard = player->inventoryList.LockRead();
                    if (const auto* inventory = *guard) {
                        rows = inventory->data.size();
                        for (const auto& item : inventory->data) {
                            if (item.object &&
                                item.object->GetFormID() == object->GetFormID()) {
                                ++matching;
                                for (const auto& stack : item.stacks) {
                                    stacked += stack.count;
                                }
                            }
                        }
                    }
                }
                if (object && matching == 0) {
                    REX::INFO(
                        "Page {} slot {} is empty for now: '{}' ({:08X}) is not in the inventory any more. It stays stored and returns if the item does.",
                        bank + 1,
                        index + 1,
                        descriptor.visual.name,
                        descriptor.form.rawFormID);
                    return SlotReadiness::kItemAbsent;
                }
                REX::WARN(
                    "Page {} slot {} could not be resolved: form {:08X} type {} -> resolved={} boundObject={} inventoryRows={} matchingRows={} totalCount={}",
                    bank + 1,
                    index + 1,
                    descriptor.form.rawFormID,
                    descriptor.form.formType,
                    form != nullptr,
                    object != nullptr,
                    rows,
                    matching,
                    stacked);
                return SlotReadiness::kBlocked;
            }

            std::uint32_t handle = kInvalidFavoriteHandle;
            if (!CreateRowHandle(handle, resolved.ownerHandle, resolved.row)) {
                REX::WARN(
                    "Page {} slot {} is present in the inventory but the engine refused a handle for it; refusing to touch the native slots",
                    bank + 1,
                    index + 1);
                return SlotReadiness::kBlocked;
            }
            ReleaseLocalInventoryHandle(handle);
            return SlotReadiness::kWritable;
        }

        // Commits a page into the twelve real Starfield favorite slots. This
        // is the only operation that mutates native favorite state, and it is
        // deliberately not run while the player is merely scrolling the wheel.
        //
        // Nothing is cleared until every occupied position of the target page
        // has been proven writable. Version 0.6.0 cleared first and discovered
        // the failure afterwards, which emptied the player's real favorites.
        // Refusing to switch page is an annoyance; destroying favorites is not
        // recoverable from inside the game.
        // g_switchInProgress tells the rest of the plugin that the native
        // favorite slots are being rewritten by us. Committing a page runs up
        // to twelve native clears and twelve native assigns, and every one of
        // them makes the engine emit a FavoriteChangedEvent; without this
        // flag each of those events queued a capture of state the commit was
        // still in the middle of changing. The flag was declared and read in
        // three places but never set by anything, so all three guards were
        // dead and the capture storm happened on every page change.
        struct NativeMutationGuard
        {
            NativeMutationGuard()
            {
                g_switchInProgress.store(true, std::memory_order_release);
            }
            ~NativeMutationGuard()
            {
                g_switchInProgress.store(false, std::memory_order_release);
            }
            NativeMutationGuard(const NativeMutationGuard&) = delete;
            NativeMutationGuard& operator=(const NativeMutationGuard&) = delete;
        };

        // a_force rewrites the native slots even when the page is already
        // the native one. Needed after editing a page in place: the stored
        // descriptors changed, so "already committed" is no longer true and
        // the next capture would otherwise write the old arrangement back.
        [[nodiscard]] bool MaterializeBankLocked(
            std::size_t targetBank,
            bool a_force = false)
        {
            if (targetBank >= g_settings.rowCount ||
                !g_sessionInitialized) {
                return false;
            }
            if (targetBank == g_nativeBank && !a_force) {
                return true;
            }

            auto* manager = GetFavoritesManager();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!manager || !player) {
                return false;
            }

            const auto previousBank = g_nativeBank;

            // Items the player no longer carries must not stop the page from
            // being used: losing a favorite is ordinary, and the vanilla wheel
            // simply shows that slot as empty. Only a failure that happens
            // while the item is right there means the mechanism itself is
            // broken, and that is the one case where nothing may be cleared.
            std::size_t blocked = 0;
            std::size_t absent = 0;
            std::size_t occupied = 0;
            for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                const auto& descriptor = g_banks[targetBank][index];
                if (descriptor.Empty() ||
                    g_settings.externallyManagedSlots[index]) {
                    continue;
                }
                ++occupied;
                switch (CanAssignDescriptor(
                    player, descriptor, targetBank, index)) {
                case SlotReadiness::kBlocked:
                    ++blocked;
                    break;
                case SlotReadiness::kItemAbsent:
                    ++absent;
                    break;
                case SlotReadiness::kWritable:
                    break;
                }
            }
            if (blocked != 0) {
                REX::WARN(
                    "Refusing to commit page {}: {} of {} stored favorite(s) are in the inventory but cannot be written, which means something is wrong with the plugin rather than with the items. Page {} stays in the native slots and nothing was cleared.",
                    targetBank + 1,
                    blocked,
                    occupied,
                    previousBank + 1);
                return false;
            }

            // Everything below rewrites native favorite state, so the
            // engine's own change notifications from here on are ours and
            // must not queue captures of half-written slots.
            const NativeMutationGuard mutating;

            // Whatever the real slots hold right now belongs to the page that
            // put it there. Take it back before overwriting anything.
            //
            // Except when that page is the one being written. This is only
            // reached with a_force, which means an edit has just changed the
            // stored page and wants it pushed out -- and the real slots still
            // hold the arrangement from before that edit. Reading them back
            // in first overwrites the edit with what it was meant to
            // replace, then faithfully writes the old order out again. The
            // swap was applied, reverted and reported as successful, all in
            // one call: on the page that is live, moving two favorites
            // looked like nothing happening at all.
            if (targetBank != previousBank) {
                const auto outgoing = CaptureNativePage(manager, player);
                ReconcileNativePageWithBank(previousBank, outgoing, true);
            }
            ApplyPendingVisualLocked();

            for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                if (g_settings.externallyManagedSlots[index]) {
                    // Clearing it would delete another mod's item and force
                    // that mod to put it back, which is the churn that made
                    // the engine hand out new slots in the first place.
                    continue;
                }
                NativeClearSlot(manager, static_cast<std::uint8_t>(index));
            }

            std::size_t written = 0;
            std::size_t failed = 0;
            const auto committed = CaptureNativePage(manager, player);
            for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                auto& descriptor = g_banks[targetBank][index];
                if (descriptor.Empty() ||
                    g_settings.externallyManagedSlots[index]) {
                    continue;
                }
                const auto success = AssignDescriptorToNativeSlot(
                    manager, player, descriptor, index);
                // One flag, and until now two different meanings written
                // into it. The carried test asks "is this item in the
                // inventory"; the commit asks "could I write it into a real
                // slot", and those two answers disagree exactly when an item
                // has left the inventory but its favourite is still
                // assignable. The commit was clearing the flag in that case,
                // so a correctly faint cell went bright again the moment the
                // wheel closed -- which is what saving appeared to do.
                // A failed write is still worth recording, so the flag is
                // raised here and never lowered; lowering belongs to the
                // carried test alone.
                if (!success) {
                    descriptor.unresolved = true;
                }
                if (!DescriptorItemIsCarried(descriptor, committed)) {
                    descriptor.unresolved = true;
                }
                if (success) {
                    ++written;
                } else {
                    ++failed;
                }
            }

            if (failed > absent) {
                // More slots failed than the checks accounted for, so the
                // engine changed under us. Put the page that was there back
                // rather than leaving the player with empty favorites.
                REX::CRITICAL(
                    "Commit of page {} failed on {} slot(s) after passing its checks; restoring page {}",
                    targetBank + 1,
                    failed,
                    previousBank + 1);
                for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                    if (g_settings.externallyManagedSlots[index]) {
                        continue;
                    }
                    NativeClearSlot(manager, static_cast<std::uint8_t>(index));
                }
                std::size_t restored = 0;
                for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                    auto& descriptor = g_banks[previousBank][index];
                    if (descriptor.Empty() ||
                        g_settings.externallyManagedSlots[index]) {
                        continue;
                    }
                    restored += static_cast<std::size_t>(
                        AssignDescriptorToNativeSlot(
                            manager, player, descriptor, index));
                }
                RebuildFavoritesData(manager);
                static_cast<void>(SaveCurrentStateLocked(false));
                REX::CRITICAL(
                    "Restored {} favorite(s) of page {}. Every page is still stored in the sidecar; do not save over a good save until this is understood.",
                    restored,
                    previousBank + 1);
                return false;
            }

            g_nativeBank = targetBank;
            RebuildFavoritesData(manager);
            static_cast<void>(SaveCurrentStateLocked(false));
            REX::INFO(
                "Committed page {} into the native favorite slots (was page {}): {} written, {} slot(s) left empty because the item is gone",
                targetBank + 1,
                previousBank + 1,
                written,
                absent);
            return true;
        }

        // Marks every stored favorite by whether its item is carried now.
        // Split out of RefreshCarriedFlags so a capture can do it too: the
        // reconcile that runs there reads the native slots, and those keep
        // holding an item the player no longer carries, so it cleared the
        // faint flag on every capture. That is why a slot went bright again
        // merely because the game was saved.
        void RefreshCarriedFlagsLocked(const NativePage& nativePage)
        {
            for (std::size_t bank = 0; bank < g_settings.rowCount; ++bank) {
                for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
                    auto& descriptor = g_banks[bank][index];
                    if (descriptor.Empty()) {
                        continue;
                    }
                    const auto before = descriptor.unresolved;
                    descriptor.unresolved =
                        !DescriptorItemIsCarried(descriptor, nativePage);
                    // Diagnose: die Blass-Markierung kommt weiterhin abhanden,
                    // und zwei Hypothesen lagen daneben. Das hier sagt, was
                    // die Trageprüfung wirklich sieht.
                    {
                        static std::atomic_int reports{ 0 };
                        if (reports.fetch_add(1) < 80) {
                            const auto* probe = ResolveForm(descriptor.form);
                            REX::INFO(
                                "carried-probe r{}c{} '{}' kind={} resolved={} formID={:08X} inCarried={} unresolved {}->{}",
                                bank + 1,
                                index + 1,
                                descriptor.form.editorID,
                                static_cast<unsigned>(descriptor.kind),
                                probe != nullptr,
                                probe ? probe->GetFormID() : 0u,
                                probe && nativePage.carriedForms.contains(
                                    probe->GetFormID()),
                                before,
                                descriptor.unresolved);
                        }
                    }
                    if (descriptor.kind != FavoriteKind::kInventory) {
                        continue;
                    }
                    const auto* form = ResolveForm(descriptor.form);
                    if (!form) {
                        continue;
                    }
                    const auto found =
                        nativePage.carriedCounts.find(form->GetFormID());
                    if (found != nativePage.carriedCounts.end()) {
                        descriptor.visual.count = found->second;
                    }
                }
            }
        }

        void CaptureCurrentBankLocked(bool saveSnapshot)
        {
            auto* manager = GetFavoritesManager();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!g_sessionInitialized || !manager || !player) {
                return;
            }
            const auto nativePage = CaptureNativePage(manager, player);
            ReconcileNativePageWithBank(g_nativeBank, nativePage, true);
            // After the reconcile, never before: it is what clears the flag.
            RefreshCarriedFlagsLocked(nativePage);
            ApplyPendingVisualLocked();
            static_cast<void>(SaveCurrentStateLocked(saveSnapshot));
        }

        // Marks every stored favorite by whether its item is carried now.
        //
        // The commit records this only for the page it writes, so every
        // other page kept whatever was true when it was last committed --
        // possibly hours earlier, which is no basis for drawing a cell
        // faint. The set of carried forms is built by the capture anyway, so
        // bringing all the pages up to date is one lookup per slot.
        void RefreshCarriedFlags()
        {
            auto* manager = GetFavoritesManager();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!manager || !player) {
                return;
            }
            const auto nativePage = CaptureNativePage(manager, player);
            std::scoped_lock lock(g_stateMutex);
            if (!g_sessionInitialized) {
                return;
            }
            RefreshCarriedFlagsLocked(nativePage);
        }
    }

    std::filesystem::path GetPluginDirectory()
    {
        std::wstring executablePath(32768, L'\0');
        const auto length = GetModuleFileNameW(
            nullptr,
            executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
        executablePath.resize(length);
        return std::filesystem::path(executablePath).parent_path() /
            L"Data" / L"SFSE" / L"Plugins";
    }

    std::filesystem::path GetConfigPath()
    {
        return GetPluginDirectory() / kConfigName;
    }

    bool IsFavoritesMenuOpen()
    {
        const auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        static const RE::BSFixedString favoritesMenu("FavoritesMenu");
        return ui->IsMenuOpen(favoritesMenu);
    }

    bool InitializeSessionIfNeeded()
    {
        if (g_disabledForConflict.load(std::memory_order_acquire)) {
            return false;
        }
        std::scoped_lock lock(g_stateMutex);
        const auto currentCharacterID = ReadCharacterID();
        if (g_sessionInitialized && currentCharacterID != 0 &&
            currentCharacterID == g_characterID) {
            return true;
        }

        // No character means the main menu, or a save that has not finished
        // loading. Initialising anyway used to adopt whatever the twelve
        // native slots happened to hold as page 1 and mark the session ready
        // under character 0. The next write then picked up whichever
        // character had meanwhile become current and saved those adopted
        // pages over that character's real file, which is how one playthrough
        // could overwrite another's favorites. Wait instead.
        if (currentCharacterID == 0) {
            return false;
        }
        if (!NativeFavoritesReady()) {
            return false;
        }
        if (g_sessionInitialized && g_characterID != 0 &&
            g_characterID != currentCharacterID) {
            REX::INFO(
                "Favorites Menu Grid switching from character {:016X} to {:016X}; each playthrough keeps its own pages",
                g_characterID,
                currentCharacterID);
        }

        auto* manager = GetFavoritesManager();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!manager || !player) {
            return false;
        }

        static_cast<void>(LoadBestStateLocked());
        const auto preferredBank = std::min(
            g_activeBank,
            g_settings.rowCount - 1);
        auto nativePage = CaptureNativePage(manager, player);
        std::array<bool, kMaxBanks> legacyBankUsed{};
        const auto migrated = MigrateLegacyVirtualFavorites(
            player, legacyBankUsed);

        if (g_stateLoaded) {
            const auto nativeItems = CountNativeForms(manager) +
                CountNativeHandles(manager);
            if (nativeItems == 0) {
                const auto recovered = RecoverNativeBankZero(manager, player);
                if (recovered != 0) {
                    nativePage = CaptureNativePage(manager, player);
                    REX::INFO(
                        "Recovered {} page-1 favorite(s) left virtual by version 0.4",
                        recovered);
                }
            }

            // The sidecar records which page was committed to the real slots,
            // but the loaded save is the authority on what those slots hold.
            // An older save, or a save made before this plugin was installed,
            // can disagree; score the loaded page against every stored page
            // rather than trusting the recorded value blindly.
            const auto identified = ChooseBankForNativePage(
                nativePage, g_nativeBank);
            if (identified != g_nativeBank) {
                REX::WARN(
                    "The loaded save's native favorites match page {}, not the recorded page {}; trusting the save",
                    identified + 1,
                    g_nativeBank + 1);
                g_nativeBank = identified;
            }
            ReconcileNativePageWithBank(g_nativeBank, nativePage, true);
        } else {
            const auto hasLegacy = std::ranges::any_of(
                legacyBankUsed,
                [](bool used) { return used; });
            // The default page is the one restored into the real slots on
            // every close, so it is where the gameplay quickkeys point. A
            // save whose favorites the mod is meeting for the first time
            // belongs there and nowhere else.
            g_nativeBank = std::min(
                g_settings.defaultRow - 1, g_settings.rowCount - 1);
            if (!hasLegacy || std::ranges::all_of(
                    g_banks[g_nativeBank],
                    [](const FavoriteSlot& slot) { return slot.Empty(); })) {
                g_banks[g_nativeBank] = nativePage.slots;
            }
        }

        g_activeBank = preferredBank;
        g_sessionInitialized = true;
        [[maybe_unused]] const auto initialStateSaved =
            SaveCurrentStateLocked(false);
        REX::INFO(
            "Favorites Menu Grid session initialized for character {:016X}: page {} is in the native slots, page {} is shown; {} legacy marker(s) migrated; source='{}'",
            g_characterID,
            g_nativeBank + 1,
            g_activeBank + 1,
            migrated,
            g_stateSource.empty() ? "new state" : g_stateSource);
        return true;
    }

    bool SwitchBank(std::size_t targetBank)
    {
        if (targetBank >= g_settings.rowCount ||
            g_disabledForConflict.load(std::memory_order_acquire)) {
            return false;
        }
        if (!InitializeSessionIfNeeded()) {
            REX::WARN(
                "Favorites Menu Grid switch ignored: native favorites are not ready");
            return false;
        }

        auto* manager = GetFavoritesManager();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!manager || !player) {
            return false;
        }

        std::size_t previousBank = 0;
        {
            std::scoped_lock lock(g_stateMutex);
            previousBank = g_activeBank;
            if (targetBank != g_activeBank) {
                // Browsing must stay free of side effects: capture whatever
                // the real slots hold back into the page that owns them, then
                // only change which page is drawn. The commit into native
                // slots happens when the player settles on a page, in
                // ProcessMenuSelection or QueueCommitActiveBank.
                const auto nativePage = CaptureNativePage(manager, player);
                ReconcileNativePageWithBank(g_nativeBank, nativePage, true);
                ApplyPendingVisualLocked();
                g_activeBank = targetBank;
                [[maybe_unused]] const auto switchedStateSaved =
                    SaveCurrentStateLocked(false);
            }
        }

        RenderActiveBank();
        UpdateGridOverlay();
        REX::INFO(
            "Favorites page shown {} -> {}; native slots still hold page {} ({} form(s), {} handle(s))",
            previousBank + 1,
            targetBank + 1,
            g_nativeBank + 1,
            CountNativeForms(manager),
            CountNativeHandles(manager));
        return true;
    }

    void QueueBankSwitch(std::size_t targetBank)
    {
        if (targetBank >= g_settings.rowCount ||
            g_switchQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto generation = g_sessionGeneration.load(
            std::memory_order_acquire);
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) {
            g_switchQueued.store(false, std::memory_order_release);
            return;
        }
        tasks->AddTask([targetBank, generation]() {
            g_switchQueued.store(false, std::memory_order_release);
            if (generation != g_sessionGeneration.load(
                    std::memory_order_acquire) ||
                !IsFavoritesMenuOpen()) {
                return;
            }
            SwitchBank(targetBank);
        });
    }

    void QueueCaptureCurrentState(bool saveSnapshot)
    {
        if (g_disabledForConflict.load(std::memory_order_acquire)) {
            return;
        }
        if (saveSnapshot) {
            g_snapshotRequested.store(true, std::memory_order_release);
        }
        if (g_captureQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto generation = g_sessionGeneration.load(
            std::memory_order_acquire);
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) {
            g_captureQueued.store(false, std::memory_order_release);
            return;
        }
        tasks->AddTask([generation]() {
            g_captureQueued.store(false, std::memory_order_release);
            if (generation != g_sessionGeneration.load(
                    std::memory_order_acquire)) {
                g_snapshotRequested.store(false, std::memory_order_release);
                return;
            }
            if (g_switchInProgress.load(std::memory_order_acquire)) {
                QueueCaptureCurrentState(
                    g_snapshotRequested.load(std::memory_order_acquire));
                return;
            }
            if (!InitializeSessionIfNeeded()) {
                return;
            }
            const auto snapshot = g_snapshotRequested.exchange(
                false, std::memory_order_acq_rel);
            std::scoped_lock lock(g_stateMutex);
            CaptureCurrentBankLocked(snapshot);
        });
    }

    bool ProcessMenuSelection(
        std::size_t globalIndex,
        bool assigning,
        const RE::Scaleform::GFx::Value* assignedItem)
    {
        const auto bank = globalIndex / kSlotsPerBank;
        const auto index = globalIndex % kSlotsPerBank;
        if (bank >= g_settings.rowCount || index >= kSlotsPerBank ||
            !InitializeSessionIfNeeded()) {
            return false;
        }

        std::scoped_lock lock(g_stateMutex);

        // The item being assigned is read before anything else touches the
        // manager: committing a page calls the native clear and assign
        // helpers, and those own the same pending-assignment fields.
        FormIdentity pendingForm;
        FavoriteVisual pendingVisual;
        bool hasPending = false;
        if (assigning) {
            if (auto* form = GetPendingAssignedForm(GetFavoritesManager())) {
                pendingForm = MakeFormIdentity(form);
                pendingVisual = CaptureVisual(assignedItem);
                if (pendingVisual.name.empty()) {
                    pendingVisual.name = !pendingForm.editorID.empty() ?
                        pendingForm.editorID :
                        std::format("{:08X}", pendingForm.rawFormID);
                }
                hasPending = true;
            } else {
                REX::WARN(
                    "Wheel assignment on page {} slot {} had no pending native form; its icon will be recovered on the next capture",
                    bank + 1,
                    index + 1);
            }
        }

        // This runs synchronously from the wheel's own selection handler, and
        // the vanilla FavoritesMenu_AssignQuickkey / FavoritesMenu_UseQuickkey
        // event is dispatched the instant it returns. Committing the shown
        // page here is what makes the engine act on the slot the player is
        // actually looking at, without the plugin having to reimplement
        // assignment or use.
        const auto committed = MaterializeBankLocked(bank);
        if (!committed) {
            // Letting the vanilla event run now would use or overwrite the
            // slot of whatever page is really loaded, which is not the page
            // the player is looking at. Refusing is the only honest option.
            REX::WARN(
                "Ignoring the selection of page {} slot {}: the real favorite slots still hold page {}, so the game would act on that page instead",
                bank + 1,
                index + 1,
                g_nativeBank + 1);
            return false;
        }

        if (hasPending) {
            // The engine performs the assignment after this returns, so the
            // descriptor it produces only appears on the following capture.
            g_pendingVisual = PendingVisual{
                true, bank, index, pendingForm, std::move(pendingVisual)
            };
        }

        if (!assigning && g_settings.toggleEquipOnSelect) {
            if (TryToggleUnequipLocked(g_banks[bank][index], bank, index)) {
                // The vanilla FavoritesMenu_UseQuickkey handler has no
                // "already equipped" case of its own; it would just replay
                // the same equip. Unequipping here and refusing the vanilla
                // dispatch is the only way to make a second selection act as
                // a toggle, the way holstering already works everywhere else
                // in the game.
                return false;
            }
        }
        return true;
    }

    void CaptureNativePageVisuals(const RE::Scaleform::GFx::Value* items)
    {
        if (!items || !items->IsArray() ||
            g_disabledForConflict.load(std::memory_order_acquire)) {
            return;
        }

        std::array<FavoriteVisual, kSlotsPerBank> harvested;
        class CardVisitor final :
            public RE::Scaleform::GFx::Value::ArrayVisitor
        {
        public:
            explicit CardVisitor(
                std::array<FavoriteVisual, kSlotsPerBank>& output) :
                output_(output)
            {}

            void Visit(
                std::uint32_t index,
                const RE::Scaleform::GFx::Value& entry) override
            {
                if (index >= kSlotsPerBank || !entry.IsObject()) {
                    return;
                }
                output_[index] = CaptureVisual(std::addressof(entry));
            }

        private:
            std::array<FavoriteVisual, kSlotsPerBank>& output_;
        } visitor(harvested);
        RE::Scaleform::GFx::Value cards = *items;
        cards.VisitElements(&visitor);

        std::scoped_lock lock(g_stateMutex);
        if (!g_sessionInitialized) {
            return;
        }
        bool changed = false;
        for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
            // A pinned slot belongs to no page, so its card is harvested
            // into the shared record instead, and never marks a page dirty.
            const auto isPinned = g_settings.externallyManagedSlots[index];
            auto& slot = isPinned ?
                g_pinnedSlots[index] : g_banks[g_nativeBank][index];
            // A pinned slot's icon has now failed to appear twice on
            // reasoning alone, so this reports what actually arrives: the
            // card the wheel handed over, and the record it is meant to
            // land in. Capped, and only for the handful of pinned slots.
            if (isPinned) {
                static std::atomic_int reports{ 0 };
                if (reports.fetch_add(1) < 8) {
                    REX::INFO(
                        "Pinned slot {}: card hasData={} name='{}' image='{}' fixture={}; record empty={} hasVisual={}",
                        index + 1,
                        harvested[index].HasData(),
                        harvested[index].name,
                        harvested[index].imageName,
                        harvested[index].fixtureType,
                        slot.Empty(),
                        slot.visual.HasData());
                }
            }
            if (!harvested[index].HasData()) {
                continue;
            }
            // A page slot with no descriptor has nothing to describe. A
            // pinned one is different: its descriptor arrives separately,
            // from the native read during the next reconcile, and that read
            // carries no card. If the harvest is skipped here because the
            // record is still empty, the visual is simply lost -- which is
            // why the pinned cell showed an editor ID and no icon at all
            // while being perfectly selectable.
            if (slot.Empty() && !isPinned) {
                continue;
            }
            // The card for an item the player is not carrying right now
            // arrives named but without an iconImage. Assigning it wholesale
            // erased the icon captured while the item was still carried, and
            // the result was written to disk -- so one look at the wheel
            // without the item left the cell iconless for good. Keep what
            // the card does not bring.
            auto merged = harvested[index];
            if (merged.fixtureType == kInvalidFixtureType &&
                merged.imageName.empty() &&
                (slot.visual.fixtureType != kInvalidFixtureType ||
                 !slot.visual.imageName.empty())) {
                merged.fixtureType = slot.visual.fixtureType;
                merged.imageDirectory = slot.visual.imageDirectory;
                merged.imageName = slot.visual.imageName;
            }
            if (slot.visual.name == merged.name &&
                slot.visual.imageName == merged.imageName &&
                slot.visual.fixtureType == merged.fixtureType &&
                slot.visual.count == merged.count) {
                continue;
            }
            slot.visual = std::move(merged);
            if (isPinned && !slot.Empty()) {
                g_pinnedVisuals[slot.form.rawFormID] = slot.visual;
            }
            changed = !isPinned || changed;
        }
        if (changed) {
            static_cast<void>(SaveCurrentStateLocked(false));
        }
    }

    FavoriteBank BuildRenderablePage(
        std::size_t& activeBank,
        bool& showingNativePage)
    {
        FavoriteBank page;
        {
            std::scoped_lock lock(g_stateMutex);
            activeBank = g_activeBank;
            showingNativePage = g_activeBank == g_nativeBank;
            page = g_banks[activeBank];
            // A pinned slot holds the same item no matter which page is
            // drawn, so every page shows it. Otherwise it would blink in
            // and out as the player scrolls past the page that is native.
            for (std::size_t slot = 0; slot < kSlotsPerBank; ++slot) {
                if (g_settings.externallyManagedSlots[slot]) {
                    page[slot] = g_pinnedSlots[slot];
                }
            }
        }
        if (showingNativePage) {
            // The engine publishes this page itself and only ever shows what
            // the player really has, so there is nothing to filter.
            return page;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return page;
        }

        std::unordered_set<RE::TESFormID> carried;
        {
            const auto guard = player->inventoryList.LockRead();
            const auto* inventory = *guard;
            if (!inventory) {
                return page;
            }
            carried.reserve(inventory->data.size());
            for (const auto& item : inventory->data) {
                if (item.object) {
                    carried.insert(item.object->GetFormID());
                }
            }
        }
        if (carried.empty()) {
            return page;
        }

        for (std::size_t index = 0; index < kSlotsPerBank; ++index) {
            auto& slot = page[index];
            if (slot.Empty() || slot.kind != FavoriteKind::kInventory ||
                g_settings.externallyManagedSlots[index]) {
                continue;
            }
            const auto* form = ResolveForm(slot.form);
            if (!form || !carried.contains(form->GetFormID())) {
                // Blank the copy only. g_banks keeps the descriptor.
                slot = FavoriteSlot{};
            }
        }
        return page;
    }

    // Empties one stored slot.
    //
    // onlyActiveBank is what the wheel needs and what the grid must not
    // have: the wheel can only ever ask about the page it is drawing, so
    // anything else is a bug worth refusing. The grid draws all of them at
    // once, and its delete corner sat there doing nothing on every row but
    // the active one -- silently, because refusing was the correct answer
    // to the wrong question.
    void ClearSlotAt(std::size_t globalIndex, bool onlyActiveBank)
    {
        const auto bank = globalIndex / kSlotsPerBank;
        const auto index = globalIndex % kSlotsPerBank;
        if (bank >= g_settings.rowCount || index >= kSlotsPerBank ||
            g_disabledForConflict.load(std::memory_order_acquire) ||
            !InitializeSessionIfNeeded()) {
            return;
        }
        if (g_settings.externallyManagedSlots[index]) {
            // Emptying it would only make the owning mod write its item
            // straight back, and that churn is what moves other favorites.
            REX::INFO(
                "Slot {} is left to another mod (ExternallyManagedSlots); not clearing it",
                index + 1);
            return;
        }

        bool clearedNative = false;
        {
            std::scoped_lock lock(g_stateMutex);
            if (onlyActiveBank && bank != g_activeBank) {
                // The wheel only ever asks for the page it is drawing.
                return;
            }
            if (g_banks[bank][index].Empty()) {
                return;
            }
            const auto name = g_banks[bank][index].visual.name;
            g_banks[bank][index] = FavoriteSlot{};

            if (bank == g_nativeBank) {
                // This page is the one the engine really holds, so the native
                // slot has to go too or the next capture would put it back.
                if (auto* manager = GetFavoritesManager()) {
                    const NativeMutationGuard mutating;
                    NativeClearSlot(
                        manager, static_cast<std::uint8_t>(index));
                    RebuildFavoritesData(manager);
                    clearedNative = true;
                }
            }
            static_cast<void>(SaveCurrentStateLocked(false));
            REX::INFO(
                "Cleared page {} slot {} ('{}'){}",
                bank + 1,
                index + 1,
                name,
                clearedNative ? " and its native favorite slot" : "");
        }

        if (!clearedNative) {
            // A page that is not in the real slots is drawn from the stored
            // descriptors, so it needs an explicit redraw. The native case
            // already republished the shuttle, which redraws by itself.
            QueueLoadedGameInitialization();
        }
    }

    void ClearSlotFromMenu(std::size_t globalIndex)
    {
        ClearSlotAt(globalIndex, true);
    }

    void ClearGridSlot(std::size_t globalIndex)
    {
        ClearSlotAt(globalIndex, false);
    }

    void LogFromMenu(std::string_view text)
    {
        REX::INFO("[wheel] {}", text);
    }

    bool SwapStoredSlots(
        std::size_t bankA,
        std::size_t slotA,
        std::size_t bankB,
        std::size_t slotB)
    {
        if (bankA >= g_settings.rowCount ||
            bankB >= g_settings.rowCount ||
            slotA >= kSlotsPerBank || slotB >= kSlotsPerBank ||
            g_disabledForConflict.load(std::memory_order_acquire) ||
            !InitializeSessionIfNeeded()) {
            return false;
        }
        if (bankA == bankB && slotA == slotB) {
            return false;
        }
        // A slot another mod owns is not ours to move: it would be written
        // straight back, and the page would end up holding a copy of it.
        if (g_settings.externallyManagedSlots[slotA] ||
            g_settings.externallyManagedSlots[slotB]) {
            REX::INFO(
                "Refusing to move slot {} or {}: left to the mod that owns it",
                slotA + 1,
                slotB + 1);
            return false;
        }

        std::scoped_lock lock(g_stateMutex);
        const auto nameA = g_banks[bankA][slotA].visual.name;
        const auto nameB = g_banks[bankB][slotB].visual.name;
        std::swap(g_banks[bankA][slotA], g_banks[bankB][slotB]);

        // Editing the page that currently occupies the real slots leaves
        // the engine holding the old arrangement, and the next capture
        // would faithfully write it back over the edit. Rewriting the
        // native slots now is what makes the change stick.
        if (bankA == g_nativeBank || bankB == g_nativeBank) {
            if (!MaterializeBankLocked(g_nativeBank, true)) {
                std::swap(g_banks[bankA][slotA], g_banks[bankB][slotB]);
                REX::WARN(
                    "Could not commit the moved favorites; the move was undone");
                return false;
            }
        }
        static_cast<void>(SaveCurrentStateLocked(false));
        REX::INFO(
            "Moved '{}' (page {} slot {}) and '{}' (page {} slot {})",
            !nameA.empty() ? nameA : "(empty)",
            bankA + 1,
            slotA + 1,
            !nameB.empty() ? nameB : "(empty)",
            bankB + 1,
            slotB + 1);
        return true;
    }

    void QueueCommitActiveBank()
    {
        if (g_disabledForConflict.load(std::memory_order_acquire) ||
            g_commitQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto generation = g_sessionGeneration.load(
            std::memory_order_acquire);
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) {
            g_commitQueued.store(false, std::memory_order_release);
            return;
        }
        tasks->AddTask([generation]() {
            g_commitQueued.store(false, std::memory_order_release);
            if (generation != g_sessionGeneration.load(
                    std::memory_order_acquire) ||
                !InitializeSessionIfNeeded()) {
                return;
            }
            std::scoped_lock lock(g_stateMutex);
            // Always back to the chosen page. The grid draws every page at
            // once, so which one is "live" is invisible - and invisible
            // state that decides what the gameplay quickkeys do is a trap.
            // One page, always the same one, whatever was clicked.
            g_activeBank = std::min(
                g_settings.defaultRow - 1, g_settings.rowCount - 1);
            static_cast<void>(MaterializeBankLocked(g_activeBank));
            // Commit is a no-op when the shown page already occupies the real
            // slots, so the capture that closing the wheel used to perform
            // still has to run on that path.
            CaptureCurrentBankLocked(false);
        });
    }

    void NoteIncomingSave(RE::SaveLoadEvent::OpType operation)
    {
        auto name = ReadIncomingSaveName(operation);
        std::scoped_lock lock(g_incomingSaveMutex);
        g_incomingSaveName = std::move(name);
        if (g_incomingSaveName.empty()) {
            REX::WARN(
                "Favorites Menu Grid could not tell which save is being loaded; falling back to this character's shared state");
        } else {
            REX::INFO(
                "Favorites Menu Grid is loading save '{}'", g_incomingSaveName);
        }
    }

    void BeginLoadTransition()
    {
        {
            // Preserve the page that is currently native before the load
            // replaces the player inventory. Save/load events are delivered
            // on the game thread, so this is the final reliable capture point.
            std::scoped_lock lock(g_stateMutex);
            if (g_sessionInitialized &&
                !g_switchInProgress.load(std::memory_order_acquire)) {
                CaptureCurrentBankLocked(false);
            }
        }
        g_sessionGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_switchQueued.store(false, std::memory_order_release);
        g_captureQueued.store(false, std::memory_order_release);
        g_commitQueued.store(false, std::memory_order_release);
        g_snapshotRequested.store(false, std::memory_order_release);
        g_favoritesMenuVisible.store(false, std::memory_order_release);
        std::scoped_lock lock(g_stateMutex);
        g_sessionInitialized = false;
        g_stateLoaded = false;
        g_characterID = 0;
        g_loadedSaveName.clear();
        g_recentSaveAtSessionStart.clear();
        g_stateSource.clear();
        g_activeBank = 0;
        g_nativeBank = 0;
        g_pendingVisual = PendingVisual{};
        g_banks = FavoriteBanks{};
        g_pinnedSlots = FavoriteBank{};
        g_pinnedVisuals.clear();
        g_pinnedSeenAt.fill({});
        REX::DEBUG("Favorites Menu Grid load transition reset complete");
    }

    void QueueLoadedGameInitialization()
    {
        const auto generation = g_sessionGeneration.load(
            std::memory_order_acquire);
        const auto* tasks = SFSE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        tasks->AddTask([generation]() {
            if (generation != g_sessionGeneration.load(
                    std::memory_order_acquire)) {
                return;
            }
            if (InitializeSessionIfNeeded()) {
                // Before drawing, so a cell that is faint is faint because
                // the item is missing right now, not because it was missing
                // the last time this page happened to be committed.
                RefreshCarriedFlags();
                RenderActiveBank();
                        UpdateGridOverlay();
            }
        });
    }
}
