#include "backup_proof.hpp"
#include "supported_game.hpp"
#include "../vendor/yyjson/yyjson.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

namespace vst {
namespace {
bool AsciiEqual(const std::string& left, const char* right) {
    size_t size = std::char_traits<char>::length(right);
    if (left.size() != size) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]), b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}
yyjson_val* Property(yyjson_val* object, const char* name) {
    if (!yyjson_is_obj(object)) return nullptr;
    yyjson_val *key, *value; size_t index, count;
    yyjson_val* found = nullptr;
    yyjson_obj_foreach(object, index, count, key, value) {
        if (AsciiEqual(std::string(yyjson_get_str(key), yyjson_get_len(key)), name)) {
            if (found) return nullptr;
            found = value;
        }
    }
    return found;
}
bool String(yyjson_val* object, const char* key, std::string& out) {
    auto value = Property(object, key);
    if (!yyjson_is_str(value) || yyjson_get_len(value) == 0) return false;
    out.assign(yyjson_get_str(value), yyjson_get_len(value));
    return out.find('\0') == std::string::npos;
}
bool Path(yyjson_val* object, const char* key, std::wstring& out, std::string& error) {
    std::string text;
    if (!String(object, key, text)) return false;
    auto wide = Wide(text);
    return !wide.empty() && BackupLocalPath(wide, out, error);
}
bool Digits(const std::string& text, size_t start, size_t count, WORD& value) noexcept {
    if (start + count > text.size()) return false;
    value = 0;
    for (size_t i = start; i < start + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = static_cast<WORD>(value * 10 + text[i] - '0');
    }
    return true;
}
bool SteamSaveData(const std::wstring& path) {
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + L'a' - L'A' : c; });
    constexpr wchar_t suffix[] = L"\\1794680\\remote\\savedata";
    constexpr size_t size = sizeof(suffix) / sizeof(wchar_t) - 1;
    return lower.find(L"\\userdata\\") != std::wstring::npos && lower.size() > size
        && lower.compare(lower.size() - size, size, suffix) == 0;
}
}

uint64_t CurrentFileTime() noexcept {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
std::string BackupTimestamp(uint64_t value) {
    FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    SYSTEMTIME system{};
    if (!FileTimeToSystemTime(&time, &system)) return {};
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%07uZ",
                  system.wYear, system.wMonth, system.wDay, system.wHour, system.wMinute,
                  system.wSecond, static_cast<unsigned>(value % 10000000));
    return buffer;
}
bool ParseBackupTimestamp(const std::string& text, uint64_t& value) noexcept {
    value = 0;
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't')
        || text[13] != ':' || text[16] != ':') return false;
    SYSTEMTIME time{};
    if (!Digits(text, 0, 4, time.wYear) || !Digits(text, 5, 2, time.wMonth)
        || !Digits(text, 8, 2, time.wDay) || !Digits(text, 11, 2, time.wHour)
        || !Digits(text, 14, 2, time.wMinute) || !Digits(text, 17, 2, time.wSecond)) return false;
    FILETIME file{};
    if (time.wYear < 1601 || !SystemTimeToFileTime(&time, &file)) return false;
    SYSTEMTIME check{};
    if (!FileTimeToSystemTime(&file, &check) || check.wYear != time.wYear || check.wMonth != time.wMonth || check.wDay != time.wDay
        || check.wHour != time.wHour || check.wMinute != time.wMinute || check.wSecond != time.wSecond) return false;
    uint64_t fraction = 0; size_t at = 19;
    if (at < text.size() && text[at] == '.') {
        size_t start = ++at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (at - start >= 7) return false;
            fraction = fraction * 10 + text[at++] - '0';
        }
        if (at == start) return false;
        for (size_t i = at - start; i < 7; ++i) fraction *= 10;
    }
    int offset_minutes = 0;
    if (at + 1 == text.size() && (text[at] == 'Z' || text[at] == 'z')) ++at;
    else if (at + 6 == text.size() && (text[at] == '+' || text[at] == '-') && text[at + 3] == ':') {
        WORD hours, minutes;
        if (!Digits(text, at + 1, 2, hours) || !Digits(text, at + 4, 2, minutes) || hours > 14 || minutes > 59 || (hours == 14 && minutes)) return false;
        offset_minutes = (hours * 60 + minutes) * (text[at] == '+' ? 1 : -1); at += 6;
    } else return false;
    if (at != text.size()) return false;
    uint64_t ticks = (static_cast<uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    int64_t offset = static_cast<int64_t>(offset_minutes) * 60 * 10000000;
    if ((offset > 0 && ticks < static_cast<uint64_t>(offset)) || ticks > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - fraction) return false;
    value = static_cast<uint64_t>(static_cast<int64_t>(ticks + fraction) - offset);
    return value != 0;
}
bool ValidSha256(const std::string& hash) noexcept {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool SameSha256(const std::string& left, const std::string& right) noexcept {
    if (!ValidSha256(left) || !ValidSha256(right)) return false;
    for (size_t i = 0; i < 64; ++i) {
        char a = left[i], b = right[i];
        if (a >= 'A' && a <= 'F') a += 'a' - 'A';
        if (b >= 'A' && b <= 'F') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}
bool PathWithin(const std::wstring& child, const std::wstring& directory) noexcept {
    return child.size() > directory.size() && child[directory.size()] == L'\\'
        && CompareStringOrdinal(child.data(), static_cast<int>(directory.size()), directory.data(), static_cast<int>(directory.size()), TRUE) == CSTR_EQUAL;
}
bool BackupLocalPath(const std::wstring& input, std::wstring& full, std::string& error) {
    auto valid_parts = [&](const std::wstring& path, bool allow_relative) {
      for (size_t at = 3; at < path.size();) {
        auto next = path.find_first_of(L"\\/", at);
        auto part = path.substr(at, next == std::wstring::npos ? path.size() - at : next - at);
        if (allow_relative && (part == L"." || part == L"..")) {
            if (next == std::wstring::npos) break;
            at = next + 1; continue;
        }
        if (part.empty() || part.back() == L'.' || part.back() == L' ' || part.find_first_of(L"<>\"|?*") != std::wstring::npos) {
            error = "Backup paths cannot use ambiguous Windows file names."; return false;
        }
        auto dot = part.find(L'.'); auto base = part.substr(0, dot);
        bool port = base.size() == 4 && (EqualPath(base.substr(0, 3), L"COM") || EqualPath(base.substr(0, 3), L"LPT"))
            && ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3');
        if (EqualPath(base, L"CON") || EqualPath(base, L"PRN") || EqualPath(base, L"AUX") || EqualPath(base, L"NUL")
            || EqualPath(base, L"CONIN$") || EqualPath(base, L"CONOUT$") || port) {
            error = "Windows device paths are not backup files."; return false;
        }
        if (next == std::wstring::npos) break;
        at = next + 1;
      }
      return true;
    };
    return valid_parts(input, true) && LocalPath(input, full, error) && valid_parts(full, false);
}
bool ReadBackupProof(const std::wstring& path, BackupProofDocument& proof, std::string& error) {
    proof = {}; error.clear();
    std::wstring full;
    if (!BackupLocalPath(path, full, error) || !EqualPath(FileName(full), L"session-proof.json")) {
        error = "Select a local session backup proof."; return false;
    }
    std::string text;
    if (!ReadSmallFile(full, kMaximumBackupDocumentBytes, text, error)) return false;
    const char* data = text.data(); size_t size = text.size();
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xef && static_cast<unsigned char>(data[1]) == 0xbb && static_cast<unsigned char>(data[2]) == 0xbf) { data += 3; size -= 3; }
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(data, size, 0), yyjson_doc_free);
    auto root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
    if (!String(root, "appId", proof.app_id) || proof.app_id != "1794680"
        || !String(root, "createdUtc", proof.created_utc) || !ParseBackupTimestamp(proof.created_utc, proof.created_file_time)
        || !Path(root, "gameDirectory", proof.game_directory, error)
        || !String(root, "gameAssemblySha256", proof.game_assembly_sha256) || !ValidSha256(proof.game_assembly_sha256)
        || !String(root, "metadataSha256", proof.metadata_sha256) || !ValidSha256(proof.metadata_sha256)) {
        error = "Invalid session backup proof."; return false;
    }
    auto files = Property(root, "files");
    if (!yyjson_is_arr(files) || yyjson_arr_size(files) < 1 || yyjson_arr_size(files) > kMaximumBackupFiles) {
        error = "Invalid backup file count."; return false;
    }
    size_t index, count; yyjson_val* value;
    yyjson_arr_foreach(files, index, count, value) {
        BackupProofFile entry;
        if (!Path(value, "originalPath", entry.original_path, error) || !Path(value, "backupPath", entry.backup_path, error)
            || !String(value, "sha256", entry.sha256) || !ValidSha256(entry.sha256)) {
            error = "Invalid backup file record."; return false;
        }
        proof.files.push_back(std::move(entry));
    }
    return true;
}
bool ValidateBackupProofFiles(const std::wstring& path, const BackupProofDocument& proof, bool require_save, std::string& error) {
    std::wstring full;
    if (!BackupLocalPath(path, full, error) || !RejectReparsePath(full, false, error)) return false;
    auto files = JoinPath(ParentPath(full), L"files");
    if (proof.files.empty() || proof.files.size() > kMaximumBackupFiles) { error = "Invalid backup file count."; return false; }
    bool has_save = false;
    std::vector<std::wstring> originals, backups;
    for (const auto& entry : proof.files) {
        std::wstring original, backup;
        if (!BackupLocalPath(entry.original_path, original, error) || !BackupLocalPath(entry.backup_path, backup, error)
            || !PathWithin(backup, files) || !RejectReparsePath(backup, false, error)) {
            error = "Backup paths must stay inside their local snapshot."; return false;
        }
        if (std::any_of(originals.begin(), originals.end(), [&](const auto& p) { return EqualPath(p, original); })
            || std::any_of(backups.begin(), backups.end(), [&](const auto& p) { return EqualPath(p, backup); })) {
            error = "Duplicate backup file record."; return false;
        }
        originals.push_back(original); backups.push_back(backup);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(backup.c_str(), GetFileExInfoStandard, &attributes) || (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            error = "Backup file is missing or linked."; return false;
        }
        std::string digest;
        if (!FileSha256(backup, digest, error) || !SameSha256(digest, entry.sha256)) { error = "Backup file SHA-256 mismatch."; return false; }
        has_save |= SteamSaveData(original) && (attributes.nFileSizeHigh || attributes.nFileSizeLow);
    }
    if (require_save && !has_save) { error = "The backup does not contain a nonempty Steam SaveData."; return false; }
    return true;
}
bool VerifyBackupProof(const std::wstring& path, const GameIdentity& game, uint64_t start, uint64_t now, std::string& error, uint64_t* expires) {
    if (expires) *expires = 0;
    BackupProofDocument proof;
    if (!ReadBackupProof(path, proof, error)) return false;
    if (!start || !now || proof.created_file_time > start || proof.created_file_time > now
        || now - proof.created_file_time > kBackupLifetimeTicks) {
        error = "Create a backup before this game launch, within the last 12 hours."; return false;
    }
    std::wstring directory;
    if (!BackupLocalPath(game.directory, directory, error) || !EqualPath(proof.game_directory, directory)
        || !SameSha256(proof.game_assembly_sha256, supported::game_sha256)
        || !SameSha256(proof.metadata_sha256, supported::metadata_sha256)) {
        error = "The backup belongs to a different game directory or version."; return false;
    }
    if (!ValidateBackupProofFiles(path, proof, true, error)) return false;
    if (expires) *expires = proof.created_file_time + kBackupLifetimeTicks;
    return true;
}
} // namespace vst
