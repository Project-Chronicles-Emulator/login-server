#include "common/world_metrics.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <vector>

namespace chronicles::world_metrics {
namespace {

constexpr std::size_t kHistogramCount = static_cast<std::size_t>(HistogramId::Count);
constexpr std::size_t kCounterCount = static_cast<std::size_t>(CounterId::Count);
constexpr std::size_t kReportedOpcodes = 32;
constexpr auto kHistogramNames = std::to_array<const char *>(
    {"gameplay_tick_us", "combat_advance_us", "protocol_mutex_wait_us", "frame_strand_latency_us",
     "tick_strand_latency_us", "outbound_queue_frames", "outbound_queue_bytes",
     "presence_mutex_wait_us", "presence_move_us", "presence_candidates",
     "presence_events_per_move", "io_post_to_run_us", "read_resume_gap_us",
     "protocol_mutex_hold_us", "presence_mutex_hold_us", "world_mobs_lock_wait_us",
     "world_mobs_lock_hold_us", "process_frame_handler_us", "client_reported_round_trip_us"});
// A name list shorter than its enum would silently leave nullptr names; sizes are checked here.
static_assert(kHistogramNames.size() == kHistogramCount,
              "every HistogramId needs exactly one name, in enum order");
constexpr auto kCounterNames = std::to_array<const char *>(
    {"queue_limit_closes",
     "protocol_mutex_acquisitions",
     "protocol_mutex_contended",
     "presence_mutex_acquisitions",
     "presence_mutex_contended",
     "protocol_mutex_hold_sum_us",
     "presence_mutex_hold_sum_us",
     "read_resume_gap_over_10ms",
     "world_mobs_lock_acquisitions",
     "world_mobs_lock_contended",
     "world_mobs_lock_hold_sum_us",
     "read_resumes_by_drain",
     "read_resumes_by_barrier",
     "read_resume_blocked_by_backlog",
     "mobs_skipped_idle_not_due",
     "mobs_evaluated"});
static_assert(kCounterNames.size() == kCounterCount,
              "every CounterId needs exactly one name, in enum order");

struct OpcodeCounters {
  std::array<std::atomic<std::uint64_t>, kOpcodeCount> frames{};
  std::array<std::atomic<std::uint64_t>, kOpcodeCount> bytes{};
};

struct Registry {
  std::array<Histogram, kHistogramCount> histograms;
  std::array<std::atomic<std::uint64_t>, kCounterCount> counters{};
  OpcodeCounters incoming;
  OpcodeCounters outgoing;
  std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
};

std::atomic<bool> metrics_enabled{false};

// CPU clock of the registered io thread. Only register_io_thread() and snapshot_json() touch it,
// both off the hot path, so a plain mutex is enough.
struct IoThreadClock {
  std::mutex mutex;
  bool registered{false};
  std::chrono::steady_clock::time_point registered_at;
  IoThreadCpuSample baseline;  // lifetime CPU of the thread when it registered
#ifdef _WIN32
  HANDLE handle{nullptr};
#else
  clockid_t clock{};
#endif
};

IoThreadClock &io_thread_clock() {
  static IoThreadClock instance;
  return instance;
}

struct IoThreadCpu {
  std::uint64_t user_us{};
  std::uint64_t kernel_us{};
  bool valid{false};
};

IoThreadCpu read_io_thread_cpu(const IoThreadClock &clock) {
  IoThreadCpu cpu;
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (clock.handle != nullptr && GetThreadTimes(clock.handle, &created, &exited, &kernel, &user)) {
    const auto to_us = [](const FILETIME &value) {
      // FILETIME counts 100 ns units.
      return ((static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10;
    };
    cpu.user_us = to_us(user);
    cpu.kernel_us = to_us(kernel);
    cpu.valid = true;
  }
#else
  timespec total{};
  if (clock_gettime(clock.clock, &total) == 0) {
    // POSIX exposes only the total; it is reported as user time.
    cpu.user_us = static_cast<std::uint64_t>(total.tv_sec) * 1000000 +
                  static_cast<std::uint64_t>(total.tv_nsec) / 1000;
    cpu.valid = true;
  }
#endif
  return cpu;
}

// Logged once: a failed registration leaves "io_thread" reporting registered=false, which is
// otherwise indistinguishable from a server that never called register_io_thread().
void note_registration_failure(const char *reason) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    spdlog::warn("Metrics: io thread CPU registration failed ({}); io_thread stays unregistered",
                 reason);
  }
}

nlohmann::json io_thread_json() {
  auto &clock = io_thread_clock();
  std::lock_guard lock(clock.mutex);
  if (!clock.registered) {
    return {{"registered", false}};
  }
  const auto cpu = read_io_thread_cpu(clock);
  const auto wall_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - clock.registered_at)
          .count());
  nlohmann::json result = {{"registered", true},
                           {"cpu_valid", cpu.valid},
                           {"lifetime_cpu_user_us", cpu.user_us},
                           {"lifetime_cpu_kernel_us", cpu.kernel_us},
                           {"lifetime_cpu_total_us", cpu.user_us + cpu.kernel_us}};
  const auto usage = io_thread_usage(clock.baseline, {cpu.user_us, cpu.kernel_us}, wall_us);
  result["cpu_user_since_registration_us"] = usage.user_us;
  result["cpu_kernel_since_registration_us"] = usage.kernel_us;
  result["cpu_total_since_registration_us"] = usage.total_us;
  result["wall_since_registration_us"] = usage.wall_us;
  result["cpu_percent_valid"] = cpu.valid && usage.percent_valid;
  if (cpu.valid && usage.percent_valid) {
    result["cpu_percent"] = usage.percent;
  }
  return result;
}

// Function-local so that nothing is constructed (or touched) unless a hook runs after enable().
Registry &registry() {
  static Registry instance;
  return instance;
}

std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

nlohmann::json histogram_json(const HistogramSnapshot &snapshot) {
  nlohmann::json buckets = nlohmann::json::array();
  for (std::size_t index = 0; index < kBucketCount; ++index) {
    if (snapshot.buckets[index] != 0) {
      buckets.push_back({{"le", Histogram::bucket_upper_bound(index)},
                         {"count", snapshot.buckets[index]}});
    }
  }
  return {{"count", snapshot.count}, {"sum", snapshot.sum},
          {"max", snapshot.max},     {"p50", snapshot.percentile(0.50)},
          {"p95", snapshot.percentile(0.95)}, {"p99", snapshot.percentile(0.99)},
          {"buckets", std::move(buckets)}};
}

nlohmann::json opcode_json(const OpcodeCounters &counters) {
  struct Entry {
    std::uint64_t opcode;
    std::uint64_t frames;
    std::uint64_t bytes;
  };
  std::vector<Entry> entries;
  for (std::size_t opcode = 0; opcode < kOpcodeCount; ++opcode) {
    const auto frames = counters.frames[opcode].load(std::memory_order_relaxed);
    if (frames != 0) {
      entries.push_back({opcode, frames, counters.bytes[opcode].load(std::memory_order_relaxed)});
    }
  }
  std::ranges::sort(entries, [](const Entry &left, const Entry &right) {
    return left.bytes > right.bytes;
  });
  if (entries.size() > kReportedOpcodes) {
    entries.resize(kReportedOpcodes);
  }
  nlohmann::json result = nlohmann::json::array();
  for (const auto &entry : entries) {
    result.push_back({{"opcode", entry.opcode}, {"frames", entry.frames}, {"bytes", entry.bytes}});
  }
  return result;
}

} // namespace

std::uint64_t HistogramSnapshot::percentile(double quantile) const {
  if (count == 0) {
    return 0;
  }
  const auto bounded = std::clamp(quantile, 0.0, 1.0);
  const auto rank = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(bounded * static_cast<double>(count))));
  std::uint64_t seen = 0;
  for (std::size_t index = 0; index < kBucketCount; ++index) {
    seen += buckets[index];
    if (seen >= rank) {
      return std::min(Histogram::bucket_upper_bound(index), max);
    }
  }
  return max;
}

std::size_t Histogram::bucket_index(std::uint64_t value) noexcept {
  if (value == 0) {
    return 0;
  }
  return std::min<std::size_t>(static_cast<std::size_t>(std::bit_width(value)), kBucketCount - 1);
}

std::uint64_t Histogram::bucket_upper_bound(std::size_t index) noexcept {
  if (index == 0) {
    return 0;
  }
  if (index >= kBucketCount - 1) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return (std::uint64_t{1} << index) - 1;
}

void Histogram::record(std::uint64_t value) noexcept {
  buckets_[bucket_index(value)].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_.fetch_add(value, std::memory_order_relaxed);
  auto previous = max_.load(std::memory_order_relaxed);
  while (value > previous &&
         !max_.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
  }
}

HistogramSnapshot Histogram::snapshot() const noexcept {
  HistogramSnapshot result;
  for (std::size_t index = 0; index < kBucketCount; ++index) {
    result.buckets[index] = buckets_[index].load(std::memory_order_relaxed);
  }
  result.count = count_.load(std::memory_order_relaxed);
  result.sum = sum_.load(std::memory_order_relaxed);
  result.max = max_.load(std::memory_order_relaxed);
  return result;
}

bool enabled() noexcept {
  return metrics_enabled.load(std::memory_order_relaxed);
}

void enable() noexcept {
  if (!metrics_enabled.load(std::memory_order_relaxed)) {
    registry();
    metrics_enabled.store(true, std::memory_order_release);
  }
}

void record(HistogramId id, std::uint64_t value) noexcept {
  if (enabled()) {
    registry().histograms[static_cast<std::size_t>(id)].record(value);
  }
}

bool record_client_round_trip(std::string_view nanoseconds) noexcept {
  std::uint64_t value{};
  const auto [end, error] =
      std::from_chars(nanoseconds.data(), nanoseconds.data() + nanoseconds.size(), value);
  constexpr std::uint64_t kMaximumReportNanoseconds = 60'000'000'000;
  if (error != std::errc{} || end != nanoseconds.data() + nanoseconds.size() ||
      value > kMaximumReportNanoseconds) {
    return false;
  }
  record(HistogramId::ClientReportedRoundTripUs, value / 1000);
  return true;
}

void add(CounterId id, std::uint64_t amount) noexcept {
  if (enabled()) {
    registry().counters[static_cast<std::size_t>(id)].fetch_add(amount, std::memory_order_relaxed);
  }
}

void count_frame_in(std::uint16_t opcode, std::uint64_t bytes) noexcept {
  if (enabled()) {
    auto &incoming = registry().incoming;
    incoming.frames[opcode].fetch_add(1, std::memory_order_relaxed);
    incoming.bytes[opcode].fetch_add(bytes, std::memory_order_relaxed);
  }
}

void count_frame_out(std::uint16_t opcode, std::uint64_t bytes) noexcept {
  if (enabled()) {
    auto &outgoing = registry().outgoing;
    outgoing.frames[opcode].fetch_add(1, std::memory_order_relaxed);
    outgoing.bytes[opcode].fetch_add(bytes, std::memory_order_relaxed);
  }
}

std::uint64_t post_stamp() noexcept {
  return enabled() ? now_ns() : 0;
}

void record_strand_latency(HistogramId id, std::uint64_t stamp) noexcept {
  if (stamp != 0) {
    record(id, (now_ns() - stamp) / 1000);
  }
}

void record_read_resume_gap_us(std::uint64_t gap_us) noexcept {
  if (!enabled()) {
    return;
  }
  record(HistogramId::ReadResumeGapUs, gap_us);
  if (gap_us > kReadResumeSlowGapUs) {
    add(CounterId::ReadResumeGapOver10Ms);
  }
}

void record_read_resume_gap(std::uint64_t stamp) noexcept {
  if (stamp != 0) {
    record_read_resume_gap_us((now_ns() - stamp) / 1000);
  }
}

IoThreadUsage io_thread_usage(IoThreadCpuSample baseline, IoThreadCpuSample current,
                              std::uint64_t wall_us) noexcept {
  const auto delta = [](std::uint64_t now, std::uint64_t before) {
    return now > before ? now - before : std::uint64_t{0};
  };
  IoThreadUsage usage;
  usage.user_us = delta(current.user_us, baseline.user_us);
  usage.kernel_us = delta(current.kernel_us, baseline.kernel_us);
  usage.total_us = usage.user_us + usage.kernel_us;
  usage.wall_us = wall_us;
  usage.percent_valid = wall_us >= kIoThreadMinWallUs;
  if (usage.percent_valid) {
    usage.percent = 100.0 * static_cast<double>(usage.total_us) / static_cast<double>(wall_us);
  }
  return usage;
}

void register_io_thread() noexcept {
  if (!enabled()) {
    return;
  }
  auto &clock = io_thread_clock();
  std::lock_guard lock(clock.mutex);
#ifdef _WIN32
  if (clock.handle != nullptr) {
    CloseHandle(clock.handle);
    clock.handle = nullptr;
  }
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &duplicate,
                       THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
    clock.registered = false;
    note_registration_failure("DuplicateHandle failed");
    return;
  }
  clock.handle = duplicate;
#else
  if (pthread_getcpuclockid(pthread_self(), &clock.clock) != 0) {
    clock.registered = false;
    note_registration_failure("pthread_getcpuclockid failed");
    return;
  }
#endif
  const auto baseline = read_io_thread_cpu(clock);
  if (!baseline.valid) {
    clock.registered = false;
    note_registration_failure("first CPU read invalid");
    return;
  }
  clock.baseline = {baseline.user_us, baseline.kernel_us};
  clock.registered_at = std::chrono::steady_clock::now();
  clock.registered = true;
}

ScopedTimer::ScopedTimer(HistogramId id) noexcept : id_(id), started_ns_(post_stamp()) {}

ScopedTimer::~ScopedTimer() {
  if (started_ns_ != 0) {
    record(id_, (now_ns() - started_ns_) / 1000);
  }
}

void TimedMutex::lock() {
  if (!enabled()) {
    mutex_.lock();
    return;
  }
  add(acquisitions_);
  if (mutex_.try_lock()) {
    acquired_ns_ = now_ns();
    return;
  }
  const auto started = now_ns();
  mutex_.lock();
  const auto acquired = now_ns();
  add(contended_);
  record(wait_histogram_, (acquired - started) / 1000);
  acquired_ns_ = acquired;
}

bool TimedMutex::try_lock() {
  if (!mutex_.try_lock()) {
    return false;
  }
  if (enabled()) {
    acquired_ns_ = now_ns();
  }
  return true;
}

void TimedMutex::unlock() {
  if (acquired_ns_ != 0) {
    const auto held_us = (now_ns() - acquired_ns_) / 1000;
    acquired_ns_ = 0;
    record(hold_histogram_, held_us);
    add(hold_sum_, held_us);
  }
  mutex_.unlock();
}

void RecursiveTimedMutex::note_acquired(bool contended, std::uint64_t wait_started_ns) {
  ++depth_;
  if (depth_ != 1) {
    return;  // nested: the owner already holds it
  }
  add(acquisitions_);
  const auto acquired = now_ns();
  if (contended) {
    add(contended_);
    record(wait_histogram_, (acquired - wait_started_ns) / 1000);
  }
  acquired_ns_ = acquired;
}

void RecursiveTimedMutex::lock() {
  if (!enabled()) {
    mutex_.lock();
    return;
  }
  if (mutex_.try_lock()) {
    note_acquired(false, 0);
    return;
  }
  // try_lock failed, so another thread owns it and this is an outermost acquisition.
  const auto started = now_ns();
  mutex_.lock();
  note_acquired(true, started);
}

bool RecursiveTimedMutex::try_lock() {
  if (!mutex_.try_lock()) {
    return false;
  }
  if (enabled()) {
    note_acquired(false, 0);
  }
  return true;
}

void RecursiveTimedMutex::unlock() {
  if (depth_ != 0) {
    --depth_;
    if (depth_ == 0) {
      const auto held_us = (now_ns() - acquired_ns_) / 1000;
      acquired_ns_ = 0;
      record(hold_histogram_, held_us);
      add(hold_sum_, held_us);
    }
  }
  mutex_.unlock();
}

std::string snapshot_json() {
  auto &state = registry();
  nlohmann::json histograms = nlohmann::json::object();
  for (std::size_t index = 0; index < kHistogramCount; ++index) {
    histograms[kHistogramNames[index]] = histogram_json(state.histograms[index].snapshot());
  }
  nlohmann::json counters = nlohmann::json::object();
  for (std::size_t index = 0; index < kCounterCount; ++index) {
    counters[kCounterNames[index]] = state.counters[index].load(std::memory_order_relaxed);
  }
  const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - state.started)
                             .count();
  return nlohmann::json{{"uptime_ms", uptime_ms},
                        {"histograms", std::move(histograms)},
                        {"counters", std::move(counters)},
                        {"io_thread", io_thread_json()},
                        {"opcodes_in", opcode_json(state.incoming)},
                        {"opcodes_out", opcode_json(state.outgoing)}}
      .dump();
}

HttpReply metrics_http_reply(bool metrics_enabled, bool peer_loopback) {
  if (!metrics_enabled) {
    return {404, R"({"error":"not_found"})"};
  }
  if (!peer_loopback) {
    return {403, R"({"error":"loopback_only"})"};
  }
  return {200, snapshot_json()};
}

} // namespace chronicles::world_metrics
