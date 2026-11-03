#pragma once

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace chronicles {

// The world worker pool is 4 threads unless --world-worker-threads says otherwise.
constexpr unsigned kDefaultWorldWorkerThreads = 4;
constexpr unsigned kMinimumWorldWorkerThreads = 1;
constexpr unsigned kMaximumWorldWorkerThreads = 64;

// Parses the whole value as a decimal integer in [1, 64]; anything else (empty, sign, spaces,
// trailing text, out of range) throws std::invalid_argument naming the option.
inline unsigned parse_world_worker_threads(std::string_view value) {
  unsigned threads{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), threads);
  if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      threads < kMinimumWorldWorkerThreads || threads > kMaximumWorldWorkerThreads) {
    throw std::invalid_argument("--world-worker-threads must be an integer between 1 and 64");
  }
  return threads;
}

} // namespace chronicles
