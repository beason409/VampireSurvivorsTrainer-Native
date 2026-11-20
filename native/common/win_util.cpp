#include "win_util.hpp"
#include "supported_game.hpp"
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace vst {
std::string WinError(const char* operation, DWORD code) { return std::string(operation) + " (Windows " + std::to_string(code) + ")"; }
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr)) return {};
    return result;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size)) return {};
    return result;
}
std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size); return path;
}
std::wstring ParentPath(const std::wstring& path) { auto i = path.find_last_of(L"\\/"); return i == std::wstring::npos ? L"" : path.substr(0, i); }
std::wstring JoinPath(const std::wstring& directory, const std::wstring& leaf) { return directory + (directory.empty() || directory.back() == L'\\' ? L"" : L"\\") + leaf; }
std::wstring FileName(const std::wstring& path) { auto i = path.find_last_of(L"\\/"); return i == std::wstring::npos ? path : path.substr(i + 1); }
bool EqualPath(const std::wstring& left, const std::wstring& right) noexcept { return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL; }
bool LocalPath(const std::wstring& input, std::wstring& full, std::string& error) {
    if (input.size() < 3 || input.size() >= 32760 || !((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z'))
        || input[1] != L':' || (input[2] != L'\\' && input[2] != L'/') || input.find(L':', 2) != std::wstring::npos || input.find(L'\0') != std::wstring::npos) {
        error = "A local absolute drive path is required."; return false;
    }
    std::wstring buffer(32768, L'\0');
    DWORD size = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!size || size >= buffer.size()) { error = WinError("Resolve local path"); return false; }
    buffer.resize(size); std::replace(buffer.begin(), buffer.end(), L'/', L'\\');
    while (buffer.size() > 3 && buffer.back() == L'\\') buffer.pop_back();
    UINT drive = GetDriveTypeW(buffer.substr(0, 3).c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE) { error = "The path must be on a local drive."; return false; }
    full = std::move(buffer); return true;
}
bool RejectReparsePath(const std::wstring& path, bool leaf_may_be_missing, std::string& error) {
    std::wstring full; if (!LocalPath(path, full, error)) return false;
    for (size_t end = 3;;) {
        std::wstring part = full.substr(0, end);
        DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            if (leaf_may_be_missing && end == full.size() && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) return true;
            error = WinError("Inspect path", code); return false;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = "Linked files or directories are not accepted."; return false; }
        if (end == full.size()) return true;
        auto next = full.find(L'\\', end == 3 ? 3 : end + 1);
        end = next == std::wstring::npos ? full.size() : next;
    }
}
namespace {
class Hash {
public:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    bool Open(std::string& error) {
        DWORD size = 0, received = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &received, 0) < 0 || size > 1024 * 1024) { error = "SHA-256 provider unavailable."; return false; }
        object.resize(size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) < 0) { error = "SHA-256 initialization failed."; return false; }
        return true;
    }
    bool Add(const void* bytes, ULONG length) { return BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)), length, 0) >= 0; }
    bool Finish(std::string& digest, std::string& error) {
        std::array<UCHAR, 32> bytes{};
        if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) { error = "SHA-256 finalization failed."; return false; }
        constexpr char hex[] = "0123456789abcdef"; digest.clear(); digest.reserve(64);
        for (auto byte : bytes) { digest.push_back(hex[byte >> 4]); digest.push_back(hex[byte & 15]); }
        return true;
    }
};
bool ExpectedFile(const std::wstring& path, const char* expected, std::string& error) {
    std::string hash; if (!FileSha256(path, hash, error)) return false;
    std::string wanted(expected); std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return c >= 'A' && c <= 'F' ? c + 32 : c; });
    if (hash != wanted) { error = "Unsupported game fingerprint: " + Utf8(FileName(path)); return false; }
    return true;
}
bool TokenUserBytes(HANDLE process, std::vector<unsigned char>& user, std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) { error = WinError("Read process identity"); return false; }
    Handle hold(token); DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) { error = "Invalid process identity."; return false; }
    user.resize(size);
    if (!GetTokenInformation(token, TokenUser, user.data(), size, &size)) { error = WinError("Read process user"); return false; }
    return true;
}
bool VdfValue(const std::string& text, const std::string& key, std::string& value) {
    bool found = false;
    for (size_t i = 0; i < text.size();) {
        if (text[i++] != '"') continue;
        std::string token;
        while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) ++i; token.push_back(text[i++]); }
        if (i == text.size()) return false; ++i;
        if (token != key) continue;
        if (found) return false;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
        if (i == text.size() || text[i++] != '"') return false;
        while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) ++i; value.push_back(text[i++]); }
        if (i == text.size()) return false; ++i; found = true;
    }
    return found;
}
}
bool Sha256(const void* bytes, size_t size, std::string& digest, std::string& error) {
    if (size > std::numeric_limits<ULONG>::max()) { error = "Hash input is too large."; return false; }
    Hash hash; if (!hash.Open(error)) return false;
    if (!hash.Add(bytes, static_cast<ULONG>(size))) { error = "SHA-256 input failed."; return false; }
    return hash.Finish(digest, error);
}
bool FileSha256(const std::wstring& path, std::string& digest, std::string& error) {
    if (!RejectReparsePath(path, false, error)) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) { error = WinError("Open fingerprint input"); return false; }
    Hash hash; if (!hash.Open(error)) return false;
    std::array<unsigned char, 65536> buffer{}; DWORD read = 0;
    for (;;) {
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { error = WinError("Read fingerprint input"); return false; }
        if (!read) break;
        if (!hash.Add(buffer.data(), read)) { error = "SHA-256 input failed."; return false; }
    }
    return hash.Finish(digest, error);
}
bool ReadSmallFile(const std::wstring& path, size_t limit, std::string& text, std::string& error) {
    if (!RejectReparsePath(path, false, error)) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit) { error = "Cannot read bounded local file."; return false; }
    text.resize(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
    if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()) { error = WinError("Read bounded file"); return false; }
    return true;
}
bool SameUserAndSession(HANDLE process, DWORD pid, std::string& error) {
    DWORD self_session = 0, session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &self_session) || !ProcessIdToSessionId(pid, &session) || session != self_session) { error = "The game must belong to this local session."; return false; }
    std::vector<unsigned char> self, target;
    if (!TokenUserBytes(GetCurrentProcess(), self, error) || !TokenUserBytes(process, target, error)) return false;
    auto a = reinterpret_cast<TOKEN_USER*>(self.data()), b = reinterpret_cast<TOKEN_USER*>(target.data());
    if (!EqualSid(a->User.Sid, b->User.Sid)) { error = "The game must belong to the current account."; return false; }
    return true;
}
bool IsNativeX64(HANDLE process, std::string& error) {
    using Query = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT machine = 0, native = 0;
    if (!query || !query(process, &machine, &native) || machine != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_AMD64) { error = "A native Windows x64 game process is required."; return false; }
    return true;
}
bool VerifyGameDirectory(const std::wstring& directory, std::string& error) {
    std::wstring full; if (!LocalPath(directory, full, error) || !RejectReparsePath(full, false, error)) return false;
    if (!ExpectedFile(JoinPath(full, L"VampireSurvivors.exe"), supported::game_exe_sha256, error)
        || !ExpectedFile(JoinPath(full, L"GameAssembly.dll"), supported::game_sha256, error)
        || !ExpectedFile(JoinPath(full, L"VampireSurvivors_Data\\il2cpp_data\\Metadata\\global-metadata.dat"), supported::metadata_sha256, error)) return false;
    std::string manifest, build, app, install;
    if (!ReadSmallFile(JoinPath(ParentPath(ParentPath(full)), L"appmanifest_1794680.acf"), 1024 * 1024, manifest, error)
        || !VdfValue(manifest, "appid", app) || app != "1794680" || !VdfValue(manifest, "buildid", build) || build != supported::build_id
        || !VdfValue(manifest, "installdir", install) || !EqualPath(FileName(full), Wide(install))) { error = "The Steam installation/build does not match the supported game."; return false; }
    return true;
}
bool VerifyGameProcess(DWORD pid, GameIdentity& identity, std::string& error) {
    if (!pid) { error = "No supported game process is selected."; return false; }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) { error = "The game process is unavailable."; return false; }
    if (!SameUserAndSession(process.get(), pid, error) || !IsNativeX64(process.get(), error)) return false;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) { error = WinError("Read game executable"); return false; }
    path.resize(length);
    if (!EqualPath(FileName(path), L"VampireSurvivors.exe")) { error = "Only VampireSurvivors.exe is accepted."; return false; }
    auto directory = ParentPath(path);
    if (!VerifyGameDirectory(directory, error)) return false;
    identity = {std::move(directory), std::move(path), pid}; return true;
}
bool VerifyCurrentGame(GameIdentity& identity, std::string& error) { return VerifyGameProcess(GetCurrentProcessId(), identity, error); }
bool FindUniqueGame(GameIdentity& identity, std::string& error) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) { error = WinError("List game processes"); return false; }
    PROCESSENTRY32W entry{sizeof(entry)}; DWORD pid = 0;
    if (!Process32FirstW(snapshot.get(), &entry)) { error = WinError("List game processes"); return false; }
    do {
        if (!EqualPath(entry.szExeFile, L"VampireSurvivors.exe")) continue;
        if (pid) { error = "Keep exactly one game instance open."; return false; }
        pid = entry.th32ProcessID;
    } while (Process32NextW(snapshot.get(), &entry));
    if (!pid) { error = "Start the supported game before connecting the native pilot."; return false; }
    return VerifyGameProcess(pid, identity, error);
}
bool HasManagedBackend(DWORD pid, std::string& error) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) { error = WinError("Inspect game modules"); return true; }
    MODULEENTRY32W module{sizeof(module)};
    if (!Module32FirstW(snapshot.get(), &module)) { error = WinError("Inspect game modules"); return true; }
    std::vector<MODULEENTRY32W> modules;
    std::wstring game_directory;
    do { modules.push_back(module); if (EqualPath(module.szModule, L"VampireSurvivors.exe")) game_directory = ParentPath(module.szExePath); }
    while (Module32NextW(snapshot.get(), &module));
    for (const auto& item : modules) {
        module = item;
        if (EqualPath(module.szModule, L"coreclr.dll") || EqualPath(module.szModule, L"mono-2.0-bdwgc.dll")) {
            error = "The managed backend is active. Exit the game and uninstall the old companion plugin before this native test."; return true;
        }
        if (!game_directory.empty() && EqualPath(module.szModule, L"winhttp.dll") && EqualPath(ParentPath(module.szExePath), game_directory)) {
            error = "The old game loader is active. Exit the game and uninstall the old companion plugin before this native test."; return true;
        }
    }
    return false;
}
bool FindRemoteModule(DWORD pid, const std::wstring& path, uintptr_t& base, DWORD& size, std::string& error) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) { error = WinError("Inspect loaded module"); return false; }
    MODULEENTRY32W module{sizeof(module)};
    if (!Module32FirstW(snapshot.get(), &module)) { error = WinError("Inspect loaded module"); return false; }
    do { if (EqualPath(module.szExePath, path)) { base = reinterpret_cast<uintptr_t>(module.modBaseAddr); size = module.modBaseSize; return true; } }
    while (Module32NextW(snapshot.get(), &module));
    error = "The verified module has not been loaded."; return false;
}
bool IsReadableMemory(const void* pointer, size_t length) {
    uintptr_t current = reinterpret_cast<uintptr_t>(pointer);
    if (!current || length > std::numeric_limits<uintptr_t>::max() - current) return false;
    uintptr_t end = current + length;
    while (current < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        DWORD access = info.Protect & 0xff;
        if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY
            && access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
        uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= current) return false; current = std::min(next, end);
    }
    return true;
}
bool IsExecutableAddressInModule(void* pointer, HMODULE module) {
    if (!module || !IsReadableMemory(module, sizeof(IMAGE_DOS_HEADER))) return false;
    auto base = reinterpret_cast<unsigned char*>(module); auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 16 * 1024 * 1024 || !IsReadableMemory(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    auto sections = IMAGE_FIRST_SECTION(nt);
    if (!IsReadableMemory(sections, nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) return false;
    auto address = reinterpret_cast<uintptr_t>(pointer), start = reinterpret_cast<uintptr_t>(base);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t section = start + sections[i].VirtualAddress;
        if (address >= section && address - section < sections[i].Misc.VirtualSize) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT || info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
            DWORD protection = info.Protect & 0xff;
            return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        }
    }
    return false;
}
bool ExportRva(const std::wstring& path, const char* name, DWORD& rva, std::string& error) {
    std::string bytes; if (!ReadSmallFile(path, 16 * 1024 * 1024, bytes, error)) return false;
    auto within = [&](size_t offset, size_t size) { return offset <= bytes.size() && size <= bytes.size() - offset; };
    if (!within(0, sizeof(IMAGE_DOS_HEADER))) { error = "Truncated native DLL."; return false; }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || !within(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) { error = "Invalid native DLL image."; return false; }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { error = "Native DLL must be PE32+ x64."; return false; }
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    size_t section_offset = reinterpret_cast<const char*>(sections) - bytes.data();
    if (!within(section_offset, nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) { error = "Invalid native section table."; return false; }
    auto offset = [&](DWORD value, size_t size) -> size_t {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (value >= sections[i].VirtualAddress) {
                auto delta = value - sections[i].VirtualAddress;
                if (delta <= sections[i].SizeOfRawData && size <= sections[i].SizeOfRawData - delta) {
                    size_t result = static_cast<size_t>(sections[i].PointerToRawData) + delta;
                    if (within(result, size)) return result;
                }
            }
        }
        return SIZE_MAX;
    };
    auto table = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    size_t table_offset = offset(table.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!table.VirtualAddress || table_offset == SIZE_MAX) { error = "Native DLL export table is missing."; return false; }
    auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(bytes.data() + table_offset);
    if (exports->NumberOfNames > 65536 || exports->NumberOfFunctions > 65536) { error = "Invalid native export count."; return false; }
    size_t names = offset(exports->AddressOfNames, exports->NumberOfNames * 4ull), ordinals = offset(exports->AddressOfNameOrdinals, exports->NumberOfNames * 2ull), functions = offset(exports->AddressOfFunctions, exports->NumberOfFunctions * 4ull);
    if (names == SIZE_MAX || ordinals == SIZE_MAX || functions == SIZE_MAX) { error = "Invalid native export arrays."; return false; }
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        DWORD name_rva = 0; std::memcpy(&name_rva, bytes.data() + names + i * 4, 4);
        size_t start = offset(name_rva, 1); if (start == SIZE_MAX) continue;
        size_t length = 0; while (length < 256 && within(start + length, 1) && bytes[start + length]) ++length;
        if (length == 256 || !within(start + length, 1) || std::string(bytes.data() + start, length) != name) continue;
        WORD ordinal = 0; std::memcpy(&ordinal, bytes.data() + ordinals + i * 2, 2);
        if (ordinal >= exports->NumberOfFunctions) break;
        DWORD value = 0; std::memcpy(&value, bytes.data() + functions + ordinal * 4, 4);
        if (value >= table.VirtualAddress && static_cast<uint64_t>(value) < static_cast<uint64_t>(table.VirtualAddress) + table.Size) break;
        for (WORD section = 0; section < nt->FileHeader.NumberOfSections; ++section) {
            if ((sections[section].Characteristics & IMAGE_SCN_MEM_EXECUTE) && value >= sections[section].VirtualAddress && value - sections[section].VirtualAddress < sections[section].Misc.VirtualSize) { rva = value; return true; }
        }
        break;
    }
    error = "Required native startup export is absent or forwarded."; return false;
}
} // namespace vst
