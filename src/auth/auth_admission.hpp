#pragma once

#include <chrono>
#include <cstddef>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace chronicles {

// Admission happens before work is queued. A permit owns one slot until the
// asynchronous handler finishes, including its exceptional exit paths.
class AuthAdmission {
  struct State {
    std::mutex mutex;
    unsigned active{};
    std::unordered_map<std::string, unsigned> peers;
  };

public:
  using Clock = std::chrono::steady_clock;

  class Permit {
  public:
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;

    ~Permit() {
      if (!active_) {
        return;
      }
      std::lock_guard lock(state_->mutex);
      --state_->active;
      auto peer = state_->peers.find(peer_);
      if (--peer->second == 0) {
        state_->peers.erase(peer);
      }
    }

  private:
    friend class AuthAdmission;
    Permit(std::shared_ptr<State> state, std::string peer)
        : state_(std::move(state)), peer_(std::move(peer)) {}

    std::shared_ptr<State> state_;
    std::string peer_;
    bool active_{};
  };

  AuthAdmission(unsigned maximum_active, unsigned maximum_per_peer,
                unsigned maximum_bad_passwords, std::size_t maximum_failure_keys,
                unsigned maximum_peer_bad_passwords = 40,
                unsigned maximum_peer_registrations = 30)
      : state_(std::make_shared<State>()), maximum_active_(maximum_active),
        maximum_per_peer_(maximum_per_peer), maximum_bad_passwords_(maximum_bad_passwords),
        maximum_failure_keys_(maximum_failure_keys),
        maximum_peer_bad_passwords_(maximum_peer_bad_passwords),
        maximum_peer_registrations_(maximum_peer_registrations) {}

  std::shared_ptr<Permit> try_acquire(std::string_view peer) {
    auto permit = std::shared_ptr<Permit>(new Permit(state_, std::string(peer)));
    std::lock_guard lock(state_->mutex);
    const auto existing = state_->peers.find(permit->peer_);
    if (state_->active >= maximum_active_ ||
        (existing != state_->peers.end() && existing->second >= maximum_per_peer_)) {
      return {};
    }
    ++state_->peers[permit->peer_];
    ++state_->active;
    permit->active_ = true;
    return permit;
  }

  bool password_allowed(std::string_view peer, std::string_view account,
                        Clock::time_point now = Clock::now()) {
    std::lock_guard lock(failure_mutex_);
    prune_counters(now);
    const auto *account_failures = find_live(failures_, key(peer, account), now);
    const auto *peer_failures = find_live(peer_failures_, std::string(peer), now);
    return (!account_failures || account_failures->count < maximum_bad_passwords_) &&
           (!peer_failures || peer_failures->count < maximum_peer_bad_passwords_);
  }

  void bad_password(std::string_view peer, std::string_view account,
                    Clock::time_point now = Clock::now()) {
    std::lock_guard lock(failure_mutex_);
    if (maximum_failure_keys_ == 0) {
      return;
    }
    prune_counters(now);
    increment(failures_, key(peer, account), now);
    increment(peer_failures_, std::string(peer), now);
  }

  bool allow_registration(std::string_view peer, Clock::time_point now = Clock::now()) {
    std::lock_guard lock(failure_mutex_);
    prune_counters(now);
    const auto peer_key = std::string(peer);
    const auto *existing = find_live(registrations_, peer_key, now);
    if (existing && existing->count >= maximum_peer_registrations_) {
      return false;
    }
    if (maximum_failure_keys_ == 0 || maximum_peer_registrations_ == 0) {
      return false;
    }
    increment(registrations_, peer_key, now);
    return true;
  }

private:
  static constexpr std::size_t kMaximumPrunedPerTable = 16;

  struct Failure {
    Clock::time_point start;
    unsigned count;
    std::list<std::string>::iterator order;
  };

  struct Counters {
    std::list<std::string> insertion_order;
    std::unordered_map<std::string, Failure> entries;
  };

  static std::string key(std::string_view peer, std::string_view account) {
    std::string value(peer);
    value.push_back('\0');
    value.append(account);
    return value;
  }

  static bool expired(const Failure &counter, Clock::time_point now) {
    return now - counter.start >= std::chrono::seconds(60);
  }

  static void erase(Counters &counters, const std::string &counter_key) {
    const auto found = counters.entries.find(counter_key);
    counters.insertion_order.erase(found->second.order);
    counters.entries.erase(found);
  }

  static Failure *find_live(Counters &counters, const std::string &counter_key,
                            Clock::time_point now) {
    const auto found = counters.entries.find(counter_key);
    if (found == counters.entries.end()) {
      return nullptr;
    }
    if (expired(found->second, now)) {
      erase(counters, counter_key);
      return nullptr;
    }
    return &found->second;
  }

  static void prune(Counters &counters, Clock::time_point now) {
    for (std::size_t removed = 0;
         removed < kMaximumPrunedPerTable && !counters.insertion_order.empty(); ++removed) {
      const auto &oldest_key = counters.insertion_order.front();
      const auto oldest = counters.entries.find(oldest_key);
      if (!expired(oldest->second, now)) {
        break;
      }
      // Erase by iterator: oldest_key aliases the list element being removed.
      counters.entries.erase(oldest);
      counters.insertion_order.pop_front();
    }
  }

  void prune_counters(Clock::time_point now) {
    prune(failures_, now);
    prune(peer_failures_, now);
    prune(registrations_, now);
  }

  void increment(Counters &counters, const std::string &counter_key, Clock::time_point now) {
    if (auto *existing = find_live(counters, counter_key, now)) {
      ++existing->count;
      return;
    }
    if (counters.entries.size() >= maximum_failure_keys_) {
      // Evict the oldest entry in constant time. This favors availability, but
      // adversarial key churn can weaken the limits for evicted peers.
      const auto &oldest_key = counters.insertion_order.front();
      const auto oldest = counters.entries.find(oldest_key);
      counters.entries.erase(oldest);
      counters.insertion_order.pop_front();
    }
    counters.insertion_order.push_back(counter_key);
    const auto position = std::prev(counters.insertion_order.end());
    try {
      counters.entries.emplace(counter_key, Failure{now, 1, position});
    } catch (...) {
      counters.insertion_order.erase(position);
      throw;
    }
  }

  std::shared_ptr<State> state_;
  unsigned maximum_active_;
  unsigned maximum_per_peer_;
  unsigned maximum_bad_passwords_;
  std::size_t maximum_failure_keys_;
  unsigned maximum_peer_bad_passwords_;
  unsigned maximum_peer_registrations_;
  std::mutex failure_mutex_;
  Counters failures_;
  Counters peer_failures_;
  Counters registrations_;
};

} // namespace chronicles
