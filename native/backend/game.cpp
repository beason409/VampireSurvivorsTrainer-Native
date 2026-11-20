#include "game.hpp"
#include "../common/chest_animation.hpp"
#include "../common/game_speed.hpp"
#include "gc_root.hpp"
#include "gold_plan.hpp"
#include "reference_field.hpp"
#include "temporary_resource.hpp"
#include "reward_backend.hpp"
#include "../common/runtime.hpp"
#include "../common/win_util.hpp"
#include "../vendor/minhook/include/MinHook.h"

#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vst {
namespace {

constexpr uint32_t kArmorToken = 100689690;
constexpr uint32_t kUpdateToken = 100695246;
constexpr uint32_t kDestroyToken = 100695105;
constexpr uint32_t kTakeDamageToken = 100689749;
constexpr uint32_t kAddXpToken = 100689674;
constexpr uint32_t kEnemyDamageToken = 100692720;
constexpr uint32_t kMovementToken = 100689702;
constexpr uint32_t kIntervalToken = 100688833;
constexpr uint32_t kResetFiringTimerToken = 100688856;

// Exact, fixed-build metadata routes. Armor and movement retain their shared
// hooks below; these entries cover the remaining ordinary-stat return paths.
struct StatRouteSpec {
    NativeStat stat;
    const char* name;
    uint32_t token;
    bool is_virtual;
    bool has_parameter = false;
};
constexpr std::array<StatRouteSpec, 17> kStatRoutes{{
    {NativeStat::MaxHealth, "MaxHp", 100689700, true},
    {NativeStat::MaxHealth, "DisplayMaxHp", 100689701, true},
    {NativeStat::Recovery, "PRegen", 100689699, true},
    {NativeStat::Might, "PPower", 100689709, true},
    {NativeStat::Might, "PPowerFinal", 100689707, false},
    {NativeStat::Might, "PPowerWithoutSilentMight", 100689708, false},
    {NativeStat::ProjectileSpeed, "PSpeed", 100689695, true},
    {NativeStat::Duration, "PDuration", 100689696, true},
    {NativeStat::Area, "PArea", 100689698, true},
    {NativeStat::Cooldown, "PCooldown", 100689704, true},
    {NativeStat::Amount, "PAmount", 100689705, true},
    {NativeStat::Luck, "PLuck", 100689693, true},
    {NativeStat::Growth, "PGrowth", 100689692, true},
    {NativeStat::Greed, "PGreed", 100689694, true},
    {NativeStat::Curse, "PCurse", 100689691, true},
    {NativeStat::Area, "PAreaFinal", 100689697, true, true},
    {NativeStat::Cooldown, "PCooldownFinal", 100689703, true, true}
}};
constexpr size_t StatIndex(NativeStat stat) noexcept { return static_cast<size_t>(stat); }

// Il2CppInterop 1.5.3, commit dbda1cb353b0f4253345dc45136d170b9e50a5a0,
// NativeMethodInfoStructHandler_29_2 (Unity >= 2023.2). The fixed game image
// independently confirms name +0x18 and flags +0x4c. See armor-route.md.
struct MethodPrefix {
    void* pointer;
    void* virtual_pointer;
    void* invoker;
    const char* name;
    void* klass;
    const void* return_type;
    const void* parameters;
    const void* runtime_data;
    const void* generic_data;
    uint32_t token;
    uint16_t flags;
    uint16_t implementation_flags;
    uint16_t slot;
    uint8_t parameter_count;
    uint8_t bitfields;
};
static_assert(sizeof(void*) == 8);
static_assert(offsetof(MethodPrefix, name) == 0x18);
static_assert(offsetof(MethodPrefix, flags) == 0x4c);

bool NameEquals(const char* text, std::string_view expected) {
    return text && IsReadableMemory(text, expected.size() + 1)
        && std::memcmp(text, expected.data(), expected.size()) == 0
        && text[expected.size()] == '\0';
}

struct Api {
    HMODULE image = nullptr;
    void* (*domain_get)() = nullptr;
    const void** (*domain_get_assemblies)(void*, size_t*) = nullptr;
    const void* (*assembly_get_image)(const void*) = nullptr;
    const char* (*image_get_name)(const void*) = nullptr;
    void* (*class_from_name)(const void*, const char*, const char*) = nullptr;
    void* (*class_get_parent)(void*) = nullptr;
    const void* (*class_get_type)(void*) = nullptr;
    void* (*type_get_object)(const void*) = nullptr;
    void* (*class_from_type)(const void*) = nullptr;
    void* (*class_get_element_class)(void*) = nullptr;
    const char* (*class_get_name)(void*) = nullptr;
    const char* (*class_get_namespace)(void*) = nullptr;
    const void* (*class_enum_basetype)(void*) = nullptr;
    const void* (*class_get_method_from_name)(void*, const char*, int) = nullptr;
    const void* (*class_get_methods)(void*, void**) = nullptr;
    const void* (*method_get_return_type)(const void*) = nullptr;
    uint32_t (*method_get_param_count)(const void*) = nullptr;
    const void* (*method_get_param)(const void*, uint32_t) = nullptr;
    const char* (*method_get_name)(const void*) = nullptr;
    uint32_t (*method_get_flags)(const void*, uint32_t*) = nullptr;
    uint32_t (*method_get_token)(const void*) = nullptr;
    char* (*type_get_name)(const void*) = nullptr;
    int (*type_get_type)(const void*) = nullptr;
    void (*free)(void*) = nullptr;
    void* (*thread_current)() = nullptr;
    void* (*thread_attach)(void*) = nullptr;
    void (*thread_detach)(void*) = nullptr;
    void* (*runtime_invoke)(const void*, void*, void**, void**) = nullptr;
    void* (*object_unbox)(void*) = nullptr;
    void* (*object_get_class)(void*) = nullptr;
    const void* (*object_get_virtual_method)(void*, const void*) = nullptr;
    const void* (*class_get_field_from_name)(void*, const char*) = nullptr;
    const void* (*class_get_fields)(void*, void**) = nullptr;
    const char* (*field_get_name)(const void*) = nullptr;
    void* (*string_new_len)(const char*, uint32_t) = nullptr;
    uint32_t (*array_length)(void*) = nullptr;
    uint32_t (*array_get_byte_length)(void*) = nullptr;
    uint32_t (*array_object_header_size)() = nullptr;
    const void* (*field_get_type)(const void*) = nullptr;
    uint32_t (*field_get_flags)(const void*) = nullptr;
    size_t (*field_get_offset)(const void*) = nullptr;
    void (*field_get_value)(void*, const void*, void*) = nullptr;
    void (*field_set_value)(void*, const void*, void*) = nullptr;
    void (*gc_wbarrier_set_field)(void*, void**, void*) = nullptr;
    void (*field_static_get_value)(const void*, void*) = nullptr;
    void* (*object_new)(void*) = nullptr;
    // This supported Unity/Interop build exports pointer-sized GC handles.
    // Using an older uint32 handle ABI truncates live handles on Windows x64.
    uintptr_t (*gchandle_new)(void*, bool) = nullptr;
    void* (*gchandle_get_target)(uintptr_t) = nullptr;
    void (*gchandle_free)(uintptr_t) = nullptr;
    int32_t (*string_length)(void*) = nullptr;
    const wchar_t* (*string_chars)(void*) = nullptr;

    bool Resolve(std::string& error) {
        image = GetModuleHandleW(L"GameAssembly.dll");
        if (!image) { error = "GameAssembly is not loaded."; return false; }
#define VST_API(member) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(image, "il2cpp_" #member)); \
        if (!member) { error = "Required IL2CPP export is absent: il2cpp_" #member; return false; }
        VST_API(domain_get); VST_API(domain_get_assemblies);
        VST_API(assembly_get_image); VST_API(image_get_name);
        VST_API(class_from_name); VST_API(class_get_parent);
        VST_API(class_from_type); VST_API(class_get_element_class);
        VST_API(class_get_name); VST_API(class_get_namespace); VST_API(class_enum_basetype);
        VST_API(class_get_method_from_name); VST_API(method_get_return_type);
        VST_API(method_get_param_count); VST_API(method_get_param);
        VST_API(method_get_name); VST_API(method_get_flags); VST_API(method_get_token);
        VST_API(type_get_name); VST_API(type_get_type); VST_API(free);
        VST_API(thread_current); VST_API(thread_attach); VST_API(thread_detach);
        VST_API(runtime_invoke); VST_API(object_unbox); VST_API(object_get_class);
        VST_API(object_get_virtual_method); VST_API(class_get_field_from_name);
        VST_API(field_get_type); VST_API(field_get_flags); VST_API(field_get_offset);
        VST_API(field_get_value); VST_API(field_static_get_value); VST_API(string_length); VST_API(string_chars);
#undef VST_API
        // Optional groups fail closed without weakening the original pilot.
#define VST_OPTIONAL(member) member = reinterpret_cast<decltype(member)>(GetProcAddress(image, "il2cpp_" #member))
        VST_OPTIONAL(class_get_methods); VST_OPTIONAL(field_set_value);
        VST_OPTIONAL(gc_wbarrier_set_field);
        VST_OPTIONAL(object_new); VST_OPTIONAL(gchandle_new);
        VST_OPTIONAL(gchandle_get_target); VST_OPTIONAL(gchandle_free);
        VST_OPTIONAL(class_get_type); VST_OPTIONAL(type_get_object);
        VST_OPTIONAL(class_get_fields); VST_OPTIONAL(field_get_name); VST_OPTIONAL(string_new_len);
        VST_OPTIONAL(array_length);
        VST_OPTIONAL(array_get_byte_length); VST_OPTIONAL(array_object_header_size);
#undef VST_OPTIONAL
        return true;
    }

    bool TypeEquals(const void* type, std::string_view expected) const {
        if (!type) return false;
        char* text = type_get_name(type);
        bool same = NameEquals(text, expected);
        if (text) free(text);
        return same;
    }

    const void* FindImage(const char* name) const {
        size_t count = 0;
        const void** assemblies = domain_get_assemblies(domain_get(), &count);
        if (!count || count > 4096 || !IsReadableMemory(assemblies, count * sizeof(void*))) return nullptr;
        for (size_t index = 0; index < count; ++index) {
            const void* candidate = assembly_get_image(assemblies[index]);
            if (candidate && NameEquals(image_get_name(candidate), name)) return candidate;
        }
        return nullptr;
    }

    const void* Method(void* klass, const char* name, const char* result,
        std::initializer_list<const char*> args, bool is_static, uint32_t token = 0,
        bool allow_inflated = false, bool allow_type_variable_result = false) const {
        if (!klass) return nullptr;
        const void* method = class_get_method_from_name(klass, name, static_cast<int>(args.size()));
        // RemoveCoins has two same-arity overloads. Resolve the exact game
        // token before checking its complete signature, never by arity alone.
        if (token && class_get_methods && (!method || method_get_token(method) != token)) {
            void* iterator = nullptr;
            for (unsigned count = 0; count < 65536; ++count) {
                const void* candidate = class_get_methods(klass, &iterator);
                if (!candidate) break;
                if (method_get_token(candidate) == token) { method = candidate; break; }
            }
        }
        if (!method || !IsReadableMemory(method, sizeof(MethodPrefix))
            || !NameEquals(method_get_name(method), name)
            || method_get_param_count(method) != args.size()) return nullptr;
        const void* result_type = method_get_return_type(method);
        bool type_variable = allow_type_variable_result && type_get_type(result_type) == 0x13;
        if (result && !TypeEquals(result_type, result) && !type_variable) return nullptr;
        uint32_t flags = method_get_flags(method, nullptr);
        if (((flags & 0x10) != 0) != is_static || (token && method_get_token(method) != token)) return nullptr;
        uint32_t index = 0;
        for (const char* type : args) if (!TypeEquals(method_get_param(method, index++), type)) return nullptr;
        MethodPrefix prefix{};
        std::memcpy(&prefix, method, sizeof(prefix));
        if (prefix.name != method_get_name(method) || prefix.return_type != method_get_return_type(method)
            || prefix.klass != klass || !IsExecutableAddressInModule(prefix.invoker, image)
            || prefix.flags != flags || prefix.token != method_get_token(method)
            || prefix.parameter_count != args.size() || (prefix.bitfields & 1) != 0
            || (!allow_inflated && (prefix.bitfields & 2) != 0)) return nullptr;
        if (type_variable && (prefix.bitfields & 2) == 0) return nullptr;
        return method;
    }

    const void* Field(void* klass, const char* name, const char* type) const {
        const void* field = class_get_field_from_name(klass, name);
        if (!field || (field_get_flags(field) & 0x10) || !TypeEquals(field_get_type(field), type)) return nullptr;
        // Offsets are obtained from the running metadata, never baked in.
        size_t offset = field_get_offset(field);
        return offset >= 2 * sizeof(void*) && offset < 1024 * 1024 ? field : nullptr;
    }

    std::string CanonicalType(const void* type) const {
        char* raw = type ? type_get_name(type) : nullptr;
        std::string result;
        if (raw) {
            for (size_t index = 0; raw[index] && index < 4096; ++index) {
                if (raw[index] == '`') { while (raw[index + 1] >= '0' && raw[index + 1] <= '9') ++index; }
                else if (raw[index] != ' ') result += raw[index];
            }
            free(raw);
        }
        return result;
    }

    // Only closed, exact collection owners may accept VAR metadata in the
    // known generic definition. Existing Method validation stays unchanged.
    const void* CollectionMethod(void* klass, std::string_view closed_type, const char* name,
        const char* result, std::initializer_list<const char*> args, uint32_t token,
        uint32_t variable_args = 0, bool variable_result = false) const {
        if (!klass || !class_get_type || CanonicalType(class_get_type(klass)) != closed_type) return nullptr;
        const void* method = class_get_method_from_name(klass, name, static_cast<int>(args.size()));
        if (!method || !IsReadableMemory(method, sizeof(MethodPrefix)) || method_get_token(method) != token
            || !NameEquals(method_get_name(method), name) || method_get_param_count(method) != args.size()) return nullptr;
        bool used_variable = false;
        auto matches = [&](const void* type, const char* expected, bool allow_variable) {
            if (TypeEquals(type, expected)) return true;
            const bool match = allow_variable && type_get_type(type) == 0x13;
            used_variable = used_variable || match;
            return match;
        };
        if (!matches(method_get_return_type(method), result, variable_result)) return nullptr;
        uint32_t index = 0;
        for (const char* expected : args) {
            if (!matches(method_get_param(method, index), expected, (variable_args & (1u << index)) != 0)) return nullptr;
            ++index;
        }
        const uint32_t flags = method_get_flags(method, nullptr);
        MethodPrefix prefix{}; std::memcpy(&prefix, method, sizeof(prefix));
        if ((flags & 0x10) || prefix.klass != klass || prefix.name != method_get_name(method)
            || prefix.flags != flags || prefix.token != token || prefix.return_type != method_get_return_type(method)
            || prefix.parameter_count != args.size() || (prefix.bitfields & 1)
            || (used_variable && !(prefix.bitfields & 2)) || !IsExecutableAddressInModule(prefix.invoker, image)) return nullptr;
        return method;
    }

    bool IsA(void* object, void* expected) const {
        if (!object || !IsReadableMemory(object, 2 * sizeof(void*))) return false;
        void* klass = object_get_class(object);
        for (unsigned depth = 0; klass && depth < 64; ++depth, klass = class_get_parent(klass))
            if (klass == expected) return true;
        return false;
    }

    bool Invoke(const void* method, void* owner, void** args, void*& result, bool virtual_call = false) const {
        const void* target = virtual_call ? object_get_virtual_method(owner, method) : method;
        if (!target) return false;
        void* exception = nullptr;
        result = runtime_invoke(target, owner, args, &exception);
        return exception == nullptr;
    }

    bool Boolean(const void* method, void* owner, bool& value) const {
        void* box = nullptr;
        if (!Invoke(method, owner, nullptr, box) || !box) return false;
        void* data = object_unbox(box);
        if (!IsReadableMemory(data, sizeof(uint8_t))) return false;
        value = *static_cast<const uint8_t*>(data) != 0;
        return true;
    }
};

// Detached normal and temporary number objects must remain visible to the
// managed GC. Every access resolves the handle again; no unrooted object is
// retained across a native callback or a collection.
static_assert(std::is_same_v<decltype(Api::gchandle_new), uintptr_t (*)(void*, bool)>);
static_assert(std::is_same_v<decltype(Api::gchandle_get_target), void* (*)(uintptr_t)>);
static_assert(std::is_same_v<decltype(Api::gchandle_free), void (*)(uintptr_t)>);
static_assert(std::is_same_v<decltype(Api::gc_wbarrier_set_field), void (*)(void*, void**, void*)>);
using StrongRoot = StrongRootT<Api>;

// No game object is accessed by Start's worker. Attaching here is solely for
// resolving immutable metadata; every instance read below runs in Unity Update.
struct MetadataThread {
    const Api& api;
    void* attached = nullptr;
    explicit MetadataThread(const Api& value) : api(value) {
        if (!api.thread_current()) attached = api.thread_attach(api.domain_get());
    }
    ~MetadataThread() { if (attached) api.thread_detach(attached); }
    bool Valid() const { return api.thread_current() != nullptr; }
};

bool EntryMatchesImage(HMODULE module, void* entry, std::string& error) {
    if (!IsExecutableAddressInModule(entry, module) || !IsReadableMemory(entry, 32)) {
        error = "Native method entry is outside the verified executable image."; return false;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    if (!IsReadableMemory(base, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE
        || nt->FileHeader.NumberOfSections > 96) return false;
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    if (!IsReadableMemory(sections, nt->FileHeader.NumberOfSections * sizeof(*sections))) return false;
    uintptr_t rva = reinterpret_cast<uintptr_t>(entry) - reinterpret_cast<uintptr_t>(base);
    uint64_t offset = 0;
    bool found = false;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress
            && rva - section.VirtualAddress <= section.SizeOfRawData
            && section.SizeOfRawData - (rva - section.VirtualAddress) >= 32) {
            offset = section.PointerToRawData + rva - section.VirtualAddress;
            found = true; break;
        }
    }
    if (!found) { error = "Native method entry has no executable file bytes."; return false; }
    const auto path = ModulePath(module);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "Cannot verify native method file bytes."; return false; }
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    std::array<uint8_t, 32> bytes{};
    DWORD read = 0;
    bool equal = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
        && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
        && read == bytes.size() && std::memcmp(entry, bytes.data(), bytes.size()) == 0;
    CloseHandle(file);
    if (!equal) error = "Native method entry has already been patched or does not match the verified image.";
    return equal;
}

void* Entry(const Api& api, const void* method, std::string& error) {
    if (!method || !IsReadableMemory(method, sizeof(MethodPrefix))) return nullptr;
    MethodPrefix prefix{}; std::memcpy(&prefix, method, sizeof(prefix));
    if (!EntryMatchesImage(api.image, prefix.pointer, error)) return nullptr;
    return prefix.pointer;
}

} // namespace

struct GameBackend::Impl {
    using UpdateFunction = void (*)(void*, const void*);
    using ArmorFunction = float (*)(void*, const void*);
    using HealthFunction = void (*)(void*, float, const void*);
    using StatArgumentFunction = float (*)(void*, float, const void*);
    using ExperienceFunction = void (*)(void*, float, int32_t, const void*);
    using DamageFunction = void (*)(void*, float, int32_t, float, int32_t, bool, const void*);
    using DealDamageFunction = void (*)(void*, float, int32_t, const void*);
    using BooleanFunction = bool (*)(void*, const void*);
    using ObjectFunction = void* (*)(void*, const void*);
    using IntFunction = int32_t (*)(void*, const void*);
    using UpgradeFunction = void (*)(void*, void*, bool, const void*);
    using SetFunction = void (*)(void*, void*, const void*);
    using GoldFloatFunction = void (*)(void*, float, bool, const void*);
    using GoldIntFunction = void (*)(void*, int32_t, bool, const void*);
    using GoldConfigFunction = void (*)(void*, int32_t, bool, void*, const void*);

    Runtime& runtime;
    Api api;
    detail::RewardBackend<Impl, Api> rewards;
    std::atomic<bool> stopping{false};
    std::atomic<bool> hooks_live{false};
    std::atomic<DWORD> unity_thread{0};
    std::atomic<uint64_t> destructions{0};
    std::atomic<uintptr_t> local_player{0};
    std::array<std::atomic<uintptr_t>, 4> managers{};
    std::atomic<bool> manager_overflow{false}, scene_dirty{false};
    bool health_route_available = true;
    bool movement_route_available = true;
    uint64_t next_scene_read = 0;
    std::string scene;
    void* manager_class = nullptr;
    void* character_class = nullptr;
    void* options_class = nullptr;
    void* scene_class = nullptr;
    void* enemy_class = nullptr;
    void* equipment_class = nullptr;
    void* weapon_class = nullptr;
    void* equipment_list_class = nullptr;
    const void* player_method = nullptr;
    const void* multiplayer_method = nullptr;
    const void* dead_method = nullptr;
    const void* initialized_method = nullptr;
    const void* config_ready_method = nullptr;
    const void* paused_method = nullptr;
    const void* armor_method = nullptr;
    const void* health_method = nullptr;
    const void* movement_method = nullptr;
    const void* time_scale_getter = nullptr;
    const void* time_scale_setter = nullptr;
    GameSpeedOverride game_speed;
    const void* interval_method = nullptr;
    const void* reset_timer_method = nullptr;
    const void* options_field = nullptr;
    const void* game_over_field = nullptr;
    const void* started_online_field = nullptr;
    const void* active_scene_method = nullptr;
    const void* scene_name_method = nullptr;
    const void* scene_handle_field = nullptr;
    const void* player_hp_field = nullptr;
    const void* player_xp_field = nullptr;
    const void* player_level_field = nullptr;
    const void* enemy_hp_field = nullptr;
    const void* enemy_dead_field = nullptr;
    void* reaper_class = nullptr;
    const void* enemy_type_field = nullptr;
    const void* weapons_method = nullptr;
    const void* active_equipment_method = nullptr;
    const void* hidden_equipment_method = nullptr;
    const void* equipment_owner_field = nullptr;
    const void* equipment_type_field = nullptr;
    const void* list_count_method = nullptr;
    const void* list_item_method = nullptr;
    const void* enemy_bullet_method = nullptr;
    const void* set_hp_method = nullptr;
    const void* stage_method = nullptr;
    const void* refresh_curse_method = nullptr;
    void* stage_class = nullptr;
    bool max_health_maintenance_ready = false;
    bool curse_maintenance_ready = false;
    struct StatRoute {
        const void* method = nullptr;
        void* entry = nullptr;
        void* original = nullptr;
        bool created = false;
    };
    std::array<StatRoute, kStatRoutes.size()> stat_routes{};
    StatRoute reaper_damage;
    std::array<bool, kNativeStatCount> stat_route_collision{};
    std::array<bool, kNativeStatCount> stat_supported{};
    std::array<float, kNativeStatCount> stat_samples{};
    std::array<bool, kNativeStatCount> stat_sample_known{};
    uintptr_t stat_sample_player = 0;
    uint64_t next_stat_sample = 0;
    uintptr_t transient_stat_player = 0;
    bool max_health_synced = false, max_health_enabled = false;
    double max_health_target = 0;
    bool curse_synced = false, curse_enabled = false;
    double curse_target = 0;
    bool curse_refresh_pending = false;
    uint64_t next_curse_refresh = 0;
    bool charm_synced = false, charm_enabled = false;
    double charm_target = 0;
    enum ExtendedHook : size_t {
        CharmRead, RerollsRead, SkipsRead, BanishesRead, RevivalsRead,
        PreserveRead, DefangRead, StatsUpgrade, StatsSet, StatsReset,
        PlayerUpgrade, PlayerReset, GoldFloat, GoldInt, GoldConfig, GoldFlat, ExtendedHookCount
    };
    std::array<StatRoute, ExtendedHookCount> extra_routes{};
    std::array<bool, kNativeStatCount> extra_failed{};
    void* player_stats_class = nullptr;
    void* modifier_class = nullptr;
    void* magnet_class = nullptr;
    void* egg_float_class = nullptr;
    void* egg_double_class = nullptr;
    void* options_data_class = nullptr;
    const void* player_stats_method = nullptr;
    const void* magnet_method = nullptr;
    const void* refresh_magnet_method = nullptr;
    const void* egg_float_ctor = nullptr;
    const void* egg_double_ctor = nullptr;
    const void* egg_float_value = nullptr;
    const void* egg_double_value = nullptr;
    const void* main_config_method = nullptr;
    const void* config_method = nullptr;
    void* adventure_manager_class = nullptr;
    void* adventure_type_class = nullptr;
    const void* adventure_manager_field = nullptr;
    const void* adventure_options_field = nullptr;
    const void* current_adventure_field = nullptr;
    const void* in_adventure_method = nullptr;
    const void* adventure_config_method = nullptr;
    const void* manager_adventure_config_method = nullptr;
    const void* selected_adventure_method = nullptr;
    bool adventure_wallet_ready = false;
    const void* coins_method = nullptr;
    const void* set_coins_method = nullptr;
    const void* save_method = nullptr;
    const void* add_coins_method = nullptr;
    bool gold_writer_ready = false;
    std::atomic<bool> writing_gold{false};
    bool resource_api_ready = false, magnet_metadata_ready = false, wallet_ready = false;
    void* menu_class = nullptr;
    void* multiplayer_class = nullptr;
    void* unity_object_class = nullptr;
    const void* menu_options_method = nullptr;
    const void* menu_multiplayer_field = nullptr;
    const void* multiplayer_instance_method = nullptr;
    const void* menu_is_multiplayer_method = nullptr;
    const void* menu_is_online_method = nullptr;
    const void* find_menu_method = nullptr;
    const void* unity_alive_method = nullptr;
    StatRoute menu_tick_route;
    StatRoute menu_update_route;
    StatRoute chest_animation_update;
    void *chest_page_class = nullptr, *chest_data_class = nullptr, *chest_playback_class = nullptr;
    const void* chest_perform_skip = nullptr;
    const void *chest_current = nullptr, *chest_playback = nullptr, *chest_playing = nullptr, *chest_skip_point = nullptr;
    const void *chest_skipped = nullptr, *chest_finished = nullptr, *chest_opening_player = nullptr, *chest_level = nullptr, *chest_arcana = nullptr;
    bool chest_animation_ready = false;
    bool menu_metadata_ready = false, menu_observing = false;
    uint64_t next_menu_frame = 0;
    enum ResourceSlot : size_t { RerollSlot, SkipSlot, BanishSlot, RevivalSlot, UsedRevivalSlot, PreserveSlot, DefangSlot, CharmSlot, MagnetSlot, ResourceSlotCount };
    enum class ResourceKind : uint8_t { EggFloat, EggDouble, Float, Int };
    struct ResourceValue { StrongRoot object; double scalar = 0; };
    struct ResourceBinding {
        const void* field = nullptr;
        ResourceKind kind = ResourceKind::Float;
        TemporaryResource<ResourceValue> budget;
    };
    std::array<ResourceBinding, ResourceSlotCount> resources{};
    StrongRoot resource_owner, magnet_owner;
    uintptr_t resource_player = 0;
    uintptr_t live_resource_player = 0;
    uint64_t live_resource_generation = 0;
    std::string live_resource_scene;
    std::atomic<uintptr_t> pending_revival_player{0};
    unsigned resource_native_depth = 0, magnet_native_depth = 0;
    bool magnet_refresh_pending = false;
    uint64_t next_magnet_refresh = 0;
    uint64_t next_resource_sample = 0;
    std::array<float, kNativeStatCount> resource_original{}, resource_current{};
    std::array<bool, kNativeStatCount> resource_known{};
    std::atomic<uintptr_t> local_wallet{0};
    void* update_entry = nullptr;
    void* destroy_entry = nullptr;
    void* armor_entry = nullptr;
    void* health_entry = nullptr;
    void* experience_entry = nullptr;
    void* damage_entry = nullptr;
    void* movement_entry = nullptr;
    void* interval_entry = nullptr;
    void* reset_timer_entry = nullptr;
    UpdateFunction update_original = nullptr;
    UpdateFunction destroy_original = nullptr;
    ArmorFunction armor_original = nullptr;
    HealthFunction health_original = nullptr;
    ExperienceFunction experience_original = nullptr;
    DamageFunction damage_original = nullptr;
    ArmorFunction movement_original = nullptr;
    ArmorFunction interval_original = nullptr;
    BooleanFunction enemy_is_bullet = nullptr;
    bool update_created = false, destroy_created = false, armor_created = false;
    bool health_created = false, experience_created = false, damage_created = false;
    bool movement_created = false, interval_created = false;
    bool health_ready = false, experience_ready = false, damage_ready = false;
    bool movement_ready = false, cooldown_ready = false, equipment_metadata_ready = false;
    bool equipment_ready = false;
    bool equipment_published = false;
    bool cooldown_route_available = false, cooldown_routes_valid = false, cooldown_weapon_seen = false;
    bool cooldown_timer_enabled = false;
    uintptr_t cooldown_timer_player = 0;
    std::array<int32_t, 9> basic_weapon_types{};
    uint64_t next_equipment_read = 0;
    uintptr_t equipment_player = 0;
    std::array<int32_t, 256> owned_types{};
    size_t owned_count = 0;
    static inline std::atomic<Impl*> active{nullptr};
    static thread_local bool sampling;
    enum class StatReadMode : uint8_t { Natural, Original, Effective };
    static thread_local StatReadMode stat_read_mode;
    struct StatSamplingScope {
        bool prior_sampling;
        StatReadMode prior_mode;
        explicit StatSamplingScope(StatReadMode mode) noexcept
            : prior_sampling(sampling), prior_mode(stat_read_mode) { sampling = true; stat_read_mode = mode; }
        ~StatSamplingScope() { sampling = prior_sampling; stat_read_mode = prior_mode; }
    };

    explicit Impl(Runtime& value) noexcept : runtime(value), rewards(*this) {}

    void* RewardEntry(const void* method, bool is_virtual, std::string& error) const {
        return is_virtual ? VirtualEntry(method, error) : Entry(api, method, error);
    }
    bool RewardEntryCollision(void* entry) const {
        if (!entry) return true;
        for (void* existing : {update_entry, destroy_entry, armor_entry, health_entry, experience_entry,
            damage_entry, movement_entry, interval_entry, menu_tick_route.entry, menu_update_route.entry, chest_animation_update.entry,
            reaper_damage.entry})
            if (existing == entry) return true;
        for (const auto& route : stat_routes) if (route.entry == entry) return true;
        for (const auto& route : extra_routes) if (route.entry == entry) return true;
        return false;
    }

    bool Resolve(const std::wstring& expected_image, bool& waiting_for_runtime, std::string& error) {
        if (!api.Resolve(error)) return false;
        if (!EqualPath(ModulePath(api.image), expected_image)) {
            error = "The loaded game assembly is outside the verified game directory.";
            return false;
        }
        if (!api.domain_get()) {
            waiting_for_runtime = true;
            error = "The IL2CPP domain is not ready.";
            return false;
        }
        MetadataThread thread(api);
        if (!thread.Valid()) { error = "Cannot attach the metadata lookup thread."; return false; }
        const void* image = api.FindImage("VampireSurvivors.Runtime.dll");
        const void* unity = api.FindImage("UnityEngine.CoreModule.dll");
        if (!image || !unity) {
            waiting_for_runtime = true;
            error = "Required game metadata images are not loaded.";
            return false;
        }
        manager_class = api.class_from_name(image, "VampireSurvivors.Framework", "GameManager");
        character_class = api.class_from_name(image, "VampireSurvivors.Objects.Characters", "CharacterController");
        options_class = api.class_from_name(image, "VampireSurvivors.Objects", "PlayerOptions");
        scene_class = api.class_from_name(unity, "UnityEngine.SceneManagement", "Scene");
        void* scene_manager = api.class_from_name(unity, "UnityEngine.SceneManagement", "SceneManager");
        if (!manager_class || !character_class || !options_class || !scene_class || !scene_manager) {
            error = "Required game classes are unavailable."; return false;
        }
        const void* update = api.Method(manager_class, "Update", "System.Void", {}, false, kUpdateToken);
        const void* destroy = api.Method(manager_class, "OnDestroy", "System.Void", {}, false, kDestroyToken);
        armor_method = api.Method(character_class, "PArmor", "System.Single", {}, false, kArmorToken);
        player_method = api.Method(manager_class, "get_Player", "VampireSurvivors.Objects.Characters.CharacterController", {}, false, 100695075);
        multiplayer_method = api.Method(manager_class, "get_IsMultiplayer", "System.Boolean", {}, false, 100695070);
        paused_method = api.Method(manager_class, "get_IsPaused", "System.Boolean", {}, false, 100694986);
        dead_method = api.Method(character_class, "get_IsDead", "System.Boolean", {}, false, 100689456);
        initialized_method = api.Method(options_class, "get_IsInitialized", "System.Boolean", {}, false, 100672343);
        config_ready_method = api.Method(options_class, "get_IsConfigReady", "System.Boolean", {}, false, 100672333);
        active_scene_method = api.Method(scene_manager, "GetActiveScene", "UnityEngine.SceneManagement.Scene", {}, true, 100669541);
        scene_name_method = api.Method(scene_class, "GetNameInternal", "System.String", {"System.Int32"}, true, 100669502);
        options_field = api.Field(manager_class, "_playerOptions", "VampireSurvivors.Objects.PlayerOptions");
        game_over_field = api.Field(manager_class, "_inGameOverState", "System.Boolean");
        started_online_field = api.Field(manager_class, "<StartedAsOnlineMultiplayerRun>k__BackingField", "System.Boolean");
        // A value-type field's metadata offset is relative to the unboxed data;
        // field_get_value handles the boxed Scene returned by runtime_invoke.
        scene_handle_field = api.class_get_field_from_name(scene_class, "m_Handle");
        if (!scene_handle_field || (api.field_get_flags(scene_handle_field) & 0x10)
            || api.field_get_offset(scene_handle_field) > 128
            || !api.TypeEquals(api.field_get_type(scene_handle_field), "System.Int32")) scene_handle_field = nullptr;
        if (!update || !destroy || !armor_method || !player_method || !multiplayer_method || !paused_method
            || !dead_method || !initialized_method || !config_ready_method || !active_scene_method
            || !scene_name_method || !options_field || !game_over_field || !started_online_field || !scene_handle_field) {
            error = "Game method, field, token, or signature validation failed."; return false;
        }
        uint32_t armor_flags = api.method_get_flags(armor_method, nullptr);
        if ((armor_flags & 0x40) == 0 || (api.method_get_flags(update, nullptr) & 7) != 1) {
            error = "Game native method attributes differ from the supported build."; return false;
        }
        MethodPrefix armor_prefix{};
        std::memcpy(&armor_prefix, armor_method, sizeof(armor_prefix));
        if (armor_prefix.virtual_pointer != armor_prefix.pointer) {
            error = "The armor virtual entry uses an unverified alternate native route."; return false;
        }
        update_entry = Entry(api, update, error);
        destroy_entry = Entry(api, destroy, error);
        armor_entry = Entry(api, armor_method, error);
        if (!update_entry || !destroy_entry || !armor_entry) return false;
        ResolveCombat(image);
        ResolveOrdinaryStats(image);
        ResolveExtended(image);
        ResolveMenu(image, unity);
        ResolveGameSpeed(unity);
        ResolveChestAnimation(image);
        ResolveReaperDamage(image);
        rewards.Resolve(image);
        return true;
    }

    void ResolveGameSpeed(const void* unity) {
        void* time = api.class_from_name(unity, "UnityEngine", "Time");
        const void* getter = api.Method(time, "get_timeScale", "System.Single", {}, true, 100668873);
        const void* setter = api.Method(time, "set_timeScale", "System.Void", {"System.Single"}, true, 100668874);
        std::string reason;
        void* get_entry = getter ? Entry(api, getter, reason) : nullptr;
        void* set_entry = setter ? Entry(api, setter, reason) : nullptr;
        if (!get_entry || !set_entry || get_entry == set_entry) return;
        time_scale_getter = getter;
        time_scale_setter = setter;
    }

    bool ReadGameSpeed(float& value) const {
        void* box = nullptr;
        if (!api.Invoke(time_scale_getter, nullptr, nullptr, box) || !box) return false;
        void* scalar = api.object_unbox(box);
        if (!IsReadableMemory(scalar, sizeof(value))) return false;
        std::memcpy(&value, scalar, sizeof(value));
        return std::isfinite(value) && value >= 0;
    }

    bool SyncGameSpeed(float requested, uintptr_t player = 0) {
        if (requested == 0 && !game_speed.OwnsValue()) return true;
        if (!time_scale_getter || !time_scale_setter
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
        float current = 0;
        if (!ReadGameSpeed(current)) return false;
        float next = game_speed.Next(current, requested);
        if (next == current) return true;
        void* args[]{&next};
        void* unused = nullptr;
        bool invoked = api.Invoke(time_scale_setter, nullptr, args, unused);
        float readback = 0;
        const bool read = ReadGameSpeed(readback);
        const bool confirmed = read && readback == next;
        // Retain ownership when an override could have reached Unity but
        // readback is temporarily unavailable. A later restoration checks
        // the live value again before writing anything.
        if (confirmed || (invoked && !read && requested != 0)) game_speed.ConfirmWrite(next);
        if (!invoked || !confirmed) return false;
        runtime.RecordGameSpeedWrite(player, current, readback, requested != 0, GetTickCount64());
        return true;
    }

    void ResolveChestAnimation(const void* image) {
        chest_page_class = api.class_from_name(image, "VampireSurvivors.UI", "OpenTreasurePage");
        chest_data_class = api.class_from_name(image, "VampireSurvivors.Data.Stage", "Treasure");
        chest_playback_class = api.class_from_name(image, "VampireSurvivors.UI", "TreasurePlaybackSettings");
        if (!chest_page_class || !chest_data_class || !chest_playback_class) return;
        chest_animation_update.method = api.Method(chest_page_class, "Update", "System.Void", {}, false, 100670116);
        chest_perform_skip = api.Method(chest_page_class, "PerformSkip", "System.Void", {}, false, 100670160);
        std::string reason;
        void* update = VirtualEntry(chest_animation_update.method, reason);
        void* skip = chest_perform_skip ? Entry(api, chest_perform_skip, reason) : nullptr;
        if (RewardEntryCollision(update) || RewardEntryCollision(skip) || update == skip) return;
        chest_current = api.Field(chest_page_class, "_currentTreasure", "VampireSurvivors.Data.Stage.Treasure");
        chest_playback = api.Field(chest_page_class, "_currentPlayback", "VampireSurvivors.UI.TreasurePlaybackSettings");
        chest_playing = api.Field(chest_page_class, "_isPlaying", "System.Boolean");
        chest_skip_point = api.Field(chest_page_class, "_animCanBeSkippedPastThisPoint", "System.Boolean");
        chest_skipped = api.Field(chest_page_class, "_isSkipped", "System.Boolean");
        chest_finished = api.Field(chest_page_class, "_animationFinished", "System.Boolean");
        chest_opening_player = api.Field(chest_data_class, "openingPlayer", "VampireSurvivors.Objects.Characters.CharacterController");
        chest_level = api.Field(chest_data_class, "<level>k__BackingField", "System.Int32");
        chest_arcana = api.Field(chest_data_class, "<hasArcana>k__BackingField", "System.Boolean");
        chest_animation_ready = chest_current && chest_playback && chest_playing && chest_skip_point && chest_skipped
            && chest_finished && chest_opening_player && chest_level && chest_arcana;
        if (chest_animation_ready) chest_animation_update.entry = update;
    }

    void TrySkipChestAnimation(void* page) {
        if (stopping.load(std::memory_order_acquire)
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return;
        const uintptr_t player = local_player.load(std::memory_order_acquire);
        if (!player || !runtime.FeatureEnabled(CombatFeature::SkipChestAnimation, player, GetTickCount64())
            || !api.IsA(page, chest_page_class)) return;

        ChestAnimationState state;
        api.field_get_value(page, chest_playing, &state.playing);
        api.field_get_value(page, chest_skip_point, &state.at_skip_point);
        api.field_get_value(page, chest_skipped, &state.skipped);
        api.field_get_value(page, chest_finished, &state.finished);
        if (!state.PlaybackReady()) return;

        void* treasure = nullptr;
        void* playback = nullptr;
        api.field_get_value(page, chest_current, &treasure);
        api.field_get_value(page, chest_playback, &playback);
        if (!api.IsA(treasure, chest_data_class) || !api.IsA(playback, chest_playback_class)
            || !VirtualRouteMatches(page, chest_animation_update.method, chest_animation_update.entry)) return;
        void* opening = nullptr;
        api.field_get_value(treasure, chest_opening_player, &opening);
        api.field_get_value(treasure, chest_level, &state.level);
        api.field_get_value(treasure, chest_arcana, &state.arcana);
        if (reinterpret_cast<uintptr_t>(opening) != player || !state.CanSkip()
            || !runtime.FeatureEnabled(CombatFeature::SkipChestAnimation, player, GetTickCount64())) return;

        void* unused = nullptr;
        if (api.Invoke(chest_perform_skip, page, nullptr, unused)) {
            api.field_get_value(page, chest_skipped, &state.skipped);
            if (state.skipped) {
                runtime.RecordChestAnimationSkip(player, GetTickCount64());
                return;
            }
        }
        runtime.SetFeatureSupported(CombatFeature::SkipChestAnimation, false,
            "The native chest animation skip could not be confirmed.");
    }

    static void ChestAnimationUpdateHook(void* page, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->chest_animation_update.original) return;
        reinterpret_cast<UpdateFunction>(self->chest_animation_update.original)(page, method);
        try { self->TrySkipChestAnimation(page); }
        catch (...) {
            try { self->runtime.SetFeatureSupported(CombatFeature::SkipChestAnimation, false); } catch (...) {}
        }
    }

    void ResolveOrdinaryStats(const void* image) {
        std::string reason;
        for (size_t index = 0; index < kStatRoutes.size(); ++index) {
            const auto& spec = kStatRoutes[index];
            auto& route = stat_routes[index];
            route.method = spec.has_parameter
                ? api.Method(character_class, spec.name, "System.Single", {"System.Single"}, false, spec.token)
                : api.Method(character_class, spec.name, "System.Single", {}, false, spec.token);
            route.entry = spec.is_virtual ? VirtualEntry(route.method, reason) : Entry(api, route.method, reason);
        }
        // A folded native entry cannot safely represent different stats or
        // getter signatures. Do not install an ambiguous return-value hook.
        for (size_t index = 0; index < kStatRoutes.size(); ++index) {
            const auto& route = stat_routes[index];
            if (!route.entry) continue;
            if (route.entry == armor_entry) {
                stat_route_collision[StatIndex(kStatRoutes[index].stat)] = true;
                stat_route_collision[StatIndex(NativeStat::Armor)] = true;
            }
            if (route.entry == movement_entry) {
                stat_route_collision[StatIndex(kStatRoutes[index].stat)] = true;
                stat_route_collision[StatIndex(NativeStat::MoveSpeed)] = true;
            }
            for (size_t previous = 0; previous < index; ++previous)
                if (route.entry == stat_routes[previous].entry
                    && (kStatRoutes[index].stat != kStatRoutes[previous].stat
                        || kStatRoutes[index].has_parameter != kStatRoutes[previous].has_parameter)) {
                    stat_route_collision[StatIndex(kStatRoutes[index].stat)] = true;
                    stat_route_collision[StatIndex(kStatRoutes[previous].stat)] = true;
                }
        }
        set_hp_method = api.Method(character_class, "set_CurrentHp", "System.Void", {"System.Single"}, false, 100689448);
        max_health_maintenance_ready = set_hp_method && player_hp_field && Entry(api, set_hp_method, reason);
        stage_class = api.class_from_name(image, "VampireSurvivors.Objects", "Stage");
        stage_method = api.Method(manager_class, "get_Stage", "VampireSurvivors.Objects.Stage", {}, false, 100694953);
        refresh_curse_method = api.Method(stage_class, "RecalculateCurseAndCharm", "System.Void", {}, false, 100672632);
        curse_maintenance_ready = stage_class && stage_method && refresh_curse_method
            && Entry(api, stage_method, reason) && Entry(api, refresh_curse_method, reason);
    }

    bool EnumInt32(const void* image, const char* space, const char* name) const {
        void* klass = api.class_from_name(image, space, name);
        return klass && api.TypeEquals(api.class_enum_basetype(klass), "System.Int32");
    }

    void ResolveExtended(const void* image) {
        constexpr const char* ef = "VampireSurvivors.Framework.NumberTypes.EggFloat";
        constexpr const char* ed = "VampireSurvivors.Framework.NumberTypes.EggDouble";
        constexpr const char* modifier = "VampireSurvivors.Objects.ModifierStats";
        constexpr const char* config = "VampireSurvivors.Data.PlayerOptionsData";
        player_stats_class = api.class_from_name(image, "VampireSurvivors.Objects", "PlayerModifierStats");
        modifier_class = api.class_from_name(image, "VampireSurvivors.Objects", "ModifierStats");
        magnet_class = api.class_from_name(image, "VampireSurvivors.Objects", "MagnetZone");
        egg_float_class = api.class_from_name(image, "VampireSurvivors.Framework.NumberTypes", "EggFloat");
        egg_double_class = api.class_from_name(image, "VampireSurvivors.Framework.NumberTypes", "EggDouble");
        options_data_class = api.class_from_name(image, "VampireSurvivors.Data", "PlayerOptionsData");
        player_stats_method = api.Method(character_class, "get_PlayerStats", "VampireSurvivors.Objects.PlayerModifierStats", {}, false, 100689485);
        magnet_method = api.Method(character_class, "get_Magnet", "VampireSurvivors.Objects.MagnetZone", {}, false, 100689563);
        refresh_magnet_method = api.Method(magnet_class, "RefreshSize", "System.Void", {}, false, 100672180);
        egg_float_ctor = api.Method(egg_float_class, ".ctor", "System.Void", {"System.Single", "System.Single"}, false, 100730442);
        egg_double_ctor = api.Method(egg_double_class, ".ctor", "System.Void", {"System.Double", "System.Double"}, false, 100730423);
        egg_float_value = api.Method(egg_float_class, "op_Implicit", "System.Single", {ef}, true, 100730447);
        egg_double_value = api.Method(egg_double_class, "op_Implicit", "System.Double", {ed}, true, 100730428);
        main_config_method = api.Method(options_class, "get_MainGameConfig", config, {}, false, 100672332);
        config_method = api.Method(options_class, "get_Config", config, {}, false, 100672335);
        coins_method = api.Method(options_data_class, "get_Coins", "System.Single", {}, false, 100731404);
        set_coins_method = api.Method(options_data_class, "set_Coins", "System.Void", {"System.Single"}, false, 100731405);
        save_method = api.Method(options_class, "Save", "System.Void", {"System.Boolean", "System.Boolean"}, false, 100672387);
        add_coins_method = api.Method(options_class, "AddCoinsFlat", "System.Void", {"System.Single"}, false, 100672430);
        auto resolve = [&](ExtendedHook index, void* klass, const char* name, const char* result,
            std::initializer_list<const char*> args, uint32_t token, bool is_virtual = false) {
            auto& route = extra_routes[index];
            route.method = api.Method(klass, name, result, args, false, token);
            std::string reason;
            route.entry = is_virtual ? VirtualEntry(route.method, reason) : Entry(api, route.method, reason);
        };
        resolve(CharmRead, player_stats_class, "get_Charm", "System.Int32", {}, 100672303);
        resolve(RerollsRead, player_stats_class, "get_ReRolls", ef, {}, 100672283);
        resolve(SkipsRead, player_stats_class, "get_Skips", ef, {}, 100672285);
        resolve(BanishesRead, player_stats_class, "get_Banish", ef, {}, 100672293);
        resolve(RevivalsRead, character_class, "PRevivals", ed, {}, 100689706, true);
        resolve(PreserveRead, player_stats_class, "get_Recycle", "System.Single", {}, 100672313);
        resolve(DefangRead, player_stats_class, "get_Defang", "System.Single", {}, 100672305);
        resolve(StatsUpgrade, player_stats_class, "Upgrade", "System.Void", {modifier, "System.Boolean"}, 100672317);
        resolve(StatsSet, player_stats_class, "Set", "System.Void", {modifier}, 100672316);
        resolve(StatsReset, player_stats_class, "ResetStats", "System.Void", {}, 100672315);
        resolve(PlayerUpgrade, character_class, "PlayerStatsUpgrade", "System.Void", {modifier, "System.Boolean"}, 100689813);
        resolve(PlayerReset, character_class, "ResetStats", "System.Void", {}, 100689812);
        resolve(GoldFloat, options_class, "RemoveCoins", "System.Void", {"System.Single", "System.Boolean"}, 100672436);
        resolve(GoldInt, options_class, "RemoveCoins", "System.Void", {"System.Int32", "System.Boolean"}, 100672435);
        resolve(GoldConfig, options_class, "RemoveCoins", "System.Void", {"System.Int32", "System.Boolean", config}, 100672434);
        resolve(GoldFlat, options_class, "RemoveCoinsFlat", "System.Single", {"System.Single"}, 100672432);
        const std::array<const char*, ResourceSlotCount> fields{{"<ReRolls>k__BackingField", "<Skips>k__BackingField",
            "<Banish>k__BackingField", "<Revivals>k__BackingField", "<UsedRevivals>k__BackingField",
            "<Recycle>k__BackingField", "<Defang>k__BackingField", "<Charm>k__BackingField", "Radius"}};
        const std::array<const char*, ResourceSlotCount> types{{ef, ef, ef, ed, "System.Int32", "System.Single", "System.Single", "System.Int32", ef}};
        const std::array<ResourceKind, ResourceSlotCount> kinds{{ResourceKind::EggFloat, ResourceKind::EggFloat,
            ResourceKind::EggFloat, ResourceKind::EggDouble, ResourceKind::Int, ResourceKind::Float, ResourceKind::Float, ResourceKind::Int, ResourceKind::EggFloat}};
        for (size_t index = 0; index < resources.size(); ++index) {
            resources[index].field = api.Field(index == MagnetSlot ? magnet_class : player_stats_class, fields[index], types[index]);
            resources[index].kind = kinds[index];
        }
        std::string reason;
        resource_api_ready = api.field_set_value && api.gc_wbarrier_set_field && api.object_new && api.gchandle_new && api.gchandle_get_target && api.gchandle_free
            && player_stats_class && player_stats_method && Entry(api, player_stats_method, reason)
            && egg_float_ctor && Entry(api, egg_float_ctor, reason) && egg_double_ctor && Entry(api, egg_double_ctor, reason)
            && egg_float_value && Entry(api, egg_float_value, reason) && egg_double_value && Entry(api, egg_double_value, reason);
        magnet_metadata_ready = resource_api_ready && magnet_method && Entry(api, magnet_method, reason)
            && refresh_magnet_method && Entry(api, refresh_magnet_method, reason) && resources[MagnetSlot].field;
        wallet_ready = main_config_method && config_method && options_data_class
            && Entry(api, main_config_method, reason) && Entry(api, config_method, reason);
        gold_writer_ready = wallet_ready && resource_api_ready && coins_method && Entry(api, coins_method, reason)
            && set_coins_method && Entry(api, set_coins_method, reason)
            && save_method && Entry(api, save_method, reason) && add_coins_method && Entry(api, add_coins_method, reason);
        constexpr const char* adventure_namespace = "VampireSurvivors.App.Scripts.Framework.Adventures";
        adventure_manager_class = api.class_from_name(image, adventure_namespace, "AdventureManager");
        adventure_type_class = api.class_from_name(image, "VampireSurvivors.Data", "AdventureType");
        adventure_manager_field = api.Field(options_class, "_adventureManager",
            "VampireSurvivors.App.Scripts.Framework.Adventures.AdventureManager");
        if (adventure_manager_class) {
            adventure_options_field = api.Field(adventure_manager_class, "_playerOptions", "VampireSurvivors.Objects.PlayerOptions");
            current_adventure_field = api.Field(adventure_manager_class, "CurrentAdventure", "VampireSurvivors.Data.AdventureType");
        }
        in_adventure_method = api.Method(adventure_manager_class, "get_IsInAdventureMode", "System.Boolean", {}, true, 100734849);
        if (!Entry(api, in_adventure_method, reason)) in_adventure_method = nullptr;
        adventure_config_method = api.Method(options_class, "get_CurrentAdventureSaveData", config, {}, false, 100672348);
        manager_adventure_config_method = api.Method(adventure_manager_class, "get_CurrentAdventureSaveData", config, {}, false, 100734855);
        selected_adventure_method = api.Method(options_data_class, "get_SelectedAdventureType",
            "System.Nullable<VampireSurvivors.Data.AdventureType>", {}, false, 100731674);
        adventure_wallet_ready = wallet_ready && in_adventure_method && adventure_manager_field
            && adventure_options_field && current_adventure_field && EnumInt32(image, "VampireSurvivors.Data", "AdventureType")
            && Entry(api, adventure_config_method, reason) && Entry(api, manager_adventure_config_method, reason)
            && Entry(api, selected_adventure_method, reason);
        // Never hook a folded entry shared by different signatures/meanings.
        for (size_t index = 0; index < extra_routes.size(); ++index) {
            void* entry = extra_routes[index].entry;
            if (!entry) continue;
            bool collision = entry == update_entry || entry == destroy_entry || entry == armor_entry
                || entry == health_entry || entry == experience_entry || entry == damage_entry
                || entry == movement_entry || entry == interval_entry;
            for (const auto& route : stat_routes) collision = collision || route.entry == entry;
            for (size_t other = 0; other < extra_routes.size(); ++other)
                if (other != index && extra_routes[other].entry == entry) collision = true;
            if (collision) {
                for (auto& route : extra_routes) if (route.entry == entry) route.entry = nullptr;
            }
        }
    }

    void ResolveMenu(const void* image, const void* unity) {
        menu_class = api.class_from_name(image, "VampireSurvivors.UI", "MainMenuPage");
        multiplayer_class = api.class_from_name(image, "VampireSurvivors.Framework", "MultiplayerManager");
        unity_object_class = api.class_from_name(unity, "UnityEngine", "Object");
        menu_options_method = api.Method(menu_class, "get_PlayerOptions", "VampireSurvivors.Objects.PlayerOptions", {}, false, 100669706);
        menu_multiplayer_field = api.Field(menu_class, "_multiplayerManager", "VampireSurvivors.Framework.MultiplayerManager");
        multiplayer_instance_method = api.Method(multiplayer_class, "get_Instance", "VampireSurvivors.Framework.MultiplayerManager", {}, true, 100695696);
        menu_is_multiplayer_method = api.Method(multiplayer_class, "get_IsMultiplayer", "System.Boolean", {}, false, 100695697);
        menu_is_online_method = api.Method(multiplayer_class, "get_IsOnlineMultiplayer", "System.Boolean", {}, false, 100695699);
        find_menu_method = api.Method(unity_object_class, "FindObjectOfType", "UnityEngine.Object", {"System.Type", "System.Boolean"}, true, 100668602);
        unity_alive_method = api.Method(unity_object_class, "op_Implicit", "System.Boolean", {"UnityEngine.Object"}, true, 100668535);
        menu_tick_route.method = api.Method(multiplayer_class, "Tick", "System.Void", {}, false, 100695709);
        menu_update_route.method = api.Method(menu_class, "Update", "System.Void", {}, false, 100669710);
        std::string reason;
        // The game registers this sealed ITickable route with its Unity
        // player-loop services, including while MainMenuPage is inactive.
        if (menu_tick_route.method && (api.method_get_flags(menu_tick_route.method, nullptr) & 0x60) == 0x60)
            menu_tick_route.entry = VirtualEntry(menu_tick_route.method, reason);
        menu_update_route.entry = VirtualEntry(menu_update_route.method, reason);
        menu_metadata_ready = resource_api_ready && api.class_get_type && api.type_get_object
            && menu_class && multiplayer_class && unity_object_class && menu_multiplayer_field
            && menu_options_method && Entry(api, menu_options_method, reason)
            && multiplayer_instance_method && Entry(api, multiplayer_instance_method, reason)
            && menu_is_multiplayer_method && Entry(api, menu_is_multiplayer_method, reason)
            && menu_is_online_method && Entry(api, menu_is_online_method, reason)
            && find_menu_method && Entry(api, find_menu_method, reason)
            && unity_alive_method && Entry(api, unity_alive_method, reason) && menu_tick_route.entry && menu_update_route.entry;
        if (!menu_metadata_ready) return;
        for (const void* entry : {menu_tick_route.entry, menu_update_route.entry}) {
            bool collision = entry == update_entry || entry == destroy_entry || entry == armor_entry
                || entry == health_entry || entry == experience_entry || entry == damage_entry
                || entry == movement_entry || entry == interval_entry || menu_tick_route.entry == menu_update_route.entry;
            for (const auto& route : stat_routes) collision = collision || route.entry == entry;
            for (const auto& route : extra_routes) collision = collision || route.entry == entry;
            if (collision) menu_metadata_ready = false;
        }
    }

    void* VirtualEntry(const void* method, std::string& reason) const {
        if (!method || (api.method_get_flags(method, nullptr) & 0x40) == 0) return nullptr;
        MethodPrefix prefix{};
        std::memcpy(&prefix, method, sizeof(prefix));
        if (prefix.virtual_pointer != prefix.pointer) return nullptr;
        return Entry(api, method, reason);
    }

    bool VirtualRouteMatches(void* object, const void* method, void* entry) const {
        if (!object || !method || !entry) return false;
        const void* route = api.object_get_virtual_method(object, method);
        MethodPrefix prefix{};
        if (!route || !IsReadableMemory(route, sizeof(prefix))) return false;
        std::memcpy(&prefix, route, sizeof(prefix));
        return prefix.pointer == entry && prefix.virtual_pointer == entry;
    }

    bool ResolveBasicWeapons(const void* image) {
        void* klass = api.class_from_name(image, "VampireSurvivors.Data", "WeaponType");
        if (!klass || !api.TypeEquals(api.class_enum_basetype(klass), "System.Int32")) return false;
        constexpr std::array<const char*, 9> names{
            "WHIP", "MAGIC_MISSILE", "KNIFE", "AXE", "CROSS", "HOLYBOOK", "FIREBALL", "HOLYWATER", "LIGHTNING"};
        for (size_t index = 0; index < names.size(); ++index) {
            const void* field = api.class_get_field_from_name(klass, names[index]);
            if (!field || (api.field_get_flags(field) & 0x50) != 0x50
                || !api.TypeEquals(api.field_get_type(field), "VampireSurvivors.Data.WeaponType")) return false;
            api.field_static_get_value(field, &basic_weapon_types[index]);
            if (basic_weapon_types[index] <= 0) return false;
            for (size_t prior = 0; prior < index; ++prior)
                if (basic_weapon_types[prior] == basic_weapon_types[index]) return false;
        }
        return true;
    }

    void ResolveCombat(const void* image) {
        player_hp_field = api.Field(character_class, "_currentHp", "System.Single");
        player_xp_field = api.Field(character_class, "_xp", "System.Single");
        player_level_field = api.Field(character_class, "_level", "System.Int32");
        health_method = api.Method(character_class, "TakeDamage", "System.Void", {"System.Single"}, false, kTakeDamageToken);
        const void* add_xp = api.Method(character_class, "AddXp", "System.Void",
            {"System.Single", "VampireSurvivors.Objects.Characters.XPMultiplierMode"}, false, kAddXpToken);
        std::string reason;
        health_entry = VirtualEntry(health_method, reason);
        health_ready = health_entry && player_hp_field;
        movement_method = api.Method(character_class, "PMoveSpeed", "System.Single", {}, false, kMovementToken);
        movement_entry = VirtualEntry(movement_method, reason);
        movement_ready = movement_entry != nullptr;
        if (add_xp && EnumInt32(image, "VampireSurvivors.Objects.Characters", "XPMultiplierMode"))
            experience_entry = Entry(api, add_xp, reason);
        experience_ready = experience_entry && player_xp_field && player_level_field;

        enemy_class = api.class_from_name(image, "VampireSurvivors.Objects.Characters", "EnemyController");
        equipment_class = api.class_from_name(image, "VampireSurvivors.Objects", "Equipment");
        weapon_class = api.class_from_name(image, "VampireSurvivors.Objects.Weapons", "Weapon");
        void* equipment_manager = api.class_from_name(image, "VampireSurvivors.Objects", "EquipmentManager");
        if (enemy_class) {
            enemy_hp_field = api.Field(enemy_class, "_hp", "System.Single");
            enemy_dead_field = api.Field(enemy_class, "<IsDead>k__BackingField", "System.Boolean");
            enemy_bullet_method = api.Method(enemy_class, "IsBullet", "System.Boolean", {}, false, 100692699);
            if (enemy_bullet_method)
                enemy_is_bullet = reinterpret_cast<BooleanFunction>(Entry(api, enemy_bullet_method, reason));
            const void* get_damaged = api.Method(enemy_class, "GetDamaged", "System.Void",
                {"System.Single", "VampireSurvivors.Data.HitVfxType", "System.Single", "VampireSurvivors.Data.WeaponType", "System.Boolean"},
                false, kEnemyDamageToken);
            if (EnumInt32(image, "VampireSurvivors.Data", "HitVfxType") && EnumInt32(image, "VampireSurvivors.Data", "WeaponType"))
                damage_entry = VirtualEntry(get_damaged, reason);
        }
        if (!equipment_class || !equipment_manager) return;
        weapons_method = api.Method(character_class, "get_WeaponsManager", "VampireSurvivors.Objects.CharacterWeaponsManager", {}, false, 100689533);
        active_equipment_method = api.Method(equipment_manager, "get_ActiveEquipment", nullptr, {}, false, 100672071);
        hidden_equipment_method = api.Method(equipment_manager, "get_HiddenEquipment", nullptr, {}, false, 100672072);
        if (active_equipment_method && !EquipmentListClass(api.class_from_type(api.method_get_return_type(active_equipment_method))))
            active_equipment_method = nullptr;
        if (hidden_equipment_method && !EquipmentListClass(api.class_from_type(api.method_get_return_type(hidden_equipment_method))))
            hidden_equipment_method = nullptr;
        equipment_owner_field = api.Field(equipment_class, "<Owner>k__BackingField", "VampireSurvivors.Objects.Characters.CharacterController");
        equipment_type_field = api.Field(equipment_class, "_equipmentType", "VampireSurvivors.Data.WeaponType");
        equipment_metadata_ready = weapons_method && active_equipment_method && hidden_equipment_method
            && equipment_owner_field && equipment_type_field;
        damage_ready = damage_entry && enemy_hp_field && enemy_dead_field && enemy_is_bullet && equipment_metadata_ready;
        if (weapon_class && ResolveBasicWeapons(image)) {
            interval_method = api.Method(weapon_class, "PInterval", "System.Single", {}, false, kIntervalToken);
            reset_timer_method = api.Method(weapon_class, "ResetFiringTimer", "System.Void", {}, false, kResetFiringTimerToken);
            interval_entry = VirtualEntry(interval_method, reason);
            reset_timer_entry = VirtualEntry(reset_timer_method, reason);
            cooldown_ready = interval_entry && reset_timer_entry && equipment_metadata_ready;
        }
    }

    void ResolveReaperDamage(const void* image) {
        if (!damage_ready || !EnumInt32(image, "VampireSurvivors.Data", "EnemyType")) return;
        reaper_class = api.class_from_name(image, "VampireSurvivors.Objects.Characters.Enemies", "EnemyReapperoni");
        enemy_type_field = api.Field(enemy_class, "_enemyType", "VampireSurvivors.Data.EnemyType");
        if (!reaper_class || !enemy_type_field) return;
        const void* method = api.Method(enemy_class, "DealDamage", "System.Void",
            {"System.Single", "VampireSurvivors.Data.WeaponType"}, false, 100692748);
        std::string reason;
        void* entry = VirtualEntry(method, reason);
        if (!RewardEntryCollision(entry)) reaper_damage = {method, entry};
    }

    bool FloatField(void* object, void* klass, const void* field, float& value) const {
        if (!field || !api.IsA(object, klass)) return false;
        api.field_get_value(object, field, &value);
        return std::isfinite(value);
    }

    bool EquipmentListClass(void* klass) const {
        if (!klass || !NameEquals(api.class_get_name(klass), "List`1")
            || !NameEquals(api.class_get_namespace(klass), "System.Collections.Generic")) return false;
        const void* items = api.class_get_field_from_name(klass, "_items");
        if (!items || (api.field_get_flags(items) & 0x10)) return false;
        void* array_class = api.class_from_type(api.field_get_type(items));
        return array_class && api.class_get_element_class(array_class) == equipment_class;
    }

    bool BasicWeapon(int32_t type) const noexcept {
        for (int32_t basic : basic_weapon_types) if (type == basic && type > 0) return true;
        return false;
    }

    bool EquipmentList(void* list, uintptr_t player, bool reset_timers = false, bool* route_only_failure = nullptr) {
        if (route_only_failure) *route_only_failure = false;
        if (!list) return true;
        if (!IsReadableMemory(list, 2 * sizeof(void*))) return false;
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        const auto still_current = [&] {
            return !stopping.load(std::memory_order_acquire)
                && generation == destructions.load(std::memory_order_acquire)
                && (!reset_timers || local_player.load(std::memory_order_acquire) == player);
        };
        if (!still_current()) return false;
        void* klass = api.object_get_class(list);
        if (klass != equipment_list_class) {
            if (!EquipmentListClass(klass)) return false;
            list_count_method = api.Method(klass, "get_Count", "System.Int32", {}, false, 0, true);
            list_item_method = api.Method(klass, "get_Item", "VampireSurvivors.Objects.Equipment", {"System.Int32"}, false, 0, true, true);
            if (!list_count_method || !list_item_method) return false;
            equipment_list_class = klass;
        }
        void* boxed_count = nullptr;
        if (!api.Invoke(list_count_method, list, nullptr, boxed_count) || !boxed_count || !still_current()) return false;
        void* count_data = api.object_unbox(boxed_count);
        if (!IsReadableMemory(count_data, sizeof(int32_t))) return false;
        int32_t count = *static_cast<const int32_t*>(count_data);
        if (count < 0 || count > 256) return false;
        bool reset_valid = true;
        for (int32_t index = 0; index < count; ++index) {
            void* args[] = {&index};
            void* item = nullptr;
            if (!api.Invoke(list_item_method, list, args, item) || !still_current()) return false;
            if (!item) continue;
            if (!api.IsA(item, equipment_class)) return false;
            void* owner = nullptr;
            int32_t type = 0;
            api.field_get_value(item, equipment_owner_field, &owner);
            api.field_get_value(item, equipment_type_field, &type);
            if (reinterpret_cast<uintptr_t>(owner) != player || type <= 0) continue;
            if (reset_timers) {
                if (!BasicWeapon(type)) continue;
                if (!api.IsA(item, weapon_class)
                    || !VirtualRouteMatches(item, interval_method, interval_entry)
                    || !VirtualRouteMatches(item, reset_timer_method, reset_timer_entry)) {
                    reset_valid = false;
                    continue;
                }
                void* result = nullptr;
                bool dead = true;
                if (!api.Boolean(dead_method, reinterpret_cast<void*>(player), dead) || dead || !still_current()) return false;
                if (!api.Invoke(reset_timer_method, item, nullptr, result) || !still_current()) return false;
                continue;
            }
            if (cooldown_ready && BasicWeapon(type)) {
                bool route = api.IsA(item, weapon_class)
                    && VirtualRouteMatches(item, interval_method, interval_entry)
                    && VirtualRouteMatches(item, reset_timer_method, reset_timer_entry);
                cooldown_routes_valid = cooldown_routes_valid && route;
                cooldown_weapon_seen = true;
            }
            bool found = false;
            for (size_t entry = 0; entry < owned_count; ++entry) if (owned_types[entry] == type) found = true;
            if (!found) {
                if (owned_count == owned_types.size()) return false;
                owned_types[owned_count++] = type;
            }
        }
        if (route_only_failure) *route_only_failure = !reset_valid;
        return reset_valid;
    }

    void RefreshEquipment(void* player, uint64_t now) {
        if (!equipment_metadata_ready || (!damage_ready && !cooldown_ready)) {
            equipment_ready = false; owned_count = 0; return;
        }
        uintptr_t identity = reinterpret_cast<uintptr_t>(player);
        if (identity == equipment_player && now < next_equipment_read) return;
        equipment_player = identity;
        next_equipment_read = now + 250;
        owned_count = 0;
        bool previous = equipment_ready;
        bool previous_cooldown = cooldown_route_available;
        equipment_ready = false;
        cooldown_routes_valid = true;
        cooldown_weapon_seen = false;
        if (player) {
            void* manager = nullptr;
            void* active_list = nullptr;
            void* hidden_list = nullptr;
            equipment_ready = api.Invoke(weapons_method, player, nullptr, manager) && manager
                && api.Invoke(active_equipment_method, manager, nullptr, active_list)
                && api.Invoke(hidden_equipment_method, manager, nullptr, hidden_list)
                && EquipmentList(active_list, identity) && EquipmentList(hidden_list, identity);
        }
        if (!equipment_ready) owned_count = 0;
        cooldown_route_available = cooldown_ready && equipment_ready && cooldown_routes_valid && cooldown_weapon_seen;
        if (!equipment_published || equipment_ready != previous || previous_cooldown != cooldown_route_available) {
            equipment_published = true;
            for (CombatFeature feature : {CombatFeature::Damage, CombatFeature::InstantKill})
                runtime.SetFeatureSupported(feature, damage_ready && equipment_ready,
                    damage_ready && equipment_ready ? "" : "The local equipment source or enemy damage entry cannot be verified.");
            runtime.SetFeatureSupported(CombatFeature::Cooldown, cooldown_route_available,
                cooldown_route_available ? "" : "No owned basic weapon with a verified native cooldown route is available.");
        }
    }

    bool OwnWeapon(int32_t type) const noexcept {
        if (!equipment_ready) return false;
        for (size_t index = 0; index < owned_count; ++index) if (owned_types[index] == type) return true;
        return false;
    }

    bool RefreshWeaponTimers(uintptr_t player, bool* route_only_failure = nullptr) {
        if (route_only_failure) *route_only_failure = false;
        // Get a fresh active list for each transition; retain no weapon object
        // between frames and never access an actor after it becomes unready.
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        void* manager = nullptr;
        void* list = nullptr;
        bool dead = true;
        if (!player || stopping.load(std::memory_order_acquire)
            || local_player.load(std::memory_order_acquire) != player
            || !api.IsA(reinterpret_cast<void*>(player), character_class)
            || !api.Boolean(dead_method, reinterpret_cast<void*>(player), dead) || dead
            || generation != destructions.load(std::memory_order_acquire)) return false;
        return api.Invoke(weapons_method, reinterpret_cast<void*>(player), nullptr, manager) && manager
            && generation == destructions.load(std::memory_order_acquire)
            && local_player.load(std::memory_order_acquire) == player
            && api.Invoke(active_equipment_method, manager, nullptr, list)
            && generation == destructions.load(std::memory_order_acquire)
            && local_player.load(std::memory_order_acquire) == player
            && EquipmentList(list, player, true, route_only_failure);
    }

    void SyncCooldownTimers(uint64_t now) {
        uintptr_t player = local_player.load(std::memory_order_acquire);
        if (!player || !cooldown_ready || stopping.load(std::memory_order_acquire)) {
            cooldown_timer_enabled = false;
            cooldown_timer_player = 0;
            return;
        }
        if (cooldown_timer_player != player) {
            cooldown_timer_player = player;
            cooldown_timer_enabled = false;
        }
        bool enabled = runtime.FeatureEnabled(CombatFeature::Cooldown, player, now);
        if (enabled == cooldown_timer_enabled) return;
        cooldown_timer_enabled = enabled;
        bool route_only_failure = false;
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        if (!RefreshWeaponTimers(player, &route_only_failure)) {
            runtime.SetFeatureSupported(CombatFeature::Cooldown, false, "The current weapon firing timers could not be refreshed safely.");
            cooldown_route_available = false;
            equipment_published = false;
            cooldown_timer_enabled = false;
            // A completed scan can have reset valid weapons before finding
            // an alternate route. With the feature now off, restore those
            // valid schedules once; never retry an invocation/state failure.
            if (enabled && route_only_failure && !stopping.load(std::memory_order_acquire)
                && generation == destructions.load(std::memory_order_acquire)
                && local_player.load(std::memory_order_acquire) == player) RefreshWeaponTimers(player);
        }
    }

    bool StatGroupReady(NativeStat stat, void* player = nullptr) const {
        if (stat_route_collision[StatIndex(stat)]) return false;
        if (StatIndex(stat) >= StatIndex(NativeStat::Magnet)) return ExtendedGroupReady(stat, player);
        if (stat == NativeStat::Armor)
            return armor_created && (!player || VirtualRouteMatches(player, armor_method, armor_entry));
        if (stat == NativeStat::MoveSpeed)
            return movement_created && (!player || VirtualRouteMatches(player, movement_method, movement_entry));
        if (stat == NativeStat::Omni) {
            for (NativeStat member : {NativeStat::Might, NativeStat::ProjectileSpeed, NativeStat::Duration, NativeStat::Area})
                if (!StatGroupReady(member, player)) return false;
            return true;
        }
        if (stat == NativeStat::MaxHealth && !max_health_maintenance_ready) return false;
        if (stat == NativeStat::Curse && !curse_maintenance_ready) return false;
        bool found = false;
        for (size_t index = 0; index < kStatRoutes.size(); ++index) {
            const auto& spec = kStatRoutes[index];
            if (spec.stat != stat) continue;
            found = true;
            const auto& route = stat_routes[index];
            if (!route.created || (player && spec.is_virtual && !VirtualRouteMatches(player, route.method, route.entry))) return false;
        }
        return found;
    }

    bool ExtendedGroupReady(NativeStat stat, void* player = nullptr) const {
        if (extra_failed[StatIndex(stat)]) return false;
        auto made = [&](ExtendedHook hook) { return extra_routes[hook].created; };
        if (stat == NativeStat::Charm)
            return resource_api_ready && resources[CharmSlot].field && made(CharmRead) && curse_maintenance_ready
                && made(StatsUpgrade) && made(StatsSet) && made(StatsReset) && made(PlayerUpgrade) && made(PlayerReset);
        if (!resource_api_ready) return false;
        if (stat == NativeStat::Magnet)
            return magnet_metadata_ready && made(PlayerUpgrade) && made(PlayerReset);
        if (!made(StatsUpgrade) || !made(StatsSet) || !made(StatsReset) || !made(PlayerUpgrade) || !made(PlayerReset)) return false;
        switch (stat) {
        case NativeStat::Rerolls: return resources[RerollSlot].field && made(RerollsRead);
        case NativeStat::Skips: return resources[SkipSlot].field && made(SkipsRead);
        case NativeStat::Banishes: return resources[BanishSlot].field && made(BanishesRead);
        case NativeStat::Revivals: return resources[RevivalSlot].field && resources[UsedRevivalSlot].field && made(RevivalsRead)
            && (!player || VirtualRouteMatches(player, extra_routes[RevivalsRead].method, extra_routes[RevivalsRead].entry));
        case NativeStat::Preserve: return resources[PreserveSlot].field && made(PreserveRead);
        case NativeStat::Defang: return resources[DefangSlot].field && made(DefangRead);
        default: return false;
        }
    }

    static NativeStat SlotStat(size_t slot) noexcept {
        constexpr std::array<NativeStat, ResourceSlotCount> map{{NativeStat::Rerolls, NativeStat::Skips,
            NativeStat::Banishes, NativeStat::Revivals, NativeStat::Revivals, NativeStat::Preserve, NativeStat::Defang, NativeStat::Charm, NativeStat::Magnet}};
        return map[slot];
    }

    void* ResourceOwner(size_t slot) const noexcept { return slot == MagnetSlot ? magnet_owner.Get() : resource_owner.Get(); }

    ResourceValue ReadResource(size_t slot, void* explicit_owner = nullptr) const {
        const auto& binding = resources[slot];
        void* owner = explicit_owner ? explicit_owner : ResourceOwner(slot);
        if (!owner || !binding.field) throw std::bad_alloc();
        ResourceValue value;
        if (binding.kind == ResourceKind::EggFloat || binding.kind == ResourceKind::EggDouble) {
            void* object = nullptr;
            api.field_get_value(owner, binding.field, &object);
            value.object = StrongRoot(api, object);
        } else if (binding.kind == ResourceKind::Float) {
            float number = 0; api.field_get_value(owner, binding.field, &number); value.scalar = number;
        } else {
            int32_t number = 0; api.field_get_value(owner, binding.field, &number); value.scalar = number;
        }
        return value;
    }

    void WriteResource(size_t slot, const ResourceValue& value) const {
        const auto& binding = resources[slot];
        void* owner = ResourceOwner(slot);
        if (!owner || !binding.field) throw std::bad_alloc();
        if (binding.kind == ResourceKind::EggFloat || binding.kind == ResourceKind::EggDouble) {
            void* object = value.object.Get();
            if (!WriteReferenceField(api, owner, binding.field, object)) throw std::bad_alloc();
        } else if (binding.kind == ResourceKind::Float) {
            float number = static_cast<float>(value.scalar); api.field_set_value(owner, binding.field, &number);
        } else {
            int32_t number = static_cast<int32_t>(value.scalar); api.field_set_value(owner, binding.field, &number);
        }
    }

    ResourceValue CreateResource(size_t slot, double requested) const {
        ResourceValue value;
        const auto kind = resources[slot].kind;
        if (kind == ResourceKind::Float || kind == ResourceKind::Int) {
            value.scalar = slot == UsedRevivalSlot ? 0 : StatToRaw(kNativeStats[StatIndex(SlotStat(slot))], requested);
            return value;
        }
        const bool wide = kind == ResourceKind::EggDouble;
        void* object = api.object_new(wide ? egg_double_class : egg_float_class);
        if (!object) throw std::bad_alloc();
        value.object = StrongRoot(api, object); // root before invoking constructor
        float narrow = static_cast<float>(requested), narrow_zero = 0;
        double broad = requested, broad_zero = 0;
        void* args[]{wide ? static_cast<void*>(&broad) : static_cast<void*>(&narrow),
            wide ? static_cast<void*>(&broad_zero) : static_cast<void*>(&narrow_zero)};
        void* result = nullptr;
        StatSamplingScope scope(StatReadMode::Original);
        if (!api.Invoke(wide ? egg_double_ctor : egg_float_ctor, value.object.Get(), args, result)) throw std::bad_alloc();
        return value;
    }

    bool ResourceNumber(size_t slot, const ResourceValue& value, float& number) const {
        auto kind = resources[slot].kind;
        if (kind == ResourceKind::Float || kind == ResourceKind::Int) {
            number = static_cast<float>(value.scalar); return std::isfinite(number);
        }
        const bool wide = kind == ResourceKind::EggDouble;
        void* object = value.object.Get();
        if (!api.IsA(object, wide ? egg_double_class : egg_float_class)) return false;
        void* args[]{object}; // runtime_invoke reference arguments are direct managed pointers
        void* box = nullptr;
        StatSamplingScope scope(StatReadMode::Original);
        if (!api.Invoke(wide ? egg_double_value : egg_float_value, nullptr, args, box) || !box) return false;
        void* data = api.object_unbox(box);
        if (!IsReadableMemory(data, wide ? sizeof(double) : sizeof(float))) return false;
        number = wide ? static_cast<float>(*static_cast<double*>(data)) : *static_cast<float*>(data);
        return std::isfinite(number);
    }

    void RestoreResource(size_t slot) {
        auto& budget = resources[slot].budget;
        const bool changed = budget.active;
        budget.Restore([&](const ResourceValue& value) { WriteResource(slot, value); });
        if (changed && slot == MagnetSlot) magnet_refresh_pending = true;
    }

    void RestoreResources(bool release = false) noexcept {
        bool stats_restored = true, magnet_restored = true;
        for (size_t slot = 0; slot < resources.size(); ++slot) {
            try { RestoreResource(slot); }
            catch (...) { if (slot == MagnetSlot) magnet_restored = false; else stats_restored = false; }
        }
        if (release && stats_restored) resource_owner = {};
        if (release && magnet_restored && !magnet_refresh_pending) magnet_owner = {};
        resource_known.fill(false);
    }

    void FailResource(size_t slot) noexcept {
        NativeStat stat = SlotStat(slot);
        extra_failed[StatIndex(stat)] = true;
        if (!writing_gold) try { PublishStatSupported(stat, false, "The temporary native resource could not be bound or restored safely.", true); } catch (...) {}
        try { RestoreResource(slot); } catch (...) {}
        if (slot == RevivalSlot || slot == UsedRevivalSlot) {
            try { RestoreResource(RevivalSlot); RestoreResource(UsedRevivalSlot); } catch (...) {}
        }
    }

    bool RebindStatsOwner(void* owner, uintptr_t player, uint64_t now) noexcept {
        if (owner == resource_owner.Get()) return true;
        try {
            if (resource_native_depth || magnet_native_depth) return false;
            if (!api.IsA(owner, player_stats_class)) throw std::bad_alloc();
            StrongRoot next_owner(api, owner);
            std::array<TemporaryResource<ResourceValue>, MagnetSlot> prepared;
            for (size_t slot = 0; slot < MagnetSlot; ++slot) {
                const auto& previous = resources[slot].budget;
                if (!previous.active) continue;
                if (!RebaseTemporaryResource(previous, ReadResource(slot, next_owner.Get()), ReadResource(slot), prepared[slot]))
                    throw std::bad_alloc();
            }
            // Keep the old owner's normal bindings on the old owner only.
            // No new-owner field is touched until all old restores succeed.
            for (size_t slot = 0; slot < MagnetSlot; ++slot) RestoreResource(slot);
            resource_owner = std::move(next_owner);
            for (size_t slot = 0; slot < MagnetSlot; ++slot) resources[slot].budget = std::move(prepared[slot]);
            for (size_t slot = 0; slot < MagnetSlot; ++slot) {
                auto& budget = resources[slot].budget;
                if (!budget.active) continue;
                if (!runtime.StatEnabled(SlotStat(slot), player, now)) RestoreResource(slot);
                else WriteResource(slot, budget.temporary);
            }
            if (resources[CharmSlot].budget.active) {
                curse_refresh_pending = true;
                next_curse_refresh = 0;
            }
            resource_known.fill(false);
            next_resource_sample = 0;
            return true;
        } catch (...) {
            // Before switching, residual budgets still belong to the old
            // root; afterwards every prepared baseline belongs to the new
            // root. In either phase, restore through that exact owner.
            for (size_t slot = 0; slot < MagnetSlot; ++slot) FailResource(slot);
            return false;
        }
    }

    bool CurrentResourceActor(uintptr_t player, uint64_t generation, bool revival_only) const {
        if (!revival_only) return CurrentActor(player, generation);
        return player && !stopping.load(std::memory_order_acquire)
            && pending_revival_player.load(std::memory_order_acquire) == player
            && player == live_resource_player && generation == live_resource_generation
            && generation == destructions.load(std::memory_order_acquire)
            && scene == live_resource_scene && api.IsA(reinterpret_cast<void*>(player), character_class);
    }

    bool RefreshMagnetFor(void* player) {
        if (!magnet_refresh_pending || !magnet_owner.Get()) return true;
        if (GetTickCount64() < next_magnet_refresh) return true;
        void* zone = nullptr;
        const uintptr_t identity = reinterpret_cast<uintptr_t>(player);
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        if (!CurrentActor(identity, generation)) return false;
        StatSamplingScope scope(StatReadMode::Effective);
        if (!api.Invoke(magnet_method, player, nullptr, zone) || zone != magnet_owner.Get()
            || !api.IsA(zone, magnet_class) || !CurrentActor(identity, generation)) return false;
        void* result = nullptr;
        if (!api.Invoke(refresh_magnet_method, zone, nullptr, result)) {
            next_magnet_refresh = GetTickCount64() + 250;
            return false;
        }
        magnet_refresh_pending = false;
        next_magnet_refresh = 0;
        return true;
    }

    void SyncResources(uint64_t now) {
        if (resource_native_depth || magnet_native_depth) return;
        uintptr_t player = local_player.load(std::memory_order_acquire);
        const bool revival_only = !player && pending_revival_player.load(std::memory_order_acquire) != 0;
        if (revival_only) player = pending_revival_player.load(std::memory_order_acquire);
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        if (!player || !CurrentResourceActor(player, generation, revival_only)) { RestoreResources(); return; }
        void* actor = reinterpret_cast<void*>(player);
        void* owner = nullptr;
        { StatSamplingScope scope(StatReadMode::Original);
            if (!player_stats_method || !api.Invoke(player_stats_method, actor, nullptr, owner) || !api.IsA(owner, player_stats_class)) owner = nullptr; }
        if (!CurrentResourceActor(player, generation, revival_only)) { RestoreResources(); return; }
        if (resource_player != player) {
            RestoreResources(true);
            for (size_t slot = 0; slot < MagnetSlot; ++slot) if (resources[slot].budget.active) return;
            if (owner && resource_api_ready) resource_owner = StrongRoot(api, owner);
            resource_player = player;
            next_resource_sample = 0;
        } else if (owner != resource_owner.Get()) {
            if (!RebindStatsOwner(owner, player, now)) return;
        }
        if (resource_owner.Get()) {
            for (size_t slot = 0; slot < MagnetSlot; ++slot) {
                if (slot == UsedRevivalSlot) continue;
                const NativeStat stat = SlotStat(slot);
                try {
                    auto& budget = resources[slot].budget;
                    if ((!revival_only || stat == NativeStat::Revivals)
                        && runtime.StatEnabled(stat, player, now) && ExtendedGroupReady(stat, actor)) {
                        const double target = runtime.StatValue(stat);
                        const bool changed = !budget.active || budget.target != target;
                        budget.Configure(target, [&] { return ReadResource(slot); },
                            [&](const ResourceValue& value) { WriteResource(slot, value); }, [&](double value) { return CreateResource(slot, value); });
                        if (slot == RevivalSlot) resources[UsedRevivalSlot].budget.Configure(0,
                            [&] { return ReadResource(UsedRevivalSlot); }, [&](const ResourceValue& value) { WriteResource(UsedRevivalSlot, value); },
                            [&](double value) { return CreateResource(UsedRevivalSlot, value); }, changed);
                    } else {
                        RestoreResource(slot);
                        if (slot == RevivalSlot) RestoreResource(UsedRevivalSlot);
                    }
                } catch (...) { FailResource(slot); }
            }
        }
        if (revival_only) {
            try { RestoreResource(MagnetSlot); } catch (...) { FailResource(MagnetSlot); }
            return; // A dead actor never receives Unity collider refreshes.
        }
        if (!magnet_metadata_ready || !CurrentActor(player, generation)) return;
        void* zone = nullptr;
        { StatSamplingScope scope(StatReadMode::Original);
            if (!api.Invoke(magnet_method, actor, nullptr, zone) || !api.IsA(zone, magnet_class)) zone = nullptr; }
        if (!CurrentActor(player, generation)) return;
        if (zone != magnet_owner.Get()) {
            try { RestoreResource(MagnetSlot); } catch (...) { FailResource(MagnetSlot); return; }
            // The old Unity component may already be destroyed. Its managed
            // Radius was restored through its rooted wrapper; never call it.
            magnet_owner = zone ? StrongRoot(api, zone) : StrongRoot{};
            magnet_refresh_pending = false;
            next_magnet_refresh = 0;
            next_resource_sample = 0;
        }
        if (magnet_owner.Get()) {
            try {
                auto& budget = resources[MagnetSlot].budget;
                const bool enabled = runtime.StatEnabled(NativeStat::Magnet, player, now) && ExtendedGroupReady(NativeStat::Magnet, actor);
                if (enabled) {
                    const double target = runtime.StatValue(NativeStat::Magnet);
                    const bool changed = !budget.active || budget.target != target;
                    budget.Configure(target, [&] { return ReadResource(MagnetSlot); },
                        [&](const ResourceValue& value) { WriteResource(MagnetSlot, value); }, [&](double value) { return CreateResource(MagnetSlot, value); });
                    magnet_refresh_pending = magnet_refresh_pending || changed;
                } else RestoreResource(MagnetSlot);
                if (!RefreshMagnetFor(actor) && magnet_refresh_pending) FailResource(MagnetSlot);
            } catch (...) { FailResource(MagnetSlot); }
        }
    }

    void SampleResources(void* player, GameObservation& state, uint64_t now, bool revival_only = false) {
        if (!resource_native_depth && !magnet_native_depth && now >= next_resource_sample) {
            next_resource_sample = now + 250;
            resource_known.fill(false);
            for (size_t slot = 0; slot < resources.size(); ++slot) {
                if (slot == UsedRevivalSlot || !ResourceOwner(slot)) continue;
                if (revival_only && slot != RevivalSlot) continue;
                const NativeStat stat = SlotStat(slot);
                if (!ExtendedGroupReady(stat, player)) continue;
                try {
                    auto& budget = resources[slot].budget;
                    float before = 0, after = 0;
                    if (budget.SampleBound([&] { return ReadResource(slot); },
                        [&](const ResourceValue& value, float& number) { return ResourceNumber(slot, value, number); }, before, after)) {
                        resource_original[StatIndex(stat)] = before;
                        resource_current[StatIndex(stat)] = after;
                        resource_known[StatIndex(stat)] = true;
                    }
                } catch (...) { FailResource(slot); }
            }
        }
        for (size_t index = StatIndex(NativeStat::Magnet); index < kNativeStatCount; ++index) {
            if (revival_only && static_cast<NativeStat>(index) != NativeStat::Revivals) continue;
            state.stat_known[index] = state.stat_current_known[index] = resource_known[index];
            state.stat_original[index] = resource_original[index];
            state.stat_current[index] = resource_current[index];
            PublishStatSupported(static_cast<NativeStat>(index), ExtendedGroupReady(static_cast<NativeStat>(index), player),
                "The complete native resource fields, mutation hooks or current-character route are unavailable.");
        }
    }

    void PublishStatSupported(NativeStat stat, bool supported, const char* reason, bool force = false) {
        const size_t index = StatIndex(stat);
        if (force || stat_supported[index] != supported) {
            stat_supported[index] = supported;
            runtime.SetStatSupported(stat, supported, supported ? "" : reason);
        }
    }

    bool ReadOriginalStat(NativeStat stat, void* player, float& value) const {
        if (stat == NativeStat::Charm) {
            void* owner = nullptr;
            StatSamplingScope scope(StatReadMode::Original);
            if (!ExtendedGroupReady(stat, player) || !api.Invoke(player_stats_method, player, nullptr, owner)
                || !api.IsA(owner, player_stats_class)) return false;
            const auto& budget = resources[CharmSlot].budget;
            value = static_cast<float>(owner == resource_owner.Get() && budget.active
                ? budget.original.scalar : ReadResource(CharmSlot, owner).scalar);
            return true;
        }
        const void* method = stat == NativeStat::Armor ? armor_method
            : stat == NativeStat::MoveSpeed ? movement_method : nullptr;
        if (!method) {
            for (size_t index = 0; index < kStatRoutes.size(); ++index)
                if (kStatRoutes[index].stat == stat && !kStatRoutes[index].has_parameter) { method = stat_routes[index].method; break; }
        }
        if (!method || !StatGroupReady(stat, player)) return false;
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        void* result = nullptr;
        bool invoked = false;
        { StatSamplingScope scope(StatReadMode::Original); invoked = api.Invoke(method, player, nullptr, result); }
        if (!invoked || !result || stopping.load(std::memory_order_acquire)
            || generation != destructions.load(std::memory_order_acquire)) return false;
        const void* data = api.object_unbox(result);
        if (!IsReadableMemory(data, sizeof(float))) return false;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value);
    }

    void RefreshOrdinaryStats(void* player, GameObservation& state, uint64_t now) {
        const uintptr_t identity = reinterpret_cast<uintptr_t>(player);
        if (identity != stat_sample_player || now >= next_stat_sample) {
            stat_sample_player = identity;
            next_stat_sample = now + 250;
            stat_sample_known.fill(false);
            const uint64_t generation = destructions.load(std::memory_order_acquire);
            for (size_t index = 0; index < kNativeStatCount; ++index) {
                NativeStat stat = static_cast<NativeStat>(index);
                if (stat == NativeStat::Omni) continue;
                if (index >= StatIndex(NativeStat::Magnet) && stat != NativeStat::Charm) continue;
                bool ready = StatGroupReady(stat, player);
                if (ready) ready = ReadOriginalStat(stat, player, stat_samples[index]);
                if (stopping.load(std::memory_order_acquire)
                    || generation != destructions.load(std::memory_order_acquire)) return;
                stat_sample_known[index] = ready;
                PublishStatSupported(stat, ready, "The current character stat route or original sample cannot be verified.");
            }
            bool omni_ready = true;
            for (NativeStat member : {NativeStat::Might, NativeStat::ProjectileSpeed, NativeStat::Duration, NativeStat::Area})
                omni_ready = omni_ready && stat_supported[StatIndex(member)];
            PublishStatSupported(NativeStat::Omni, omni_ready, "Unified bonuses require all four verified ordinary-stat routes.");
        }
        state.stat_original = stat_samples;
        state.stat_known = stat_sample_known;
        state.armor_original = stat_sample_known[StatIndex(NativeStat::Armor)]
            ? stat_samples[StatIndex(NativeStat::Armor)] : std::numeric_limits<float>::quiet_NaN();
    }

    bool CurrentActor(uintptr_t player, uint64_t generation) const {
        if (!player || stopping.load(std::memory_order_acquire)
            || local_player.load(std::memory_order_acquire) != player
            || generation != destructions.load(std::memory_order_acquire)
            || !api.IsA(reinterpret_cast<void*>(player), character_class)) return false;
        bool dead = true;
        bool known = false;
        { StatSamplingScope scope(StatReadMode::Original);
            known = api.Boolean(dead_method, reinterpret_cast<void*>(player), dead); }
        return known && !dead
            && !stopping.load(std::memory_order_acquire)
            && generation == destructions.load(std::memory_order_acquire)
            && local_player.load(std::memory_order_acquire) == player;
    }

    void SyncOrdinaryStats(void* manager, uint64_t now) {
        const uintptr_t player = local_player.load(std::memory_order_acquire);
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        if (!player || stopping.load(std::memory_order_acquire)) return;
        if (transient_stat_player != player) {
            transient_stat_player = player;
            max_health_synced = curse_synced = false;
            max_health_enabled = curse_enabled = false;
            charm_synced = charm_enabled = false;
            curse_refresh_pending = false;
            next_curse_refresh = 0;
        }
        // Retain only the transition identity/state across a temporary unsafe
        // interval. It is never dereferenced without a fresh current actor.
        const bool max_enabled = runtime.StatEnabled(NativeStat::MaxHealth, player, now);
        const double max_target = max_enabled ? runtime.StatValue(NativeStat::MaxHealth) : 0;
        const bool max_changed = (!max_health_synced && max_enabled)
            || (max_health_synced && (max_enabled != max_health_enabled || (max_enabled && max_target != max_health_target)));
        const bool next_curse = runtime.StatEnabled(NativeStat::Curse, player, now);
        const double next_target = next_curse ? runtime.StatValue(NativeStat::Curse) : 0;
        const bool next_charm = runtime.StatEnabled(NativeStat::Charm, player, now);
        const double next_charm_target = next_charm ? runtime.StatValue(NativeStat::Charm) : 0;
        const bool curse_changed = (curse_refresh_pending || (!curse_synced && next_curse)
            || (curse_synced && (next_curse != curse_enabled || (next_curse && next_target != curse_target)))
            || (!charm_synced && next_charm)
            || (charm_synced && (next_charm != charm_enabled || (next_charm && next_charm_target != charm_target))))
            && now >= next_curse_refresh;
        if (!max_changed && !curse_changed) {
            max_health_synced = curse_synced = true;
            max_health_enabled = max_enabled; max_health_target = max_target;
            curse_enabled = next_curse; curse_target = next_target;
            charm_synced = true; charm_enabled = next_charm; charm_target = next_charm_target;
            return;
        }
        if (!CurrentActor(player, generation)) return;
        if (max_changed && max_health_maintenance_ready) {
            float natural_max = 0, hp = 0;
            if (ReadOriginalStat(NativeStat::MaxHealth, reinterpret_cast<void*>(player), natural_max)
                && CurrentActor(player, generation)
                && FloatField(reinterpret_cast<void*>(player), character_class, player_hp_field, hp)) {
                float effective_max = runtime.StatEnabled(NativeStat::MaxHealth, player, GetTickCount64())
                    ? static_cast<float>(runtime.StatValue(NativeStat::MaxHealth)) : natural_max;
                if (std::isfinite(effective_max) && effective_max > 0 && hp > effective_max) {
                    void* arguments[]{&effective_max};
                    void* result = nullptr;
                    bool success = false;
                    { StatSamplingScope scope(StatReadMode::Effective);
                        if (CurrentActor(player, generation)) success = api.Invoke(set_hp_method, reinterpret_cast<void*>(player), arguments, result); }
                    if (!success) {
                        PublishStatSupported(NativeStat::MaxHealth, false, "The current maximum health could not be restored safely.", true);
                        return;
                    }
                }
            } else {
                PublishStatSupported(NativeStat::MaxHealth, false, "The current maximum health could not be checked safely.", true);
                return;
            }
        }
        max_health_synced = true;
        max_health_enabled = max_enabled;
        max_health_target = max_target;
        if (curse_changed && curse_maintenance_ready) {
            if (!CurrentActor(player, generation)) return;
            // Retain the attempted state until refresh succeeds. A failed
            // enable disables the capability and leaves a natural-refresh
            // transition pending for the next safe current-actor frame.
            curse_synced = true; curse_enabled = next_curse; curse_target = next_target;
            charm_synced = true; charm_enabled = next_charm; charm_target = next_charm_target;
            void* stage = nullptr;
            void* result = nullptr;
            bool success = false;
            if (api.Invoke(stage_method, manager, nullptr, stage) && api.IsA(stage, stage_class) && CurrentActor(player, generation)) {
                // Effective refreshes see the new PCurse return, while these
                // internal reads never masquerade as natural getter evidence.
                StatSamplingScope scope(StatReadMode::Effective);
                success = api.Invoke(refresh_curse_method, stage, nullptr, result);
            }
            if (!success) {
                curse_refresh_pending = true;
                next_curse_refresh = GetTickCount64() + 250;
                PublishStatSupported(NativeStat::Curse, false, "The stage curse calculation could not be refreshed safely.", true);
                PublishStatSupported(NativeStat::Charm, false, "The stage charm calculation could not be refreshed safely.", true);
                return;
            }
            curse_refresh_pending = false;
            next_curse_refresh = 0;
        }
        if (!CurrentActor(player, generation)) return;
        curse_synced = true;
        curse_enabled = next_curse;
        curse_target = next_target;
        charm_synced = true; charm_enabled = next_charm; charm_target = next_charm_target;
    }

    bool ReadScene(uint64_t now) {
        if (now < next_scene_read && !scene.empty()) return true;
        next_scene_read = now + 250;
        void* box = nullptr;
        if (!api.Invoke(active_scene_method, nullptr, nullptr, box) || !api.IsA(box, scene_class)) return false;
        int32_t handle = 0;
        api.field_get_value(box, scene_handle_field, &handle);
        void* args[] = {&handle};
        void* text = nullptr;
        if (!api.Invoke(scene_name_method, nullptr, args, text) || !text) return false;
        int length = api.string_length(text);
        const wchar_t* chars = api.string_chars(text);
        if (length <= 0 || length > 128 || !IsReadableMemory(chars, static_cast<size_t>(length) * sizeof(wchar_t))) return false;
        int bytes = length ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, nullptr, 0, nullptr, nullptr) : 0;
        if (length && !bytes) return false;
        scene.resize(static_cast<size_t>(bytes));
        if (bytes) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, scene.data(), bytes, nullptr, nullptr);
        return true;
    }

    bool MenuContext(void* expected_tick, GoldWallet& wallet) {
        wallet = {};
        if (!menu_metadata_ready || stopping.load(std::memory_order_acquire) || scene != "MainMenu"
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()
            || manager_overflow.load(std::memory_order_acquire)) return false;
        const uint64_t generation = destructions.load(std::memory_order_acquire);
        StatSamplingScope scope(StatReadMode::Original);
        void* singleton = nullptr;
        if (!api.Invoke(multiplayer_instance_method, nullptr, nullptr, singleton) || !api.IsA(singleton, multiplayer_class)
            || (expected_tick && singleton != expected_tick)
            || !VirtualRouteMatches(singleton, menu_tick_route.method, menu_tick_route.entry)) return false;
        bool multiplayer = true, online = true;
        if (!api.Boolean(menu_is_multiplayer_method, singleton, multiplayer) || multiplayer
            || !api.Boolean(menu_is_online_method, singleton, online) || online) return false;
        const void* type = api.class_get_type(menu_class);
        void* managed_type = type ? api.type_get_object(type) : nullptr;
        if (!managed_type) return false;
        StrongRoot type_root(api, managed_type);
        bool include_inactive = true;
        void* find_args[]{type_root.Get(), &include_inactive};
        void* menu = nullptr;
        if (!api.Invoke(find_menu_method, nullptr, find_args, menu) || !api.IsA(menu, menu_class)) return false;
        StrongRoot menu_root(api, menu);
        void* alive_args[]{menu_root.Get()};
        void* alive_box = nullptr;
        if (!api.Invoke(unity_alive_method, nullptr, alive_args, alive_box) || !alive_box) return false;
        const auto* alive = static_cast<const uint8_t*>(api.object_unbox(alive_box));
        if (!IsReadableMemory(alive, sizeof(uint8_t)) || !*alive) return false;
        void* menu_multiplayer = nullptr;
        void* options = nullptr;
        api.field_get_value(menu_root.Get(), menu_multiplayer_field, &menu_multiplayer);
        if (menu_multiplayer != singleton || !api.Invoke(menu_options_method, menu_root.Get(), nullptr, options)
            || !api.IsA(options, options_class)) return false;
        bool initialized = false, config_ready = false;
        if (!api.Boolean(initialized_method, options, initialized) || !initialized
            || !api.Boolean(config_ready_method, options, config_ready) || !config_ready
            || !ReadMenuWallet(options, wallet)) return false;
        // Normally no gameplay manager exists in MainMenu. If a manager is
        // retained during a transition, it must independently confirm that
        // no local character or multiplayer run is still active.
        unsigned count = 0;
        for (const auto& candidate : managers) if (uintptr_t value = candidate.load(std::memory_order_acquire)) {
            if (++count > 1) return false;
            void* manager = reinterpret_cast<void*>(value);
            void* player = nullptr;
            void* manager_options = nullptr;
            bool started_online = true, is_multiplayer = true;
            if (!api.IsA(manager, manager_class) || !api.Invoke(player_method, manager, nullptr, player) || player) return false;
            api.field_get_value(manager, options_field, &manager_options);
            api.field_get_value(manager, started_online_field, &started_online);
            if (manager_options != options || started_online || !api.Boolean(multiplayer_method, manager, is_multiplayer) || is_multiplayer) return false;
        }
        return generation == destructions.load(std::memory_order_acquire);
    }

    void MenuFrame(void* tick_owner, uint64_t frame_generation) {
        if (stopping.load(std::memory_order_acquire) || writing_gold || !menu_metadata_ready) return;
        void* singleton = nullptr;
        { StatSamplingScope scope(StatReadMode::Original);
            if (!api.Invoke(multiplayer_instance_method, nullptr, nullptr, singleton) || (tick_owner && singleton != tick_owner)
                || !api.IsA(singleton, multiplayer_class) || !VirtualRouteMatches(singleton, menu_tick_route.method, menu_tick_route.entry)) return; }
        DWORD expected = 0, current = GetCurrentThreadId();
        unity_thread.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
        if (unity_thread.load(std::memory_order_acquire) != current) return;
        const uint64_t now = GetTickCount64();
        if (frame_generation != destructions.load(std::memory_order_acquire)) return;
        // Catalog construction has its own short per-frame budget. Keeping it
        // ahead of the observation throttle avoids 250 ms between small slices.
        rewards.Tick();
        if (now < next_menu_frame) return;
        next_menu_frame = now + 250;
        if (!ReadScene(now)) return;
        if (scene != "MainMenu") {
            if (menu_observing) {
                menu_observing = false;
                if (!local_player.load(std::memory_order_acquire)) {
                    local_wallet.store(0, std::memory_order_release);
                    GameObservation waiting; waiting.verified = true; waiting.scene = scene;
                    runtime.Observe(waiting, now);
                }
            }
            return;
        }
        GameObservation state;
        state.verified = true; state.scene = scene;
        state.paused_known = true; state.paused = true;
        GoldWallet wallet;
        state.safe_mode = MenuContext(tick_owner, wallet);
        if (stopping.load(std::memory_order_acquire) || frame_generation != destructions.load(std::memory_order_acquire)) return;
        if (state.safe_mode) {
            state.gold_wallet = wallet;
            if (wallet.adventure < 0) state.wallet = wallet.options;
            state.gold_known = ReadCoins(reinterpret_cast<void*>(wallet.config), state.gold);
            state.gold_write_ready = gold_writer_ready && state.gold_known;
        }
        if (stopping.load(std::memory_order_acquire) || frame_generation != destructions.load(std::memory_order_acquire)) return;
        menu_observing = true;
        local_player.store(0, std::memory_order_release);
        pending_revival_player.store(0, std::memory_order_release);
        live_resource_player = 0;
        live_resource_scene.clear();
        local_wallet.store(state.safe_mode ? state.wallet : 0, std::memory_order_release);
        RestoreResources(true);
        SyncGameSpeed(0);
        runtime.Observe(state, now);
        runtime.Pump(GetTickCount64());
    }

    static void MenuTickHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->menu_tick_route.original) return;
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        reinterpret_cast<UpdateFunction>(self->menu_tick_route.original)(owner, method);
        try {
            if (!self->stopping.load(std::memory_order_acquire) && generation == self->destructions.load(std::memory_order_acquire))
                self->MenuFrame(owner, generation);
        } catch (...) { self->Disarm("Native menu observation failed."); }
    }

    static void MenuUpdateHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->menu_update_route.original) return;
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        reinterpret_cast<UpdateFunction>(self->menu_update_route.original)(owner, method);
        try {
            if (!self->stopping.load(std::memory_order_acquire) && generation == self->destructions.load(std::memory_order_acquire)
                && self->api.IsA(owner, self->menu_class)) self->MenuFrame(nullptr, generation);
        } catch (...) { self->Disarm("Native menu update observation failed."); }
    }

    bool Observe(void* manager, GameObservation& state, uint64_t now) {
        // Getter calls nested in an internal state observation are raw
        // samples, not gameplay reads or occasions to modify the world.
        StatSamplingScope observation_scope(StatReadMode::Original);
        state.verified = true;
        if (!api.IsA(manager, manager_class) || !ReadScene(now)) return false;
        state.scene = scene;
        bool multiplayer = true, started_online = true, game_over = true;
        void* options = nullptr;
        api.field_get_value(manager, options_field, &options);
        api.field_get_value(manager, game_over_field, &game_over);
        api.field_get_value(manager, started_online_field, &started_online);
        if (!api.Boolean(multiplayer_method, manager, multiplayer) || !api.IsA(options, options_class)) return false;
        bool initialized = false, config_ready = false;
        if (!api.Boolean(initialized_method, options, initialized) || !api.Boolean(config_ready_method, options, config_ready)) return false;
        state.safe_mode = !multiplayer && !started_online && initialized && config_ready;
        if (state.safe_mode && state.scene == "MainMenu") {
            GoldWallet wallet;
            state.safe_mode = MenuContext(nullptr, wallet) && wallet.options == reinterpret_cast<uintptr_t>(options);
            if (state.safe_mode) {
                state.gold_wallet = wallet;
                if (wallet.adventure < 0) state.wallet = wallet.options;
                state.gold_known = ReadCoins(reinterpret_cast<void*>(wallet.config), state.gold);
            }
        } else if (void* main_config = nullptr; state.safe_mode && ReadMainConfig(options, main_config)) {
            state.wallet = reinterpret_cast<uintptr_t>(options);
            state.gold_known = ReadCoins(main_config, state.gold);
        }
        state.paused_known = api.Boolean(paused_method, manager, state.paused);
        void* player = nullptr;
        if (!api.Invoke(player_method, manager, nullptr, player)) return false;
        state.gold_write_ready = gold_writer_ready && state.safe_mode && state.gold_known && !player && state.gold_wallet.config;
        if (!state.safe_mode || !player) return true;
        if (!api.IsA(player, character_class)) return false;
        bool dead = true;
        if (!api.Boolean(dead_method, player, dead)) return false;
        if (dead || game_over) {
            const uintptr_t identity = reinterpret_cast<uintptr_t>(player);
            const uint64_t generation = destructions.load(std::memory_order_acquire);
            float hp = 1;
            const bool natural_death = dead || (game_over && FloatField(player, character_class, player_hp_field, hp) && hp <= 0);
            const bool continuing = pending_revival_player.load(std::memory_order_acquire) == identity;
            if ((natural_death || continuing) && identity == live_resource_player && generation == live_resource_generation
                && state.scene == live_resource_scene && identity == resource_player
                && resources[RevivalSlot].budget.active && resources[UsedRevivalSlot].budget.active
                && ExtendedGroupReady(NativeStat::Revivals, player)
                && runtime.StatEnabled(NativeStat::Revivals, identity, now)) {
                void* stats = nullptr;
                if (api.Invoke(player_stats_method, player, nullptr, stats) && api.IsA(stats, player_stats_class)
                    && generation == destructions.load(std::memory_order_acquire)) {
                    state.player = identity;
                    state.revival_pending = true;
                    if (stats == resource_owner.Get()) SampleResources(player, state, now, true);
                }
            }
            return true;
        }
        state.player = reinterpret_cast<uintptr_t>(player);
        state.player_ready = true;
        state.health_known = FloatField(player, character_class, player_hp_field, state.health);
        state.experience_known = FloatField(player, character_class, player_xp_field, state.experience);
        if (player_level_field) {
            api.field_get_value(player, player_level_field, &state.level);
            state.level_known = true;
        }
        RefreshEquipment(player, now);
        if (movement_ready) {
            bool matches = !stat_route_collision[StatIndex(NativeStat::MoveSpeed)]
                && VirtualRouteMatches(player, movement_method, movement_entry);
            if (matches != movement_route_available) {
                movement_route_available = matches;
                runtime.SetFeatureSupported(CombatFeature::Movement, matches,
                    matches ? "" : "The current character uses a different native movement method.");
            }
        }
        if (health_ready) {
            const void* virtual_health = api.object_get_virtual_method(player, health_method);
            MethodPrefix health_route{};
            bool matches = virtual_health && IsReadableMemory(virtual_health, sizeof(health_route));
            if (matches) {
                std::memcpy(&health_route, virtual_health, sizeof(health_route));
                matches = health_route.pointer == health_entry && health_route.virtual_pointer == health_entry;
            }
            if (matches != health_route_available) {
                health_route_available = matches;
                runtime.SetFeatureSupported(CombatFeature::Health, matches,
                    matches ? "" : "The current character uses a different native health method.");
            }
        }
        RefreshOrdinaryStats(player, state, now);
        if (resource_player == state.player) {
            void* stats = nullptr;
            if (api.Invoke(player_stats_method, player, nullptr, stats) && stats == resource_owner.Get()) SampleResources(player, state, now);
        }
        return true;
    }

    void Frame(void* manager, uint64_t frame_generation) {
        if (stopping.load(std::memory_order_acquire) || writing_gold) return;
        DWORD expected = 0, current = GetCurrentThreadId();
        unity_thread.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
        if (unity_thread.load(std::memory_order_acquire) != current) { runtime.DisableAll(); return; }
        uint64_t now = GetTickCount64();
        GameObservation state;
        uintptr_t identity = reinterpret_cast<uintptr_t>(manager);
        bool tracked = false;
        for (auto& candidate : managers) if (candidate.load(std::memory_order_acquire) == identity) tracked = true;
        if (!tracked) {
            for (auto& candidate : managers) {
                uintptr_t empty = 0;
                if (candidate.compare_exchange_strong(empty, identity, std::memory_order_acq_rel)) { tracked = true; break; }
            }
        }
        if (!tracked) manager_overflow.store(true, std::memory_order_release);
        unsigned count = 0;
        for (auto& candidate : managers) if (candidate.load(std::memory_order_acquire)) ++count;
        if (count != 1 || manager_overflow.load(std::memory_order_acquire)) {
            RestoreResources();
            SyncGameSpeed(0);
            local_player.store(0, std::memory_order_release);
            pending_revival_player.store(0, std::memory_order_release);
            live_resource_player = 0;
            live_resource_scene.clear();
            local_wallet.store(0, std::memory_order_release);
            equipment_ready = false; owned_count = 0; equipment_player = 0; equipment_published = false;
            cooldown_timer_enabled = false; cooldown_timer_player = 0;
            stat_sample_player = 0; stat_sample_known.fill(false);
            state.verified = true;
            state.error = "Multiple active game managers were observed; native effects are disabled.";
            runtime.Observe(state, now);
            runtime.Pump(now);
            return;
        }
        if (scene_dirty.exchange(false, std::memory_order_acq_rel)) { scene.clear(); next_scene_read = 0; }
        if (!Observe(manager, state, now)) {
            state = {};
            state.verified = true;
            state.error = "Native state observation failed; native effects are disabled.";
        }
        // A sampled getter can enter managed/native game code. A teardown
        // nested in that observation has already cleared Runtime; never put
        // the old player back after the nested OnDestroy returns.
        if (stopping.load(std::memory_order_acquire)
            || frame_generation != destructions.load(std::memory_order_acquire)) return;
        local_player.store(state.safe_mode && state.player_ready ? state.player : 0, std::memory_order_release);
        pending_revival_player.store(state.safe_mode && state.revival_pending ? state.player : 0, std::memory_order_release);
        if (state.safe_mode && state.player_ready) {
            live_resource_player = state.player;
            live_resource_generation = frame_generation;
            live_resource_scene = state.scene;
        } else if (!state.revival_pending) {
            live_resource_player = 0;
            live_resource_scene.clear();
        }
        local_wallet.store(state.safe_mode ? state.wallet : 0, std::memory_order_release);
        if (!state.safe_mode || !state.player_ready) {
            equipment_ready = false; owned_count = 0; equipment_player = 0; equipment_published = false;
            stat_sample_player = 0; stat_sample_known.fill(false);
        }
        runtime.Observe(state, now);
        rewards.Tick();
        // Disables are immediate; enables are queued. Observe the effective
        // state on both sides of Pump so off/on between frames resets both
        // schedules instead of retaining an old long or short firing timer.
        SyncCooldownTimers(now);
        SyncResources(GetTickCount64());
        SyncOrdinaryStats(manager, GetTickCount64());
        if (stopping.load(std::memory_order_acquire)
            || frame_generation != destructions.load(std::memory_order_acquire)) return;
        runtime.Pump(GetTickCount64());
        SyncCooldownTimers(GetTickCount64());
        SyncResources(GetTickCount64());
        SyncOrdinaryStats(manager, GetTickCount64());
        if (frame_generation != destructions.load(std::memory_order_acquire)) return;
        const uint64_t speed_now = GetTickCount64();
        const float requested = !stopping.load(std::memory_order_acquire) && state.paused_known && !state.paused
            ? runtime.ActiveGameSpeed(state.player, speed_now) : 0;
        if (!SyncGameSpeed(requested, state.player))
            runtime.SetFeatureSupported(CombatFeature::GameSpeed, false, "Game speed could not be verified or restored safely.");
    }

    static void UpdateHook(void* manager, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->update_original) return;
        uint64_t destruction = self->destructions.load(std::memory_order_acquire);
        // Game exceptions from the original Update keep their original path.
        self->update_original(manager, method);
        try {
            if (self->stopping.load(std::memory_order_acquire)) { self->CleanupStopped(manager); return; }
            if (!self->stopping.load(std::memory_order_acquire)
                && destruction == self->destructions.load(std::memory_order_acquire)) self->Frame(manager, destruction);
        } catch (...) { self->Disarm("Native observation failed."); }
    }

    static void DestroyHook(void* manager, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->destroy_original) return;
        // The rooted stats wrappers are still valid before the game's native
        // teardown. Restore their normal bindings before dropping identity.
        self->RestoreResources(true);
        if (self->unity_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
            try { self->SyncGameSpeed(0); } catch (...) {}
        }
        self->rewards.Clear();
        self->destructions.fetch_add(1, std::memory_order_acq_rel);
        self->local_player.store(0, std::memory_order_release);
        self->pending_revival_player.store(0, std::memory_order_release);
        self->live_resource_player = 0;
        self->live_resource_scene.clear();
        self->local_wallet.store(0, std::memory_order_release);
        self->resource_player = 0;
        if (!self->resources[MagnetSlot].budget.active) {
            self->magnet_refresh_pending = false;
            self->magnet_owner = {};
        }
        for (auto& candidate : self->managers) {
            uintptr_t identity = reinterpret_cast<uintptr_t>(manager);
            candidate.compare_exchange_strong(identity, 0, std::memory_order_acq_rel);
        }
        try {
            if (!self->stopping.load(std::memory_order_acquire)) {
                self->equipment_ready = false;
                self->owned_count = 0;
                self->equipment_player = 0;
                self->equipment_published = false;
                self->cooldown_timer_enabled = false;
                self->cooldown_timer_player = 0;
                self->stat_sample_player = 0;
                self->stat_sample_known.fill(false);
                self->transient_stat_player = 0;
                self->max_health_synced = self->curse_synced = false;
                self->charm_synced = self->charm_enabled = false;
                self->curse_refresh_pending = false;
                self->next_curse_refresh = 0;
                self->runtime.DisableAll();
                GameObservation ended;
                ended.verified = true;
                ended.scene = "Waiting for game scene";
                self->runtime.Observe(ended, GetTickCount64());
                self->scene_dirty.store(true, std::memory_order_release);
            }
        } catch (...) { self->Disarm("Native teardown failed."); }
        self->scene_dirty.store(true, std::memory_order_release);
        // Always execute the original teardown, outside the trainer fence.
        self->destroy_original(manager, method);
    }

    static float ArmorHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->armor_original) return 0;
        float value = self->armor_original(owner, method);
        if (self->stopping.load(std::memory_order_acquire) || stat_read_mode == StatReadMode::Original) return value;
        if (stat_read_mode == StatReadMode::Effective)
            return self->runtime.FilterStat(NativeStat::Armor, reinterpret_cast<uintptr_t>(owner), value, GetTickCount64(), false);
        return self->runtime.FilterArmor(reinterpret_cast<uintptr_t>(owner), value, GetTickCount64());
    }

    bool LocalStatCallback(void* owner) const noexcept {
        return stat_read_mode != StatReadMode::Original && !stopping.load(std::memory_order_acquire)
            && unity_thread.load(std::memory_order_acquire) == GetCurrentThreadId()
            && owner && local_player.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(owner);
    }

    template <size_t Index>
    static float StatHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->stat_routes[Index].original) return 0;
        const float value = reinterpret_cast<ArmorFunction>(self->stat_routes[Index].original)(owner, method);
        if (!self->LocalStatCallback(owner)) return value;
        return self->runtime.FilterStat(kStatRoutes[Index].stat, reinterpret_cast<uintptr_t>(owner), value, GetTickCount64(), !sampling);
    }

    template <size_t Index>
    static float StatArgumentHook(void* owner, float parameter, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->stat_routes[Index].original) return 0;
        const float value = reinterpret_cast<StatArgumentFunction>(self->stat_routes[Index].original)(owner, parameter, method);
        if (!self->LocalStatCallback(owner)) return value;
        return self->runtime.FilterStatFinal(kStatRoutes[Index].stat, reinterpret_cast<uintptr_t>(owner), value, parameter, GetTickCount64(), !sampling);
    }

    bool LocalCallback(void* owner) const noexcept {
        return !sampling && !stopping.load(std::memory_order_acquire)
            && unity_thread.load(std::memory_order_acquire) == GetCurrentThreadId()
            && owner && local_player.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(owner);
    }

    static float MovementHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->movement_original) return 0;
        float value = self->movement_original(owner, method);
        if (!self->LocalStatCallback(owner)) return value;
        const uint64_t now = GetTickCount64();
        value = self->runtime.FilterStat(NativeStat::MoveSpeed, reinterpret_cast<uintptr_t>(owner), value, now, !sampling);
        return self->runtime.FilterMovement(reinterpret_cast<uintptr_t>(owner), value, now, !sampling);
    }

    static float IntervalHook(void* weapon, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->interval_original) return 0;
        float value = self->interval_original(weapon, method);
        if ((sampling && stat_read_mode != StatReadMode::Effective) || self->stopping.load(std::memory_order_acquire)
            || self->unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return value;
        try {
            if (!self->api.IsA(weapon, self->weapon_class)) return value;
            void* owner = nullptr;
            int32_t type = 0;
            self->api.field_get_value(weapon, self->equipment_owner_field, &owner);
            self->api.field_get_value(weapon, self->equipment_type_field, &type);
            if (!self->LocalStatCallback(owner) || !self->BasicWeapon(type)
                || !self->VirtualRouteMatches(weapon, self->interval_method, self->interval_entry)
                || !self->VirtualRouteMatches(weapon, self->reset_timer_method, self->reset_timer_entry)) return value;
            return self->runtime.FilterCooldown(reinterpret_cast<uintptr_t>(owner), value, true, GetTickCount64(), !sampling);
        } catch (...) { self->Disarm("Native cooldown observation failed."); return value; }
    }

    static void HealthHook(void* owner, float input, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->health_original) return;
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        float before = 0, effective = input;
        bool observed = false;
        try {
            observed = self->LocalCallback(owner) && self->FloatField(owner, self->character_class, self->player_hp_field, before);
            if (observed && before > 0)
                effective = self->runtime.FilterHealthDamage(reinterpret_cast<uintptr_t>(owner), input, GetTickCount64());
        } catch (...) { effective = input; observed = false; self->Disarm("Native health observation failed."); }
        // Retain the game's normal hit feedback and invulnerability callbacks.
        // Only the positive-damage argument is filtered; no HP is restored.
        self->health_original(owner, effective, method);
        try {
            float after = 0;
            if (observed && generation == self->destructions.load(std::memory_order_acquire)
                && self->LocalCallback(owner) && self->FloatField(owner, self->character_class, self->player_hp_field, after))
                self->runtime.ObserveHealthResult(reinterpret_cast<uintptr_t>(owner), before, after, GetTickCount64());
        } catch (...) { self->Disarm("Native health result observation failed."); }
    }

    static void ExperienceHook(void* owner, float input, int32_t mode, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->experience_original) return;
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        float before = 0, effective = input;
        int32_t level_before = 0;
        bool observed = false;
        try {
            observed = self->LocalCallback(owner) && self->FloatField(owner, self->character_class, self->player_xp_field, before);
            if (observed) {
                self->api.field_get_value(owner, self->player_level_field, &level_before);
                effective = self->runtime.FilterExperience(reinterpret_cast<uintptr_t>(owner), input, GetTickCount64());
            }
        } catch (...) { effective = input; observed = false; self->Disarm("Native experience observation failed."); }
        // The native multiplier mode, growth rules, level-up and native UI run
        // unchanged after the explicit incoming-amount multiplier.
        self->experience_original(owner, effective, mode, method);
        try {
            float after = 0;
            if (observed && generation == self->destructions.load(std::memory_order_acquire)
                && self->LocalCallback(owner) && self->FloatField(owner, self->character_class, self->player_xp_field, after)) {
                int32_t level_after = 0;
                self->api.field_get_value(owner, self->player_level_field, &level_after);
                self->runtime.ObserveExperienceResult(reinterpret_cast<uintptr_t>(owner), before, after, level_before, level_after, GetTickCount64());
            }
        } catch (...) { self->Disarm("Native experience result observation failed."); }
    }

    uintptr_t DamageOwner() const noexcept {
        if (sampling || stopping.load(std::memory_order_acquire)
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return 0;
        return local_player.load(std::memory_order_acquire);
    }

    bool ReadOwnedEnemyHit(void* enemy, float input, int32_t weapon_type, float& hp) const {
        if (!std::isfinite(input) || input <= 0 || !OwnWeapon(weapon_type)
            || !FloatField(enemy, enemy_class, enemy_hp_field, hp) || hp <= 0) return false;
        bool dead = true;
        api.field_get_value(enemy, enemy_dead_field, &dead);
        return !dead && !enemy_is_bullet(enemy, enemy_bullet_method);
    }

    bool UsesReaperDamage(void* enemy) const {
        if (!reaper_damage.created || !enemy || api.object_get_class(enemy) != reaper_class) return false;
        constexpr int32_t red_death = 60, red_death_variant = 245;
        int32_t type = -1;
        api.field_get_value(enemy, enemy_type_field, &type);
        return (type == red_death || type == red_death_variant)
            && VirtualRouteMatches(enemy, reaper_damage.method, reaper_damage.entry);
    }

    bool ReadEnemyHitResult(void* enemy, uintptr_t owner, uint64_t generation, float& hp) const {
        return generation == destructions.load(std::memory_order_acquire)
            && owner == local_player.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire)
            && FloatField(enemy, enemy_class, enemy_hp_field, hp);
    }

    static void DamageHook(void* enemy, float input, int32_t hit_vfx, float knockback,
        int32_t weapon_type, bool has_knockback, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->damage_original) return;
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        const uintptr_t owner = self->DamageOwner();
        float before = 0, effective = input;
        bool observed = false, reaper = false;
        try {
            if (owner && self->ReadOwnedEnemyHit(enemy, input, weapon_type, before)) {
                observed = true;
                reaper = self->UsesReaperDamage(enemy);
                const uint64_t now = GetTickCount64();
                effective = self->runtime.FilterDamage(owner, input, true, now);
                if (!reaper) effective = self->runtime.FilterInstantKill(owner, effective, before, true, now);
            }
        } catch (...) { effective = input; observed = false; self->Disarm("Native enemy observation failed."); }
        // Knockback, visuals, native invulnerability and reward/death processing
        // retain all original arguments and the original call.
        self->damage_original(enemy, effective, hit_vfx, knockback, weapon_type, has_knockback, method);
        try {
            float after = 0;
            if (observed && self->ReadEnemyHitResult(enemy, owner, generation, after)) {
                const uint64_t now = GetTickCount64();
                self->runtime.ObserveDamageResult(owner, before, after, true, now);
                if (!reaper) self->runtime.ObserveInstantKillResult(owner, before, after, true, now);
            }
        } catch (...) { self->Disarm("Native enemy result observation failed."); }
    }

    static void ReaperDamageHook(void* enemy, float input, int32_t weapon_type, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->reaper_damage.original) return;
        auto original = reinterpret_cast<DealDamageFunction>(self->reaper_damage.original);
        const uintptr_t owner = self->DamageOwner();
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        float before = 0, effective = input;
        bool observed = false;
        try {
            observed = owner && self->UsesReaperDamage(enemy) && self->ReadOwnedEnemyHit(enemy, input, weapon_type, before);
            if (observed) effective = self->runtime.FilterInstantKill(owner, input, before, true, GetTickCount64(), /*final_damage=*/true);
        } catch (...) { self->Disarm("Native Reaper hit observation failed."); }
        // Both native hit routes reach this subtraction after resistance. The
        // original still invokes the Reaper's own death routine and callbacks.
        original(enemy, effective, weapon_type, method);
        try {
            float after = 0;
            if (observed && self->ReadEnemyHitResult(enemy, owner, generation, after))
                self->runtime.ObserveInstantKillResult(owner, before, after, true, GetTickCount64());
        } catch (...) { self->Disarm("Native Reaper result observation failed."); }
    }

    bool LocalResourceCallback(void* owner, bool character = false, bool mutation = false) const noexcept {
        return (mutation || !stopping.load(std::memory_order_acquire)) && unity_thread.load(std::memory_order_acquire) == GetCurrentThreadId()
            && owner && resource_player && (mutation || resource_player == local_player.load(std::memory_order_acquire)
                || (character && resource_player == pending_revival_player.load(std::memory_order_acquire)))
            && (character ? reinterpret_cast<uintptr_t>(owner) == resource_player : owner == resource_owner.Get());
    }

    void EnterResourceMutation(bool magnet) noexcept {
        auto& depth = magnet ? magnet_native_depth : resource_native_depth;
        if (depth++ != 0) return;
        const size_t begin = magnet ? MagnetSlot : 0, end = magnet ? ResourceSlotCount : MagnetSlot;
        for (size_t slot = begin; slot < end; ++slot) {
            try { resources[slot].budget.Suspend([&] { return ReadResource(slot); },
                [&](const ResourceValue& value) { WriteResource(slot, value); }); }
            catch (...) { FailResource(slot); }
        }
    }

    void ExitResourceMutation(bool magnet) noexcept {
        auto& depth = magnet ? magnet_native_depth : resource_native_depth;
        if (depth == 0 || --depth != 0) return;
        const size_t begin = magnet ? MagnetSlot : 0, end = magnet ? ResourceSlotCount : MagnetSlot;
        for (size_t slot = begin; slot < end; ++slot) {
            try { resources[slot].budget.Resume([&] { return ReadResource(slot); },
                [&](const ResourceValue& value) { WriteResource(slot, value); }); }
            catch (...) { FailResource(slot); }
        }
        next_resource_sample = 0;
        if (!magnet && resources[CharmSlot].budget.active) {
            curse_refresh_pending = true;
            next_curse_refresh = 0;
        }
        if (!magnet && !magnet_native_depth && resource_player) {
            const uint64_t generation = destructions.load(std::memory_order_acquire);
            const bool revival_only = !local_player.load(std::memory_order_acquire)
                && pending_revival_player.load(std::memory_order_acquire) == resource_player;
            try {
                if (CurrentResourceActor(resource_player, generation, revival_only)) {
                    void* owner = nullptr;
                    StatSamplingScope sample(StatReadMode::Original);
                    if (api.Invoke(player_stats_method, reinterpret_cast<void*>(resource_player), nullptr, owner)
                        && api.IsA(owner, player_stats_class)) RebindStatsOwner(owner, resource_player, GetTickCount64());
                }
            } catch (...) { for (size_t slot = 0; slot < MagnetSlot; ++slot) FailResource(slot); }
        }
        if (magnet && resources[MagnetSlot].budget.active && !stopping.load(std::memory_order_acquire)) {
            magnet_refresh_pending = true;
            try {
                if (!RefreshMagnetFor(reinterpret_cast<void*>(resource_player))) FailResource(MagnetSlot);
            } catch (...) { FailResource(MagnetSlot); }
        }
    }

    struct ResourceMutationScope {
        Impl* self;
        bool magnet;
        ResourceMutationScope(Impl* value, bool is_magnet) noexcept : self(value), magnet(is_magnet) {
            if (self) self->EnterResourceMutation(magnet);
        }
        ~ResourceMutationScope() { if (self) self->ExitResourceMutation(magnet); }
    };

    template <ExtendedHook Index>
    static void ResourceUpgradeHook(void* owner, void* modifier, bool eggs, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[Index].original) return;
        if constexpr (Index == PlayerUpgrade) {
            Impl* context = self->LocalResourceCallback(owner, true, true) ? self : nullptr;
            ResourceMutationScope stats_scope(context, false);
            ResourceMutationScope magnet_scope(context, true);
            reinterpret_cast<UpgradeFunction>(self->extra_routes[Index].original)(owner, modifier, eggs, method);
        } else {
            ResourceMutationScope scope(self->LocalResourceCallback(owner, false, true) ? self : nullptr, false);
            reinterpret_cast<UpgradeFunction>(self->extra_routes[Index].original)(owner, modifier, eggs, method);
        }
    }

    static void ResourceSetHook(void* owner, void* modifier, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[StatsSet].original) return;
        ResourceMutationScope scope(self->LocalResourceCallback(owner, false, true) ? self : nullptr, false);
        reinterpret_cast<SetFunction>(self->extra_routes[StatsSet].original)(owner, modifier, method);
    }

    static void ResourceResetHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[StatsReset].original) return;
        ResourceMutationScope scope(self->LocalResourceCallback(owner, false, true) ? self : nullptr, false);
        reinterpret_cast<UpdateFunction>(self->extra_routes[StatsReset].original)(owner, method);
    }

    static void CharacterResetHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[PlayerReset].original) return;
        Impl* context = self->LocalResourceCallback(owner, true, true) ? self : nullptr;
        ResourceMutationScope stats_scope(context, false);
        ResourceMutationScope magnet_scope(context, true);
        reinterpret_cast<UpdateFunction>(self->extra_routes[PlayerReset].original)(owner, method);
    }

    static int32_t CharmHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[CharmRead].original) return 0;
        const int32_t value = reinterpret_cast<IntFunction>(self->extra_routes[CharmRead].original)(owner, method);
        if (sampling || self->resource_native_depth || !self->LocalResourceCallback(owner)) return value;
        const auto& budget = self->resources[CharmSlot].budget;
        const float before = budget.active ? static_cast<float>(budget.original.scalar) : static_cast<float>(value);
        self->runtime.ObserveStatRead(NativeStat::Charm, self->resource_player, before, static_cast<float>(value),
            budget.active && !budget.suspended, GetTickCount64());
        return value;
    }

    template <ExtendedHook Index, ResourceSlot Slot>
    static void* ResourceObjectHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[Index].original) return nullptr;
        void* result = reinterpret_cast<ObjectFunction>(self->extra_routes[Index].original)(owner, method);
        if (sampling || self->resource_native_depth || !self->LocalResourceCallback(owner, Index == RevivalsRead)) return result;
        try {
            auto& budget = self->resources[Slot].budget;
            ResourceValue current; current.object = StrongRoot(self->api, result);
            float before = 0, after = 0;
            if (self->ResourceNumber(Slot, budget.active ? budget.original : current, before)
                && self->ResourceNumber(Slot, current, after)) self->runtime.ObserveStatRead(SlotStat(Slot),
                    self->resource_player, before, after, budget.active && !budget.suspended, GetTickCount64());
        } catch (...) { self->FailResource(Slot); }
        return result;
    }

    template <ExtendedHook Index, ResourceSlot Slot>
    static float ResourceFloatHook(void* owner, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[Index].original) return 0;
        const float result = reinterpret_cast<ArmorFunction>(self->extra_routes[Index].original)(owner, method);
        if (sampling || self->resource_native_depth || !self->LocalResourceCallback(owner)) return result;
        const auto& budget = self->resources[Slot].budget;
        const float before = budget.active ? static_cast<float>(budget.original.scalar) : result;
        self->runtime.ObserveStatRead(SlotStat(Slot), self->resource_player, before, result, budget.active && !budget.suspended, GetTickCount64());
        return result;
    }

    bool ReadMainConfig(void* options, void*& main) const {
        main = nullptr;
        if (!wallet_ready || !api.IsA(options, options_class)) return false;
        void* config = nullptr;
        StatSamplingScope scope(StatReadMode::Original);
        return api.Invoke(main_config_method, options, nullptr, main) && api.IsA(main, options_data_class)
            && api.Invoke(config_method, options, nullptr, config) && config == main;
    }

    bool ReadMenuWallet(void* options, GoldWallet& wallet) const {
        bool in_adventure = false;
        if (in_adventure_method && !api.Boolean(in_adventure_method, nullptr, in_adventure)) return false;
        void* config = nullptr;
        int32_t adventure = -1;
        if (!in_adventure) {
            if (!ReadMainConfig(options, config)) return false;
        } else {
            if (!adventure_wallet_ready) return false;
            void* manager = nullptr;
            api.field_get_value(options, adventure_manager_field, &manager);
            if (!api.IsA(manager, adventure_manager_class)) return false;
            void* owner = nullptr;
            api.field_get_value(manager, adventure_options_field, &owner);
            if (owner != options) return false;
            api.field_get_value(manager, current_adventure_field, &adventure);
            void* active_config = nullptr;
            void* manager_config = nullptr;
            void* main_config = nullptr;
            void* selected = nullptr;
            if (adventure < 0 || adventure > 9
                || !api.Invoke(adventure_config_method, options, nullptr, config) || !api.IsA(config, options_data_class)
                || !api.Invoke(manager_adventure_config_method, manager, nullptr, manager_config) || manager_config != config
                || !api.Invoke(config_method, options, nullptr, active_config) || active_config != config
                || !api.Invoke(main_config_method, options, nullptr, main_config) || !api.IsA(main_config, options_data_class)
                || main_config == config
                || !api.Invoke(selected_adventure_method, config, nullptr, selected) || !api.IsA(selected, adventure_type_class)) return false;
            const auto* saved_adventure = static_cast<const int32_t*>(api.object_unbox(selected));
            if (!IsReadableMemory(saved_adventure, sizeof(*saved_adventure)) || *saved_adventure != adventure) return false;
        }
        wallet = {reinterpret_cast<uintptr_t>(options), reinterpret_cast<uintptr_t>(config), adventure};
        return true;
    }

    bool ReadCoins(void* config, double& value) const {
        if (!coins_method || !api.IsA(config, options_data_class)) return false;
        void* box = nullptr;
        StatSamplingScope scope(StatReadMode::Original);
        if (!api.Invoke(coins_method, config, nullptr, box) || !box) return false;
        void* data = api.object_unbox(box);
        if (!IsReadableMemory(data, sizeof(float))) return false;
        value = *static_cast<float*>(data);
        return std::isfinite(value);
    }

    bool MainWallet(void* options, void* explicit_config = nullptr) {
        if (!wallet_ready || writing_gold || stopping.load(std::memory_order_acquire) || !options
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()
            || local_wallet.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(options)
            || !api.IsA(options, options_class)) return false;
        void* main = nullptr;
        if (!ReadMainConfig(options, main) || (explicit_config && explicit_config != main)) return false;
        if (scene == "MainMenu") {
            GoldWallet menu;
            // A menu can switch to an online lobby between 250 ms samples.
            // Recheck the singleton's live mode at the deduction itself.
            return MenuContext(nullptr, menu) && menu.adventure < 0 && menu.options == reinterpret_cast<uintptr_t>(options);
        }
        return true;
    }

    bool GoldWriteContext(const GoldWallet& expected, uint64_t generation) {
        if (!gold_writer_ready || stopping.load(std::memory_order_acquire) || generation != destructions.load(std::memory_order_acquire)
            || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()
            || manager_overflow.load(std::memory_order_acquire)) return false;
        next_scene_read = 0;
        if (!ReadScene(GetTickCount64()) || scene != "MainMenu") return false;
        GoldWallet current;
        return MenuContext(nullptr, current) && current == expected
            && generation == destructions.load(std::memory_order_acquire);
    }

    bool SetWalletCoins(const GoldWallet& wallet, double desired, uint64_t generation) {
        if (!GoldWriteContext(wallet, generation)) return false;
        auto* config = reinterpret_cast<void*>(wallet.config);
        float amount = static_cast<float>(desired);
        void* args[]{&amount};
        void* result = nullptr;
        double actual = 0;
        return api.Invoke(set_coins_method, config, args, result) && GoldWriteContext(wallet, generation)
            && ReadCoins(config, actual) && actual == desired;
    }

    bool ChangeGold(const GoldWallet& wallet, double desired, uint64_t generation) {
        if (wallet.adventure >= 0) return SetWalletCoins(wallet, desired, generation);
        auto* options = reinterpret_cast<void*>(wallet.options);
        auto* config = reinterpret_cast<void*>(wallet.config);
        double current = 0;
        if (!GoldWriteContext(wallet, generation) || !ReadCoins(config, current)) return false;
        GoldPlan plan;
        if (!PlanGoldChange(current, desired, plan)) return false;
        for (size_t index = 0; index < plan.count; ++index) {
            const GoldStep& step = plan.steps[index];
            if (!GoldWriteContext(wallet, generation)) return false;
            float amount = step.amount;
            bool flag = false;
            void* add_args[]{&amount};
            void* remove_args[]{&amount, &flag};
            void* result = nullptr;
            const bool adding = step.operation == GoldOperation::Add;
            if (!api.Invoke(adding ? add_coins_method : extra_routes[GoldFloat].method,
                options, adding ? add_args : remove_args, result)
                || !GoldWriteContext(wallet, generation)
                || !ReadCoins(config, current) || current != static_cast<double>(step.expected)) return false;
        }
        return GoldWriteContext(wallet, generation);
    }

    bool SaveGold(const GoldWallet& wallet, uint64_t generation) {
        if (!GoldWriteContext(wallet, generation)) return false;
        bool commit = true, backup = true;
        void* args[]{&commit, &backup};
        void* result = nullptr;
        return api.Invoke(save_method, reinterpret_cast<void*>(wallet.options), args, result) && GoldWriteContext(wallet, generation);
    }

    static bool WriteGold(void* context, const GoldWallet& wallet, double desired, double& actual, std::string& error) {
        auto* self = static_cast<Impl*>(context);
        if (!self || !self->resource_api_ready || !std::isfinite(desired) || desired < 0 || desired > 999999999
            || desired != std::trunc(desired) || static_cast<double>(static_cast<float>(desired)) != desired) {
            error = "The gold value cannot be represented exactly by the native wallet."; return false;
        }
        const uint64_t generation = self->destructions.load(std::memory_order_acquire);
        if (!self->GoldWriteContext(wallet, generation)) {
            error = "The selected gold wallet changed."; return false;
        }
        auto* options = reinterpret_cast<void*>(wallet.options);
        auto* config = reinterpret_cast<void*>(wallet.config);
        StrongRoot options_root(self->api, options), config_root(self->api, config);
        double before = 0;
        if (options_root.Get() != options || config_root.Get() != config
            || !self->ReadCoins(config, before) || before < 0) { error = "The native wallet balance is unavailable."; return false; }
        actual = before;
        struct WritingScope {
            Impl& self;
            explicit WritingScope(Impl& value) : self(value) { self.writing_gold = true; }
            ~WritingScope() { self.writing_gold = false; }
        } writing(*self);
        bool success = false;
        try {
            success = self->ChangeGold(wallet, desired, generation)
                && self->ReadCoins(config_root.Get(), actual) && actual == desired
                && self->SaveGold(wallet, generation)
                && self->ReadCoins(config_root.Get(), actual) && actual == desired;
        } catch (...) { success = false; }
        if (success) return true;
        bool restored = false;
        try {
            bool balance_restored = self->ChangeGold(wallet, before, generation);
            // An ordinary wallet's float subtraction can lose a small prior balance.
            if (!balance_restored && wallet.adventure < 0) balance_restored = self->SetWalletCoins(wallet, before, generation);
            restored = balance_restored && self->SaveGold(wallet, generation)
                && self->ReadCoins(config_root.Get(), actual) && actual == before;
        } catch (...) { restored = false; }
        error = restored ? "The native gold update failed; the previous balance was restored and saved."
            : "The native gold update failed and its previous balance could not be confirmed restored.";
        return false;
    }

    template <ExtendedHook Index>
    static void GoldSpendHook(void* options, float amount, bool flag, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[Index].original) return;
        float effective = amount;
        try { if (!sampling) effective = self->runtime.FilterGoldSpend(reinterpret_cast<uintptr_t>(options), amount,
            self->MainWallet(options), GetTickCount64()); } catch (...) { effective = amount; }
        reinterpret_cast<GoldFloatFunction>(self->extra_routes[Index].original)(options, effective, flag, method);
    }

    static void GoldSpendIntHook(void* options, int32_t amount, bool flag, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[GoldInt].original) return;
        int32_t effective = amount;
        try { if (!sampling && amount > 0 && self->runtime.FilterGoldSpend(reinterpret_cast<uintptr_t>(options),
            static_cast<float>(amount), self->MainWallet(options), GetTickCount64()) == 0) effective = 0; } catch (...) {}
        reinterpret_cast<GoldIntFunction>(self->extra_routes[GoldInt].original)(options, effective, flag, method);
    }

    static void GoldSpendConfigHook(void* options, int32_t amount, bool flag, void* config, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[GoldConfig].original) return;
        int32_t effective = amount;
        try { if (!sampling && amount > 0 && config && self->runtime.FilterGoldSpend(reinterpret_cast<uintptr_t>(options),
            static_cast<float>(amount), self->MainWallet(options, config), GetTickCount64()) == 0) effective = 0; } catch (...) {}
        reinterpret_cast<GoldConfigFunction>(self->extra_routes[GoldConfig].original)(options, effective, flag, config, method);
    }

    static float GoldSpendFlatHook(void* options, float amount, const void* method) {
        Impl* self = active.load(std::memory_order_acquire);
        if (!self || !self->extra_routes[GoldFlat].original) return 0;
        float effective = amount;
        try { if (!sampling) effective = self->runtime.FilterGoldSpend(reinterpret_cast<uintptr_t>(options), amount,
            self->MainWallet(options), GetTickCount64()); } catch (...) { effective = amount; }
        return reinterpret_cast<StatArgumentFunction>(self->extra_routes[GoldFlat].original)(options, effective, method);
    }

    bool Hook(std::string& error) {
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) { error = "Cannot initialize the native hook engine."; return false; }
        update_created = MH_CreateHook(update_entry, reinterpret_cast<void*>(&UpdateHook), reinterpret_cast<void**>(&update_original)) == MH_OK;
        if (update_created) destroy_created = MH_CreateHook(destroy_entry, reinterpret_cast<void*>(&DestroyHook), reinterpret_cast<void**>(&destroy_original)) == MH_OK;
        if (destroy_created) armor_created = MH_CreateHook(armor_entry, reinterpret_cast<void*>(&ArmorHook), reinterpret_cast<void**>(&armor_original)) == MH_OK;
        if (!update_created || !destroy_created || !armor_created) { error = "Cannot prepare all native game hooks."; return false; }
        if (menu_metadata_ready) menu_tick_route.created = MH_CreateHook(menu_tick_route.entry,
            reinterpret_cast<void*>(&MenuTickHook), &menu_tick_route.original) == MH_OK;
        if (menu_metadata_ready) menu_update_route.created = MH_CreateHook(menu_update_route.entry,
            reinterpret_cast<void*>(&MenuUpdateHook), &menu_update_route.original) == MH_OK;
        menu_metadata_ready = menu_metadata_ready && menu_tick_route.created && menu_update_route.created;
        if (chest_animation_ready) chest_animation_update.created = MH_CreateHook(chest_animation_update.entry,
            reinterpret_cast<void*>(&ChestAnimationUpdateHook), &chest_animation_update.original) == MH_OK;
        if (health_ready) {
            health_created = MH_CreateHook(health_entry, reinterpret_cast<void*>(&HealthHook), reinterpret_cast<void**>(&health_original)) == MH_OK;
            health_ready = health_created;
        }
        if (experience_ready) {
            experience_created = MH_CreateHook(experience_entry, reinterpret_cast<void*>(&ExperienceHook), reinterpret_cast<void**>(&experience_original)) == MH_OK;
            experience_ready = experience_created;
        }
        if (damage_ready) {
            damage_created = MH_CreateHook(damage_entry, reinterpret_cast<void*>(&DamageHook), reinterpret_cast<void**>(&damage_original)) == MH_OK;
            damage_ready = damage_created;
        }
        if (damage_ready && reaper_damage.entry) reaper_damage.created = MH_CreateHook(reaper_damage.entry,
            reinterpret_cast<void*>(&ReaperDamageHook), &reaper_damage.original) == MH_OK;
        if (movement_ready) {
            movement_created = MH_CreateHook(movement_entry, reinterpret_cast<void*>(&MovementHook), reinterpret_cast<void**>(&movement_original)) == MH_OK;
            movement_ready = movement_created;
        }
        if (cooldown_ready) {
            interval_created = MH_CreateHook(interval_entry, reinterpret_cast<void*>(&IntervalHook), reinterpret_cast<void**>(&interval_original)) == MH_OK;
            cooldown_ready = interval_created;
        }
        const std::array<void*, kStatRoutes.size()> stat_detours{{
            reinterpret_cast<void*>(&StatHook<0>), reinterpret_cast<void*>(&StatHook<1>),
            reinterpret_cast<void*>(&StatHook<2>), reinterpret_cast<void*>(&StatHook<3>),
            reinterpret_cast<void*>(&StatHook<4>), reinterpret_cast<void*>(&StatHook<5>),
            reinterpret_cast<void*>(&StatHook<6>), reinterpret_cast<void*>(&StatHook<7>),
            reinterpret_cast<void*>(&StatHook<8>), reinterpret_cast<void*>(&StatHook<9>),
            reinterpret_cast<void*>(&StatHook<10>), reinterpret_cast<void*>(&StatHook<11>),
            reinterpret_cast<void*>(&StatHook<12>), reinterpret_cast<void*>(&StatHook<13>),
            reinterpret_cast<void*>(&StatHook<14>), reinterpret_cast<void*>(&StatArgumentHook<15>),
            reinterpret_cast<void*>(&StatArgumentHook<16>)
        }};
        for (size_t index = 0; index < stat_routes.size(); ++index) {
            auto& route = stat_routes[index];
            if (!route.entry || stat_route_collision[StatIndex(kStatRoutes[index].stat)]) continue;
            bool shared = false;
            for (size_t previous = 0; previous < index; ++previous)
                if (stat_routes[previous].entry == route.entry && kStatRoutes[previous].stat == kStatRoutes[index].stat) {
                    route.created = stat_routes[previous].created;
                    route.original = stat_routes[previous].original;
                    shared = true;
                    break;
                }
            if (!shared) route.created = MH_CreateHook(route.entry, stat_detours[index], &route.original) == MH_OK;
        }
        const std::array<void*, ExtendedHookCount> extra_detours{{
            reinterpret_cast<void*>(&CharmHook),
            reinterpret_cast<void*>(&ResourceObjectHook<RerollsRead, RerollSlot>),
            reinterpret_cast<void*>(&ResourceObjectHook<SkipsRead, SkipSlot>),
            reinterpret_cast<void*>(&ResourceObjectHook<BanishesRead, BanishSlot>),
            reinterpret_cast<void*>(&ResourceObjectHook<RevivalsRead, RevivalSlot>),
            reinterpret_cast<void*>(&ResourceFloatHook<PreserveRead, PreserveSlot>),
            reinterpret_cast<void*>(&ResourceFloatHook<DefangRead, DefangSlot>),
            reinterpret_cast<void*>(&ResourceUpgradeHook<StatsUpgrade>), reinterpret_cast<void*>(&ResourceSetHook),
            reinterpret_cast<void*>(&ResourceResetHook), reinterpret_cast<void*>(&ResourceUpgradeHook<PlayerUpgrade>),
            reinterpret_cast<void*>(&CharacterResetHook),
            reinterpret_cast<void*>(&GoldSpendHook<GoldFloat>), reinterpret_cast<void*>(&GoldSpendIntHook),
            reinterpret_cast<void*>(&GoldSpendConfigHook), reinterpret_cast<void*>(&GoldSpendFlatHook)
        }};
        for (size_t index = 0; index < extra_routes.size(); ++index) {
            auto& route = extra_routes[index];
            if (route.entry) route.created = MH_CreateHook(route.entry, extra_detours[index], &route.original) == MH_OK;
        }
        wallet_ready = wallet_ready && extra_routes[GoldFloat].created && extra_routes[GoldInt].created
            && extra_routes[GoldConfig].created && extra_routes[GoldFlat].created;
        gold_writer_ready = gold_writer_ready && wallet_ready && menu_metadata_ready;
        if (!rewards.Prepare(error)) return false;
        Impl* absent = nullptr;
        if (!active.compare_exchange_strong(absent, this, std::memory_order_acq_rel)) { error = "A native game backend already exists."; return false; }
        // Publish startup capabilities before Update can make a stricter
        // current-character/equipment decision. Never overwrite that decision
        // from the startup worker after callbacks have begun.
        runtime.SetArmorSupported(true);
        runtime.SetFeatureSupported(CombatFeature::Health, health_ready,
            health_ready ? "" : "The version-verified native health entry is unavailable.");
        runtime.SetFeatureSupported(CombatFeature::Experience, experience_ready,
            experience_ready ? "" : "The version-verified native experience entry is unavailable.");
        runtime.SetFeatureSupported(CombatFeature::Damage, damage_ready,
            damage_ready ? "" : "The version-verified native damage or equipment entry is unavailable.");
        runtime.SetFeatureSupported(CombatFeature::InstantKill, damage_ready,
            damage_ready ? "" : "The version-verified native damage or equipment entry is unavailable.");
        const bool movement_supported = movement_ready && !stat_route_collision[StatIndex(NativeStat::MoveSpeed)];
        runtime.SetFeatureSupported(CombatFeature::Movement, movement_supported,
            movement_supported ? "" : "The version-verified native movement entry is unavailable or ambiguous.");
        runtime.SetFeatureSupported(CombatFeature::Cooldown, cooldown_ready,
            cooldown_ready ? "" : "The version-verified native interval, timer or equipment entry is unavailable.");
        runtime.SetFeatureSupported(CombatFeature::NoGoldSpend, wallet_ready,
            wallet_ready ? "" : "The complete native ordinary-wallet deduction routes are unavailable.");
        const bool game_speed_ready = time_scale_getter && time_scale_setter;
        runtime.SetFeatureSupported(CombatFeature::GameSpeed, game_speed_ready,
            game_speed_ready ? "" : "The version-verified Unity game speed methods are unavailable.");
        runtime.SetFeatureSupported(CombatFeature::SkipChestAnimation, chest_animation_update.created,
            chest_animation_update.created ? "" : "The version-verified chest animation skip route is unavailable.");
        if (gold_writer_ready) runtime.SetGoldWriter(&WriteGold, this);
        for (size_t index = 0; index < kNativeStatCount; ++index) {
            const NativeStat stat = static_cast<NativeStat>(index);
            PublishStatSupported(stat, StatGroupReady(stat), "The version-verified ordinary-stat entries or maintenance route are unavailable.", true);
        }
        if (MH_QueueEnableHook(update_entry) != MH_OK || MH_QueueEnableHook(destroy_entry) != MH_OK
            || MH_QueueEnableHook(armor_entry) != MH_OK
            || (health_created && MH_QueueEnableHook(health_entry) != MH_OK)
            || (experience_created && MH_QueueEnableHook(experience_entry) != MH_OK)
            || (damage_created && MH_QueueEnableHook(damage_entry) != MH_OK)
            || (reaper_damage.created && MH_QueueEnableHook(reaper_damage.entry) != MH_OK)
            || (movement_created && MH_QueueEnableHook(movement_entry) != MH_OK)
            || (interval_created && MH_QueueEnableHook(interval_entry) != MH_OK)
            || (menu_tick_route.created && MH_QueueEnableHook(menu_tick_route.entry) != MH_OK)
            || (chest_animation_update.created && MH_QueueEnableHook(chest_animation_update.entry) != MH_OK)
            || (menu_update_route.created && MH_QueueEnableHook(menu_update_route.entry) != MH_OK)) {
            error = "Cannot enable the complete native hook set."; return false;
        }
        for (size_t index = 0; index < stat_routes.size(); ++index) {
            const auto& route = stat_routes[index];
            if (!route.created) continue;
            bool shared = false;
            for (size_t previous = 0; previous < index; ++previous)
                if (stat_routes[previous].created && stat_routes[previous].entry == route.entry) shared = true;
            if (!shared && MH_QueueEnableHook(route.entry) != MH_OK) {
                error = "Cannot enable an ordinary-stat hook."; return false;
            }
        }
        for (const auto& route : extra_routes) if (route.created && MH_QueueEnableHook(route.entry) != MH_OK) {
            error = "Cannot enable an extended native hook."; return false;
        }
        if (!rewards.Enable(error)) return false;
        if (MH_ApplyQueued() != MH_OK) { error = "Cannot activate the complete native hook set."; return false; }
        hooks_live.store(true, std::memory_order_release);
        return true;
    }

    void CleanupStopped(void* manager) noexcept {
        if (writing_gold || unity_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return;
        try { SyncGameSpeed(0); } catch (...) {}
        rewards.Clear();
        RestoreResources(true);
        if (!magnet_refresh_pending || !magnet_owner.Get() || !magnet_metadata_ready) return;
        try {
            const uint64_t generation = destructions.load(std::memory_order_acquire);
            void* player = nullptr;
            void* zone = nullptr;
            bool dead = true;
            StatSamplingScope scope(StatReadMode::Original);
            // Read a fresh live owner through the current manager. A cached
            // character from a previous scene is never dereferenced here.
            if (!api.IsA(manager, manager_class) || !api.Invoke(player_method, manager, nullptr, player)
                || reinterpret_cast<uintptr_t>(player) != resource_player || !api.IsA(player, character_class)
                || !api.Boolean(dead_method, player, dead) || dead
                || !api.Invoke(magnet_method, player, nullptr, zone) || zone != magnet_owner.Get()
                || generation != destructions.load(std::memory_order_acquire)) return;
            void* result = nullptr;
            if (api.Invoke(refresh_magnet_method, zone, nullptr, result)) {
                magnet_refresh_pending = false;
                magnet_owner = {};
            }
        } catch (...) {}
    }

    void Disarm(const char* reason) noexcept {
        // This atomic gate is authoritative even if reporting/disabling itself
        // encounters a C++ allocation or synchronization exception.
        stopping.store(true, std::memory_order_release);
        local_player.store(0, std::memory_order_release);
        pending_revival_player.store(0, std::memory_order_release);
        local_wallet.store(0, std::memory_order_release);
        try { runtime.SetGoldWriter(nullptr, nullptr); } catch (...) { }
        try { runtime.DisableAll(); } catch (...) { }
        try { runtime.SetArmorSupported(false, reason); } catch (...) { }
        for (CombatFeature feature : {CombatFeature::Health, CombatFeature::Experience, CombatFeature::Damage,
            CombatFeature::Movement, CombatFeature::Cooldown, CombatFeature::InstantKill, CombatFeature::NoGoldSpend,
            CombatFeature::GameSpeed, CombatFeature::SkipChestAnimation}) {
            try { runtime.SetFeatureSupported(feature, false, reason); } catch (...) { }
        }
        for (size_t index = 0; index < kNativeStatCount; ++index)
            try { runtime.SetStatSupported(static_cast<NativeStat>(index), false, reason); } catch (...) { }
    }

    void Stop() noexcept {
        Disarm("The native backend is stopped.");
        rewards.Stop();
        // Also erase queued enables left by a partially failed activation.
        // After activation these two callbacks remain resident solely for
        // main-thread restoration; Stop's worker never touches Unity objects.
        const bool cleanup_live = hooks_live.load(std::memory_order_acquire);
        if (update_created && !cleanup_live) { MH_QueueDisableHook(update_entry); MH_DisableHook(update_entry); }
        if (destroy_created && !cleanup_live) { MH_QueueDisableHook(destroy_entry); MH_DisableHook(destroy_entry); }
        if (armor_created) { MH_QueueDisableHook(armor_entry); MH_DisableHook(armor_entry); }
        if (health_created) { MH_QueueDisableHook(health_entry); MH_DisableHook(health_entry); }
        if (experience_created) { MH_QueueDisableHook(experience_entry); MH_DisableHook(experience_entry); }
        if (damage_created) { MH_QueueDisableHook(damage_entry); MH_DisableHook(damage_entry); }
        if (reaper_damage.created) { MH_QueueDisableHook(reaper_damage.entry); MH_DisableHook(reaper_damage.entry); }
        if (movement_created) { MH_QueueDisableHook(movement_entry); MH_DisableHook(movement_entry); }
        if (interval_created) { MH_QueueDisableHook(interval_entry); MH_DisableHook(interval_entry); }
        if (menu_tick_route.created) { MH_QueueDisableHook(menu_tick_route.entry); MH_DisableHook(menu_tick_route.entry); }
        if (menu_update_route.created) { MH_QueueDisableHook(menu_update_route.entry); MH_DisableHook(menu_update_route.entry); }
        if (chest_animation_update.created) {
            MH_QueueDisableHook(chest_animation_update.entry); MH_DisableHook(chest_animation_update.entry);
        }
        for (size_t index = 0; index < stat_routes.size(); ++index) {
            const auto& route = stat_routes[index];
            if (!route.created) continue;
            bool shared = false;
            for (size_t previous = 0; previous < index; ++previous)
                if (stat_routes[previous].created && stat_routes[previous].entry == route.entry) shared = true;
            if (!shared) { MH_QueueDisableHook(route.entry); MH_DisableHook(route.entry); }
        }
        for (size_t index = 0; index < extra_routes.size(); ++index) {
            const auto& route = extra_routes[index];
            if (!route.created || (cleanup_live && index >= StatsUpgrade && index <= PlayerReset)) continue;
            MH_QueueDisableHook(route.entry); MH_DisableHook(route.entry);
        }
        // No MH_RemoveHook/Uninitialize or object free: an already executing
        // callback may still need its original trampoline after Stop returns.
    }
};

thread_local bool GameBackend::Impl::sampling = false;
thread_local GameBackend::Impl::StatReadMode GameBackend::Impl::stat_read_mode = GameBackend::Impl::StatReadMode::Natural;

GameBackend::GameBackend(Runtime& runtime) noexcept : runtime_(runtime) {}
GameBackend::~GameBackend() { Stop(); }

bool GameBackend::Start(std::string& error) {
    if (impl_) { error = "The native backend can only be started once."; return false; }
    GameIdentity identity;
    if (!VerifyCurrentGame(identity, error)) { runtime_.SetArmorSupported(false, error); return false; }
    impl_ = new (std::nothrow) Impl(runtime_);
    if (!impl_) { error = "Cannot allocate native backend state."; runtime_.SetArmorSupported(false, error); return false; }
    const auto expected_image = JoinPath(identity.directory, L"GameAssembly.dll");
    const uint64_t deadline = GetTickCount64() + 30000;
    bool resolved = false;
    // Connecting during launch can precede IL2CPP metadata registration. Only
    // retry that readiness stage, before any hooks or runtime routes exist.
    for (;;) {
        bool waiting_for_runtime = false;
        resolved = impl_->Resolve(expected_image, waiting_for_runtime, error);
        if (resolved || !waiting_for_runtime || GetTickCount64() >= deadline) break;
        Sleep(250);
        error.clear();
    }
    if (!resolved || !impl_->Hook(error)) {
        impl_->Stop();
        runtime_.SetArmorSupported(false, error);
        return false;
    }
    return true;
}

void GameBackend::Stop() noexcept { if (impl_) impl_->Stop(); }

} // namespace vst
