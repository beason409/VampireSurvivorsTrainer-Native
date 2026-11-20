#pragma once

#include "../common/runtime.hpp"
#include <memory>

namespace vst {
struct GameIdentity;

class PipeServer {
public:
    explicit PipeServer(Runtime& runtime);
    PipeServer(Runtime& runtime, const GameIdentity& game, uint64_t process_start_file_time);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    bool Start();
    void Stop();
    std::string Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vst
