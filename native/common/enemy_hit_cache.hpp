#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vst {

struct EnemyHitClass {
    bool target = false;
    bool custom_hit = false;
};

// IL2CPP classes and their declared hit routes stay valid for the loaded game.
// Enemy instances and their mutable fields are checked separately on each hit.
class EnemyHitClassCache {
public:
    template <class Resolve>
    EnemyHitClass Get(const void* klass, Resolve resolve) {
        if (!klass) return {};
        const auto bits = reinterpret_cast<uintptr_t>(klass);
        auto& entry = entries_[((bits >> 4) ^ (bits >> 11)) % entries_.size()];
        if (entry.klass != klass) entry = {klass, resolve()};
        return entry.classification;
    }

private:
    struct Entry {
        const void* klass = nullptr;
        EnemyHitClass classification;
    };
    std::array<Entry, 128> entries_{};
};

} // namespace vst
