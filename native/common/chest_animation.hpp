#pragma once

#include <cstdint>

namespace vst {
inline constexpr char kSkipChestAnimationId[] = "skipChestAnimation";

struct ChestAnimationState {
    bool playing = false, at_skip_point = false, skipped = false, finished = false;
    bool arcana = false;
    int32_t level = 0;

    bool PlaybackReady() const noexcept { return playing && at_skip_point && !skipped && !finished; }
    bool CanSkip() const noexcept { return PlaybackReady() && !arcana && level >= 1 && level <= 3; }
};
}
