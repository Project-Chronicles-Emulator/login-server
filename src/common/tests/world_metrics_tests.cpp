#include "common/world_metrics.hpp"
#include "net/world_worker_threads.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <chrono>
#include <limits>
#include <thread>
#include <vector>

namespace chronicles::world_metrics {
namespace {

TEST(WorldMetrics, BucketsAreLog2AndTheLastBucketAbsorbsHugeValues) {
  EXPECT_EQ(Histogram::bucket_index(0), 0u);
  EXPECT_EQ(Histogram::bucket_index(1), 1u);
  EXPECT_EQ(Histogram::bucket_index(2), 2u);
  EXPECT_EQ(Histogram::bucket_index(3), 2u);
  EXPECT_EQ(Histogram::bucket_index(4), 3u);
  EXPECT_EQ(Histogram::bucket_index(1023), 10u);
  EXPECT_EQ(Histogram::bucket_index(1024), 11u);
  EXPECT_EQ(Histogram::bucket_index(std::numeric_limits<std::uint64_t>::max()), kBucketCount - 1);

  EXPECT_EQ(Histogram::bucket_upper_bound(0), 0u);
  EXPECT_EQ(Histogram::bucket_upper_bound(1), 1u);
  EXPECT_EQ(Histogram::bucket_upper_bound(3), 7u);
  EXPECT_EQ(Histogram::bucket_upper_bound(kBucketCount - 1),
            std::numeric_limits<std::uint64_t>::max());
  // Every value lies at or below the upper bound of its own bucket.
  for (const std::uint64_t value : {0u, 1u, 2u, 5u, 99u, 4096u, 1000000u}) {
    EXPECT_LE(value, Histogram::bucket_upper_bound(Histogram::bucket_index(value)));
  }
}

TEST(WorldMetrics, PercentilesFollowTheRecordedDistributionAndNeverUnderestimate) {
  Histogram histogram;
  for (int index = 0; index < 90; ++index) {
    histogram.record(100);
  }
  for (int index = 0; index < 9; ++index) {
    histogram.record(1000);
  }
  histogram.record(50000);

  const auto snapshot = histogram.snapshot();
  EXPECT_EQ(snapshot.count, 100u);
  EXPECT_EQ(snapshot.sum, 90u * 100 + 9u * 1000 + 50000u);
  EXPECT_EQ(snapshot.max, 50000u);
  // 100 falls in bucket [64, 127], 1000 in [512, 1023], 50000 in [32768, 65535].
  EXPECT_EQ(snapshot.percentile(0.50), 127u);
  EXPECT_EQ(snapshot.percentile(0.90), 127u);
  EXPECT_EQ(snapshot.percentile(0.95), 1023u);
  EXPECT_EQ(snapshot.percentile(0.99), 1023u);
  // The top percentile is capped by the true maximum rather than the bucket bound 65535.
  EXPECT_EQ(snapshot.percentile(1.0), 50000u);
  EXPECT_GE(snapshot.percentile(0.50), 100u);
}

TEST(WorldMetrics, EmptyHistogramHasZeroPercentiles) {
  const auto snapshot = Histogram{}.snapshot();
  EXPECT_EQ(snapshot.count, 0u);
  EXPECT_EQ(snapshot.percentile(0.99), 0u);
}

TEST(WorldMetrics, ConcurrentRecordingLosesNoSamples) {
  Histogram histogram;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 20000;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&histogram, thread] {
      for (int index = 0; index < kPerThread; ++index) {
        histogram.record(static_cast<std::uint64_t>(thread * 10 + index % 7));
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  const auto snapshot = histogram.snapshot();
  EXPECT_EQ(snapshot.count, static_cast<std::uint64_t>(kThreads * kPerThread));
  std::uint64_t bucket_total = 0;
  for (const auto bucket : snapshot.buckets) {
    bucket_total += bucket;
  }
  EXPECT_EQ(bucket_total, snapshot.count);
  EXPECT_EQ(snapshot.max, 36u);
}

TEST(WorldMetrics, EndpointDoesNotExistWithoutTheFlagAndRefusesRemotePeers) {
  const auto disabled_local = metrics_http_reply(false, true);
  EXPECT_EQ(disabled_local.status, 404);
  const auto disabled_remote = metrics_http_reply(false, false);
  EXPECT_EQ(disabled_remote.status, 404);
  EXPECT_EQ(disabled_remote.body.find("histograms"), std::string::npos);

  const auto remote = metrics_http_reply(true, false);
  EXPECT_EQ(remote.status, 403);
  EXPECT_EQ(remote.body.find("histograms"), std::string::npos);

  const auto local = metrics_http_reply(true, true);
  ASSERT_EQ(local.status, 200);
  const auto document = nlohmann::json::parse(local.body);
  EXPECT_TRUE(document.at("histograms").contains("gameplay_tick_us"));
  EXPECT_TRUE(document.at("counters").contains("queue_limit_closes"));
  EXPECT_TRUE(document.at("opcodes_out").is_array());
}

TEST(WorldMetrics, EnabledHooksCountOpcodesAndContendedMutexWaits) {
  enable();
  ASSERT_TRUE(enabled());
  const auto before = nlohmann::json::parse(snapshot_json());
  const auto count_for = [](const nlohmann::json &document, const char *direction,
                            std::uint64_t opcode) -> std::uint64_t {
    for (const auto &entry : document.at(direction)) {
      if (entry.at("opcode") == opcode) {
        return entry.at("frames");
      }
    }
    return 0;
  };

  count_frame_out(177, 40);
  count_frame_out(177, 40);
  count_frame_in(20087, 83);

  TimedMutex mutex;
  mutex.lock();
  std::thread contender([&mutex] {
    std::lock_guard lock(mutex);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  mutex.unlock();
  contender.join();

  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(count_for(after, "opcodes_out", 177) - count_for(before, "opcodes_out", 177), 2u);
  EXPECT_EQ(count_for(after, "opcodes_in", 20087) - count_for(before, "opcodes_in", 20087), 1u);
  const auto contended = [](const nlohmann::json &document) -> std::uint64_t {
    return document.at("counters").at("protocol_mutex_contended");
  };
  EXPECT_GE(contended(after) - contended(before), 1u);
  EXPECT_GE(after.at("histograms").at("protocol_mutex_wait_us").at("max").get<std::uint64_t>(),
            10000u);
}

std::uint64_t histogram_field(const nlohmann::json &document, const char *name,
                              const char *field) {
  return document.at("histograms").at(name).at(field).get<std::uint64_t>();
}

std::uint64_t counter_field(const nlohmann::json &document, const char *name) {
  return document.at("counters").at(name).get<std::uint64_t>();
}

TEST(WorldMetrics, MutexHoldTimeIsRecordedWithItsSumForBothMutexKinds) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());

  TimedMutex protocol;
  protocol.lock();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  protocol.unlock();

  TimedMutex presence(HistogramId::PresenceMutexWaitUs, CounterId::PresenceMutexAcquisitions,
                      CounterId::PresenceMutexContended, HistogramId::PresenceMutexHoldUs,
                      CounterId::PresenceMutexHoldSumUs);
  ASSERT_TRUE(presence.try_lock());
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  presence.unlock();

  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(after, "protocol_mutex_hold_us", "count") -
                histogram_field(before, "protocol_mutex_hold_us", "count"),
            1u);
  EXPECT_GE(histogram_field(after, "protocol_mutex_hold_us", "max"), 15000u);
  EXPECT_GE(counter_field(after, "protocol_mutex_hold_sum_us") -
                counter_field(before, "protocol_mutex_hold_sum_us"),
            15000u);
  EXPECT_EQ(histogram_field(after, "presence_mutex_hold_us", "count") -
                histogram_field(before, "presence_mutex_hold_us", "count"),
            1u);
  EXPECT_GE(counter_field(after, "presence_mutex_hold_sum_us") -
                counter_field(before, "presence_mutex_hold_sum_us"),
            10000u);
}

TEST(WorldMetrics, ContendedAcquisitionHoldStartsAfterTheWait) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  TimedMutex mutex;
  mutex.lock();
  std::thread contender([&mutex] {
    mutex.lock();
    mutex.unlock();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  mutex.unlock();
  contender.join();
  const auto after = nlohmann::json::parse(snapshot_json());
  // Two holds were recorded; the contender's hold must not include its 40 ms wait, so the
  // sum grows by about the first thread's hold (>= 40 ms) and not by roughly twice that.
  EXPECT_EQ(histogram_field(after, "protocol_mutex_hold_us", "count") -
                histogram_field(before, "protocol_mutex_hold_us", "count"),
            2u);
  const auto grown = counter_field(after, "protocol_mutex_hold_sum_us") -
                     counter_field(before, "protocol_mutex_hold_sum_us");
  EXPECT_GE(grown, 40000u);
  EXPECT_LT(grown, 75000u);
}

TEST(WorldMetrics, ReadResumeGapIsRecordedAndSlowGapsAreCounted) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  record_read_resume_gap_us(50);
  record_read_resume_gap_us(kReadResumeSlowGapUs);
  record_read_resume_gap_us(kReadResumeSlowGapUs + 1);
  record_read_resume_gap(0);  // a zero stamp means "not parked while enabled"
  const auto stamp = post_stamp();
  ASSERT_NE(stamp, 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  record_read_resume_gap(stamp);
  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(after, "read_resume_gap_us", "count") -
                histogram_field(before, "read_resume_gap_us", "count"),
            4u);
  // The boundary value is not slow; the 10001 us gap and the 15 ms sleep are.
  EXPECT_EQ(counter_field(after, "read_resume_gap_over_10ms") -
                counter_field(before, "read_resume_gap_over_10ms"),
            2u);
}

TEST(WorldMetrics, IoPostToRunLatencyUsesTheStrandStampPath) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  const auto stamp = post_stamp();
  std::this_thread::sleep_for(std::chrono::milliseconds(12));
  record_strand_latency(HistogramId::IoPostToRunUs, stamp);
  record_strand_latency(HistogramId::IoPostToRunUs, 0);  // disabled-time stamp: no sample
  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(after, "io_post_to_run_us", "count") -
                histogram_field(before, "io_post_to_run_us", "count"),
            1u);
  EXPECT_GE(histogram_field(after, "io_post_to_run_us", "max"), 12000u);
}

// CPU time of the calling thread in microseconds, read independently of the code under test.
std::uint64_t this_thread_cpu_us() {
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  EXPECT_TRUE(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
  const auto to_us = [](const FILETIME &value) {
    return ((static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10;
  };
  return to_us(kernel) + to_us(user);
#else
  timespec total{};
  EXPECT_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &total), 0);
  return static_cast<std::uint64_t>(total.tv_sec) * 1000000 +
         static_cast<std::uint64_t>(total.tv_nsec) / 1000;
#endif
}

// Spins until the calling thread has used at least this much additional CPU (not wall time), so
// a descheduled thread cannot finish the loop with less CPU than the test needs. The wall-clock
// limit only stops a stuck machine from hanging the test.
void burn_thread_cpu_us(std::uint64_t amount_us) {
  const auto target = this_thread_cpu_us() + amount_us;
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  volatile std::uint64_t sink = 0;
  while (this_thread_cpu_us() < target && std::chrono::steady_clock::now() < give_up) {
    sink = sink + 1;
  }
}

TEST(WorldMetrics, IoThreadCpuCoversOnlyTheTimeAfterRegistration) {
  enable();
  std::thread worker([] {
    // CPU burned BEFORE registration must not count towards the reported usage.
    constexpr std::uint64_t kBeforeUs = 80000;
    constexpr std::uint64_t kAfterUs = 60000;
    burn_thread_cpu_us(kBeforeUs);
    register_io_thread();
    burn_thread_cpu_us(kAfterUs);

    const auto io = nlohmann::json::parse(snapshot_json()).at("io_thread");
    ASSERT_TRUE(io.at("registered").get<bool>());
    ASSERT_TRUE(io.at("cpu_valid").get<bool>());
    const auto lifetime = io.at("lifetime_cpu_total_us").get<std::uint64_t>();
    const auto since = io.at("cpu_total_since_registration_us").get<std::uint64_t>();
    const auto wall = io.at("wall_since_registration_us").get<std::uint64_t>();
    // The raw total includes both burns; the since-registration figure only the second one.
    EXPECT_GE(lifetime, kBeforeUs + kAfterUs);
    EXPECT_GE(since, 20000u);       // an always-zero delta fails here
    EXPECT_LE(since, wall + 20000u);  // one thread cannot use more CPU than wall time
    EXPECT_LT(since, lifetime);       // the pre-registration burn is excluded, with no wrap
    EXPECT_GE(lifetime - since, kBeforeUs / 2);
    // Under a second of wall time no percentage is reported, instead of a misleading one.
    EXPECT_FALSE(io.at("cpu_percent_valid").get<bool>());
    EXPECT_FALSE(io.contains("cpu_percent"));
  });
  worker.join();
}

TEST(WorldMetrics, IoThreadPercentIsComputedFromDeltasAndStaysWithinOneCore) {
  // A thread that already had 1.5 s of CPU, sampled 354 ms later with 20 ms more CPU.
  const auto usage = io_thread_usage({1000000, 500000}, {1010000, 510000}, 1100000);
  EXPECT_TRUE(usage.percent_valid);
  EXPECT_EQ(usage.user_us, 10000u);
  EXPECT_EQ(usage.kernel_us, 10000u);
  EXPECT_EQ(usage.total_us, 20000u);
  EXPECT_NEAR(usage.percent, 100.0 * 20000.0 / 1100000.0, 1e-9);
  EXPECT_LE(usage.percent, 100.0);

  const auto short_window = io_thread_usage({0, 0}, {5000, 5000}, kIoThreadMinWallUs - 1);
  EXPECT_FALSE(short_window.percent_valid);
  EXPECT_EQ(short_window.percent, 0.0);
  EXPECT_EQ(short_window.total_us, 10000u);

  const auto reset = io_thread_usage({900, 900}, {100, 100}, 2000000);
  EXPECT_EQ(reset.total_us, 0u);
  EXPECT_EQ(reset.percent, 0.0);
  EXPECT_TRUE(reset.percent_valid);
}

TEST(WorldMetrics, SnapshotHasTheNewSeriesNames) {
  enable();
  const auto document = nlohmann::json::parse(snapshot_json());
  for (const char *name : {"io_post_to_run_us", "read_resume_gap_us", "protocol_mutex_hold_us",
                           "presence_mutex_hold_us"}) {
    EXPECT_TRUE(document.at("histograms").contains(name)) << name;
  }
  for (const char *name : {"protocol_mutex_hold_sum_us", "presence_mutex_hold_sum_us",
                           "read_resume_gap_over_10ms"}) {
    EXPECT_TRUE(document.at("counters").contains(name)) << name;
  }
  EXPECT_TRUE(document.contains("io_thread"));
  EXPECT_FALSE(document.at("io_thread").contains("cpu_total_us"));  // renamed: lifetime_/since_
}

TEST(WorldMetrics, NestedRecursiveLocksCountOneAcquisitionAndOneHold) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  RecursiveTimedMutex mutex;
  mutex.lock();
  mutex.lock();
  ASSERT_TRUE(mutex.try_lock());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  mutex.unlock();
  mutex.unlock();
  // Inner releases must not end the hold: nothing is recorded until the outermost release.
  const auto during = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(during, "world_mobs_lock_hold_us", "count"),
            histogram_field(before, "world_mobs_lock_hold_us", "count"));
  mutex.unlock();

  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(counter_field(after, "world_mobs_lock_acquisitions") -
                counter_field(before, "world_mobs_lock_acquisitions"),
            1u);
  EXPECT_EQ(counter_field(after, "world_mobs_lock_contended") -
                counter_field(before, "world_mobs_lock_contended"),
            0u);
  EXPECT_EQ(histogram_field(after, "world_mobs_lock_hold_us", "count") -
                histogram_field(before, "world_mobs_lock_hold_us", "count"),
            1u);
  EXPECT_GE(counter_field(after, "world_mobs_lock_hold_sum_us") -
                counter_field(before, "world_mobs_lock_hold_sum_us"),
            20000u);
  EXPECT_EQ(histogram_field(after, "world_mobs_lock_wait_us", "count"),
            histogram_field(before, "world_mobs_lock_wait_us", "count"));
}

TEST(WorldMetrics, ContendedRecursiveLockRecordsWaitAndHoldsSeparately) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  RecursiveTimedMutex mutex;
  mutex.lock();
  mutex.lock();  // the owner's nested lock never waits
  std::thread contender([&mutex] {
    mutex.lock();
    mutex.unlock();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  mutex.unlock();
  mutex.unlock();
  contender.join();

  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(counter_field(after, "world_mobs_lock_acquisitions") -
                counter_field(before, "world_mobs_lock_acquisitions"),
            2u);
  EXPECT_EQ(counter_field(after, "world_mobs_lock_contended") -
                counter_field(before, "world_mobs_lock_contended"),
            1u);
  EXPECT_EQ(histogram_field(after, "world_mobs_lock_wait_us", "count") -
                histogram_field(before, "world_mobs_lock_wait_us", "count"),
            1u);
  EXPECT_GE(histogram_field(after, "world_mobs_lock_wait_us", "max"), 30000u);
  EXPECT_EQ(histogram_field(after, "world_mobs_lock_hold_us", "count") -
                histogram_field(before, "world_mobs_lock_hold_us", "count"),
            2u);
  // The contender's hold starts after its wait, so the two holds sum to about the owner's
  // 40 ms and not to roughly twice that.
  const auto grown = counter_field(after, "world_mobs_lock_hold_sum_us") -
                     counter_field(before, "world_mobs_lock_hold_sum_us");
  EXPECT_GE(grown, 40000u);
  EXPECT_LT(grown, 75000u);
}

TEST(WorldMetrics, RecursiveLockStillExcludesOtherThreads) {
  enable();
  RecursiveTimedMutex mutex;
  int shared = 0;
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&mutex, &shared] {
      for (int step = 0; step < 2000; ++step) {
        std::lock_guard outer(mutex);
        std::lock_guard inner(mutex);
        ++shared;
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(shared, 8000);
}

TEST(WorldMetrics, ProcessFrameSeriesExistInTheSnapshot) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  {
    const ScopedTimer timer(HistogramId::ProcessFrameHandlerUs);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(after, "process_frame_handler_us", "count") -
                histogram_field(before, "process_frame_handler_us", "count"),
            1u);
  EXPECT_GE(histogram_field(after, "process_frame_handler_us", "max"), 4000u);
}

TEST(WorldMetrics, ClientRoundTripTelemetryRejectsMalformedAndUnboundedReports) {
  enable();
  const auto before = nlohmann::json::parse(snapshot_json());
  EXPECT_TRUE(record_client_round_trip("120000000"));
  EXPECT_FALSE(record_client_round_trip("-1"));
  EXPECT_FALSE(record_client_round_trip("60000000001"));
  EXPECT_FALSE(record_client_round_trip("18446744073709551616"));
  EXPECT_FALSE(record_client_round_trip("10junk"));
  EXPECT_FALSE(record_client_round_trip(""));
  const auto after = nlohmann::json::parse(snapshot_json());
  EXPECT_EQ(histogram_field(after, "client_reported_round_trip_us", "count") -
                histogram_field(before, "client_reported_round_trip_us", "count"),
            1u);
  EXPECT_GE(histogram_field(after, "client_reported_round_trip_us", "max"), 120000u);
}

TEST(WorldWorkerThreads, DefaultIsFourAndTheRangeIsOneToSixtyFour) {
  EXPECT_EQ(kDefaultWorldWorkerThreads, 4u);
  EXPECT_EQ(parse_world_worker_threads("4"), 4u);
  EXPECT_EQ(parse_world_worker_threads("1"), 1u);
  EXPECT_EQ(parse_world_worker_threads("8"), 8u);
  EXPECT_EQ(parse_world_worker_threads("64"), 64u);
  EXPECT_EQ(parse_world_worker_threads("08"), 8u);
}

TEST(WorldWorkerThreads, MalformedOrOutOfRangeValuesAreRejected) {
  for (const char *bad : {"", "0", "65", "-1", "+4", " 4", "4 ", "4x", "x4", "4.0", "0x10",
                          "99999999999999999999", "1e1"}) {
    EXPECT_THROW(parse_world_worker_threads(bad), std::invalid_argument) << bad;
  }
}

} // namespace
} // namespace chronicles::world_metrics
