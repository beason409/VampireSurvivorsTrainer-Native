#pragma once

#include <cmath>

namespace vst {
inline constexpr char kGameSpeedId[] = "gameSpeed";
inline constexpr float kGameSpeedMin = 1, kGameSpeedMax = 10, kGameSpeedDefault = 4;

inline bool ValidGameSpeed(double value) noexcept {
    return std::isfinite(value) && value >= kGameSpeedMin && value <= kGameSpeedMax;
}

// Only restore a value we still own. Zero is controlled by the game's
// pause flow. That flow may put our last value back when it resumes.
class GameSpeedOverride {
    float natural_ = 0, written_ = 0;

public:
    bool OwnsValue() const noexcept { return written_ > 0; }

    float Next(float current, float requested) noexcept {
        if (!std::isfinite(current) || current <= 0) return current;
        if (current != written_) {
            natural_ = current;
            written_ = 0;
        }
        if (ValidGameSpeed(requested)) return requested;
        return OwnsValue() ? natural_ : current;
    }

    void ConfirmWrite(float value) noexcept { written_ = value == natural_ ? 0 : value; }
};
}
