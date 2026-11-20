#pragma once

#include "runtime.hpp"
#include <cstddef>
#include <string_view>

namespace vst {

inline constexpr size_t kMaximumMessageCharacters = 262144;
inline constexpr size_t kMaximumMessageBytes = kMaximumMessageCharacters * 4;

// Counts UTF-16 code units, matching the existing .NET wire limit.
bool Utf16Length(std::string_view text, size_t& length) noexcept;
bool ValidRequestId(std::string_view id) noexcept;
bool ParseRequest(std::string_view json, Request& request, std::string& error);
std::string SerializeResponse(const Response& response);

} // namespace vst
