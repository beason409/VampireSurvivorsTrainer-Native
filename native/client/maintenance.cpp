#include "maintenance.hpp"
#include "../vendor/yyjson/yyjson.h"
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <memory>

namespace vst::maintenance {
namespace {
constexpr wchar_t kProofLeaf[] = L"session-proof.json";
struct Entry { std::wstring original, relative; std::string sha256; uint64_t length = 0; };
struct Manifest {
    std::string id, purpose = "session", created_utc, assembly_sha256, metadata_sha256, build_id;
    std::wstring game_directory;
    uint64_t created_file_time = 0;
    bool complete_inventory = false;
    std::vector<Entry> files;
};
struct Location { std::wstring path; bool directory = false, steam_remote = false, cloud_cache = false; };
struct Layout { Options options; std::vector<Location> locations; };

bool SameText(const std::string& a, const char* b) {
    if (a.size() != std::char_traits<char>::length(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}
yyjson_val* Property(yyjson_val* object, const char* name) {
    yyjson_val *key, *value, *found = nullptr; size_t i, size;
    if (!yyjson_is_obj(object)) return nullptr;
    yyjson_obj_foreach(object, i, size, key, value) {
        if (SameText(std::string(yyjson_get_str(key), yyjson_get_len(key)), name)) {
            if (found) return nullptr;
            found = value;
        }
    }
    return found;
}
bool String(yyjson_val* object, const char* key, std::string& value, bool allow_empty = false) {
    auto item = Property(object, key);
    if (!yyjson_is_str(item)) return false;
    value.assign(yyjson_get_str(item), yyjson_get_len(item));
    return (allow_empty || !value.empty()) && value.find('\0') == std::string::npos;
}
void Text(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, const std::string& value) {
    yyjson_mut_obj_add_strcpy(doc, object, key, value.c_str());
}
bool Exists(const std::wstring& path, bool& exists, bool& directory, std::string& error) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) { exists = directory = false; return true; }
        error = WinError("Inspect local file", code); return false;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) { error = "Linked files or directories are not accepted."; return false; }
    exists = true; directory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0; return true;
}
// Like RejectReparsePath, but allows an absent tail for a save that will be restored.
bool InspectPath(const std::wstring& input, std::string& error) {
    std::wstring path; if (!BackupLocalPath(input, path, error)) return false;
    for (size_t at = 3;;) {
        bool exists, directory;
        if (!Exists(path.substr(0, at), exists, directory, error)) return false;
        if (!exists) return true;
        if (at == path.size()) return true;
        if (!directory) { error = "A parent path is not a directory."; return false; }
        auto next = path.find(L'\\', at == 3 ? 3 : at + 1);
        at = next == std::wstring::npos ? path.size() : next;
    }
}
class PrivateSecurity {
public:
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
    bool Open(std::string& error) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) { error = WinError("Read backup owner"); return false; }
        Handle token(raw); DWORD size = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (!size || size > 65536) { error = "Cannot determine backup owner."; return false; }
        std::vector<unsigned char> bytes(size);
        if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) { error = WinError("Read backup owner"); return false; }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) { error = WinError("Read backup owner"); return false; }
        std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) { error = WinError("Protect backup storage"); return false; }
        attributes.lpSecurityDescriptor = descriptor; return true;
    }
};
bool Directory(const std::wstring& input, bool private_storage, std::string& error) {
    std::wstring path; if (!BackupLocalPath(input, path, error)) return false;
    PrivateSecurity security;
    if (private_storage && !security.Open(error)) return false;
    for (size_t at = 3;;) {
        auto part = path.substr(0, at); bool exists, directory;
        if (!Exists(part, exists, directory, error)) return false;
        if (!exists) {
            if (!CreateDirectoryW(part.c_str(), private_storage ? &security.attributes : nullptr)) {
                DWORD code = GetLastError();
                if (code != ERROR_ALREADY_EXISTS) { error = WinError("Create local directory", code); return false; }
            }
            if (!Exists(part, exists, directory, error)) return false;
        }
        if (!directory) { error = "Backup parent is not a directory."; return false; }
        if (at == path.size()) return true;
        auto next = path.find(L'\\', at == 3 ? 3 : at + 1);
        at = next == std::wstring::npos ? path.size() : next;
    }
}
std::string RandomId() {
    std::array<UCHAR, 8> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
bool FileLength(const std::wstring& path, uint64_t& size, std::string& error) {
    if (!RejectReparsePath(path, false, error)) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "Backup input is missing or not a file."; return false;
    }
    size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow; return true;
}
bool AtomicCopy(const std::wstring& source, const std::wstring& destination, const std::string& expected,
                bool private_storage, std::string& error) {
    if (!RejectReparsePath(source, false, error) || !InspectPath(destination, error)
        || !Directory(ParentPath(destination), private_storage, error)) return false;
    auto random = RandomId();
    if (random.empty()) { error = "Backup random source unavailable."; return false; }
    auto temporary = destination + L".partial-" + Wide(random);
    PrivateSecurity security;
    if (private_storage && !security.Open(error)) return false;
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION information{};
    if (!input || !GetFileInformationByHandle(input.get(), &information)
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = "Snapshot input is missing or linked."; return false;
    }
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              private_storage ? &security.attributes : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output) { error = WinError("Create snapshot file"); return false; }
    bool success = false;
    do {
        std::array<unsigned char, 65536> bytes{}; DWORD read = 0;
        bool copied = true;
        for (;;) {
            if (!ReadFile(input.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { copied = false; break; }
            if (!read) break;
            DWORD written = 0;
            if (!WriteFile(output.get(), bytes.data(), read, &written, nullptr) || written != read) { copied = false; break; }
        }
        if (!copied || !FlushFileBuffers(output.get())) { error = WinError("Copy and flush snapshot file"); break; }
        output.reset(); input.reset();
        std::string actual;
        if (!FileSha256(temporary, actual, error) || !SameSha256(actual, expected)) { error = "Snapshot copy SHA-256 mismatch."; break; }
        if (!InspectPath(destination, error) || !RejectReparsePath(ParentPath(destination), false, error)) break;
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = WinError("Apply snapshot file"); break; }
        success = true;
    } while (false);
    output.reset(); input.reset();
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}
bool AtomicJson(const std::wstring& destination, yyjson_mut_doc* doc, std::string& error) {
    size_t size = 0;
    std::unique_ptr<char, decltype(&free)> text(yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &size), free);
    if (!text || size > kMaximumBackupDocumentBytes) { error = "Backup record exceeds the document limit."; return false; }
    if (!Directory(ParentPath(destination), true, error) || !InspectPath(destination, error)) return false;
    auto random = RandomId();
    if (random.empty()) { error = "Backup random source unavailable."; return false; }
    auto temporary = destination + L".partial-" + Wide(random);
    PrivateSecurity security; if (!security.Open(error)) return false;
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) { error = WinError("Create backup record"); return false; }
    DWORD written = 0;
    bool success = WriteFile(file.get(), text.get(), static_cast<DWORD>(size), &written, nullptr) && written == size && FlushFileBuffers(file.get());
    if (!success) error = WinError("Write backup record");
    file.reset();
    if (success) success = InspectPath(destination, error) && MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!success) { if (error.empty()) error = WinError("Commit backup record"); DeleteFileW(temporary.c_str()); }
    return success;
}
bool Stopped(const Options& options, std::string& error) {
    if (options.stopped_check) return options.stopped_check(error);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) { error = WinError("Check game and Steam processes"); return false; }
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.get(), &entry)) { error = WinError("Check game and Steam processes"); return false; }
    do {
        if (EqualPath(entry.szExeFile, L"VampireSurvivors.exe") || EqualPath(entry.szExeFile, L"steam.exe")) {
            error = "Exit the game and Steam before backing up or restoring saves."; return false;
        }
    } while (Process32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) { error = WinError("Check game and Steam processes"); return false; }
    return true;
}
bool VdfValue(const std::string& text, const char* key, std::string& value) {
    bool found = false;
    for (size_t i = 0; i < text.size();) {
        if (text[i++] != '"') continue;
        std::string token;
        while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) ++i; token += text[i++]; }
        if (i == text.size()) return false; ++i;
        if (!SameText(token, key)) continue;
        if (found) return false;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
        if (i == text.size() || text[i++] != '"') return false;
        while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) ++i; value += text[i++]; }
        if (i == text.size()) return false; ++i; found = true;
    }
    return found;
}
bool Digits(const std::wstring& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
bool Children(const std::wstring& directory, bool directories, std::vector<std::wstring>& result, std::string& error) {
    if (!InspectPath(directory, error)) return false;
    bool exists, is_directory;
    if (!Exists(directory, exists, is_directory, error)) return false;
    if (!exists) return true;
    if (!is_directory) { error = "Save location is not a directory."; return false; }
    WIN32_FIND_DATAW item{};
    HANDLE raw = FindFirstFileW(JoinPath(directory, L"*").c_str(), &item);
    if (raw == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        error = WinError("Enumerate save directory"); return false;
    }
    bool success = true;
    do {
        if (EqualPath(item.cFileName, L".") || EqualPath(item.cFileName, L"..")) continue;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = "Linked save files or directories are not accepted."; success = false; break; }
        if (((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directories) result.push_back(JoinPath(directory, item.cFileName));
    } while (FindNextFileW(raw, &item));
    if (success && GetLastError() != ERROR_NO_MORE_FILES) { error = WinError("Enumerate save directory"); success = false; }
    FindClose(raw); return success;
}
bool Normalize(const Options& input, Options& out, std::string& error) {
    out = input;
    if (!BackupLocalPath(input.game_directory, out.game_directory, error) || !BackupLocalPath(input.data_directory, out.data_directory, error)
        || !BackupLocalPath(input.profile_directory, out.profile_directory, error) || !RejectReparsePath(out.game_directory, false, error)
        || !InspectPath(out.data_directory, error) || !InspectPath(out.profile_directory, error)) return false;
    out.steam_directories.clear();
    for (const auto& root : input.steam_directories) {
        std::wstring full;
        if (!BackupLocalPath(root, full, error) || !InspectPath(full, error)) return false;
        if (std::none_of(out.steam_directories.begin(), out.steam_directories.end(), [&](const auto& p) { return EqualPath(p, full); })) out.steam_directories.push_back(std::move(full));
    }
    return true;
}
std::wstring AppManifest(const std::wstring& game) {
    auto common = ParentPath(game), steamapps = ParentPath(common);
    if (!EqualPath(FileName(common), L"common") || !EqualPath(FileName(steamapps), L"steamapps")) return {};
    return JoinPath(steamapps, L"appmanifest_1794680.acf");
}
bool GetLayout(const Options& input, Layout& layout, std::string& error) {
    layout = {};
    if (!Normalize(input, layout.options, error)) return false;
    const auto& options = layout.options;
    std::wstring account; auto manifest = AppManifest(options.game_directory);
    if (!manifest.empty()) {
        bool exists, directory;
        if (!InspectPath(manifest, error) || !Exists(manifest, exists, directory, error)) return false;
        if (exists) {
            std::string text, owner;
            if (!ReadSmallFile(manifest, kMaximumBackupDocumentBytes, text, error)) return false;
            if (VdfValue(text, "LastOwner", owner)) {
                uint64_t id = 0; auto parsed = std::from_chars(owner.data(), owner.data() + owner.size(), id);
                constexpr uint64_t base = 76561197960265728ULL;
                if (parsed.ec == std::errc{} && parsed.ptr == owner.data() + owner.size() && id >= base) account = Wide(std::to_string(id - base));
            }
        }
    }
    std::vector<std::wstring> app_roots;
    for (const auto& steam : options.steam_directories) {
        auto userdata = JoinPath(steam, L"userdata");
        if (!account.empty()) app_roots.push_back(JoinPath(JoinPath(userdata, account), L"1794680"));
        else {
            std::vector<std::wstring> users;
            if (!Children(userdata, true, users, error)) return false;
            for (const auto& user : users) {
                if (!Digits(FileName(user))) continue;
                auto app = JoinPath(user, L"1794680"); bool exists, directory;
                if (!InspectPath(app, error) || !Exists(app, exists, directory, error)) return false;
                if (exists && directory) app_roots.push_back(std::move(app));
            }
        }
    }
    if (account.empty() && app_roots.size() > 1) { error = "Multiple Steam save accounts found; launch and exit the game through Steam first."; return false; }
    for (const auto& app : app_roots) {
        layout.locations.push_back({JoinPath(app, L"remote"), true, true, false});
        layout.locations.push_back({JoinPath(app, L"remotecache.vdf"), false, false, true});
    }
    auto roaming = JoinPath(options.profile_directory, L"AppData\\Roaming");
    layout.locations.push_back({JoinPath(roaming, L"Vampire_Survivors\\saves"), true});
    layout.locations.push_back({JoinPath(roaming, L"Vampire_Survivors_Data\\saves"), true});
    if (!account.empty()) layout.locations.push_back({JoinPath(roaming, L"Vampire_Survivors_" + account + L"\\saves"), true});
    auto local = JoinPath(options.profile_directory, L"AppData\\LocalLow\\poncle\\Vampire Survivors");
    layout.locations.push_back({JoinPath(local, L"prefs.bin"), false});
    layout.locations.push_back({JoinPath(local, L"prefs.bin.bak"), false});
    for (const auto& location : layout.locations) {
        if (!InspectPath(location.path, error)) return false;
        if (PathWithin(options.data_directory, location.path) || EqualPath(options.data_directory, location.path)
            || PathWithin(location.path, options.data_directory)) { error = "Backup storage must be separate from game saves."; return false; }
    }
    return true;
}
bool Allowed(const Layout& layout, const std::wstring& path) {
    return std::any_of(layout.locations.begin(), layout.locations.end(), [&](const auto& location) {
        return location.directory ? PathWithin(path, location.path) : EqualPath(path, location.path);
    });
}
bool SteamSave(const Layout& layout, const std::wstring& path) {
    return EqualPath(FileName(path), L"SaveData") && std::any_of(layout.locations.begin(), layout.locations.end(), [&](const auto& location) {
        return location.steam_remote && PathWithin(path, location.path);
    });
}
bool Cloud(const Layout& layout, const std::wstring& path) {
    return std::any_of(layout.locations.begin(), layout.locations.end(), [&](const auto& location) {
        return (location.cloud_cache && EqualPath(path, location.path))
            || (location.steam_remote && EqualPath(path, JoinPath(location.path, L"steam_autocloud.vdf")));
    });
}
bool WalkFiles(const std::wstring& directory, std::vector<std::wstring>& files, unsigned depth, std::string& error) {
    if (depth > 32) { error = "Save directory nesting exceeds the backup limit."; return false; }
    if (!Children(directory, false, files, error)) return false;
    if (files.size() > kMaximumBackupFiles) { error = "Too many save files for one backup."; return false; }
    std::vector<std::wstring> directories;
    if (!Children(directory, true, directories, error)) return false;
    for (const auto& child : directories) if (!WalkFiles(child, files, depth + 1, error)) return false;
    return true;
}
bool Inventory(const Layout& layout, std::vector<std::wstring>& files, std::string& error) {
    files.clear();
    for (const auto& location : layout.locations) {
        if (!InspectPath(location.path, error)) return false;
        bool exists, directory;
        if (!Exists(location.path, exists, directory, error)) return false;
        if (!exists) continue;
        if (location.directory) {
            if (!directory) { error = "A save directory is a regular file."; return false; }
            if (!WalkFiles(location.path, files, 0, error)) return false;
        }
        else { if (directory) { error = "A save file is a directory."; return false; } files.push_back(location.path); }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    files.erase(std::unique(files.begin(), files.end(), [](const auto& a, const auto& b) { return EqualPath(a, b); }), files.end());
    if (files.size() > kMaximumBackupFiles) { error = "Too many save files for one backup."; return false; }
    return true;
}
bool CaptureFingerprint(const Options& options, Manifest& manifest, std::string& error) {
    uint64_t ignored;
    if (!FileLength(JoinPath(options.game_directory, L"VampireSurvivors.exe"), ignored, error)
        || !FileSha256(JoinPath(options.game_directory, L"GameAssembly.dll"), manifest.assembly_sha256, error)
        || !FileSha256(JoinPath(options.game_directory, L"VampireSurvivors_Data\\il2cpp_data\\Metadata\\global-metadata.dat"), manifest.metadata_sha256, error)) return false;
    auto app = AppManifest(options.game_directory); bool exists, directory;
    if (!app.empty()) {
        if (!InspectPath(app, error) || !Exists(app, exists, directory, error)) return false;
        if (exists) {
            std::string text;
            if (!ReadSmallFile(app, kMaximumBackupDocumentBytes, text, error)) return false;
            VdfValue(text, "buildid", manifest.build_id);
        }
    }
    return true;
}
BackupInfo Info(const Manifest& manifest, const std::wstring& directory, const std::wstring& path) {
    return {manifest.id, manifest.created_utc, directory, path, JoinPath(directory, kProofLeaf), manifest.files.size()};
}
bool WriteManifest(const std::wstring& path, const Manifest& manifest, std::string& error) {
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
    if (!doc) { error = "Cannot allocate backup record."; return false; }
    auto root = yyjson_mut_obj(doc.get()); yyjson_mut_doc_set_root(doc.get(), root);
    yyjson_mut_obj_add_uint(doc.get(), root, "formatVersion", 1);
    Text(doc.get(), root, "purpose", manifest.purpose);
    yyjson_mut_obj_add_bool(doc.get(), root, "completeInventory", manifest.complete_inventory);
    Text(doc.get(), root, "id", manifest.id); Text(doc.get(), root, "createdUtc", manifest.created_utc);
    Text(doc.get(), root, "gameDirectory", Utf8(manifest.game_directory));
    auto fingerprint = yyjson_mut_obj_add_obj(doc.get(), root, "fingerprint");
    Text(doc.get(), fingerprint, "appId", "1794680"); Text(doc.get(), fingerprint, "buildId", manifest.build_id);
    Text(doc.get(), fingerprint, "unityVersion", ""); Text(doc.get(), fingerprint, "gameDirectory", Utf8(manifest.game_directory));
    Text(doc.get(), fingerprint, "gameAssemblySha256", manifest.assembly_sha256); Text(doc.get(), fingerprint, "metadataSha256", manifest.metadata_sha256);
    auto files = yyjson_mut_obj_add_arr(doc.get(), root, "files");
    for (const auto& entry : manifest.files) {
        auto file = yyjson_mut_arr_add_obj(doc.get(), files);
        Text(doc.get(), file, "originalPath", Utf8(entry.original)); Text(doc.get(), file, "relativeBackupPath", Utf8(entry.relative));
        Text(doc.get(), file, "sha256", entry.sha256); yyjson_mut_obj_add_uint(doc.get(), file, "length", entry.length);
    }
    return AtomicJson(path, doc.get(), error);
}
bool WriteProof(const std::wstring& path, const Manifest& manifest, std::string& error) {
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
    if (!doc) { error = "Cannot allocate backup proof."; return false; }
    auto root = yyjson_mut_obj(doc.get()); yyjson_mut_doc_set_root(doc.get(), root);
    Text(doc.get(), root, "appId", "1794680"); Text(doc.get(), root, "createdUtc", manifest.created_utc);
    Text(doc.get(), root, "gameDirectory", Utf8(manifest.game_directory));
    Text(doc.get(), root, "gameAssemblySha256", manifest.assembly_sha256); Text(doc.get(), root, "metadataSha256", manifest.metadata_sha256);
    auto files = yyjson_mut_obj_add_arr(doc.get(), root, "files");
    for (const auto& entry : manifest.files) {
        auto file = yyjson_mut_arr_add_obj(doc.get(), files);
        Text(doc.get(), file, "originalPath", Utf8(entry.original)); Text(doc.get(), file, "backupPath", Utf8(JoinPath(ParentPath(path), entry.relative)));
        Text(doc.get(), file, "sha256", entry.sha256);
    }
    return AtomicJson(path, doc.get(), error);
}
bool CreateCore(const Layout& layout, const char* purpose, BackupInfo& result, std::string& error) {
    result = {};
    if (!Stopped(layout.options, error)) return false;
    Manifest manifest; manifest.game_directory = layout.options.game_directory;
    manifest.purpose = purpose; manifest.complete_inventory = true;
    if (!CaptureFingerprint(layout.options, manifest, error)) return false;
    std::vector<std::wstring> files;
    if (!Inventory(layout, files, error)) return false;
    bool has_save = false;
    for (const auto& file : files) {
        uint64_t length;
        if (!FileLength(file, length, error)) return false;
        has_save |= length > 0 && SteamSave(layout, file);
    }
    if (manifest.purpose != "before-restore" && !has_save) { error = "No nonempty SaveData for the current Steam account was found."; return false; }
    manifest.created_file_time = CurrentFileTime(); manifest.created_utc = BackupTimestamp(manifest.created_file_time);
    std::string stamp = manifest.created_utc;
    stamp.erase(std::remove_if(stamp.begin(), stamp.end(), [](char c) { return c == '-' || c == ':' || c == '.' || c == 'T' || c == 'Z'; }), stamp.end());
    auto random = RandomId(); if (random.empty()) { error = "Backup random source unavailable."; return false; }
    manifest.id = manifest.purpose + "-" + stamp.substr(0, 8) + "-" + stamp.substr(8, 6) + "-" + stamp.substr(14) + "-" + random.substr(0, 6);
    auto root = JoinPath(layout.options.data_directory, L"backups");
    auto directory = JoinPath(root, Wide(manifest.id));
    if (!Directory(root, true, error)) return false;
    bool exists, is_directory;
    if (!Exists(directory, exists, is_directory, error) || exists) { error = "Backup identifier collision."; return false; }
    if (!Directory(directory, true, error)) return false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!Stopped(layout.options, error) || !Allowed(layout, files[i])) return false;
        char ordinal[16]; std::snprintf(ordinal, sizeof(ordinal), "%04u", static_cast<unsigned>(i));
        Entry entry; entry.original = files[i]; entry.relative = JoinPath(JoinPath(L"files", Wide(ordinal)), FileName(files[i]));
        if (!FileSha256(entry.original, entry.sha256, error)
            || !AtomicCopy(entry.original, JoinPath(directory, entry.relative), entry.sha256, true, error)
            || !FileLength(JoinPath(directory, entry.relative), entry.length, error)) return false;
        manifest.files.push_back(std::move(entry));
    }
    if (!Stopped(layout.options, error)) return false;
    for (const auto& entry : manifest.files) {
        std::string digest;
        if (!FileSha256(entry.original, digest, error) || !SameSha256(digest, entry.sha256)) { error = "A save changed during capture; wait for Steam sync and retry."; return false; }
    }
    std::vector<std::wstring> after;
    if (!Inventory(layout, after, error)) return false;
    if (after.size() != files.size() || !std::equal(after.begin(), after.end(), files.begin(), [](const auto& a, const auto& b) { return EqualPath(a, b); })) {
        error = "The save inventory changed during capture."; return false;
    }
    auto path = JoinPath(directory, L"manifest.json");
    if (!WriteManifest(path, manifest, error) || !Stopped(layout.options, error)
        || !WriteProof(JoinPath(directory, kProofLeaf), manifest, error)) return false;
    result = Info(manifest, directory, path); return true;
}
bool ReadManifest(const Options& options, const std::wstring& input, Manifest& manifest, std::string& error) {
    manifest = {};
    std::wstring path;
    if (!BackupLocalPath(input, path, error) || !PathWithin(path, JoinPath(options.data_directory, L"backups"))
        || !RejectReparsePath(path, false, error)) { error = "Select a backup inside this trainer's backup folder."; return false; }
    if (EqualPath(FileName(path), kProofLeaf)) {
        BackupProofDocument proof;
        if (!ReadBackupProof(path, proof, error)) return false;
        manifest.id = Utf8(FileName(ParentPath(path))); manifest.created_utc = proof.created_utc; manifest.created_file_time = proof.created_file_time;
        manifest.game_directory = proof.game_directory; manifest.assembly_sha256 = proof.game_assembly_sha256; manifest.metadata_sha256 = proof.metadata_sha256;
        auto prefix = ParentPath(path);
        for (const auto& file : proof.files) {
            uint64_t length;
            if (!PathWithin(file.backup_path, JoinPath(prefix, L"files")) || !FileLength(file.backup_path, length, error)) { error = "Invalid legacy backup path."; return false; }
            manifest.files.push_back({file.original_path, file.backup_path.substr(prefix.size() + 1), file.sha256, length});
        }
        return true;
    }
    if (!EqualPath(FileName(path), L"manifest.json")) { error = "Select a backup manifest or session proof."; return false; }
    std::string text;
    if (!ReadSmallFile(path, kMaximumBackupDocumentBytes, text, error)) return false;
    const char* bytes = text.data(); size_t size = text.size();
    if (size >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) { bytes += 3; size -= 3; }
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(bytes, size, 0), yyjson_doc_free);
    auto root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    std::string game;
    auto version = Property(root, "formatVersion");
    if (!yyjson_is_uint(version) || yyjson_get_uint(version) != 1
        || !String(root, "id", manifest.id) || !String(root, "createdUtc", manifest.created_utc)
        || !ParseBackupTimestamp(manifest.created_utc, manifest.created_file_time)
        || !String(root, "gameDirectory", game) || !BackupLocalPath(Wide(game), manifest.game_directory, error)) { error = "Invalid backup manifest."; return false; }
    auto purpose = Property(root, "purpose");
    if (purpose && !String(root, "purpose", manifest.purpose)) { error = "Invalid backup purpose."; return false; }
    auto inventory = Property(root, "completeInventory");
    if (inventory && !yyjson_is_bool(inventory)) { error = "Invalid backup inventory flag."; return false; }
    manifest.complete_inventory = yyjson_is_true(inventory);
    auto fingerprint = Property(root, "fingerprint");
    if (!String(fingerprint, "gameAssemblySha256", manifest.assembly_sha256) || !ValidSha256(manifest.assembly_sha256)
        || !String(fingerprint, "metadataSha256", manifest.metadata_sha256) || !ValidSha256(manifest.metadata_sha256)) {
        error = "Invalid backup fingerprint."; return false;
    }
    auto files = Property(root, "files");
    if (!yyjson_is_arr(files) || yyjson_arr_size(files) > kMaximumBackupFiles || (!yyjson_arr_size(files) && manifest.purpose != "before-restore")) {
        error = "Invalid backup inventory."; return false;
    }
    size_t i, count; yyjson_val* file;
    yyjson_arr_foreach(files, i, count, file) {
        Entry entry; std::string original, relative; auto length = Property(file, "length");
        if (!String(file, "originalPath", original) || !BackupLocalPath(Wide(original), entry.original, error)
            || !String(file, "relativeBackupPath", relative) || !String(file, "sha256", entry.sha256) || !ValidSha256(entry.sha256)
            || !yyjson_is_uint(length)) { error = "Invalid backup entry."; return false; }
        entry.relative = Wide(relative); entry.length = yyjson_get_uint(length);
        if (entry.relative.empty() || entry.relative[0] == L'\\' || entry.relative[0] == L'/' || entry.relative.find(L':') != std::wstring::npos) { error = "Invalid relative backup path."; return false; }
        manifest.files.push_back(std::move(entry));
    }
    return true;
}
bool ValidateManifest(const Layout& layout, const std::wstring& path, const Manifest& manifest, std::string& error) {
    if (!EqualPath(layout.options.game_directory, manifest.game_directory)) { error = "This backup belongs to a different game directory."; return false; }
    std::vector<std::wstring> originals, backups; bool has_save = false;
    auto directory = ParentPath(path);
    for (const auto& entry : manifest.files) {
        if (!Allowed(layout, entry.original) || !InspectPath(entry.original, error)) { error = "A backup target is outside this game's save locations or is linked."; return false; }
        std::wstring backup;
        if (!BackupLocalPath(JoinPath(directory, entry.relative), backup, error) || !PathWithin(backup, JoinPath(directory, L"files"))) { error = "A backup file escaped its snapshot folder."; return false; }
        if (std::any_of(originals.begin(), originals.end(), [&](const auto& p) { return EqualPath(entry.original, p); })
            || std::any_of(backups.begin(), backups.end(), [&](const auto& p) { return EqualPath(backup, p); })) { error = "Duplicate backup record."; return false; }
        originals.push_back(entry.original); backups.push_back(backup);
        uint64_t length; std::string digest;
        if (!FileLength(backup, length, error) || length != entry.length || !FileSha256(backup, digest, error) || !SameSha256(digest, entry.sha256)) {
            error = "Backup content verification failed; current saves were not changed."; return false;
        }
        has_save |= entry.length && SteamSave(layout, entry.original);
    }
    if (manifest.purpose != "before-restore" && !has_save) { error = "The backup lacks a nonempty Steam SaveData."; return false; }
    return true;
}
bool MarkChanged(const Layout& layout, const std::wstring& path, std::string& error) {
    uint64_t cached_second = 0; bool cached = false;
    auto remote = std::find_if(layout.locations.begin(), layout.locations.end(), [&](const auto& p) { return p.steam_remote && PathWithin(path, p.path); });
    if (remote == layout.locations.end()) { error = "SaveData is outside its Steam account."; return false; }
    auto cache = JoinPath(ParentPath(remote->path), L"remotecache.vdf"); bool exists, directory;
    if (!InspectPath(cache, error) || !Exists(cache, exists, directory, error)) return false;
    if (exists) {
        std::string text;
        if (!ReadSmallFile(cache, kMaximumBackupDocumentBytes, text, error)) return false;
        for (size_t at = 0; at < text.size();) {
            auto token = text.find('"', at); if (token == std::string::npos) break;
            auto end = text.find('"', token + 1); if (end == std::string::npos) break;
            at = end + 1;
            if (!SameText(text.substr(token + 1, end - token - 1), "SaveData")) continue;
            auto open = text.find('{', at), close = open == std::string::npos ? open : text.find('}', open + 1);
            if (close == std::string::npos) break;
            std::string seconds;
            if (VdfValue(text.substr(open + 1, close - open - 1), "localtime", seconds)) {
                auto parsed = std::from_chars(seconds.data(), seconds.data() + seconds.size(), cached_second);
                cached = parsed.ec == std::errc{} && parsed.ptr == seconds.data() + seconds.size();
            }
            break;
        }
    }
    constexpr uint64_t unix_epoch = 116444736000000000ULL;
    auto now = CurrentFileTime(); unsigned retries = 0;
    while (cached && now >= unix_epoch && (now - unix_epoch) / 10000000 == cached_second) {
        if (++retries > 100) { error = "The system clock did not advance; retain the safety backup and retry."; return false; }
        Sleep(20);
        if (!Stopped(layout.options, error)) return false;
        now = CurrentFileTime();
    }
    if (!Stopped(layout.options, error) || !RejectReparsePath(path, false, error)) return false;
    Handle file(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    FILETIME stamp{static_cast<DWORD>(now), static_cast<DWORD>(now >> 32)};
    if (!file || !SetFileTime(file.get(), nullptr, nullptr, &stamp)) { error = WinError("Mark restored SaveData changed"); return false; }
    return true;
}
bool Apply(const Layout& layout, const std::wstring& path, const Manifest& manifest, size_t& restored, std::string& error) {
    restored = 0;
    if (!ValidateManifest(layout, path, manifest, error)) return false;
    std::vector<std::wstring> extras;
    if (manifest.complete_inventory) {
        std::vector<std::wstring> files;
        if (!Inventory(layout, files, error)) return false;
        for (const auto& file : files) if (!Cloud(layout, file) && std::none_of(manifest.files.begin(), manifest.files.end(), [&](const auto& entry) { return EqualPath(file, entry.original); })) extras.push_back(file);
    }
    for (const auto& entry : manifest.files) {
        if (Cloud(layout, entry.original)) continue;
        if (!Stopped(layout.options, error) || !AtomicCopy(JoinPath(ParentPath(path), entry.relative), entry.original, entry.sha256, false, error)) return false;
        if (SteamSave(layout, entry.original) && !MarkChanged(layout, entry.original, error)) return false;
        ++restored;
    }
    for (const auto& file : extras) {
        if (!Stopped(layout.options, error) || !Allowed(layout, file) || !RejectReparsePath(file, false, error)) return false;
        if (!DeleteFileW(file.c_str())) { error = WinError("Remove save absent from snapshot"); return false; }
    }
    return true;
}
void RegistryPath(HKEY hive, const wchar_t* name, REGSAM view, std::vector<std::wstring>& paths) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return;
    std::array<wchar_t, 32768> value{}; DWORD size = static_cast<DWORD>(sizeof(value)), type = 0;
    auto result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size); RegCloseKey(key);
    if (result == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t) && size <= sizeof(value) && value.back() == 0) paths.emplace_back(value.data());
}
std::wstring KnownFolder(int folder) {
    std::array<wchar_t, MAX_PATH> value{};
    if (FAILED(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, value.data()))) return {};
    return value.data();
}
} // namespace

bool MakeDefaultOptions(const std::wstring& game, const std::wstring& data, Options& options, std::string& error) {
    error.clear(); Options candidate; candidate.game_directory = game; candidate.data_directory = data;
    candidate.profile_directory = KnownFolder(CSIDL_PROFILE);
    if (candidate.data_directory.empty()) {
        DWORD count = GetEnvironmentVariableW(L"VS_TRAINER_DATA", nullptr, 0);
        if (count > 1 && count < 32768) {
            std::wstring value(count, L'\0'); DWORD copied = GetEnvironmentVariableW(L"VS_TRAINER_DATA", value.data(), count);
            if (copied && copied < count) { value.resize(copied); candidate.data_directory = value; }
        }
        if (candidate.data_directory.empty()) {
            auto local = KnownFolder(CSIDL_LOCAL_APPDATA);
            if (local.empty()) { error = "Cannot find private application storage."; return false; }
            candidate.data_directory = JoinPath(local, L"Beason9527.VSTrainer.NativePilot\\data");
        }
    }
    RegistryPath(HKEY_CURRENT_USER, L"SteamPath", 0, candidate.steam_directories);
    RegistryPath(HKEY_LOCAL_MACHINE, L"InstallPath", KEY_WOW64_32KEY, candidate.steam_directories);
    RegistryPath(HKEY_LOCAL_MACHINE, L"InstallPath", KEY_WOW64_64KEY, candidate.steam_directories);
    auto program = KnownFolder(CSIDL_PROGRAM_FILESX86);
    if (!program.empty()) candidate.steam_directories.push_back(JoinPath(program, L"Steam"));
    return Normalize(candidate, options, error);
}
bool CreateBackup(const Options& options, BackupInfo& result, std::string& error) {
    error.clear(); Layout layout;
    return GetLayout(options, layout, error) && CreateCore(layout, "session", result, error);
}
bool ListBackups(const Options& options, std::vector<BackupInfo>& results, std::string& error) {
    error.clear(); results.clear(); Options normalized;
    if (!Normalize(options, normalized, error)) return false;
    std::vector<std::wstring> directories;
    if (!Children(JoinPath(normalized.data_directory, L"backups"), true, directories, error)) return false;
    for (const auto& directory : directories) {
        std::string ignored; bool exists, is_directory;
        auto proof = JoinPath(directory, kProofLeaf);
        if (!RejectReparsePath(proof, false, ignored) || !Exists(proof, exists, is_directory, ignored) || !exists || is_directory) continue;
        auto path = JoinPath(directory, L"manifest.json");
        if (!Exists(path, exists, is_directory, ignored)) continue;
        if (!exists) path = proof;
        Manifest manifest;
        if (!ReadManifest(normalized, path, manifest, ignored)) continue;
        results.push_back(Info(manifest, directory, path));
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.created_utc > b.created_utc; });
    return true;
}
bool RestoreBackup(const Options& options, const std::wstring& input, RestoreResult& result, std::string& error) {
    result = {}; error.clear(); Layout layout;
    if (!GetLayout(options, layout, error) || !Stopped(layout.options, error)) return false;
    std::wstring path; Manifest desired;
    if (!BackupLocalPath(input, path, error) || !ReadManifest(layout.options, path, desired, error)
        || !ValidateManifest(layout, path, desired, error)) return false;
    // A manifest is not complete until the companion proof was committed last.
    if (!RejectReparsePath(JoinPath(ParentPath(path), kProofLeaf), false, error)) return false;
    if (!CreateCore(layout, "before-restore", result.safety_backup, error)) return false;
    Manifest safety;
    if (!ReadManifest(layout.options, result.safety_backup.manifest_path, safety, error)) return false;
    if (Apply(layout, path, desired, result.restored_file_count, error)) return true;
    std::string original = error, rollback_error; size_t ignored = 0;
    if (Stopped(layout.options, rollback_error) && Apply(layout, result.safety_backup.manifest_path, safety, ignored, rollback_error)) {
        result.rolled_back = true; error = "Restore failed; the pre-restore snapshot was restored. " + original;
    } else error = "Restore and automatic rollback did not complete. Keep the pre-restore snapshot for recovery. " + original;
    return false;
}
bool LatestSessionProof(const Options& options, std::wstring& path, std::string& error) {
    error.clear(); path.clear(); Layout layout;
    if (!GetLayout(options, layout, error)) return false;
    std::vector<BackupInfo> backups;
    if (!ListBackups(layout.options, backups, error)) return false;
    Manifest current;
    if (!CaptureFingerprint(layout.options, current, error)) return false;
    const uint64_t now = CurrentFileTime();
    for (const auto& backup : backups) {
        BackupProofDocument proof; std::string ignored;
        if (!ReadBackupProof(backup.proof_path, proof, ignored) || !EqualPath(proof.game_directory, layout.options.game_directory)
            || proof.created_file_time > now || now - proof.created_file_time > kBackupLifetimeTicks
            || !SameSha256(proof.game_assembly_sha256, current.assembly_sha256) || !SameSha256(proof.metadata_sha256, current.metadata_sha256)) continue;
        if (!ValidateBackupProofFiles(backup.proof_path, proof, true, ignored)) continue;
        bool allowed = std::all_of(proof.files.begin(), proof.files.end(), [&](const auto& entry) { return Allowed(layout, entry.original_path) && InspectPath(entry.original_path, ignored); });
        if (!allowed || std::none_of(proof.files.begin(), proof.files.end(), [&](const auto& entry) { return SteamSave(layout, entry.original_path); })) continue;
        path = backup.proof_path; return true;
    }
    error = "Exit the game and Steam, then create a fresh backup before enabling no-gold-spend."; return false;
}
} // namespace vst::maintenance
