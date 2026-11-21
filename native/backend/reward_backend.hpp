#pragma once

#include "gc_root.hpp"
#include "reference_field.hpp"
#include "../common/chest_rewards.hpp"
#include "../common/runtime.hpp"
#include "../common/win_util.hpp"
#include "../vendor/minhook/include/MinHook.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vst::detail {

// All instance access is made by native callbacks or Runtime's main-thread
// adapter. No wrapper assembly, save data, fixed address or game table ships.
template <class Host, class Api>
class RewardBackend {
    using Root = StrongRootT<Api>;
    Host& h;
    static constexpr const char* kType = "VampireSurvivors.Data.WeaponType";
    static constexpr const char* kWeaponData = "VampireSurvivors.Data.Weapons.WeaponData";
    static constexpr const char* kDataList = "System.Collections.Generic.List<VampireSurvivors.Data.Weapons.WeaponData>";
    static constexpr const char* kTypes = "System.Collections.Generic.List<VampireSurvivors.Data.WeaponType>";
    static constexpr const char* kPrizes = "System.Collections.Generic.List<VampireSurvivors.TreasurePrizeTypePair>";
    static constexpr const char* kPrizeSlots = "System.Collections.Generic.List<System.Nullable<VampireSurvivors.Data.PrizeType>>";
    static constexpr const char* kItems = "System.Collections.Generic.List<VampireSurvivors.LevelUpItemUI>";
    static constexpr const char* kConverted = "System.Collections.Generic.Dictionary<VampireSurvivors.Data.WeaponType,System.Collections.Generic.List<VampireSurvivors.Data.Weapons.WeaponData>>";
    static constexpr const char* kRaw = "System.Collections.Generic.Dictionary<VampireSurvivors.Data.WeaponType,Newtonsoft.Json.Linq.JArray>";
    enum RouteIndex : size_t { Generate, Pair, Pull, Claim, Populate, Availability, Initial, Friendship, Reroll, Spawn, Select, RouteCount };
    struct Route { const void* method = nullptr; void* entry = nullptr; void* original = nullptr; bool created = false; };
    std::array<Route, RouteCount> routes{};
    void *data_class = nullptr, *header_class = nullptr, *treasure_class = nullptr, *pair_class = nullptr;
    void *page_class = nullptr, *item_class = nullptr, *level_factory_class = nullptr, *weapon_factory_class = nullptr;
    const void *manager_data = nullptr, *menu_data = nullptr, *converted = nullptr, *raw = nullptr;
    const void *accessories = nullptr, *get_weapon = nullptr, *get_accessory = nullptr, *equipment_level = nullptr, *equipment_max = nullptr;
    const void *weapons_facade = nullptr, *facade_factory = nullptr, *weapon_prefab = nullptr, *data_for_level = nullptr;
    const void *header_hidden = nullptr, *header_always_hidden = nullptr, *header_special = nullptr, *header_evolution = nullptr;
    const void *header_passive = nullptr, *header_group = nullptr, *header_level = nullptr, *header_name = nullptr, *header_frame = nullptr, *header_bases = nullptr;
    const void *name_term = nullptr, *dlc_loaded = nullptr;
    const void *treasure_opening = nullptr, *treasure_prizes = nullptr, *treasure_accumulated = nullptr;
    const void *treasure_level = nullptr, *treasure_arcana = nullptr, *treasure_slots = nullptr;
    void* prize_class = nullptr;
    const void *pair_type = nullptr, *pair_weapon = nullptr;
    const void *page_character = nullptr, *page_items = nullptr, *page_choices = nullptr, *page_data = nullptr, *page_source = nullptr, *page_factory = nullptr;
    const void *page_selected = nullptr, *page_banish = nullptr, *item_weapon = nullptr, *item_index = nullptr, *item_data = nullptr, *item_limit = nullptr;
    int32_t prize_fixed = 0, prize_arcana = 0, prize_survarot = 0, group_base = 0, group_extra = 0;
    int32_t prize_existing_any = 0;
    bool catalog_ready = false, chest_ready = false, upgrade_ready = false;
    bool five_chest_ready = false;
    Root promoted_chest;
    int32_t previous_chest_level = 0;
    struct EnumItem { std::string id; int32_t type = 0; };
    struct CatalogItem { RewardItemDefinition item; int32_t type = 0; };
    std::vector<EnumItem> enum_items;
    std::vector<CatalogItem> catalog;
    std::unordered_map<std::string, size_t> by_id;
    Root catalog_owner;
    int32_t catalog_count = -1;
    std::string catalog_language;
    size_t catalog_english = 0;
    uint64_t catalog_retry_at = 0;
    struct IconMethods {
        void* texture2d = nullptr;
        void* color32 = nullptr;
        const void *key = nullptr, *sprite = nullptr, *texture = nullptr, *rect = nullptr, *rotation = nullptr;
        const void *width = nullptr, *height = nullptr, *create = nullptr, *release = nullptr, *blit = nullptr;
        const void *active = nullptr, *set_active = nullptr, *construct = nullptr, *read = nullptr, *pixels = nullptr, *destroy = nullptr;
    } icon;
    struct IconPixels {
        std::string rgba_hex;
        int32_t width = 0, height = 0;
    };
    bool icons_ready = false;
    uint64_t next_icon_read = 0;
    std::unordered_map<std::string, IconPixels> icons;
    // Localization never changes the game's selected language or loads data.
    void *localization_class = nullptr, *language_source_class = nullptr, *language_class = nullptr, *term_class = nullptr;
    const void *current_language = nullptr, *translation = nullptr, *language_sources = nullptr, *languages = nullptr;
    const void *language_name = nullptr, *language_code = nullptr, *get_term = nullptr, *term_languages = nullptr, *term_translation = nullptr;
    struct EnglishSlot { Root source; int32_t index = 0; };
    std::vector<EnglishSlot> english_slots;
    struct CatalogBuild {
        enum class Stage { Sources, Source, Language, Entry, Level, Details, English, VerifyOwner, VerifySource, VerifyLanguage, Commit };
        Stage stage = Stage::Sources;
        Root owner, source, sources, language_source, language_entries, levels, header, term;
        int32_t count = 0, source_count = 0, source_index = 0, language_count = 0, language_index = 0;
        int32_t length = 0, level_index = 0, highest = 0;
        size_t enum_index = 0, total_levels = 0, english_index = 0;
        uint64_t destruction = 0;
        uintptr_t actor = 0;
        std::string scene, language, term_key;
        CatalogItem item;
        std::vector<CatalogItem> items;
        std::vector<EnglishSlot> english;
    };
    std::shared_ptr<CatalogBuild> catalog_build;
    std::string catalog_error;
    uint64_t seen_epoch = 0;
    struct PendingChest { Root treasure, prizes; uint64_t epoch = 0; uintptr_t owner = 0; int32_t type = 0; std::string id, name; bool claiming = false; };
    struct PendingOffer { Root page, button; uint64_t epoch = 0; uintptr_t owner = 0; int32_t type = 0; std::string id, name; bool selecting = false; };
    std::unordered_map<uintptr_t, std::shared_ptr<PendingChest>> chests;
    std::unordered_map<uintptr_t, std::shared_ptr<PendingOffer>> offers;
    struct Generation { RewardBackend* self = nullptr; Root factory, treasure; uintptr_t owner = 0; uint64_t epoch = 0; int32_t type = 0; std::string id, name; bool attempted = false, applied = false; };
    struct Population { RewardBackend* self = nullptr; Root page, factory; uintptr_t owner = 0; uint64_t epoch = 0; std::string id; bool observed = false; };
    struct Menu { RewardBackend* self = nullptr; Root page, choices, data, button; uintptr_t owner = 0; uint64_t epoch = 0; int32_t type = 0; std::string id, name; bool reroll = false, attempted = false, applied = false, failed = false; unsigned rendered = 0; };
    static inline std::atomic<RewardBackend*> active{nullptr};
    static inline thread_local Generation* generation = nullptr;
    static inline thread_local Generation* forced_pair = nullptr;
    static inline thread_local Population* population = nullptr;
    static inline thread_local Menu* menu = nullptr;
    template <class Scope> struct Boundary {
        Scope*& slot; Scope* previous;
        explicit Boundary(Scope*& value) noexcept : slot(value), previous(value) { slot = nullptr; }
        ~Boundary() { slot = previous; }
    };
    struct Unwind {
        RewardBackend& self; RewardUse use; uint64_t epoch; int exceptions = std::uncaught_exceptions();
        ~Unwind() { if (epoch && std::uncaught_exceptions() > exceptions) self.Unavailable(use, "The native reward flow raised an exception; future forcing is disabled.", epoch); }
    };
    template <class Callback> struct Finally {
        Callback callback;
        explicit Finally(Callback value) : callback(std::move(value)) {}
        ~Finally() { callback(); }
    };
    [[noreturn]] static void Fail(const char* message) { throw std::runtime_error(message); }
    Api& A() const { return h.api; }
    void* C(const void* image, const char* space, const char* name) { return A().class_from_name(image, space, name); }
    const void* F(void* klass, const char* name, std::string_view expected, bool is_static = false) {
        if (!klass) return nullptr;
        const void* field = A().class_get_field_from_name(klass, name);
        if (!field || (((A().field_get_flags(field) & 0x10) != 0) != is_static)
            || A().CanonicalType(A().field_get_type(field)) != expected) return nullptr;
        const size_t offset = A().field_get_offset(field);
        return is_static || (offset >= 2 * sizeof(void*) && offset < 1024 * 1024) ? field : nullptr;
    }
    const void* M(void* klass, const char* name, const char* result, std::initializer_list<const char*> args, uint32_t token, bool is_static = false) {
        const void* method = A().Method(klass, name, result, args, is_static, token);
        std::string error;
        return method && h.RewardEntry(method, false, error) ? method : nullptr;
    }
    void RouteMethod(RouteIndex index, void* klass, const char* name, const char* result, std::initializer_list<const char*> args, uint32_t token) {
        auto& route = routes[index];
        route.method = A().Method(klass, name, result, args, false, token);
        std::string error; route.entry = h.RewardEntry(route.method, false, error);
    }
    template <class T> T Field(void* owner, const void* field) const {
        if (!owner || !field) Fail("Reward object data is unavailable.");
        T value{}; A().field_get_value(owner, field, &value); return value;
    }
    void* Object(const void* method, void* owner, void** args = nullptr) const {
        void* result = nullptr;
        if (!method || !A().Invoke(method, owner, args, result)) Fail("A verified native reward read failed.");
        return result;
    }
    template <class T> T Scalar(const void* method, void* owner, void** args = nullptr) const {
        void* box = Object(method, owner, args);
        void* data = box ? A().object_unbox(box) : nullptr;
        if (!IsReadableMemory(data, sizeof(T))) Fail("A native reward value is unavailable.");
        T value{}; std::memcpy(&value, data, sizeof(value)); return value;
    }
    std::string Text(void* text) const {
        if (!text) return {};
        const int32_t length = A().string_length(text);
        const wchar_t* chars = A().string_chars(text);
        if (length < 0 || length > 8192 || !IsReadableMemory(chars, static_cast<size_t>(length) * sizeof(wchar_t))) return {};
        return Utf8(std::wstring(chars, static_cast<size_t>(length)));
    }
    static bool Named(std::string_view id) {
        if (id.empty() || id.size() > 128) return false;
        for (char ch : id) if (!(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '_') return false;
        return id != "VOID" && id != "CANDYBOX" && id != "CANDYBOX2" && id != "ARMADIO";
    }
    static std::string Normalize(std::string_view value) {
        std::wstring source = Wide(std::string(value)), result;
        bool tag = false, space = false;
        for (wchar_t ch : source) {
            if (ch == L'<') { tag = true; continue; }
            if (tag) { if (ch == L'>') tag = false; continue; }
            if (std::iswspace(ch)) { space = !result.empty(); continue; }
            if (ch < 32 || (ch >= 0x7f && ch <= 0x9f)) continue;
            if (space) { result += L' '; space = false; }
            if (result.size() >= 96) break;
            result += ch;
        }
        if (!result.empty() && result.back() >= 0xd800 && result.back() <= 0xdbff) result.pop_back();
        return Utf8(result);
    }
    static bool Untranslated(std::string_view value, std::string_view term) {
        auto starts = [&](std::string_view prefix) {
            if (value.size() < prefix.size()) return false;
            for (size_t index = 0; index < prefix.size(); ++index) {
                char ch = value[index]; if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                if (ch != prefix[index]) return false;
            }
            return true;
        };
        return value.empty() || value == term || starts("lang/") || starts("weaponlang/");
    }
    const void* CM(void* collection, std::string_view type, const char* name, const char* result,
        std::initializer_list<const char*> args, uint32_t token, uint32_t vars = 0, bool result_var = false) const {
        if (!collection || !IsReadableMemory(collection, 2 * sizeof(void*))) Fail("A reward collection is unavailable.");
        const void* method = A().CollectionMethod(A().object_get_class(collection), type, name, result, args, token, vars, result_var);
        if (!method) throw std::runtime_error("Cannot verify native collection " + std::string(type) + "." + name + ".");
        return method;
    }
    int32_t ListCount(void* list, std::string_view type, int32_t maximum = 10000) const {
        const int32_t count = Scalar<int32_t>(CM(list, type, "get_Count", "System.Int32", {}, 100675784), list);
        if (count < 0 || count > maximum) Fail("The native reward collection exceeds its bound.");
        return count;
    }
    void* ListObject(void* list, std::string_view type, const char* element, int32_t index) const {
        void* args[]{&index};
        return Object(CM(list, type, "get_Item", element, {"System.Int32"}, 100675790, 0, true), list, args);
    }
    int32_t ListType(void* list, int32_t index) const {
        void* args[]{&index};
        return Scalar<int32_t>(CM(list, kTypes, "get_Item", kType, {"System.Int32"}, 100675790, 0, true), list, args);
    }
    void SetType(void* list, int32_t index, int32_t value) const {
        void* args[]{&index, &value};
        Object(CM(list, kTypes, "set_Item", "System.Void", {"System.Int32", kType}, 100675791, 2), list, args);
    }
    bool ContainsType(void* list, int32_t value) const {
        void* args[]{&value};
        return Scalar<bool>(CM(list, kTypes, "Contains", "System.Boolean", {kType}, 100675804, 1), list, args);
    }
    int32_t DictCount(void* dict, std::string_view type) const {
        int32_t count = Scalar<int32_t>(CM(dict, type, "get_Count", "System.Int32", {}, 100675641), dict);
        if (count < 0 || count > 10000) Fail("The native weapon catalog exceeds its bound.");
        return count;
    }
    void* DictObject(void* dict, std::string_view type, const char* element, int32_t key) const {
        void* args[]{&key};
        if (!Scalar<bool>(CM(dict, type, "ContainsKey", "System.Boolean", {kType}, 100675655, 1), dict, args)) return nullptr;
        return Object(CM(dict, type, "get_Item", element, {kType}, 100675648, 1, true), dict, args);
    }
    bool Context(uintptr_t& owner, void*& manager, uint64_t epoch = 0) const {
        owner = h.local_player.load(std::memory_order_acquire); manager = nullptr;
        if (!owner || h.stopping.load(std::memory_order_acquire) || h.unity_thread.load() != GetCurrentThreadId()
            || h.manager_overflow.load() || (epoch && !h.runtime.RewardContextCurrent(owner, epoch, GetTickCount64()))) return false;
        unsigned count = 0;
        for (const auto& candidate : h.managers) if (uintptr_t value = candidate.load()) { manager = reinterpret_cast<void*>(value); ++count; }
        if (count != 1 || !A().IsA(manager, h.manager_class) || !h.CurrentActor(owner, h.destructions.load())) return false;
        void* current = Object(h.player_method, manager);
        return reinterpret_cast<uintptr_t>(current) == owner && !Scalar<bool>(h.multiplayer_method, manager)
            && !Field<bool>(manager, h.started_online_field) && !Field<bool>(manager, h.game_over_field);
    }
    bool Current(uintptr_t owner, uint64_t epoch) const {
        uintptr_t actual = 0; void* manager = nullptr;
        return Context(actual, manager, epoch) && actual == owner;
    }
    void SyncEpoch() {
        const uint64_t epoch = h.runtime.RewardEpoch();
        if (seen_epoch != epoch) { chests.clear(); offers.clear(); seen_epoch = epoch; }
    }
    void Unavailable(RewardUse use, const char* message, uint64_t epoch = 0) noexcept {
        if (epoch && h.runtime.RewardEpoch() != epoch) return;
        try { h.runtime.RewardUnavailable(use, message); } catch (...) {}
    }
public:
    explicit RewardBackend(Host& host) : h(host) {}
    void Resolve(const void* image);
    bool Prepare(std::string& error);
    bool Enable(std::string& error);
    void Stop() noexcept;
    void Clear() noexcept { RestoreChestLevel(); chests.clear(); offers.clear(); catalog_build.reset(); }
    void Tick() { SyncEpoch(); AdvanceCatalog(); }
private:
    bool PrepareFiveItemChest(void* treasure, uintptr_t owner, Root& original_slots, Root& padded_slots);
    void RestoreChestLevel() noexcept;
    void DisableFiveItemChests() noexcept {
        try { h.runtime.SetFeatureSupported(CombatFeature::FiveItemChests, false,
            "The native five-reward chest route could not be confirmed."); } catch (...) {}
    }
    void ResolveLocalization();
    void ResolveIcons(const void* game_image);
    void ReadIcon(const std::string& id, Response& result);
    IconPixels ReadIconPixels(const CatalogItem& item);
    void RefreshEnglish();
    std::string LocalName(void* header, int32_t type, bool english);
    std::string EnglishName(void* term, const std::string& key, const EnglishSlot& slot);
    void* DataSource();
    bool RefreshCatalog();
    void AdvanceCatalog() noexcept;
    CatalogItem& Validate(const std::string& id, bool upgrade, bool run_required);
    void* Equipment(int32_t type);
    void ValidateEvolution(void* header, int32_t type, void* data);
    void Force(Menu& scope);
    void Finish(Menu& scope);
    bool Adapter(const Request& request, Response& result, std::string& error);
    static bool AdapterThunk(void* self, const Request& request, Response& result, std::string& error) { return static_cast<RewardBackend*>(self)->Adapter(request, result, error); }
    static void* GenerateHook(void*, void*, const void*);
    static void* PairHook(void*, int32_t, int32_t, void*, bool, const void*);
    static int32_t PullHook(void*, int32_t, const void*);
    static void ClaimHook(void*, void*, bool, const void*);
    static void PopulateHook(void*, const void*);
    static bool AvailabilityHook(void*, void*, const void*);
    static void InitialHook(void*, const void*);
    static void* FriendshipHook(void*, bool, const void*);
    static void RerollHook(void*, const void*);
    static void SpawnHook(void*, void*, int32_t, int32_t, const void*);
    static void SelectHook(void*, int32_t, void*, const void*);
    void BeginMenu(void* page, bool reroll, Menu& scope);
};

template <class Host, class Api>
void RewardBackend<Host, Api>::Resolve(const void* image) {
    auto& api = A();
    data_class = C(image, "VampireSurvivors.Data", "DataManager");
    header_class = C(image, "VampireSurvivors.Data.Weapons", "WeaponData");
    treasure_class = C(image, "VampireSurvivors.Data.Stage", "Treasure");
    pair_class = C(image, "VampireSurvivors", "TreasurePrizeTypePair");
    page_class = C(image, "VampireSurvivors.UI", "LevelUpPage");
    item_class = C(image, "VampireSurvivors", "LevelUpItemUI");
    level_factory_class = C(image, "VampireSurvivors.Framework", "LevelUpFactory");
    weapon_factory_class = C(image, "VampireSurvivors.Framework", "WeaponFactory");
    void* treasure_factory = C(image, "VampireSurvivors", "TreasureFactory");
    void* equipment = C(image, "VampireSurvivors.Objects", "Equipment");
    void* weapon_manager = C(image, "VampireSurvivors.Objects", "CharacterWeaponsManager");
    void* accessory_manager = C(image, "VampireSurvivors.Objects", "CharacterAccessoriesManager");
    void* facade = C(image, "VampireSurvivors.Framework", "WeaponsFacade");
    manager_data = M(h.manager_class, "get_DataManager", "VampireSurvivors.Data.DataManager", {}, 100694957);
    menu_data = F(h.menu_class, "_dataManager", "VampireSurvivors.Data.DataManager");
    converted = M(data_class, "GetConvertedWeapons", nullptr, {}, 100731220);
    raw = M(data_class, "get_AllWeaponData", nullptr, {}, 100731144);
    if (converted && api.CanonicalType(api.method_get_return_type(converted)) != kConverted) converted = nullptr;
    if (raw && api.CanonicalType(api.method_get_return_type(raw)) != kRaw) raw = nullptr;
    accessories = M(h.character_class, "get_AccessoriesManager", "VampireSurvivors.Objects.CharacterAccessoriesManager", {}, 100689534);
    get_weapon = M(weapon_manager, "GetWeaponByType", "VampireSurvivors.Objects.Weapons.Weapon", {kType, "System.Boolean"}, 100672065);
    get_accessory = M(accessory_manager, "GetAccessoryByType", "VampireSurvivors.Objects.Accessory", {kType, "System.Boolean"}, 100672055);
    equipment_level = M(equipment, "get_Level", "System.Int32", {}, 100672029);
    equipment_max = M(equipment, "IsMaxLevel", "System.Boolean", {}, 100672048);
    weapons_facade = M(h.manager_class, "get_WeaponsFacade", "VampireSurvivors.Framework.WeaponsFacade", {}, 100694971);
    facade_factory = F(facade, "_weaponFactory", "VampireSurvivors.Framework.WeaponFactory");
    weapon_prefab = M(weapon_factory_class, "GetWeaponPrefab", "VampireSurvivors.Objects.Weapons.Weapon", {kType, "VampireSurvivors.Data.WeaponType&"}, 100695918);
    data_for_level = M(C(image, "VampireSurvivors.Tools", "DataHelper"), "GetWeaponDataForLevel", "System.Boolean",
        {"Newtonsoft.Json.Linq.JArray", "System.Int32", "VampireSurvivors.Data.Weapons.WeaponData&"}, 100666712, true);
    header_hidden = F(header_class, "<hidden>k__BackingField", "System.Boolean");
    header_always_hidden = F(header_class, "<alwaysHidden>k__BackingField", "System.Boolean");
    header_special = F(header_class, "<isSpecialOnly>k__BackingField", "System.Boolean");
    header_evolution = F(header_class, "<isEvolution>k__BackingField", "System.Boolean");
    header_passive = F(header_class, "<isPowerUp>k__BackingField", "System.Boolean");
    header_group = F(header_class, "<contentGroup>k__BackingField", "VampireSurvivors.Data.ContentGroupType");
    header_level = F(header_class, "<level>k__BackingField", "System.Int32");
    header_name = F(header_class, "<name>k__BackingField", "System.String");
    header_frame = F(header_class, "<frameName>k__BackingField", "System.String");
    header_bases = F(header_class, "<evolvesFrom>k__BackingField", kTypes);
    name_term = M(header_class, "GetLocalizedNameTerm", "System.String", {kType}, 100731939);
    dlc_loaded = M(C(image, "VampireSurvivors.Data", "ContentGroupMethods"), "IsDlcLoadedForContentGroup", "System.Boolean",
        {"VampireSurvivors.Data.ContentGroupType"}, 100731768, true);
    auto literal = [&](void* klass, const char* name, const char* expected, int32_t& value) {
        const void* field = klass ? api.class_get_field_from_name(klass, name) : nullptr;
        if (!field || (api.field_get_flags(field) & 0x50) != 0x50 || !api.TypeEquals(api.field_get_type(field), expected)) return false;
        api.field_static_get_value(field, &value); return true;
    };
    void* types = C(image, "VampireSurvivors.Data", "WeaponType");
    bool enum_ready = types && api.class_get_fields && api.field_get_name && api.TypeEquals(api.class_enum_basetype(types), "System.Int32");
    if (enum_ready) {
        void* iterator = nullptr;
        for (unsigned index = 0; index < 10000; ++index) {
            const void* field = api.class_get_fields(types, &iterator);
            if (!field) break;
            if ((api.field_get_flags(field) & 0x50) != 0x50 || !api.TypeEquals(api.field_get_type(field), kType)) continue;
            const char* name = api.field_get_name(field);
            if (!name || !IsReadableMemory(name, 129)) continue;
            size_t length = 0; while (length < 129 && name[length]) ++length;
            if (length > 128 || !Named(std::string_view(name, length))) continue;
            int32_t value = 0; api.field_static_get_value(field, &value);
            if (std::any_of(enum_items.begin(), enum_items.end(), [&](const EnumItem& entry) { return entry.type == value; })) continue;
            enum_items.push_back({std::string(name, length), value});
        }
        enum_ready = !enum_items.empty();
    }
    bool content_ready = literal(C(image, "VampireSurvivors.Data", "ContentGroupType"), "BASE", "VampireSurvivors.Data.ContentGroupType", group_base)
        && literal(C(image, "VampireSurvivors.Data", "ContentGroupType"), "EXTRA", "VampireSurvivors.Data.ContentGroupType", group_extra);
    prize_class = C(image, "VampireSurvivors.Data", "PrizeType");
    bool prize_ready = literal(prize_class, "FIXED", "VampireSurvivors.Data.PrizeType", prize_fixed)
        && literal(prize_class, "EVO_ARCANA", "VampireSurvivors.Data.PrizeType", prize_arcana)
        && literal(prize_class, "SURVAROT", "VampireSurvivors.Data.PrizeType", prize_survarot);
    treasure_opening = F(treasure_class, "openingPlayer", "VampireSurvivors.Objects.Characters.CharacterController");
    treasure_prizes = F(treasure_class, "prizes", kPrizes);
    treasure_accumulated = F(treasure_class, "accumulatedWeaponPrizes", kTypes);
    treasure_level = F(treasure_class, "<level>k__BackingField", "System.Int32");
    treasure_arcana = F(treasure_class, "<hasArcana>k__BackingField", "System.Boolean");
    treasure_slots = F(treasure_class, "<prizeTypes>k__BackingField", kPrizeSlots);
    five_chest_ready = prize_ready && treasure_opening && treasure_prizes && treasure_level && treasure_arcana && treasure_slots
        && api.field_set_value && api.gc_wbarrier_set_field && api.object_new && api.value_box
        && api.gchandle_new && api.gchandle_get_target && api.gchandle_free && api.class_get_type
        && literal(prize_class, "EXISTING_ANY", "VampireSurvivors.Data.PrizeType", prize_existing_any);
    pair_type = F(pair_class, "prizeType", "VampireSurvivors.Data.PrizeType");
    pair_weapon = F(pair_class, "prizeWeapon", kType);
    page_character = M(page_class, "GetCharacterControllingUi", "VampireSurvivors.Objects.Characters.CharacterController", {}, 100669606);
    page_items = M(page_class, "get_LevelUpItems", nullptr, {}, 100669546);
    if (page_items && api.CanonicalType(api.method_get_return_type(page_items)) != kItems) page_items = nullptr;
    page_choices = F(page_class, "_currentWeapons", kTypes);
    page_data = F(page_class, "_data", "VampireSurvivors.Data.DataManager");
    page_source = F(page_class, "_weaponData", kConverted);
    page_factory = F(page_class, "_levelUpFactory", "VampireSurvivors.Framework.LevelUpFactory");
    page_selected = F(page_class, "_hasSelected", "System.Boolean"); page_banish = F(page_class, "_isBanishMode", "System.Boolean");
    item_weapon = M(item_class, "GetWeaponType", kType, {}, 100666057);
    item_index = M(item_class, "get_Index", "System.Int32", {}, 100666046);
    item_data = F(item_class, "_data", kWeaponData); item_limit = F(item_class, "_isLimitBreak", "System.Boolean");
    RouteMethod(Generate, treasure_factory, "GenerateNewPrizes", nullptr, {"VampireSurvivors.Data.Stage.Treasure"}, 100663781);
    if (routes[Generate].method && api.CanonicalType(api.method_get_return_type(routes[Generate].method)) != kPrizes) routes[Generate].entry = nullptr;
    RouteMethod(Pair, treasure_factory, "MakePrizePairFromAvailablePowerUps", "VampireSurvivors.TreasurePrizeTypePair",
        {"VampireSurvivors.Data.PrizeType", kType, "VampireSurvivors.Objects.Characters.CharacterController", "System.Boolean"}, 100663782);
    RouteMethod(Pull, level_factory_class, "PullExisting", kType, {kType}, 100695604);
    RouteMethod(Claim, treasure_class, "ClaimPrizes", "System.Void", {"VampireSurvivors.Objects.Characters.CharacterController", "System.Boolean"}, 100732031);
    RouteMethod(Populate, page_class, "Populate", "System.Void", {}, 100669619);
    RouteMethod(Availability, level_factory_class, "HasPowerupsInStore", "System.Boolean", {"VampireSurvivors.Objects.Characters.CharacterController"}, 100695598);
    RouteMethod(Initial, page_class, "PickRandomLevelUps", "System.Void", {}, 100669621);
    RouteMethod(Friendship, level_factory_class, "FindFriendshipAmuletTargets", nullptr, {"System.Boolean"}, 100695630);
    if (routes[Friendship].method && api.CanonicalType(api.method_get_return_type(routes[Friendship].method))
        != "System.Collections.Generic.List<VampireSurvivors.Objects.Characters.CharacterController>") routes[Friendship].entry = nullptr;
    RouteMethod(Reroll, page_class, "ResetLevelUpViewsAfterReRoll", "System.Void", {}, 100669622);
    RouteMethod(Spawn, page_class, "SpawnWeapon", "System.Void", {kWeaponData, kType, "System.Int32"}, 100669629);
    RouteMethod(Select, page_class, "SelectWeapon", "System.Void", {kType, "VampireSurvivors.LevelUpItemUI"}, 100669571);
    for (size_t index = 0; index < routes.size(); ++index) {
        void* entry = routes[index].entry;
        if (!entry) continue;
        bool collision = h.RewardEntryCollision(entry);
        for (size_t other = 0; other < routes.size(); ++other) if (index != other && routes[other].entry == entry) collision = true;
        if (collision) for (auto& route : routes) if (route.entry == entry) route.entry = nullptr;
    }
    catalog_ready = api.gchandle_new && api.gchandle_get_target && api.gchandle_free && api.class_get_type && enum_ready && content_ready
        && data_class && header_class && manager_data && menu_data && converted && header_hidden && header_always_hidden && header_special
        && header_evolution && header_passive && header_group && header_level && header_name && header_frame && name_term && dlc_loaded;
    chest_ready = catalog_ready && prize_ready && treasure_opening && treasure_prizes && treasure_accumulated && pair_type && pair_weapon
        && accessories && h.weapons_method && get_weapon && get_accessory && equipment_level && equipment_max;
    upgrade_ready = chest_ready && raw && weapons_facade && facade_factory && weapon_prefab && data_for_level && header_bases
        && page_character && page_items && page_choices && page_data && page_source && page_factory && page_selected && page_banish
        && item_weapon && item_index && item_data && item_limit;
    ResolveLocalization();
    ResolveIcons(image);
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::Prepare(std::string&) {
    const std::array<void*, RouteCount> callbacks{{reinterpret_cast<void*>(&GenerateHook), reinterpret_cast<void*>(&PairHook),
        reinterpret_cast<void*>(&PullHook), reinterpret_cast<void*>(&ClaimHook), reinterpret_cast<void*>(&PopulateHook),
        reinterpret_cast<void*>(&AvailabilityHook), reinterpret_cast<void*>(&InitialHook), reinterpret_cast<void*>(&FriendshipHook),
        reinterpret_cast<void*>(&RerollHook), reinterpret_cast<void*>(&SpawnHook), reinterpret_cast<void*>(&SelectHook)}};
    for (size_t index = 0; index < routes.size(); ++index) {
        auto& route = routes[index];
        if (route.entry) route.created = MH_CreateHook(route.entry, callbacks[index], &route.original) == MH_OK;
    }
    for (size_t index = Generate; index <= Claim; ++index) chest_ready = chest_ready && routes[index].created;
    five_chest_ready = five_chest_ready && routes[Generate].created && routes[Claim].created;
    for (size_t index = Populate; index <= Select; ++index) upgrade_ready = upgrade_ready && routes[index].created;
    active.store(this, std::memory_order_release);
    auto missing = [&](bool upgrade) {
        std::string result = "Missing native reward members: ";
        bool first = true;
        auto require = [&](bool present, const char* name) {
            if (present) return;
            if (!first) result += ", "; first = false; result += name;
        };
        for (const auto& entry : std::initializer_list<std::pair<const void*, const char*>>{
            {manager_data,"GameManager.DataManager"},{menu_data,"MainMenuPage._dataManager"},{converted,"DataManager.GetConvertedWeapons"},
            {header_hidden,"WeaponData.hidden"},{header_always_hidden,"WeaponData.alwaysHidden"},{header_special,"WeaponData.isSpecialOnly"},
            {header_evolution,"WeaponData.isEvolution"},{header_passive,"WeaponData.isPowerUp"},{header_group,"WeaponData.contentGroup"},
            {header_level,"WeaponData.level"},{header_name,"WeaponData.name"},{header_frame,"WeaponData.frameName"},
            {name_term,"WeaponData.GetLocalizedNameTerm"},{dlc_loaded,"ContentGroupMethods.IsDlcLoadedForContentGroup"},
            {accessories,"Character.AccessoriesManager"},{get_weapon,"GetWeaponByType"},{get_accessory,"GetAccessoryByType"},
            {equipment_level,"Equipment.Level"},{equipment_max,"Equipment.IsMaxLevel"}}) require(entry.first != nullptr, entry.second);
        require(!enum_items.empty(), "WeaponType named enum literals");
        if (!upgrade) {
            require(treasure_opening && treasure_prizes && treasure_accumulated, "Treasure openingPlayer/prizes/accumulatedWeaponPrizes");
            require(pair_type && pair_weapon, "TreasurePrizeTypePair prizeType/prizeWeapon");
        } else {
            for (const auto& entry : std::initializer_list<std::pair<const void*, const char*>>{
                {raw,"DataManager.AllWeaponData"},{weapons_facade,"GameManager.WeaponsFacade"},{facade_factory,"WeaponsFacade._weaponFactory"},
                {weapon_prefab,"WeaponFactory.GetWeaponPrefab"},{data_for_level,"DataHelper.GetWeaponDataForLevel"},{header_bases,"WeaponData.evolvesFrom"},
                {page_character,"LevelUpPage.GetCharacterControllingUi"},{page_items,"LevelUpPage.LevelUpItems"},{page_choices,"LevelUpPage._currentWeapons"},
                {page_source,"LevelUpPage._weaponData"},{page_data,"LevelUpPage._data"},{page_factory,"LevelUpPage._levelUpFactory"},
                {page_selected,"LevelUpPage._hasSelected"},{page_banish,"LevelUpPage._isBanishMode"},{item_weapon,"LevelUpItemUI.GetWeaponType"},
                {item_index,"LevelUpItemUI.Index"},{item_data,"LevelUpItemUI._data"},{item_limit,"LevelUpItemUI._isLimitBreak"}}) require(entry.first != nullptr, entry.second);
        }
        static constexpr std::array<const char*, RouteCount> names{{"GenerateNewPrizes", "MakePrizePairFromAvailablePowerUps", "PullExisting", "ClaimPrizes",
            "Populate", "HasPowerupsInStore", "PickRandomLevelUps", "FindFriendshipAmuletTargets", "ResetLevelUpViewsAfterReRoll", "SpawnWeapon", "SelectWeapon"}};
        for (size_t index = upgrade ? Populate : Generate, last = upgrade ? Select : Claim; index <= last; ++index)
            require(routes[index].created, names[index]);
        if (first) result += "required enum/export or exact metadata validation";
        return result;
    };
    h.runtime.SetRewardSupported(RewardUse::Chest, chest_ready, chest_ready ? "" : missing(false));
    h.runtime.SetFeatureSupported(CombatFeature::FiveItemChests, five_chest_ready,
        five_chest_ready ? "" : "The version-verified five-reward chest route is unavailable.");
    h.runtime.SetRewardSupported(RewardUse::Upgrade, upgrade_ready, upgrade_ready ? "" : missing(true));
    if (catalog_ready) h.runtime.SetRewardAdapter(&AdapterThunk, this);
    return true;
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::Enable(std::string& error) {
    for (const auto& route : routes) if (route.created && MH_QueueEnableHook(route.entry) != MH_OK) {
        error = "Cannot enable a native reward hook."; return false;
    }
    return true;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::Stop() noexcept {
    try { h.runtime.SetRewardAdapter(nullptr, nullptr); } catch (...) {}
    for (RewardUse use : {RewardUse::Chest, RewardUse::Upgrade}) {
        try { h.runtime.SetRewardSupported(use, false, "The native reward backend is stopped."); } catch (...) {}
    }
    for (const auto& route : routes) if (route.created) { MH_QueueDisableHook(route.entry); MH_DisableHook(route.entry); }
    // Rooted pending entries are released by the next game-thread lifecycle
    // callback; a worker does not touch native objects or free trampolines.
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::Adapter(const Request& request, Response& result, std::string& error) {
    try {
        if (!catalog_ready || h.stopping.load() || h.unity_thread.load() != GetCurrentThreadId()) Fail("Native reward data is unavailable.");
        SyncEpoch();
        if (request.kind == "getItemCatalog") {
            if (request.catalog_offset < 0 || request.catalog_limit < 1 || request.catalog_limit > 200) Fail("Invalid reward catalog page.");
            result.has_item_catalog = true;
            result.item_icons_supported = icons_ready;
            if (!RefreshCatalog()) {
                result.item_catalog_loading = true;
                result.item_catalog_total = 0;
                result.item_catalog.clear();
                return true;
            }
            result.item_catalog_total = static_cast<int>(catalog.size());
            const size_t first = std::min(static_cast<size_t>(request.catalog_offset), catalog.size());
            const size_t last = std::min(first + static_cast<size_t>(request.catalog_limit), catalog.size());
            for (size_t index = first; index < last; ++index) result.item_catalog.push_back(catalog[index].item);
            return true;
        }
        if (request.kind == "getItemIcon") {
            ReadIcon(request.item_id, result);
            return true;
        }
        if (request.kind != "setChestReward") Fail("Unknown native reward request.");
        if (request.enabled) result.error_scope = "chest";
        const std::string target_name = Validate(request.item_id, false, request.enabled || request.upgrade_enabled).item.name;
        if (request.enabled && !chest_ready) { result.error_scope = "chest"; Fail("The native chest reward route is unavailable."); }
        if (request.upgrade_enabled) {
            result.error_scope = "upgrade";
            if (!upgrade_ready) Fail("The native upgrade choice route is unavailable.");
            Validate(request.item_id, true, true);
        }
        result.error_scope.clear(); result.state.chest_reward.target_item_name = target_name;
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "Native reward validation failed."; return false; }
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::PrepareFiveItemChest(void* treasure, uintptr_t owner, Root& original_slots, Root& padded_slots) {
    if (!five_chest_ready || !h.runtime.FeatureEnabled(CombatFeature::FiveItemChests, owner, GetTickCount64())
        || !A().IsA(treasure, treasure_class)
        || reinterpret_cast<uintptr_t>(Field<void*>(treasure, treasure_opening)) != owner) return false;
    uintptr_t actor = 0; void* manager = nullptr;
    if (!Context(actor, manager) || actor != owner || Field<bool>(treasure, treasure_arcana)) return false;
    const int32_t level = Field<int32_t>(treasure, treasure_level);
    if (level < 1 || level > kFiveItemChestLevel) return false;
    void* slots = Field<void*>(treasure, treasure_slots);
    if (!slots) return false;
    std::array<std::optional<int32_t>, 32> values{};
    const int32_t count = ListCount(slots, kPrizeSlots, static_cast<int32_t>(values.size()));
    const auto get = CM(slots, kPrizeSlots, "System.Collections.IList.get_Item", "System.Object", {"System.Int32"}, 100675793);
    for (int32_t i = 0; i < count; ++i) {
        void* args[]{&i};
        void* box = Object(get, slots, args);
        if (!box) continue; // Boxing a nullable prize yields its enum or null.
        void* value = A().object_unbox(box);
        if (A().object_get_class(box) != prize_class || !IsReadableMemory(value, sizeof(int32_t)))
            Fail("A native chest prize slot has an unexpected type.");
        int32_t type = 0;
        std::memcpy(&type, value, sizeof(type));
        values[i] = type;
    }
    const auto plan = PlanFiveItemChest(level, false, std::span(values).first(count),
        prize_existing_any, prize_arcana, prize_survarot);
    if (!plan.eligible) return false;
    if (plan.pad) {
        original_slots = Root(A(), slots);
        padded_slots = Root(A(), A().object_new(A().object_get_class(slots)));
        void* expanded = padded_slots.Get();
        Object(CM(expanded, kPrizeSlots, ".ctor", "System.Void", {}, 100675779), expanded);
        const auto add = CM(expanded, kPrizeSlots, "System.Collections.IList.Add", "System.Int32", {"System.Object"}, 100675797);
        int32_t index = 0;
        for (int32_t type : plan.slots) {
            Root box(A(), A().value_box(prize_class, &type));
            if (!box.Get()) Fail("A native chest prize slot could not be boxed.");
            void* args[]{box.Get()};
            if (Scalar<int32_t>(add, expanded, args) != index++) Fail("A native chest prize slot could not be appended.");
        }
    }
    if (!h.runtime.FeatureEnabled(CombatFeature::FiveItemChests, owner, GetTickCount64())) return false;
    // A new local chest takes over the single-player reward screen. Its
    // predecessor can have been abandoned after generation.
    RestoreChestLevel();
    if (level != kFiveItemChestLevel) {
        // Keep the promoted tier through the native UI and settlement.
        promoted_chest = Root(A(), treasure);
        previous_chest_level = level;
        int32_t promoted = kFiveItemChestLevel;
        A().field_set_value(treasure, treasure_level, &promoted);
        if (Field<int32_t>(treasure, treasure_level) != promoted) Fail("The native chest tier could not be set.");
    }
    if (plan.pad && !WriteReferenceField(A(), treasure, treasure_slots, padded_slots.Get()))
        Fail("The native chest prize list could not be set.");
    return true;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::RestoreChestLevel() noexcept {
    if (void* treasure = promoted_chest.Get()) {
        int32_t level = 0;
        A().field_get_value(treasure, treasure_level, &level);
        if (level == kFiveItemChestLevel) A().field_set_value(treasure, treasure_level, &previous_chest_level);
    }
    promoted_chest = {};
}

template <class Host, class Api>
void* RewardBackend<Host, Api>::GenerateHook(void* factory, void* treasure, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Generate].original) return nullptr;
    Boundary<Generation> generation_boundary(generation), pair_boundary(forced_pair);
    Generation scope; scope.self = self;
    try {
        self->SyncEpoch();
        uintptr_t owner = 0; void* manager = nullptr;
        if (self->chest_ready && self->Context(owner, manager)) {
            const auto target = self->h.runtime.RewardTarget(RewardUse::Chest, owner, GetTickCount64());
            const uint64_t epoch = self->h.runtime.RewardEpoch();
            if (!target.empty() && self->A().IsA(treasure, self->treasure_class)) {
                void* opening = self->template Field<void*>(treasure, self->treasure_opening);
                if (!opening || reinterpret_cast<uintptr_t>(opening) == owner) {
                    auto old = self->chests.find(reinterpret_cast<uintptr_t>(treasure));
                    if (old == self->chests.end() || !old->second->claiming) {
                        self->chests.erase(reinterpret_cast<uintptr_t>(treasure));
                        if (self->chests.size() >= 256) Fail("Too many pending native chests; chest forcing is disabled.");
                        auto& selected = self->Validate(target, false, true);
                        if (self->Current(owner, epoch)) {
                            scope.factory = Root(self->A(), factory); scope.treasure = Root(self->A(), treasure);
                            scope.owner = owner; scope.epoch = epoch; scope.type = selected.type;
                            scope.id = selected.item.id; scope.name = selected.item.name; generation = &scope;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Chest, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Chest, "Native chest validation failed.", scope.epoch); }
    Unwind unwind{*self, RewardUse::Chest, scope.epoch};
    const bool game_thread = self->h.unity_thread.load() == GetCurrentThreadId();
    Root original_slots, padded_slots;
    bool five_ready = false, five_confirmed = false;
    const auto restore_slots = [&] {
        if (padded_slots.Get() && self->template Field<void*>(treasure, self->treasure_slots) == padded_slots.Get()
            && !WriteReferenceField(self->A(), treasure, self->treasure_slots, original_slots.Get())) self->DisableFiveItemChests();
    };
    if (game_thread && treasure && self->promoted_chest.Get() == treasure) self->RestoreChestLevel();
    const uintptr_t five_owner = game_thread ? self->h.local_player.load(std::memory_order_acquire) : 0;
    try {
        if (five_owner) five_ready = self->PrepareFiveItemChest(treasure, five_owner, original_slots, padded_slots);
    } catch (...) {
        restore_slots();
        if (treasure && self->promoted_chest.Get() == treasure) self->RestoreChestLevel();
        self->DisableFiveItemChests();
    }
    Finally restore_five([&] {
        if (!five_ready) return;
        restore_slots();
        if (!five_confirmed) {
            if (self->promoted_chest.Get() == treasure) self->RestoreChestLevel();
            self->DisableFiveItemChests();
        }
    });
    using Function = void* (*)(void*, void*, const void*);
    void* result = reinterpret_cast<Function>(self->routes[Generate].original)(factory, treasure, method);
    if (five_ready) {
        try {
            five_confirmed = self->template Field<void*>(treasure, self->treasure_prizes) == result
                && self->ListCount(result, kPrizes, kFiveItemChestCount) == kFiveItemChestCount;
            if (!five_confirmed) Fail("The native chest did not generate five rewards.");
            self->h.runtime.RecordChestEvent(CombatFeature::FiveItemChests, five_owner, GetTickCount64());
        } catch (...) { five_confirmed = false; }
    }
    try {
        if (generation == &scope && scope.applied && self->Current(scope.owner, scope.epoch)) {
            void* prizes = self->template Field<void*>(scope.treasure.Get(), self->treasure_prizes);
            void* accumulated = self->template Field<void*>(scope.treasure.Get(), self->treasure_accumulated);
            self->ListCount(prizes, kPrizes, 128);
            if (!self->ContainsType(accumulated, scope.type)) Fail("The native chest settlement list does not contain the selected item.");
            auto pending = std::make_shared<PendingChest>();
            pending->treasure = scope.treasure; pending->prizes = Root(self->A(), prizes);
            pending->owner = scope.owner; pending->epoch = scope.epoch; pending->type = scope.type;
            pending->id = scope.id; pending->name = scope.name;
            self->chests[reinterpret_cast<uintptr_t>(scope.treasure.Get())] = std::move(pending);
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Chest, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Chest, "Native chest settlement verification failed.", scope.epoch); }
    return result;
}

template <class Host, class Api>
void* RewardBackend<Host, Api>::PairHook(void* factory, int32_t prize, int32_t weapon, void* player, bool flag, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Pair].original) return nullptr;
    Boundary<Generation> boundary(forced_pair);
    Generation* scope = generation;
    try {
        if (scope && scope->self == self && !scope->attempted && scope->factory.Get() == factory
            && reinterpret_cast<uintptr_t>(player) == scope->owner && self->Current(scope->owner, scope->epoch)
            && self->h.runtime.RewardTarget(RewardUse::Chest, scope->owner, GetTickCount64()) == scope->id
            && prize != self->prize_arcana && prize != self->prize_survarot) {
            scope->attempted = true; forced_pair = scope;
            prize = self->prize_fixed; weapon = scope->type;
        }
    } catch (...) { self->Unavailable(RewardUse::Chest, "Native chest pair validation failed.", scope ? scope->epoch : 0); }
    using Function = void* (*)(void*, int32_t, int32_t, void*, bool, const void*);
    void* result = reinterpret_cast<Function>(self->routes[Pair].original)(factory, prize, weapon, player, flag, method);
    try {
        if (forced_pair == scope && scope && self->Current(scope->owner, scope->epoch)) {
            Root pair(self->A(), result);
            if (!self->A().IsA(pair.Get(), self->pair_class)
                || self->template Field<int32_t>(pair.Get(), self->pair_type) != self->prize_fixed
                || self->template Field<int32_t>(pair.Get(), self->pair_weapon) != scope->type)
                Fail("The native chest did not create the selected fixed pair.");
            scope->applied = true;
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Chest, exception.what(), scope ? scope->epoch : 0); }
    catch (...) { self->Unavailable(RewardUse::Chest, "The native fixed pair could not be verified.", scope ? scope->epoch : 0); }
    return result;
}

template <class Host, class Api>
int32_t RewardBackend<Host, Api>::PullHook(void* factory, int32_t type, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Pull].original) return type;
    auto* scope = forced_pair;
    try {
        if (scope && scope->self == self && type == scope->type && self->Current(scope->owner, scope->epoch)
            && self->h.runtime.RewardTarget(RewardUse::Chest, scope->owner, GetTickCount64()) == scope->id) return scope->type;
    } catch (...) { self->Unavailable(RewardUse::Chest, "The scoped native reward lookup failed.", scope ? scope->epoch : 0); }
    using Function = int32_t (*)(void*, int32_t, const void*);
    return reinterpret_cast<Function>(self->routes[Pull].original)(factory, type, method);
}

template <class Host, class Api>
void RewardBackend<Host, Api>::ClaimHook(void* treasure, void* player, bool flag, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Claim].original) return;
    const bool restore_level = treasure && self->h.unity_thread.load() == GetCurrentThreadId()
        && self->promoted_chest.Get() == treasure;
    Finally restore_five([&] {
        if (restore_level && self->promoted_chest.Get() == treasure) self->RestoreChestLevel();
    });
    std::shared_ptr<PendingChest> pending;
    Root before; int32_t level = -1;
    try {
        self->SyncEpoch();
        auto found = self->chests.find(reinterpret_cast<uintptr_t>(treasure));
        if (found != self->chests.end() && !found->second->claiming
            && reinterpret_cast<uintptr_t>(player) == found->second->owner && self->Current(found->second->owner, found->second->epoch)
            && self->template Field<void*>(treasure, self->treasure_prizes) == found->second->prizes.Get()) {
            pending = found->second; before = Root(self->A(), self->Equipment(pending->type));
            if (before.Get()) level = self->template Scalar<int32_t>(self->equipment_level, before.Get());
            pending->claiming = true;
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Chest, exception.what(), pending ? pending->epoch : 0); pending.reset(); }
    catch (...) { self->Unavailable(RewardUse::Chest, "The native chest claim could not be observed.", pending ? pending->epoch : 0); pending.reset(); }
    Finally cleanup([&] {
        if (!pending) return;
        pending->claiming = false;
        auto found = self->chests.find(reinterpret_cast<uintptr_t>(treasure));
        if (found != self->chests.end() && found->second == pending) self->chests.erase(found);
    });
    Unwind unwind{*self, RewardUse::Chest, pending ? pending->epoch : 0};
    using Function = void (*)(void*, void*, bool, const void*);
    reinterpret_cast<Function>(self->routes[Claim].original)(treasure, player, flag, method);
    try {
        if (pending && self->Current(pending->owner, pending->epoch)) {
            Root after(self->A(), self->Equipment(pending->type));
            const bool granted = after.Get() && (!before.Get() || after.Get() != before.Get()
                || self->template Scalar<int32_t>(self->equipment_level, after.Get()) > level);
            if (!granted) Fail("The native chest returned without acquiring or upgrading the selected item.");
            self->h.runtime.RecordReward(RewardUse::Chest, pending->owner, pending->epoch, pending->id, pending->name,
                true, "Chest reward acquired: " + pending->name, GetTickCount64());
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Chest, exception.what(), pending ? pending->epoch : 0); }
    catch (...) { self->Unavailable(RewardUse::Chest, "The native chest result could not be verified.", pending ? pending->epoch : 0); }
}

template <class Host, class Api>
void RewardBackend<Host, Api>::PopulateHook(void* page, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Populate].original) return;
    Boundary<Population> boundary(population);
    Population scope; scope.self = self;
    try {
        self->SyncEpoch(); self->offers.erase(reinterpret_cast<uintptr_t>(page));
        uintptr_t owner = 0; void* manager = nullptr;
        if (self->upgrade_ready && self->Context(owner, manager) && self->A().IsA(page, self->page_class)
            && reinterpret_cast<uintptr_t>(self->Object(self->page_character, page)) == owner) {
            const auto target = self->h.runtime.RewardTarget(RewardUse::Upgrade, owner, GetTickCount64());
            if (!target.empty()) {
                self->Validate(target, true, true);
                scope.page = Root(self->A(), page); scope.factory = Root(self->A(), self->template Field<void*>(page, self->page_factory));
                scope.owner = owner; scope.epoch = self->h.runtime.RewardEpoch(); scope.id = target; population = &scope;
            }
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The upgrade page could not be verified.", scope.epoch); }
    Unwind unwind{*self, RewardUse::Upgrade, scope.epoch};
    using Function = void (*)(void*, const void*);
    reinterpret_cast<Function>(self->routes[Populate].original)(page, method);
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::AvailabilityHook(void* factory, void* player, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Availability].original) return false;
    using Function = bool (*)(void*, void*, const void*);
    const bool result = reinterpret_cast<Function>(self->routes[Availability].original)(factory, player, method);
    auto* scope = population;
    try {
        if (scope && scope->self == self && !scope->observed && scope->factory.Get() == factory
            && reinterpret_cast<uintptr_t>(player) == scope->owner && self->Current(scope->owner, scope->epoch)
            && self->h.runtime.RewardTarget(RewardUse::Upgrade, scope->owner, GetTickCount64()) == scope->id) {
            scope->observed = true;
            if (!result) self->Unavailable(RewardUse::Upgrade, "This is a gold, food or limit-break menu; normal native choices were retained.", scope->epoch);
        }
    } catch (...) { self->Unavailable(RewardUse::Upgrade, "The native upgrade phase could not be verified.", scope ? scope->epoch : 0); }
    return result;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::BeginMenu(void* page, bool reroll, Menu& scope) {
    SyncEpoch(); offers.erase(reinterpret_cast<uintptr_t>(page));
    uintptr_t owner = 0; void* manager = nullptr;
    if (!upgrade_ready || !Context(owner, manager) || !A().IsA(page, page_class)
        || reinterpret_cast<uintptr_t>(Object(page_character, page)) != owner) return;
    const auto target = h.runtime.RewardTarget(RewardUse::Upgrade, owner, GetTickCount64());
    if (target.empty()) return;
    auto& selected = Validate(target, true, true);
    scope.self = this; scope.page = Root(A(), page); scope.owner = owner; scope.epoch = h.runtime.RewardEpoch();
    scope.id = selected.item.id; scope.name = selected.item.name; scope.type = selected.type; scope.reroll = reroll;
    menu = &scope;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::Force(Menu& scope) {
    scope.attempted = true;
    if (!Current(scope.owner, scope.epoch) || h.runtime.RewardTarget(RewardUse::Upgrade, scope.owner, GetTickCount64()) != scope.id)
        Fail("Upgrade forcing was disabled before candidate enumeration.");
    Validate(scope.id, true, true);
    void* choices = Field<void*>(scope.page.Get(), page_choices);
    const int32_t count = ListCount(choices, kTypes, 32);
    if (!count) Fail("The native menu has no ordinary replaceable candidate.");
    void* data = Field<void*>(scope.page.Get(), page_data);
    void* source = Field<void*>(scope.page.Get(), page_source);
    Root levels(A(), DictObject(source, kConverted, kDataList, scope.type));
    if (!ListCount(levels.Get(), kDataList, 1000)) Fail("The page has no selected weapon data.");
    Root header(A(), ListObject(levels.Get(), kDataList, kWeaponData, 0));
    if (!A().IsA(header.Get(), header_class) || !A().IsA(data, data_class)) Fail("The page weapon data is unavailable.");
    ValidateEvolution(header.Get(), scope.type, data);
    if (Field<bool>(header.Get(), header_evolution)) {
        void* bases = Field<void*>(header.Get(), header_bases);
        if (bases) ListCount(bases, kTypes, 64);
    }
    if (scope.reroll) {
        void* items = Object(page_items, scope.page.Get());
        if (items) for (int32_t index = 0, size = ListCount(items, kItems, 32); index < size; ++index) {
            void* item = ListObject(items, kItems, "VampireSurvivors.LevelUpItemUI", index);
            if (item && Field<bool>(item, item_limit)) Fail("Limit-break rerolls retain the native choice rules.");
        }
    }
    int32_t existing = -1;
    for (int32_t index = 0; index < count; ++index) if (ListType(choices, index) == scope.type) {
        if (existing >= 0) Fail("The native menu already contains duplicate target candidates.");
        existing = index;
    }
    const int32_t first = ListType(choices, 0);
    if (existing > 0) SetType(choices, existing, first);
    try { if (existing != 0) SetType(choices, 0, scope.type); }
    catch (...) { if (existing > 0) try { SetType(choices, existing, scope.type); } catch (...) {} throw; }
    scope.choices = Root(A(), choices); scope.data = std::move(header); scope.applied = true;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::Finish(Menu& scope) {
    if (!scope.self || scope.failed) return;
    if (!Current(scope.owner, scope.epoch)) return;
    void* choices = Field<void*>(scope.page.Get(), page_choices);
    if (!scope.applied || scope.rendered != 1 || choices != scope.choices.Get() || !ContainsType(choices, scope.type))
        Fail("The native menu did not bind exactly one selected candidate.");
    unsigned matches = 0;
    void* items = Object(page_items, scope.page.Get());
    for (int32_t index = 0, count = ListCount(items, kItems, 32); index < count; ++index) {
        void* item = ListObject(items, kItems, "VampireSurvivors.LevelUpItemUI", index);
        if (item && !Field<bool>(item, item_limit) && Scalar<int32_t>(item_weapon, item) == scope.type) ++matches;
    }
    if (matches != 1 || !scope.button.Get()) Fail("The actual native upgrade button is not unique.");
    if (offers.size() >= 32) offers.clear();
    auto pending = std::make_shared<PendingOffer>();
    pending->page = scope.page; pending->button = scope.button; pending->owner = scope.owner; pending->epoch = scope.epoch;
    pending->id = scope.id; pending->name = scope.name; pending->type = scope.type;
    offers[reinterpret_cast<uintptr_t>(scope.page.Get())] = std::move(pending);
    h.runtime.RecordReward(RewardUse::Upgrade, scope.owner, scope.epoch, scope.id, scope.name, false,
        "Upgrade option offered: " + scope.name, GetTickCount64());
}

template <class Host, class Api>
void RewardBackend<Host, Api>::InitialHook(void* page, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Initial].original) return;
    Boundary<Menu> boundary(menu); Menu scope;
    try { self->BeginMenu(page, false, scope); }
    catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The initial upgrade menu could not be validated.", scope.epoch); }
    Unwind unwind{*self, RewardUse::Upgrade, scope.epoch};
    using Function = void (*)(void*, const void*);
    reinterpret_cast<Function>(self->routes[Initial].original)(page, method);
    try { self->Finish(scope); }
    catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The initial native upgrade offer could not be verified.", scope.epoch); }
}

template <class Host, class Api>
void* RewardBackend<Host, Api>::FriendshipHook(void* factory, bool flag, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Friendship].original) return nullptr;
    using Function = void* (*)(void*, bool, const void*);
    void* result = reinterpret_cast<Function>(self->routes[Friendship].original)(factory, flag, method);
    auto* scope = menu;
    try {
        if (scope && scope->self == self && !scope->reroll && !scope->attempted && flag
            && self->template Field<void*>(scope->page.Get(), self->page_factory) == factory) self->Force(*scope);
    } catch (const std::exception& exception) { if (scope) scope->failed = true; self->Unavailable(RewardUse::Upgrade, exception.what(), scope ? scope->epoch : 0); }
    catch (...) { if (scope) scope->failed = true; self->Unavailable(RewardUse::Upgrade, "The native initial candidate could not be replaced.", scope ? scope->epoch : 0); }
    return result;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::RerollHook(void* page, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Reroll].original) return;
    Boundary<Menu> boundary(menu); Menu scope;
    try { self->BeginMenu(page, true, scope); if (scope.self) self->Force(scope); }
    catch (const std::exception& exception) { scope.failed = true; self->Unavailable(RewardUse::Upgrade, exception.what(), scope.epoch); }
    catch (...) { scope.failed = true; self->Unavailable(RewardUse::Upgrade, "The native reroll candidate could not be replaced.", scope.epoch); }
    Unwind unwind{*self, RewardUse::Upgrade, scope.epoch};
    using Function = void (*)(void*, const void*);
    reinterpret_cast<Function>(self->routes[Reroll].original)(page, method);
    try { self->Finish(scope); }
    catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), scope.epoch); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The native reroll offer could not be verified.", scope.epoch); }
}

template <class Host, class Api>
void RewardBackend<Host, Api>::SpawnHook(void* page, void* data, int32_t type, int32_t index, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Spawn].original) return;
    using Function = void (*)(void*, void*, int32_t, int32_t, const void*);
    reinterpret_cast<Function>(self->routes[Spawn].original)(page, data, type, index, method);
    auto* scope = menu;
    try {
        if (scope && scope->self == self && !scope->failed && scope->applied && scope->page.Get() == page && type == scope->type
            && self->Current(scope->owner, scope->epoch)) {
            if (scope->data.Get() != data) Fail("The displayed upgrade data differs from the selected target.");
            void* items = self->Object(self->page_items, page);
            const int32_t count = self->ListCount(items, kItems, 32);
            if (!count) Fail("The native upgrade button was not created.");
            Root button(self->A(), self->ListObject(items, kItems, "VampireSurvivors.LevelUpItemUI", count - 1));
            if (!self->A().IsA(button.Get(), self->item_class) || self->template Scalar<int32_t>(self->item_weapon, button.Get()) != type
                || self->template Scalar<int32_t>(self->item_index, button.Get()) != index
                || self->template Field<void*>(button.Get(), self->item_data) != data || self->template Field<bool>(button.Get(), self->item_limit))
                Fail("The native upgrade button selection binding could not be verified.");
            scope->button = std::move(button); ++scope->rendered;
        }
    } catch (const std::exception& exception) { if (scope) scope->failed = true; self->Unavailable(RewardUse::Upgrade, exception.what(), scope ? scope->epoch : 0); }
    catch (...) { if (scope) scope->failed = true; self->Unavailable(RewardUse::Upgrade, "The rendered native upgrade button could not be verified.", scope ? scope->epoch : 0); }
}

template <class Host, class Api>
void RewardBackend<Host, Api>::SelectHook(void* page, int32_t type, void* item, const void* method) {
    auto* self = active.load(std::memory_order_acquire);
    if (!self || !self->routes[Select].original) return;
    std::shared_ptr<PendingOffer> pending; Root before; int32_t level = -1;
    try {
        self->SyncEpoch();
        auto found = self->offers.find(reinterpret_cast<uintptr_t>(page));
        if (found != self->offers.end() && !found->second->selecting && item && !self->template Field<bool>(page, self->page_banish)
            && !self->template Field<bool>(page, self->page_selected) && self->Current(found->second->owner, found->second->epoch)
            && reinterpret_cast<uintptr_t>(self->Object(self->page_character, page)) == found->second->owner) {
            if (type != found->second->type || item != found->second->button.Get()
                || self->template Scalar<int32_t>(self->item_weapon, item) != type) self->offers.erase(found);
            else {
                pending = found->second; before = Root(self->A(), self->Equipment(type));
                if (before.Get()) level = self->template Scalar<int32_t>(self->equipment_level, before.Get());
                pending->selecting = true;
            }
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), pending ? pending->epoch : 0); pending.reset(); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The actual upgrade selection could not be observed.", pending ? pending->epoch : 0); pending.reset(); }
    Finally cleanup([&] {
        if (!pending) return;
        pending->selecting = false;
        auto found = self->offers.find(reinterpret_cast<uintptr_t>(page));
        if (found != self->offers.end() && found->second == pending) self->offers.erase(found);
    });
    Unwind unwind{*self, RewardUse::Upgrade, pending ? pending->epoch : 0};
    using Function = void (*)(void*, int32_t, void*, const void*);
    reinterpret_cast<Function>(self->routes[Select].original)(page, type, item, method);
    try {
        if (pending && self->Current(pending->owner, pending->epoch)) {
            Root after(self->A(), self->Equipment(type));
            const bool granted = after.Get() && (!before.Get() || after.Get() != before.Get()
                || self->template Scalar<int32_t>(self->equipment_level, after.Get()) > level);
            if (!granted) Fail("The native selection returned without acquiring or upgrading the selected item; no extra item was granted.");
            self->h.runtime.RecordReward(RewardUse::Upgrade, pending->owner, pending->epoch, pending->id, pending->name,
                true, "Upgrade acquired: " + pending->name, GetTickCount64());
        }
    } catch (const std::exception& exception) { self->Unavailable(RewardUse::Upgrade, exception.what(), pending ? pending->epoch : 0); }
    catch (...) { self->Unavailable(RewardUse::Upgrade, "The actual native upgrade result could not be verified.", pending ? pending->epoch : 0); }
}

} // namespace vst::detail

#include "reward_catalog.inl"
#include "reward_icons.inl"
