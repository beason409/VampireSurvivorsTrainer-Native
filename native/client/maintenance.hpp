#pragma once
#include "../common/backup_proof.hpp"
#include <functional>

namespace vst::maintenance {

struct Options {
    std::wstring game_directory, data_directory, profile_directory;
    std::vector<std::wstring> steam_directories;
    // Offline fixtures may supply a process provider. Production leaves this empty.
    std::function<bool(std::string&)> stopped_check;
};
struct BackupInfo {
    std::string id, created_utc;
    std::wstring directory, manifest_path, proof_path;
    size_t file_count = 0;
};
struct RestoreResult {
    BackupInfo safety_backup;
    size_t restored_file_count = 0;
    bool rolled_back = false;
};

// All calls are synchronous. Run them on the maintenance worker, not the UI thread.
bool MakeDefaultOptions(const std::wstring& game_directory, const std::wstring& data_directory,
                        Options& options, std::string& error);
bool CreateBackup(const Options& options, BackupInfo& result, std::string& error);
bool ListBackups(const Options& options, std::vector<BackupInfo>& results, std::string& error);
bool RestoreBackup(const Options& options, const std::wstring& manifest_path,
                   RestoreResult& result, std::string& error);
bool LatestSessionProof(const Options& options, std::wstring& proof_path, std::string& error);

} // namespace vst::maintenance
