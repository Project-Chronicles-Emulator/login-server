#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace chronicles {

// Drain staged rewards before admitting players; never process beyond the startup limit.
template <typename RecoverPage, typename HasStaged>
std::size_t recover_startup_rewards(std::size_t page_size, std::size_t limit,
                                    RecoverPage recover_page, HasStaged has_staged) {
  if (page_size == 0 || limit == 0) {
    throw std::invalid_argument("Invalid startup reward recovery limit");
  }

  std::size_t recovered_total = 0;
  while (recovered_total < limit) {
    const auto requested = std::min(page_size, limit - recovered_total);
    const auto recovered = recover_page(requested);
    if (recovered > requested) {
      throw std::runtime_error("Invalid startup reward recovery page count");
    }
    recovered_total += recovered;
    if (recovered < requested) {
      return recovered_total;
    }
  }

  if (has_staged()) {
    throw std::runtime_error("Pending startup recovery exceeds the configured limit");
  }
  return recovered_total;
}

} // namespace chronicles
