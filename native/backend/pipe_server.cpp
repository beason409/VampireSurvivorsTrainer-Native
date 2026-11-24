#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include "pipe_server.hpp"
#include "../common/protocol.hpp"
#include "../common/backup_proof.hpp"
#include "../common/chest_animation.hpp"
#include "../common/chest_rewards.hpp"
#include "../common/game_speed.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace vst {
namespace {

class PipeHandle {
public:
    PipeHandle() = default;
    explicit PipeHandle(HANDLE value) : value_(value) {}
    ~PipeHandle() { reset(); }
    PipeHandle(const PipeHandle&) = delete;
    PipeHandle& operator=(const PipeHandle&) = delete;
    PipeHandle(PipeHandle&& other) noexcept : value_(other.release()) {}
    PipeHandle& operator=(PipeHandle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    HANDLE release() { HANDLE value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) { if (valid()) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_ = nullptr;
};

bool TokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<unsigned char>& data) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (size == 0 || size > 1024 * 1024) return false;
    data.resize(size);
    return GetTokenInformation(token, kind, data.data(), size, &size) != FALSE;
}

bool Identity(std::wstring& owner, std::wstring& user) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    PipeHandle token(raw);
    std::vector<unsigned char> user_data, owner_data;
    if (!TokenData(token.get(), TokenUser, user_data) || !TokenData(token.get(), TokenOwner, owner_data)) return false;
    LPWSTR user_text = nullptr, owner_text = nullptr;
    bool valid = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user_data.data())->User.Sid, &user_text)
        && ConvertSidToStringSidW(reinterpret_cast<TOKEN_OWNER*>(owner_data.data())->Owner, &owner_text);
    if (valid) { user = user_text; owner = owner_text; }
    if (user_text) LocalFree(user_text);
    if (owner_text) LocalFree(owner_text);
    return valid;
}

PipeHandle CreateRestrictedPipe() {
    std::wstring owner, user;
    if (!Identity(owner, user)) return {};
    auto sddl = L"O:" + owner + L"D:P(A;;GA;;;" + user + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return {};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    auto name = L"\\\\.\\pipe\\VampireSurvivors.Trainer.v1." + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &attributes);
    DWORD error = GetLastError();
    LocalFree(descriptor);
    SetLastError(error);
    return PipeHandle(pipe);
}

PipeHandle VerifyPeer(HANDLE pipe) {
    ULONG pid = 0, session = 0;
    DWORD own_session = 0, actual_session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid || !GetNamedPipeClientSessionId(pipe, &session)
        || !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || own_session != session
        || !ProcessIdToSessionId(pid, &actual_session) || actual_session != session) return {};
    PipeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.valid() || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return {};
    return process;
}

bool PeerAlive(const PipeHandle& peer) { return peer.valid() && WaitForSingleObject(peer.get(), 0) == WAIT_TIMEOUT; }

bool FinishIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, uint64_t deadline, DWORD& transferred) {
    uint64_t now = GetTickCount64();
    DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(std::min<uint64_t>(deadline - now, MAXDWORD - 1));
    HANDLE events[] = {stop, operation.hEvent};
    DWORD wait = WaitForMultipleObjects(2, events, FALSE, remaining);
    if (wait == WAIT_OBJECT_0 + 1) return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    // The OVERLAPPED and its buffers stay alive until the cancellation completes.
    WaitForSingleObject(operation.hEvent, INFINITE);
    GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    return false;
}

bool Connect(HANDLE pipe, HANDLE stop) {
    PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    HANDLE events[] = {stop, event.get()};
    DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    DWORD transferred = 0;
    if (wait == WAIT_OBJECT_0 + 1) return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(event.get(), INFINITE);
    GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    return false;
}

bool ReadChunk(HANDLE pipe, HANDLE stop, uint64_t deadline, char* buffer, DWORD capacity, DWORD& read) {
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) return false;
    PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ReadFile(pipe, buffer, capacity, &read, &operation)) return read != 0;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return FinishIo(pipe, operation, stop, deadline, read) && read != 0;
}

bool WriteLine(HANDLE pipe, HANDLE stop, const Response& response) {
    std::string json = SerializeResponse(response);
    size_t count = 0;
    if (json.empty() || json.size() > kMaximumMessageBytes || !Utf16Length(json, count) || count > kMaximumMessageCharacters) return false;
    json.push_back('\n');
    uint64_t deadline = GetTickCount64() + 2000;
    size_t offset = 0;
    while (offset < json.size()) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) return false;
        PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD written = 0;
        BOOL complete = WriteFile(pipe, json.data() + offset, static_cast<DWORD>(json.size() - offset), &written, &operation);
        if (!complete && (GetLastError() != ERROR_IO_PENDING || !FinishIo(pipe, operation, stop, deadline, written))) return false;
        if (!written) return false;
        offset += written;
    }
    return true;
}

class LineReader {
public:
    bool Read(HANDLE pipe, HANDLE stop, uint32_t timeout, std::string& line) {
        uint64_t deadline = GetTickCount64() + timeout;
        while (true) {
            auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                if (newline > kMaximumMessageBytes) return false;
                line.assign(buffer_.data(), newline);
                buffer_.erase(0, newline + 1);
                while (!line.empty() && line.back() == '\r') line.pop_back();
                size_t count = 0;
                return Utf16Length(line, count) && count <= kMaximumMessageCharacters;
            }
            if (buffer_.size() > kMaximumMessageBytes) return false;
            std::array<char, 4096> chunk{};
            DWORD read = 0;
            if (!ReadChunk(pipe, stop, deadline, chunk.data(), static_cast<DWORD>(chunk.size()), read)) return false;
            buffer_.append(chunk.data(), read);
        }
    }
private:
    std::string buffer_;
};

bool Allowed(const Request& request) {
    return request.kind == "hello" || request.kind == "heartbeat" || request.kind == "getState" || request.kind == "getGold"
        || request.kind == "disableAll" || request.kind == "disableStats" || request.kind == "setGold" || request.kind == "setAdventureGold"
        || request.kind == "getItemCatalog" || request.kind == "getItemIcon" || request.kind == "setChestReward"
        || (request.kind == "setStat" && FindNativeStat(request.stat) != nullptr)
        || (request.kind == "setFeature" && (request.feature == "health" || request.feature == "experience" || request.feature == "damage"
            || request.feature == "movement" || request.feature == "cooldown" || request.feature == "instantKill"
            || request.feature == "noGoldSpend" || request.feature == kGameSpeedId
            || request.feature == kSkipChestAnimationId || request.feature == kFiveItemChestsId));
}

} // namespace

struct PipeServer::Impl {
    explicit Impl(Runtime& value, const GameIdentity& identity = {}, uint64_t start = 0)
        : runtime(value), game(identity), process_start(start) {}
    Runtime& runtime;
    GameIdentity game;
    uint64_t process_start = 0;
    PipeHandle stop, worker, watchdog, first_pipe;
    mutable std::mutex error_mutex;
    std::string error;

    void SetError(const char* message) { std::lock_guard lock(error_mutex); error = message; }

    static DWORD WINAPI WatchdogEntry(void* pointer) {
        auto self = static_cast<Impl*>(pointer);
        while (WaitForSingleObject(self->stop.get(), 250) == WAIT_TIMEOUT) self->runtime.CheckHeartbeat(GetTickCount64());
        return 0;
    }

    static DWORD WINAPI WorkerEntry(void* pointer) {
        static_cast<Impl*>(pointer)->Listen();
        return 0;
    }

    void Serve(HANDLE pipe) {
        auto peer = VerifyPeer(pipe);
        if (!PeerAlive(peer)) return;
        uint64_t session = 0;
        bool handshaken = false;
        std::wstring backup_token;
        std::string backup_error;
        LineReader reader;
        while (WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0) {
            std::string line;
            if (!reader.Read(pipe, stop.get(), handshaken ? 5000 : 2000, line) || !PeerAlive(peer)) break;
            Request request;
            uint64_t received_at = GetTickCount64();
            std::string parse_error;
            bool parsed = ParseRequest(line, request, parse_error);
            bool invalid = !parsed || (!handshaken && request.kind != "hello")
                || (handshaken && request.kind == "hello") || !Allowed(request);
            if (invalid) {
                if (session) runtime.EndSession(session);
                else runtime.DisableAll();
                Response response;
                response.id = request.id;
                response.error = !parse_error.empty() ? parse_error : "Handshake or native pilot command is invalid.";
                response.state = runtime.Snapshot(GetTickCount64());
                WriteLine(pipe, stop.get(), response);
                break;
            }
            if (!PeerAlive(peer)) break;
            if (!session) session = runtime.BeginSession(GetTickCount64());
            const bool spend_protection = request.kind == "setFeature" && request.feature == "noGoldSpend";
            // Only an already verified host can authorize a startup snapshot.
            // Rehash on explicit proof changes or spend protection requests, never on
            // every heartbeat or inside the Unity frame.
            if (request.kind == "hello" || !request.backup_token.empty() || spend_protection) {
                if (!request.backup_token.empty()) backup_token = Wide(request.backup_token);
                uint64_t wall_now = CurrentFileTime();
                uint64_t proof_expires = 0;
                bool valid = process_start && game.pid == GetCurrentProcessId() && !backup_token.empty()
                    && VerifyBackupProof(backup_token, game, process_start, wall_now, backup_error, &proof_expires);
                uint64_t deadline = 0;
                if (valid) {
                    wall_now = CurrentFileTime();
                    if (proof_expires >= wall_now)
                        deadline = GetTickCount64() + (proof_expires - wall_now) / 10000;
                    else valid = false;
                }
                runtime.SetBackupAuthorization(session, valid, deadline);
            }
            auto ticket = runtime.Submit(std::move(request), session, received_at);
            auto response = runtime.Await(ticket, 1500);
            if (!handshaken) handshaken = response.ok;
            if (!WriteLine(pipe, stop.get(), response) || !handshaken) break;
        }
        if (session) runtime.EndSession(session);
    }

    void Listen() {
        auto pipe = std::move(first_pipe);
        while (WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0) {
            if (!pipe.valid()) pipe = CreateRestrictedPipe();
            if (!pipe.valid()) { SetError("Native pipe creation failed; another backend may already own the name."); break; }
            if (Connect(pipe.get(), stop.get())) Serve(pipe.get());
            DisconnectNamedPipe(pipe.get());
            pipe.reset();
            if (WaitForSingleObject(stop.get(), 200) != WAIT_TIMEOUT) break;
        }
        runtime.DisableAll();
    }
};

PipeServer::PipeServer(Runtime& runtime) : impl_(std::make_unique<Impl>(runtime)) {}
PipeServer::PipeServer(Runtime& runtime, const GameIdentity& game, uint64_t start)
    : impl_(std::make_unique<Impl>(runtime, game, start)) {}
PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start() {
    if (impl_->worker.valid()) return true;
    impl_->stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl_->stop.valid()) { impl_->SetError("Cannot create native pipe stop event."); return false; }
    impl_->first_pipe = CreateRestrictedPipe();
    if (!impl_->first_pipe.valid()) { impl_->SetError("Cannot exclusively create the native game pipe."); return false; }
    impl_->worker.reset(CreateThread(nullptr, 0, &Impl::WorkerEntry, impl_.get(), 0, nullptr));
    if (!impl_->worker.valid()) { impl_->first_pipe.reset(); impl_->SetError("Cannot start native pipe worker."); return false; }
    impl_->watchdog.reset(CreateThread(nullptr, 0, &Impl::WatchdogEntry, impl_.get(), 0, nullptr));
    if (!impl_->watchdog.valid()) { Stop(); impl_->SetError("Cannot start native pipe watchdog."); return false; }
    return true;
}

void PipeServer::Stop() {
    impl_->runtime.DisableAll();
    if (impl_->stop.valid()) SetEvent(impl_->stop.get());
    if (impl_->worker.valid()) WaitForSingleObject(impl_->worker.get(), INFINITE);
    if (impl_->watchdog.valid()) WaitForSingleObject(impl_->watchdog.get(), INFINITE);
    impl_->worker.reset();
    impl_->watchdog.reset();
    impl_->first_pipe.reset();
    impl_->stop.reset();
}

std::string PipeServer::Error() const { std::lock_guard lock(impl_->error_mutex); return impl_->error; }

} // namespace vst
