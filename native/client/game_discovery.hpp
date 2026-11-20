#pragma once

#include "../common/win_util.hpp"
#include <tlhelp32.h>

namespace vst::client {
inline constexpr wchar_t kGameExecutable[] = L"VampireSurvivors.exe";
inline constexpr char kNoRunningGame[] = "No running game was detected.";
inline constexpr char kNoGameInFolder[] = "No game is running from the selected folder. Start that Steam installation and try again.";
inline constexpr char kMultipleGames[] = "Multiple game instances were detected. Choose the game folder to connect.";
inline constexpr char kMultipleGamesInFolder[] = "Multiple games are running from this folder. Leave one instance open and try again.";
inline constexpr char kMissingGameExecutable[] = "Choose the folder containing VampireSurvivors.exe.";

struct GameCandidate {
    DWORD pid;
    std::wstring executable;
};

struct GameScan {
    DWORD pid = 0;
    std::string error;
    bool operator==(const GameScan&) const = default;
};

class AutomaticConnectionSchedule {
    DWORD pid_ = 0;
    uint64_t due_ = 0;
    unsigned failures_ = 0;
public:
    void Observe(DWORD pid, uint64_t now) noexcept {
        if (pid_ == pid) return;
        pid_ = pid;
        due_ = now + 1800;
        failures_ = 0;
    }
    bool Due(uint64_t now) const noexcept { return pid_ && failures_ < 4 && now >= due_; }
    void Complete(uint64_t now, bool retry) noexcept {
        if (retry) {
            ++failures_;
            due_ = now + (3000ull << failures_);
        } else failures_ = 4;
    }
};

inline GameScan SelectGameCandidate(const std::vector<GameCandidate>& candidates, const std::wstring& expected_executable) {
    DWORD pid = 0;
    for (const auto& candidate : candidates) {
        if (!expected_executable.empty() && !EqualPath(candidate.executable, expected_executable)) continue;
        if (pid) return {0, expected_executable.empty() ? kMultipleGames : kMultipleGamesInFolder};
        pid = candidate.pid;
    }
    if (pid) return {pid, {}};
    return {0, expected_executable.empty() ? kNoRunningGame : kNoGameInFolder};
}

inline GameScan ScanRunningGames(const std::wstring& expected_executable = {}) {
    DWORD local_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &local_session)) return {0, WinError("Read the Windows session")};
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return {0, WinError("List game processes")};
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.get(), &entry)) return {0, WinError("List game processes")};
    std::vector<GameCandidate> candidates;
    do {
        if (!EqualPath(entry.szExeFile, kGameExecutable)) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != local_session) continue;
        GameCandidate candidate{entry.th32ProcessID, {}};
        if (!expected_executable.empty()) {
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.pid));
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (process && QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
                path.resize(length);
                candidate.executable = std::move(path);
            }
        }
        candidates.push_back(std::move(candidate));
    } while (Process32NextW(snapshot.get(), &entry));
    const DWORD code = GetLastError();
    if (code != ERROR_NO_MORE_FILES) return {0, WinError("List game processes", code)};
    return SelectGameCandidate(candidates, expected_executable);
}

inline bool ResolveGameFolder(const std::wstring& selected, std::wstring& executable, std::string& error) {
    std::wstring folder;
    if (!LocalPath(selected, folder, error)) return false;
    executable = JoinPath(folder, kGameExecutable);
    if (!RejectReparsePath(executable, true, error)) return false;
    auto attributes = GetFileAttributesW(executable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = kMissingGameExecutable;
        return false;
    }
    return true;
}

inline bool CheckGameDiscoveryPolicy() {
    const std::wstring first = L"C:\\Fixture Library\\VampireSurvivors.exe";
    const std::wstring second = L"D:\\Fixture Library\\VampireSurvivors.exe";
    const std::vector<GameCandidate> candidates{{21, first}, {22, second}, {23, {}}};
    AutomaticConnectionSchedule schedule;
    if (schedule.Due(10000)) return false;
    schedule.Observe(21, 10000);
    if (schedule.Due(11799) || !schedule.Due(11800)) return false;
    schedule.Complete(12000, true);
    schedule.Observe(21, 17000);
    if (schedule.Due(17999) || !schedule.Due(18000)) return false;
    schedule.Complete(18000, false);
    if (schedule.Due(100000)) return false;
    schedule.Observe(0, 100000);
    schedule.Observe(22, 101000);
    return schedule.Due(102800) && SelectGameCandidate({}, {}).error == kNoRunningGame
        && SelectGameCandidate({{21, {}}}, {}).pid == 21
        && SelectGameCandidate(candidates, {}).error == kMultipleGames
        && SelectGameCandidate(candidates, L"d:\\fixture library\\vampiresurvivors.exe").pid == 22
        && SelectGameCandidate({{21, first}}, second).error == kNoGameInFolder
        && SelectGameCandidate({{23, {}}}, second).error == kNoGameInFolder
        && SelectGameCandidate({{21, first}, {22, first}}, first).error == kMultipleGamesInFolder;
}
} // namespace vst::client
