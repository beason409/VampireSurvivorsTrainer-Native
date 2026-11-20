#pragma once
#include "win_util.hpp"

namespace vst {

inline constexpr size_t kMaximumBackupDocumentBytes = 1024 * 1024;
inline constexpr size_t kMaximumBackupFiles = 256;
inline constexpr uint64_t kBackupLifetimeTicks = 12ULL * 60 * 60 * 10000000;

struct BackupProofFile {
    std::wstring original_path, backup_path;
    std::string sha256;
};
struct BackupProofDocument {
    std::string app_id, created_utc, game_assembly_sha256, metadata_sha256;
    std::wstring game_directory;
    uint64_t created_file_time = 0;
    std::vector<BackupProofFile> files;
};

uint64_t CurrentFileTime() noexcept;
std::string BackupTimestamp(uint64_t file_time);
bool ParseBackupTimestamp(const std::string& text, uint64_t& file_time) noexcept;
bool ValidSha256(const std::string& value) noexcept;
bool SameSha256(const std::string& left, const std::string& right) noexcept;
bool PathWithin(const std::wstring& child, const std::wstring& directory) noexcept;
// Also rejects Win32 device names and trailing dot/space aliases before access.
bool BackupLocalPath(const std::wstring& input, std::wstring& full, std::string& error);

// Reads only the bounded local proof document. It does not grant authorization.
bool ReadBackupProof(const std::wstring& path, BackupProofDocument& proof, std::string& error);
// Rechecks every backed-up file. Original paths need not still exist.
bool ValidateBackupProofFiles(const std::wstring& proof_path, const BackupProofDocument& proof,
                              bool require_nonempty_save, std::string& error);
// game must come from VerifyGameProcess/VerifyCurrentGame; this verifies the backup,
// not a second copy of the already verified, running game executable.
bool VerifyBackupProof(const std::wstring& proof, const GameIdentity& game,
                       uint64_t gameStartFileTime, uint64_t nowFileTime, std::string& error,
                       uint64_t* expires_file_time = nullptr);

} // namespace vst
