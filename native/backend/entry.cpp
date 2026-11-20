#include "game.hpp"
#include "pipe_server.hpp"
#include "../common/runtime.hpp"
#include "../common/win_util.hpp"
#include <atomic>
#include <new>
#include <memory>

namespace {
std::atomic<unsigned> startup{0};
// The module and callbacks stay resident until the game exits. No callback can
// return into an unloaded DLL, and closing the client only clears effects.
struct Context {
    vst::Runtime runtime;
    vst::GameBackend game{runtime};
    vst::PipeServer pipe;
    Context(const vst::GameIdentity& identity, uint64_t start) : pipe(runtime, identity, start) {}
};
Context* context = nullptr;
struct LaunchContext { vst::GameIdentity identity; uint64_t start = 0; };

DWORD WINAPI Bootstrap(void* pointer) try {
    std::unique_ptr<LaunchContext> launch(static_cast<LaunchContext*>(pointer));
    auto instance = new (std::nothrow) Context(launch->identity, launch->start);
    if (!instance) { startup.store(3); return ERROR_NOT_ENOUGH_MEMORY; }
    context = instance;
    std::string error;
    vst::GameObservation waiting;
    waiting.verified = true;
    waiting.scene = "Waiting for gameplay";
    instance->runtime.Observe(waiting, GetTickCount64());
    if (!instance->game.Start(error)) instance->runtime.SetArmorSupported(false, error);
    if (!instance->pipe.Start()) {
        instance->runtime.DisableAll();
        instance->game.Stop();
        startup.store(3);
        return ERROR_PIPE_NOT_CONNECTED;
    }
    startup.store(2);
    return ERROR_SUCCESS;
} catch (...) {
    if (context) { context->game.Stop(); try { context->runtime.DisableAll(); } catch (...) {} }
    startup.store(3);
    return ERROR_UNHANDLED_EXCEPTION;
}
}

extern "C" __declspec(dllexport) DWORD WINAPI StartNativeTrainer(void*) {
    unsigned expected = 0;
    if (!startup.compare_exchange_strong(expected, 1))
        return expected == 1 || expected == 2 ? ERROR_ALREADY_INITIALIZED : ERROR_BAD_ENVIRONMENT;
    vst::GameIdentity game;
    std::string error;
    if (!vst::VerifyCurrentGame(game, error) || vst::HasManagedBackend(GetCurrentProcessId(), error)) {
        startup.store(3);
        return ERROR_BAD_ENVIRONMENT;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { startup.store(3); return GetLastError(); }
    uint64_t start = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::unique_ptr<LaunchContext> launch(new (std::nothrow) LaunchContext{std::move(game), start});
    if (!launch) { startup.store(3); return ERROR_NOT_ENOUGH_MEMORY; }
    HANDLE thread = CreateThread(nullptr, 0, Bootstrap, launch.get(), 0, nullptr);
    if (!thread) { startup.store(3); return GetLastError(); }
    launch.release();
    CloseHandle(thread);
    return ERROR_SUCCESS;
}

// The static C++ runtime and TLS keep their normal thread notifications.
// All trainer work begins at the explicit, verified startup export instead.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
