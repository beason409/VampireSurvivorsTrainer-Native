#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace vst {
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { HANDLE value = value_; value_ = INVALID_HANDLE_VALUE; return value; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept { if (*this) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct GameIdentity {
    std::wstring directory, executable;
    DWORD pid = 0;
};

std::string WinError(const char* operation, DWORD code = GetLastError());
std::string Utf8(const std::wstring& value);
std::wstring Wide(const std::string& value);
std::wstring ModulePath(HMODULE module = nullptr);
std::wstring ParentPath(const std::wstring& value);
std::wstring JoinPath(const std::wstring& directory, const std::wstring& leaf);
std::wstring FileName(const std::wstring& value);
bool EqualPath(const std::wstring& left, const std::wstring& right) noexcept;
bool LocalPath(const std::wstring& input, std::wstring& full, std::string& error);
bool RejectReparsePath(const std::wstring& full, bool leaf_may_be_missing, std::string& error);
bool Sha256(const void* bytes, size_t size, std::string& digest, std::string& error);
bool FileSha256(const std::wstring& path, std::string& digest, std::string& error);
bool ReadSmallFile(const std::wstring& path, size_t limit, std::string& text, std::string& error);
bool SameUserAndSession(HANDLE process, DWORD pid, std::string& error);
bool IsNativeX64(HANDLE process, std::string& error);
bool VerifyGameDirectory(const std::wstring& directory, std::string& error);
bool VerifyGameProcess(DWORD pid, GameIdentity& identity, std::string& error);
bool VerifyCurrentGame(GameIdentity& identity, std::string& error);
bool FindUniqueGame(GameIdentity& identity, std::string& error);
bool HasManagedBackend(DWORD pid, std::string& error);
bool FindRemoteModule(DWORD pid, const std::wstring& exact_path, uintptr_t& base, DWORD& size, std::string& error);
bool IsExecutableAddressInModule(void* pointer, HMODULE module);
bool IsReadableMemory(const void* pointer, size_t length);
bool ExportRva(const std::wstring& path, const char* export_name, DWORD& rva, std::string& error);
} // namespace vst
