#pragma once

#include <utility>

namespace vst {

// One temporary consumable budget, independent of its normal native value.
// Value can own a strong GC reference. Read/Write/Create are supplied by the
// verified backend; this policy itself has no game, allocation or timing API.
template <class Value>
struct TemporaryResource {
    bool active = false;
    bool suspended = false;
    double target = 0;
    Value original{};
    Value temporary{};

    template <class Read, class Write, class Create>
    void Configure(double requested, Read&& read, Write&& write, Create&& create, bool force = false) {
        if (active && target == requested && !force) return;
        Value replacement = create(requested);
        if (!active) { original = read(); active = true; }
        temporary = std::move(replacement);
        target = requested;
        if (!suspended) write(temporary);
    }

    template <class Read, class Write>
    void Suspend(Read&& read, Write&& write) {
        if (!active || suspended) return;
        // Native consumption may have replaced the temporary object.
        Value remaining = read();
        write(original);
        temporary = std::move(remaining);
        suspended = true;
    }

    template <class Read, class Write>
    void Resume(Read&& read, Write&& write) {
        if (!active || !suspended) return;
        // Normal upgrades/resets operate only on the normal binding.
        original = read();
        write(temporary);
        suspended = false;
    }

    template <class Write>
    void Restore(Write&& write) {
        if (!active) return;
        if (!suspended) write(original);
        active = false;
        suspended = false;
        original = {};
        temporary = {};
    }

    template <class Read, class Decode, class Scalar>
    bool SampleBound(Read&& read, Decode&& decode, Scalar& normal, Scalar& bound) const {
        Value current = read();
        // Sample the native bound value verbatim. Consumable fields already
        // reflect remaining amounts; a used counter is no extra deduction.
        return decode(active ? original : current, normal) && decode(current, bound);
    }
};

// Prepare a binding for a replacement owner of the same current character.
// The new owner's normal baseline is independent of the old owner; retain
// the actual remaining temporary value rather than recreating the target.
// No field is written here, so callers can prepare all slots before switching
// ownership and can restore the old owner before binding the replacement.
template <class Value>
bool RebaseTemporaryResource(const TemporaryResource<Value>& previous,
        Value normal, Value remaining, TemporaryResource<Value>& replacement) {
    if (previous.suspended) return false;
    TemporaryResource<Value> prepared;
    if (previous.active) {
        prepared.active = true;
        prepared.target = previous.target;
        prepared.original = std::move(normal);
        prepared.temporary = std::move(remaining);
    }
    replacement = std::move(prepared);
    return true;
}

} // namespace vst
