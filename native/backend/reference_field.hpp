#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace vst {

// Match the generated IL2CPP reference-field setters exactly: owner, address
// of the managed reference slot, raw managed object. A pointer to a local
// object variable is never passed as the value. Scalars use a separate API.
template <class Api>
bool WriteReferenceField(const Api& api, void* owner, const void* field, void* object) {
    if (!owner || !field || !api.gc_wbarrier_set_field) return false;
    const size_t offset = api.field_get_offset(field);
    if (offset < 2 * sizeof(void*) || offset >= 1024 * 1024 || offset % alignof(void*) != 0) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(owner);
    if (address > std::numeric_limits<uintptr_t>::max() - offset) return false;
    auto** slot = reinterpret_cast<void**>(address + offset);
    api.gc_wbarrier_set_field(owner, slot, object);
    void* actual = nullptr;
    api.field_get_value(owner, field, &actual);
    return actual == object;
}

} // namespace vst
