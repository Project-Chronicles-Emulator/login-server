#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace chronicles {

// Wall-clock values on the wire (*_epochtime fields) count 100 ns ticks since the Unix epoch.
inline constexpr std::int64_t kEpochTicksPerSecond = 10'000'000;

inline std::int64_t unix_epoch_ticks_now() {
  using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, kEpochTicksPerSecond>>;
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<EpochTicks>(since_epoch).count();
}

// Server deadlines use 100 ns ticks; the native client's monotonic wire clock uses ns.
inline std::string wire_monotonic_tick(std::int64_t internal_tick) {
  constexpr std::int64_t kNanosecondsPerInternalTick = 100;
  if (internal_tick < 0 ||
      internal_tick > std::numeric_limits<std::int64_t>::max() / kNanosecondsPerInternalTick) {
    throw std::out_of_range("Internal monotonic tick cannot be represented on the wire");
  }

  return std::to_string(internal_tick * kNanosecondsPerInternalTick);
}

} // namespace chronicles
