#pragma once
#ifdef _WIN32
#include "../client/maintenance.hpp"
#include "../vendor/yyjson/yyjson.h"
#include "supported_game.hpp"
#include <winioctl.h>
#include <algorithm>
#include <iterator>
#include <filesystem>
#include <cstdio>
#include <memory>

namespace vst_backup_tests {
namespace detail {
inline bool Check(bool result, const char* expression, int line) {
    if (!result) std::fprintf(stderr, "  backup fixture line %d: %s\n", line, expression);
    return result;
}
#define VST_BACKUP_CHECK(...) do { if (!::vst_backup_tests::detail::Check((__VA_ARGS__), #__VA_ARGS__, __LINE__)) return false; } while (false)

inline bool Put(const std::wstring& path, const std::string& bytes) {
    std::error_code ec; std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) return false;
    vst::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    return file && WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
}
inline std::string Get(const std::wstring& path) { std::string bytes, error; vst::ReadSmallFile(path, 2 * 1024 * 1024, bytes, error); return bytes; }
inline bool Junction(const std::wstring& link, const std::wstring& destination) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    vst::Handle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) return false;
    auto target = L"\\??\\" + destination;
    struct Buffer { DWORD tag; WORD length, reserved; WORD sub_offset, sub_length, print_offset, print_length; wchar_t path[1024]; };
    Buffer buffer{}; buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    if (target.size() + 1 >= std::size(buffer.path)) return false;
    buffer.sub_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    buffer.print_offset = static_cast<WORD>(buffer.sub_length + sizeof(wchar_t));
    std::copy(target.begin(), target.end(), buffer.path);
    buffer.length = static_cast<WORD>(8 + buffer.sub_length + 2 * sizeof(wchar_t));
    DWORD received = 0;
    return DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, &buffer, buffer.length + 8, nullptr, 0, &received, nullptr) != FALSE;
}
struct Fixture {
    std::wstring root, savedata, cache, cloud, prefs, roaming, outsider;
    vst::maintenance::Options options;
    bool good = false, stopped = true;
    unsigned stopped_calls = 0;
    std::function<bool(unsigned)> on_stopped;
    Fixture() {
        std::wstring cwd(32768, L'\0'); DWORD size = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
        if (!size || size >= cwd.size()) return;
        cwd.resize(size); std::string error;
        // Fixture writes are confined to the ignored checkout artifacts tree.
        if (GetFileAttributesW(vst::JoinPath(cwd, L"native\\version.json").c_str()) == INVALID_FILE_ATTRIBUTES
            || !vst::RejectReparsePath(cwd, false, error)) return;
        auto base = vst::JoinPath(cwd, L"artifacts\\native\\maintenance-fixtures");
        root = vst::JoinPath(base, std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(vst::CurrentFileTime()));
        std::error_code ec; std::filesystem::create_directories(root, ec);
        if (ec || !vst::RejectReparsePath(root, false, error) || !vst::PathWithin(root, base)) return;
        auto steam = vst::JoinPath(root, L"steam");
        options.game_directory = vst::JoinPath(steam, L"steamapps\\common\\Fixture Game");
        options.profile_directory = vst::JoinPath(root, L"profile"); options.data_directory = vst::JoinPath(root, L"data");
        options.steam_directories = {steam};
        options.stopped_check = [this](std::string& error) {
            ++stopped_calls;
            if (!stopped || (on_stopped && !on_stopped(stopped_calls))) { error = "Offline fixture process is running."; return false; }
            return true;
        };
        auto app = vst::JoinPath(steam, L"userdata\\42\\1794680");
        savedata = vst::JoinPath(app, L"remote\\SaveData"); cache = vst::JoinPath(app, L"remotecache.vdf");
        cloud = vst::JoinPath(app, L"remote\\steam_autocloud.vdf");
        prefs = vst::JoinPath(options.profile_directory, L"AppData\\LocalLow\\poncle\\Vampire Survivors\\prefs.bin");
        roaming = vst::JoinPath(options.profile_directory, L"AppData\\Roaming\\Vampire_Survivors_42\\saves\\backup.sav");
        outsider = vst::JoinPath(root, L"unrelated\\do-not-touch.txt");
        good = Put(vst::JoinPath(options.game_directory, L"VampireSurvivors.exe"), "offline exe")
            && Put(vst::JoinPath(options.game_directory, L"GameAssembly.dll"), "offline assembly")
            && Put(vst::JoinPath(options.game_directory, L"VampireSurvivors_Data\\il2cpp_data\\Metadata\\global-metadata.dat"), "offline metadata")
            && Put(vst::JoinPath(steam, L"steamapps\\appmanifest_1794680.acf"), "\"AppState\" { \"appid\" \"1794680\" \"buildid\" \"offline\" \"LastOwner\" \"76561197960265770\" }")
            && Put(savedata, "save-v1") && Put(cache, "\"SaveData\" { \"localtime\" \"1\" }") && Put(cloud, "cloud-account-v1")
            && Put(prefs, "prefs-v1") && Put(roaming, "roaming-v1") && Put(outsider, "unrelated");
    }
    ~Fixture() {
        if (root.empty()) return;
        // Never delete a computed path unless it is this exact, checked fixture leaf.
        std::string ignored; std::wstring full;
        if (!vst::LocalPath(root, full, ignored) || !vst::EqualPath(full, root) || vst::FileName(vst::ParentPath(root)) != L"maintenance-fixtures") return;
        std::error_code ec; std::filesystem::remove_all(std::filesystem::path(root), ec);
    }
};
inline bool EditJson(const std::wstring& path, const std::function<void(yyjson_mut_doc*, yyjson_mut_val*)>& edit) {
    auto text = Get(path);
    const char* bytes = text.data(); size_t length = text.size();
    if (length >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) { bytes += 3; length -= 3; }
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> immutable(yyjson_read(bytes, length, 0), yyjson_doc_free);
    if (!immutable) return false;
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_doc_mut_copy(immutable.get(), nullptr), yyjson_mut_doc_free);
    if (!doc) return false;
    edit(doc.get(), yyjson_mut_doc_get_root(doc.get()));
    size_t size;
    std::unique_ptr<char, decltype(&free)> output(yyjson_mut_write(doc.get(), 0, &size), free);
    return output && Put(path, std::string(output.get(), size));
}
inline void Set(yyjson_mut_doc* doc, yyjson_mut_val* root, const char* key, const std::string& value) {
    yyjson_mut_obj_remove_key(root, key); yyjson_mut_obj_add_strcpy(doc, root, key, value.c_str());
}
inline bool PinnedProof(const vst::maintenance::BackupInfo& info, uint64_t created) {
    return EditJson(info.proof_path, [&](auto doc, auto root) {
        Set(doc, root, "createdUtc", vst::BackupTimestamp(created));
        Set(doc, root, "gameAssemblySha256", vst::supported::game_sha256); Set(doc, root, "metadataSha256", vst::supported::metadata_sha256);
    });
}
} // namespace detail

inline bool ProofBoundsTimeAndFingerprint() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    const auto created = vst::CurrentFileTime() - 10000000;
    VST_BACKUP_CHECK(detail::PinnedProof(backup, created));
    auto pinned_text = detail::Get(backup.proof_path);
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, std::string("\xef\xbb\xbf") + pinned_text));
    vst::GameIdentity game{fixture.options.game_directory, vst::JoinPath(fixture.options.game_directory, L"VampireSurvivors.exe"), 1};
    uint64_t expires = 99;
    VST_BACKUP_CHECK(vst::VerifyBackupProof(backup.proof_path, game, created + 1, created + vst::kBackupLifetimeTicks, error, &expires));
    VST_BACKUP_CHECK(expires == created + vst::kBackupLifetimeTicks);
    VST_BACKUP_CHECK(!vst::VerifyBackupProof(backup.proof_path, game, created + 1, created + vst::kBackupLifetimeTicks + 1, error, &expires) && expires == 0);
    VST_BACKUP_CHECK(!vst::VerifyBackupProof(backup.proof_path, game, created - 1, created, error));
    VST_BACKUP_CHECK(!vst::VerifyBackupProof(backup.proof_path, game, created + 1, created - 1, error));
    auto wrong = game; wrong.directory = fixture.root;
    VST_BACKUP_CHECK(!vst::VerifyBackupProof(backup.proof_path, wrong, created + 1, created + 1, error));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) { detail::Set(doc, root, "metadataSha256", std::string(64, '0')); }));
    VST_BACKUP_CHECK(!vst::VerifyBackupProof(backup.proof_path, game, created + 1, created + 1, error));
    vst::BackupProofDocument parsed;
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.manifest_path, parsed, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, pinned_text));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        yyjson_mut_obj_remove_key(root, "files"); yyjson_mut_obj_add_arr(doc, root, "files");
    }));
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.proof_path, parsed, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, pinned_text));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        auto files = yyjson_mut_obj_get(root, "files");
        for (size_t index = yyjson_mut_arr_size(files); index <= vst::kMaximumBackupFiles; ++index) yyjson_mut_arr_add_obj(doc, files);
    }));
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.proof_path, parsed, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, std::string(vst::kMaximumBackupDocumentBytes + 1, ' ')));
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.proof_path, parsed, error));
    uint64_t time = 0;
    VST_BACKUP_CHECK(vst::ParseBackupTimestamp("2026-11-17T10:20:30.1234567Z", time));
    VST_BACKUP_CHECK(vst::BackupTimestamp(time) == "2026-11-17T10:20:30.1234567Z");
    uint64_t offset_time = 0;
    VST_BACKUP_CHECK(vst::ParseBackupTimestamp("2026-11-17T11:20:30.1234567+01:00", offset_time) && offset_time == time);
    VST_BACKUP_CHECK(!vst::ParseBackupTimestamp("2026-02-30T10:20:30Z", time));
    VST_BACKUP_CHECK(!vst::ParseBackupTimestamp("2026-11-17T10:20:30", time));
    std::wstring local;
    VST_BACKUP_CHECK(!vst::BackupLocalPath(vst::JoinPath(fixture.root, L".. \\escape"), local, error));
    VST_BACKUP_CHECK(!vst::BackupLocalPath(vst::JoinPath(fixture.root, L"SaveData. "), local, error));
    VST_BACKUP_CHECK(!vst::BackupLocalPath(vst::JoinPath(fixture.root, L"NUL.txt"), local, error));
    VST_BACKUP_CHECK(!vst::BackupLocalPath(vst::JoinPath(fixture.root, L"COM1"), local, error));
    return true;
}

inline bool ProofTamperDuplicateAndEscapeRejected() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    auto original = detail::Get(backup.proof_path); vst::BackupProofDocument proof;
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, proof, error));
    VST_BACKUP_CHECK(vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        auto files = yyjson_mut_obj_get(root, "files");
        auto second = yyjson_mut_arr_get(files, 1), first = yyjson_mut_arr_get(files, 0);
        auto text = std::string(yyjson_mut_get_str(yyjson_mut_obj_get(first, "originalPath")));
        for (auto& c : text) if (c >= 'a' && c <= 'z') c += 'A' - 'a';
        detail::Set(doc, second, "originalPath", text);
    }));
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, proof, error));
    VST_BACKUP_CHECK(!vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, original));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        auto first = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "files"), 0);
        detail::Set(doc, first, "originalPath", "\\\\server\\share\\SaveData");
    }));
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.proof_path, proof, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, original));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        auto first = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "files"), 0);
        detail::Set(doc, first, "backupPath", vst::Utf8(fixture.outsider));
    }));
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, proof, error));
    VST_BACKUP_CHECK(!vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, original));
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, proof, error));
    auto content = detail::Get(proof.files.front().backup_path);
    VST_BACKUP_CHECK(detail::Put(proof.files.front().backup_path, content + "tampered"));
    VST_BACKUP_CHECK(!vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(detail::Put(proof.files.front().backup_path, content));
    VST_BACKUP_CHECK(DeleteFileW(proof.files.front().backup_path.c_str()));
    VST_BACKUP_CHECK(!vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(detail::Put(backup.proof_path, original));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) { yyjson_mut_obj_add_strcpy(doc, root, "APPID", "1794680"); }));
    VST_BACKUP_CHECK(!vst::ReadBackupProof(backup.proof_path, proof, error));
    return true;
}

inline bool MaintenanceCaptureRestoreAndCloudPreservation() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error) && backup.file_count == 5);
    std::vector<vst::maintenance::BackupInfo> backups;
    VST_BACKUP_CHECK(vst::maintenance::ListBackups(fixture.options, backups, error) && backups.size() == 1);
    std::wstring proof;
    VST_BACKUP_CHECK(vst::maintenance::LatestSessionProof(fixture.options, proof, error) && vst::EqualPath(proof, backup.proof_path));
    auto extra = vst::JoinPath(vst::ParentPath(fixture.savedata), L"extra-save.bin");
    VST_BACKUP_CHECK(detail::Put(fixture.savedata, "save-v2") && detail::Put(fixture.prefs, "prefs-v2") && detail::Put(extra, "extra-v2"));
    VST_BACKUP_CHECK(detail::Put(fixture.cache, "\"SaveData\" { \"localtime\" \"2\" }") && detail::Put(fixture.cloud, "cloud-account-v2"));
    vst::maintenance::RestoreResult restore;
    VST_BACKUP_CHECK(vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, restore, error));
    VST_BACKUP_CHECK(restore.restored_file_count == 3 && !restore.rolled_back && restore.safety_backup.file_count == 6);
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "save-v1" && detail::Get(fixture.prefs) == "prefs-v1");
    VST_BACKUP_CHECK(GetFileAttributesW(extra.c_str()) == INVALID_FILE_ATTRIBUTES);
    VST_BACKUP_CHECK(detail::Get(fixture.cache) == "\"SaveData\" { \"localtime\" \"2\" }" && detail::Get(fixture.cloud) == "cloud-account-v2");
    VST_BACKUP_CHECK(detail::Get(fixture.outsider) == "unrelated");
    vst::maintenance::RestoreResult undo;
    VST_BACKUP_CHECK(vst::maintenance::RestoreBackup(fixture.options, restore.safety_backup.manifest_path, undo, error));
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "save-v2" && detail::Get(extra) == "extra-v2");
    VST_BACKUP_CHECK(detail::Get(fixture.cloud) == "cloud-account-v2");
    return true;
}

inline bool MaintenanceLegacyProofOverlayAndMissingSaveRecovery() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    VST_BACKUP_CHECK(DeleteFileW(backup.manifest_path.c_str()));
    std::vector<vst::maintenance::BackupInfo> backups;
    VST_BACKUP_CHECK(vst::maintenance::ListBackups(fixture.options, backups, error) && backups.size() == 1
        && vst::EqualPath(backups.front().manifest_path, backup.proof_path));
    auto extra = vst::JoinPath(vst::ParentPath(fixture.savedata), L"extra-save.bin");
    VST_BACKUP_CHECK(detail::Put(extra, "keep-overlay-extra") && DeleteFileW(fixture.savedata.c_str()));
    vst::maintenance::RestoreResult restore;
    VST_BACKUP_CHECK(vst::maintenance::RestoreBackup(fixture.options, backup.proof_path, restore, error));
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "save-v1" && detail::Get(extra) == "keep-overlay-extra");
    vst::maintenance::RestoreResult undo;
    VST_BACKUP_CHECK(vst::maintenance::RestoreBackup(fixture.options, restore.safety_backup.manifest_path, undo, error));
    VST_BACKUP_CHECK(GetFileAttributesW(fixture.savedata.c_str()) == INVALID_FILE_ATTRIBUTES && detail::Get(extra) == "keep-overlay-extra");
    return true;
}

inline bool MaintenanceGuardsTamperAndWhitelist() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    fixture.stopped = false;
    VST_BACKUP_CHECK(!vst::maintenance::CreateBackup(fixture.options, backup, error));
    VST_BACKUP_CHECK(GetFileAttributesW(vst::JoinPath(fixture.options.data_directory, L"backups").c_str()) == INVALID_FILE_ATTRIBUTES);
    fixture.stopped = true;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    auto manifest = detail::Get(backup.manifest_path);
    VST_BACKUP_CHECK(detail::Put(fixture.savedata, "current-save"));
    vst::maintenance::RestoreResult restore;
    fixture.stopped = false;
    VST_BACKUP_CHECK(!vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, restore, error) && restore.safety_backup.id.empty());
    fixture.stopped = true;
    VST_BACKUP_CHECK(detail::EditJson(backup.manifest_path, [&](auto doc, auto root) {
        auto first = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "files"), 0);
        detail::Set(doc, first, "originalPath", vst::Utf8(fixture.outsider));
    }));
    VST_BACKUP_CHECK(!vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, restore, error) && restore.safety_backup.id.empty());
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "current-save" && detail::Get(fixture.outsider) == "unrelated");
    VST_BACKUP_CHECK(detail::Put(backup.manifest_path, manifest));
    VST_BACKUP_CHECK(detail::EditJson(backup.manifest_path, [&](auto doc, auto root) {
        auto first = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "files"), 0);
        detail::Set(doc, first, "relativeBackupPath", "..\\..\\outside.txt");
    }));
    VST_BACKUP_CHECK(!vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, restore, error) && restore.safety_backup.id.empty());
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "current-save");
    VST_BACKUP_CHECK(detail::Put(backup.manifest_path, manifest));
    vst::BackupProofDocument parsed;
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, parsed, error));
    VST_BACKUP_CHECK(detail::Put(parsed.files.front().backup_path, "corrupt"));
    VST_BACKUP_CHECK(!vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, restore, error) && restore.safety_backup.id.empty());
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "current-save");
    // A change during capture must not emit a complete proof.
    unsigned calls = 0;
    fixture.on_stopped = [&](unsigned) {
        if (++calls == 6) return detail::Put(fixture.prefs, "changed-after-copy");
        return true;
    };
    vst::maintenance::BackupInfo incomplete;
    VST_BACKUP_CHECK(!vst::maintenance::CreateBackup(fixture.options, incomplete, error));
    VST_BACKUP_CHECK(incomplete.proof_path.empty());
    return true;
}

inline bool MaintenanceRollbackAfterPartialApply() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    VST_BACKUP_CHECK(detail::Put(fixture.savedata, "before-failed-restore") && detail::Put(fixture.prefs, "prefs-before-failure"));
    // Make exactly one guarded mutation fail after the complete safety snapshot.
    bool injected = false;
    fixture.on_stopped = [&](unsigned) {
        std::vector<vst::maintenance::BackupInfo> list; std::string ignored;
        if (!vst::maintenance::ListBackups(fixture.options, list, ignored)) return true;
        if (!injected && list.size() >= 2 && detail::Get(fixture.prefs) == "prefs-v1") { injected = true; return false; }
        return true;
    };
    vst::maintenance::RestoreResult result;
    VST_BACKUP_CHECK(!vst::maintenance::RestoreBackup(fixture.options, backup.manifest_path, result, error));
    VST_BACKUP_CHECK(injected && result.rolled_back && !result.safety_backup.id.empty());
    VST_BACKUP_CHECK(detail::Get(fixture.savedata) == "before-failed-restore" && detail::Get(fixture.prefs) == "prefs-before-failure");
    return true;
}

inline bool MaintenanceReparseAndAmbiguousAccountsRejected() {
    detail::Fixture fixture; VST_BACKUP_CHECK(fixture.good);
    vst::maintenance::BackupInfo backup; std::string error;
    VST_BACKUP_CHECK(vst::maintenance::CreateBackup(fixture.options, backup, error));
    // Junctions need no developer mode or symlink privilege. Build one inside the fixture.
    auto link = vst::JoinPath(vst::ParentPath(fixture.savedata), L"linked");
    VST_BACKUP_CHECK(detail::Junction(link, vst::ParentPath(fixture.outsider)));
    vst::maintenance::BackupInfo rejected;
    VST_BACKUP_CHECK(!vst::maintenance::CreateBackup(fixture.options, rejected, error));
    VST_BACKUP_CHECK(RemoveDirectoryW(link.c_str()));
    VST_BACKUP_CHECK(detail::Get(fixture.outsider) == "unrelated");
    auto backup_link = vst::JoinPath(backup.directory, L"files\\linked");
    VST_BACKUP_CHECK(detail::Junction(backup_link, vst::ParentPath(fixture.outsider)));
    VST_BACKUP_CHECK(detail::EditJson(backup.proof_path, [&](auto doc, auto root) {
        auto first = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "files"), 0);
        detail::Set(doc, first, "backupPath", vst::Utf8(vst::JoinPath(backup_link, vst::FileName(fixture.outsider))));
    }));
    vst::BackupProofDocument proof;
    VST_BACKUP_CHECK(vst::ReadBackupProof(backup.proof_path, proof, error));
    VST_BACKUP_CHECK(!vst::ValidateBackupProofFiles(backup.proof_path, proof, true, error));
    VST_BACKUP_CHECK(RemoveDirectoryW(backup_link.c_str()));
    auto appmanifest = vst::JoinPath(vst::ParentPath(vst::ParentPath(fixture.options.game_directory)), L"appmanifest_1794680.acf");
    VST_BACKUP_CHECK(detail::Put(appmanifest, "\"AppState\" { \"appid\" \"1794680\" }")
        && detail::Put(vst::JoinPath(fixture.options.steam_directories.front(), L"userdata\\43\\1794680\\remote\\SaveData"), "other-account"));
    VST_BACKUP_CHECK(!vst::maintenance::CreateBackup(fixture.options, rejected, error));
    return true;
}

#undef VST_BACKUP_CHECK
} // namespace vst_backup_tests
#endif
