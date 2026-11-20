#pragma once

#include <string>

namespace vst {

class Runtime;

// The pilot owns the lifecycle/armor hooks and validated optional gameplay
// hooks, including ordinary-stat getters, movement/interval returns and
// main-thread health-limit, stage and timer refreshes, isolated temporary
// resource bindings, and verified ordinary-wallet deduction/save routes.
// Only an already enabled revival budget may survive a freshly verified
// natural-death interval of the same actor; ordinary effects still close.
// It is never hot-unloaded:
// stopped hook state and trampolines remain valid until the game process exits.
class GameBackend {
public:
    explicit GameBackend(Runtime& runtime) noexcept;
    ~GameBackend();
    GameBackend(const GameBackend&) = delete;
    GameBackend& operator=(const GameBackend&) = delete;

    bool Start(std::string& error);
    void Stop() noexcept;

private:
    struct Impl;
    Runtime& runtime_;
    Impl* impl_ = nullptr;
};

} // namespace vst
