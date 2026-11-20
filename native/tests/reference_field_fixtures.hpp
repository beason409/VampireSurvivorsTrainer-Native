#pragma once
#include "../backend/reference_field.hpp"
#include <cstdio>

namespace vst_reference_tests {
#define VST_REFERENCE_CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "reference fixture line %d: %s\n", __LINE__, #__VA_ARGS__); return false; } } while (0)
struct Object { void* klass = nullptr; void* monitor = nullptr; void* reference = nullptr; };
struct Calls {
    void* owner = nullptr; void** slot = nullptr; void* value = nullptr;
    unsigned writes = 0; bool wrong_readback = false;
};
inline Calls* current = nullptr;
inline size_t Offset(const void* field) { return *static_cast<const size_t*>(field); }
inline void Barrier(void* owner, void** slot, void* value) {
    current->owner = owner; current->slot = slot; current->value = value; ++current->writes;
    *slot = value;
}
inline void Read(void* owner, const void* field, void* output) {
    void* value = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(owner) + Offset(field));
    *static_cast<void**>(output) = current->wrong_readback ? nullptr : value;
}
struct Api {
    size_t (*field_get_offset)(const void*) = &Offset;
    void (*gc_wbarrier_set_field)(void*, void**, void*) = &Barrier;
    void (*field_get_value)(void*, const void*, void*) = &Read;
};
inline bool ReferenceWritesUseRawObjectAndExactManagedSlot() {
    Calls calls; current = &calls; Api api; Object owner; int object = 42;
    size_t offset = offsetof(Object, reference);
    VST_REFERENCE_CHECK(vst::WriteReferenceField(api, &owner, &offset, &object));
    VST_REFERENCE_CHECK(calls.writes == 1 && calls.owner == &owner && calls.slot == &owner.reference
        && calls.value == &object && owner.reference == &object);
    VST_REFERENCE_CHECK(vst::WriteReferenceField(api, &owner, &offset, nullptr));
    VST_REFERENCE_CHECK(calls.writes == 2 && calls.value == nullptr && owner.reference == nullptr);
    return true;
}
inline bool ReferenceWritesRejectBadOffsetsAndDetectReadbackMismatch() {
    Calls calls; current = &calls; Api api; Object owner; int object = 42;
    for (size_t offset : {size_t{0}, size_t{1}, 2 * sizeof(void*) + 1, size_t{1024 * 1024}})
        VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, &owner, &offset, &object));
    size_t valid = offsetof(Object, reference);
    auto overflow = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - valid + 1);
    VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, overflow, &valid, &object));
    VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, nullptr, &valid, &object));
    VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, &owner, nullptr, &object));
    api.gc_wbarrier_set_field = nullptr;
    VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, &owner, &valid, &object));
    VST_REFERENCE_CHECK(calls.writes == 0);
    api.gc_wbarrier_set_field = &Barrier; calls.wrong_readback = true;
    VST_REFERENCE_CHECK(!vst::WriteReferenceField(api, &owner, &valid, &object));
    VST_REFERENCE_CHECK(calls.writes == 1 && owner.reference == &object);
    calls.wrong_readback = false;
    VST_REFERENCE_CHECK(vst::WriteReferenceField(api, &owner, &valid, nullptr));
    return true;
}
#undef VST_REFERENCE_CHECK
}
