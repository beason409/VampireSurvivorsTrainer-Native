#pragma once
#include "../backend/gc_root.hpp"
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace vst_gc_tests {
#define VST_GC_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "GC fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)
struct Api {
    mutable std::unordered_map<uintptr_t, void*> targets;
    mutable uintptr_t next = static_cast<uintptr_t>(0xF000000100000001ULL);
    mutable unsigned allocated = 0, freed = 0;
    mutable bool invalid = false, fail = false;
    uintptr_t gchandle_new(void* object, bool pinned) const {
        if (pinned) invalid = true;
        if (fail) return 0;
        uintptr_t handle = next++; targets[handle] = object; ++allocated; return handle;
    }
    void* gchandle_get_target(uintptr_t handle) const {
        auto found = targets.find(handle);
        if (found == targets.end()) { invalid = true; return nullptr; }
        return found->second;
    }
    void gchandle_free(uintptr_t handle) const {
        if (targets.erase(handle) != 1) invalid = true;
        else ++freed;
    }
    void Relocate(void* previous, void* current) const {
        for (auto& entry : targets) if (entry.second == previous) entry.second = current;
    }
};
using Root = vst::StrongRootT<Api>;
inline bool HighHandleBitsSurviveCopyMoveLookupAndFree() {
    Api api; int object = 1, moved_object = 2;
    {
        Root original(api, &object);
        VST_GC_CHECK(original.handle > std::numeric_limits<uint32_t>::max() && original.Get() == &object);
        Root copy(original);
        VST_GC_CHECK(copy.handle != original.handle && copy.Get() == &object);
        Root moved(std::move(copy));
        VST_GC_CHECK(copy.handle == 0 && !copy.Get() && moved.Get() == &object);
        api.Relocate(&object, &moved_object);
        VST_GC_CHECK(original.Get() == &moved_object && moved.Get() == &moved_object);
        Root assigned; assigned = original;
        VST_GC_CHECK(assigned.Get() == &moved_object && assigned.handle != original.handle);
        original = original;
        VST_GC_CHECK(original.Get() == &moved_object && api.targets.size() == 3 && !api.invalid);
    }
    VST_GC_CHECK(api.targets.empty() && api.allocated == api.freed && !api.invalid);
    return true;
}
inline bool RootAllocationFailurePreservesExistingOwnership() {
    Api api; int first = 1, second = 2;
    {
        Root empty, null(api, nullptr);
        VST_GC_CHECK(!empty.Get() && !null.Get() && api.allocated == 0);
        Root source(api, &first), destination(api, &second);
        api.fail = true;
        try { destination = source; VST_GC_CHECK(false); } catch (const std::bad_alloc&) {}
        VST_GC_CHECK(source.Get() == &first && destination.Get() == &second && api.targets.size() == 2);
        api.fail = false; destination = std::move(source);
        VST_GC_CHECK(!source.Get() && destination.Get() == &first && api.targets.size() == 1);
    }
    VST_GC_CHECK(api.targets.empty() && api.allocated == api.freed && !api.invalid);
    return true;
}
#undef VST_GC_CHECK
}
