#pragma once

#include <cstdint>
#include <new>
#include <utility>

namespace vst {

// The supported Unity build uses pointer-sized GC handles. Api supplies the
// three matching exports; a test adapter can exercise this exact ownership
// policy without loading IL2CPP or touching a game object.
template <class Api>
struct StrongRootT {
    const Api* api = nullptr;
    uintptr_t handle = 0;
    static_assert(sizeof(handle) == sizeof(void*));

    StrongRootT() = default;
    StrongRootT(const Api& value, void* object) : api(&value) {
        if (object && !(handle = api->gchandle_new(object, false))) throw std::bad_alloc();
    }
    StrongRootT(const StrongRootT& other) : StrongRootT() {
        if (other.api) { StrongRootT clone(*other.api, other.Get()); Swap(clone); }
    }
    StrongRootT(StrongRootT&& other) noexcept { Swap(other); }
    StrongRootT& operator=(StrongRootT other) noexcept { Swap(other); return *this; }
    ~StrongRootT() { if (handle) api->gchandle_free(handle); }

    void Swap(StrongRootT& other) noexcept { std::swap(api, other.api); std::swap(handle, other.handle); }
    void* Get() const noexcept { return handle ? api->gchandle_get_target(handle) : nullptr; }
};

} // namespace vst
