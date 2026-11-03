#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace chronicles::world_metrics {

// Fixed log2 buckets: bucket 0 holds the value 0 and bucket i (1..kBucketCount-1) holds
// [2^(i-1), 2^i - 1]. The last bucket also absorbs every larger value.
constexpr std::size_t kBucketCount = 40;

struct HistogramSnapshot {
  std::uint64_t count{};
  std::uint64_t sum{};
  std::uint64_t max{};
  std::array<std::uint64_t, kBucketCount> buckets{};

  // Upper bound of the bucket that holds the requested quantile (0 < quantile <= 1), so the
  // result never underestimates the real percentile. Returns 0 for an empty histogram.
  std::uint64_t percentile(double quantile) const;
};

// Lock-free recorder. record() is a handful of relaxed atomic operations and never allocates.
class Histogram {
public:
  static std::size_t bucket_index(std::uint64_t value) noexcept;
  static std::uint64_t bucket_upper_bound(std::size_t index) noexcept;

  void record(std::uint64_t value) noexcept;
  HistogramSnapshot snapshot() const noexcept;

private:
  std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> max_{0};
};

enum class HistogramId : std::size_t {
  GameplayTickUs,            // whole resolve_gameplay_tick handler on the worker strand
  CombatAdvanceUs,           // advance_session_combat
  ProtocolMutexWaitUs,       // only contended acquisitions; uncontended ones are counted
  FrameStrandLatencyUs,      // post to run of the incoming-frame handler
  TickStrandLatencyUs,       // post to run of the gameplay tick handler
  OutboundQueueFrames,       // queue depth right after each enqueue
  OutboundQueueBytes,        // queued bytes right after each enqueue
  PresenceMutexWaitUs,       // only contended presence-mutex acquisitions
  PresenceMoveUs,            // WorldPresence::move wait plus hold
  PresenceCandidates,        // candidates visited per presence move
  PresenceEventsPerMove,     // events produced per presence move
  IoPostToRunUs,             // asio::post to a session's io executor until the handler starts
  ReadResumeGapUs,           // resume_read_after_write_ = true until the next read_length()
  ProtocolMutexHoldUs,       // lock acquired until unlock, protocol mutex
  PresenceMutexHoldUs,       // lock acquired until unlock, presence mutex
  WorldMobsLockWaitUs,       // contended outermost acquisitions of a map's WorldMobs mutex
  WorldMobsLockHoldUs,       // outermost acquire until the matching outermost release
  ProcessFrameHandlerUs,     // process_frame handler body on the worker strand
  ClientReportedRoundTripUs, // bounded client telemetry, not a server timing measurement
  Count
};

enum class CounterId : std::size_t {
  QueueLimitCloses,
  ProtocolMutexAcquisitions,
  ProtocolMutexContended,
  PresenceMutexAcquisitions,
  PresenceMutexContended,
  ProtocolMutexHoldSumUs,
  PresenceMutexHoldSumUs,
  ReadResumeGapOver10Ms,
  WorldMobsLockAcquisitions,  // outermost acquisitions only
  WorldMobsLockContended,
  WorldMobsLockHoldSumUs,
  ReadResumesByDrain,         // parked read resumed because the outbound queue emptied
  ReadResumesByBarrier,       // parked read resumed because its own frames were written
  ReadResumeBlockedByBacklog, // barrier met but the queue backlog guard held the read
  MobsSkippedIdleNotDue,  // advance_npc_ai returned early: idle mob not due
  MobsEvaluated,          // advance_npc_ai ran the full path (world-mob sessions)
  Count
};

constexpr std::size_t kOpcodeCount = 65536;

// True only after enable(). Every hook starts with this relaxed load, which is the whole cost
// of the feature when the server is started without --metrics.
bool enabled() noexcept;

// Allocates nothing before the first call. Not reversible during the process lifetime.
void enable() noexcept;

void record(HistogramId id, std::uint64_t value) noexcept;
void add(CounterId id, std::uint64_t amount = 1) noexcept;
void count_frame_in(std::uint16_t opcode, std::uint64_t bytes) noexcept;
void count_frame_out(std::uint16_t opcode, std::uint64_t bytes) noexcept;

// NetworkDelay is a one-way telemetry message. Invalid or implausible reports are discarded;
// these samples never adjust combat deadlines, transport clocks, or admission decisions.
bool record_client_round_trip(std::string_view nanoseconds) noexcept;

// Monotonic nanoseconds, or 0 while disabled so that callers can skip their second clock read.
std::uint64_t post_stamp() noexcept;
void record_strand_latency(HistogramId id, std::uint64_t stamp) noexcept;

// Gaps above this many microseconds also increment CounterId::ReadResumeGapOver10Ms.
constexpr std::uint64_t kReadResumeSlowGapUs = 10000;

// Records one read-resume gap in microseconds (histogram, and the slow-gap counter).
void record_read_resume_gap_us(std::uint64_t gap_us) noexcept;

// Same, from a post_stamp() taken when the read was parked; a zero stamp records nothing.
void record_read_resume_gap(std::uint64_t stamp) noexcept;

// CPU time of one thread in microseconds, as the OS reports it: lifetime totals of that thread.
struct IoThreadCpuSample {
  std::uint64_t user_us{};
  std::uint64_t kernel_us{};
};

// Below this much wall time since registration a CPU percentage is not meaningful.
constexpr std::uint64_t kIoThreadMinWallUs = 1000000;

// CPU used between two lifetime samples of the same thread, against the wall time between them.
struct IoThreadUsage {
  std::uint64_t user_us{};
  std::uint64_t kernel_us{};
  std::uint64_t total_us{};
  std::uint64_t wall_us{};
  bool percent_valid{false};  // false when wall_us < kIoThreadMinWallUs
  double percent{0.0};        // of ONE core, 0 while !percent_valid
};

// A current sample below the baseline (clock reset) counts as zero CPU, never as a wrapped value.
IoThreadUsage io_thread_usage(IoThreadCpuSample baseline, IoThreadCpuSample current,
                              std::uint64_t wall_us) noexcept;

// Registers the calling thread as the io thread whose CPU time /internal/metrics reports under
// "io_thread". The thread's lifetime CPU at registration is the baseline, so the reported
// cpu_*_since_registration_us and cpu_percent cover only the time after this call:
//   registered, cpu_valid, lifetime_cpu_user_us/kernel_us/total_us (raw OS totals, includes
//   time before registration), cpu_user_since_registration_us, cpu_kernel_since_registration_us,
//   cpu_total_since_registration_us, wall_since_registration_us, and cpu_percent plus
//   cpu_percent_valid (cpu_percent is omitted until kIoThreadMinWallUs has passed).
// Call it from the thread that runs io_context.run(); a second call replaces the first. It does
// nothing while metrics are disabled.
void register_io_thread() noexcept;

class ScopedTimer {
public:
  explicit ScopedTimer(HistogramId id) noexcept;
  ~ScopedTimer();
  ScopedTimer(const ScopedTimer &) = delete;
  ScopedTimer &operator=(const ScopedTimer &) = delete;

private:
  HistogramId id_;
  std::uint64_t started_ns_;
};

// Drop-in BasicLockable for std::lock_guard/scoped_lock/unique_lock. Disabled: a plain mutex
// lock behind one atomic load. Enabled: an uncontended try_lock is only counted; a contended
// acquisition records its wait.
class TimedMutex {
public:
  // Defaults feed the protocol mutex counters; other mutexes pass their own ids.
  TimedMutex() = default;
  TimedMutex(HistogramId wait_histogram, CounterId acquisitions, CounterId contended,
             HistogramId hold_histogram, CounterId hold_sum)
      : wait_histogram_(wait_histogram), acquisitions_(acquisitions), contended_(contended),
        hold_histogram_(hold_histogram), hold_sum_(hold_sum) {}

  void lock();
  bool try_lock();
  void unlock();

private:
  std::mutex mutex_;
  HistogramId wait_histogram_{HistogramId::ProtocolMutexWaitUs};
  CounterId acquisitions_{CounterId::ProtocolMutexAcquisitions};
  CounterId contended_{CounterId::ProtocolMutexContended};
  HistogramId hold_histogram_{HistogramId::ProtocolMutexHoldUs};
  CounterId hold_sum_{CounterId::ProtocolMutexHoldSumUs};
  // Written only by the thread that holds mutex_, so the mutex orders it. Zero when the
  // acquisition was not timed (metrics disabled), which makes unlock() skip the recording.
  std::uint64_t acquired_ns_{0};
};

// BasicLockable over std::recursive_mutex with the TimedMutex series, counting only the OUTERMOST
// acquisition and release: a nested lock by the owning thread is neither an acquisition nor a
// wait nor a separate hold, so the hold time is the whole span the thread owns the mutex.
class RecursiveTimedMutex {
public:
  RecursiveTimedMutex() = default;
  RecursiveTimedMutex(HistogramId wait_histogram, CounterId acquisitions, CounterId contended,
                      HistogramId hold_histogram, CounterId hold_sum)
      : wait_histogram_(wait_histogram), acquisitions_(acquisitions), contended_(contended),
        hold_histogram_(hold_histogram), hold_sum_(hold_sum) {}

  void lock();
  bool try_lock();
  void unlock();

private:
  void note_acquired(bool contended, std::uint64_t wait_started_ns);

  std::recursive_mutex mutex_;
  HistogramId wait_histogram_{HistogramId::WorldMobsLockWaitUs};
  CounterId acquisitions_{CounterId::WorldMobsLockAcquisitions};
  CounterId contended_{CounterId::WorldMobsLockContended};
  HistogramId hold_histogram_{HistogramId::WorldMobsLockHoldUs};
  CounterId hold_sum_{CounterId::WorldMobsLockHoldSumUs};
  // Both are written only by the owning thread. depth_ counts timed (metrics-enabled) nested
  // acquisitions; it stays 0 for acquisitions made while metrics were disabled.
  std::uint32_t depth_{0};
  std::uint64_t acquired_ns_{0};
};

// JSON document: uptime, histogram summaries (count, sum, max, p50/p95/p99, buckets), counters
// and the busiest opcodes in each direction. Cumulative values; rates come from differences.
std::string snapshot_json();

struct HttpReply {
  int status{};
  std::string body;
};

// GET /internal/metrics policy: 404 while the server was not started with --metrics (the
// endpoint does not exist), 403 for a non-loopback peer, otherwise 200 with snapshot_json().
// The caller passes enabled(); the parameter keeps the policy testable without global state.
HttpReply metrics_http_reply(bool metrics_enabled, bool peer_loopback);

} // namespace chronicles::world_metrics
