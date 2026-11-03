#pragma once

#include <cstddef>
#include <string_view>

namespace chronicles {

inline constexpr std::size_t kMinimumPasswordBytes = 8;
inline constexpr std::size_t kMaximumPasswordBytes = 256;

inline bool is_valid_new_password(std::string_view password) {
  return password.size() >= kMinimumPasswordBytes && password.size() <= kMaximumPasswordBytes;
}

} // namespace chronicles
