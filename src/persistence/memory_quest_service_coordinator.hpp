#pragma once

#include "persistence/quest_service_effects.hpp"

#include <cstdint>
#include <exception>
#include <map>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace chronicles {

inline void advance_quest_membership_generation(
    std::map<std::int64_t, std::int64_t> &generations, std::int64_t character_id) {
  auto &generation = generations[character_id];
  if (generation == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("Quest membership generation exhausted");
  }
  ++generation;
}

// This lock is acquired before a character, company, or guild store lock. It serializes
// quest-effect capture with membership changes across the otherwise independent memory stores.
struct MemoryQuestServiceCoordinator {
  // Callers hold mutex while reading or changing any of these maps.
  bool company_effect_pending(std::int64_t character_id) const {
    const auto it = pending_company.lower_bound({character_id, 0});
    return it != pending_company.end() && it->first == character_id;
  }

  bool guild_effect_pending(std::int64_t character_id) const {
    const auto it = pending_guild_by_character.lower_bound({character_id, 0});
    return it != pending_guild_by_character.end() && it->first == character_id;
  }

  bool guild_effect_pending_for_guild(std::int64_t guild_id) const {
    const auto it = pending_guild_by_guild.lower_bound({guild_id, 0, 0});
    return it != pending_guild_by_guild.end() && std::get<0>(*it) == guild_id;
  }

  std::size_t pending_count(std::int64_t character_id) const noexcept {
    auto company = pending_company.lower_bound({character_id, 0});
    auto guild = pending_guild_by_character.lower_bound({character_id, 0});
    std::size_t count = 0;
    while ((company != pending_company.end() && company->first == character_id) ||
           (guild != pending_guild_by_character.end() && guild->first == character_id)) {
      const auto company_sequence = company != pending_company.end() && company->first == character_id
                                        ? company->second : std::numeric_limits<std::uint64_t>::max();
      const auto guild_sequence = guild != pending_guild_by_character.end() && guild->first == character_id
                                      ? guild->second : std::numeric_limits<std::uint64_t>::max();
      if (company_sequence <= guild_sequence) {
        ++company;
      }
      if (guild_sequence <= company_sequence) {
        ++guild;
      }
      ++count;
    }
    return count;
  }

  // Called with mutex held, before the corresponding character state is published. Every
  // allocation completes here; on failure the event and all partial indexes are rolled back.
  bool record_event(QuestServiceEffectEvent event) {
    if (event.character_id <= 0 || event.world_id <= 0 || event.effect_id.size() != 32 ||
        events.size() >= kMaximumStoredQuestServiceEvents ||
        pending_count(event.character_id) >= kMaximumPendingQuestServiceEffectsPerCharacter ||
        next_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    const auto key = std::make_pair(event.character_id, event.effect_id);
    event.enqueue_sequence = next_sequence;
    auto [entry, inserted] = events.emplace(key, std::move(event));
    if (!inserted) {
      return false;
    }
    const auto &stored = entry->second;
    const auto character_sequence = std::make_pair(stored.character_id, stored.enqueue_sequence);
    const auto guild_sequence =
        std::make_tuple(stored.effects.expected_guild_id, stored.enqueue_sequence,
                        stored.character_id);
    try {
      if (!stored.company_applied || !stored.guild_applied) {
        pending_order.emplace(std::make_pair(stored.world_id, stored.enqueue_sequence), key);
      }
      if (!stored.company_applied) {
        pending_company.insert(character_sequence);
      }
      if (!stored.guild_applied) {
        pending_guild_by_character.insert(character_sequence);
        pending_guild_by_guild.insert(guild_sequence);
      }
    } catch (...) {
      pending_order.erase({stored.world_id, stored.enqueue_sequence});
      pending_company.erase(character_sequence);
      pending_guild_by_character.erase(character_sequence);
      pending_guild_by_guild.erase(guild_sequence);
      events.erase(entry);
      throw;
    }
    ++next_sequence;
    return true;
  }

  bool can_mark_component(const QuestServiceEffectEvent &event,
                          QuestServiceComponent component) const noexcept {
    const auto pending = pending_order.find({event.world_id, event.enqueue_sequence});
    if (pending == pending_order.end() || pending->second.first != event.character_id ||
        pending->second.second != event.effect_id) {
      return false;
    }
    const auto stored = events.find(pending->second);
    if (stored == events.end() || &stored->second != &event) {
      return false;
    }
    const auto key = std::make_pair(event.character_id, event.enqueue_sequence);
    const auto first_company = pending_company.lower_bound({event.character_id, 0});
    const auto first_guild = pending_guild_by_character.lower_bound({event.character_id, 0});
    if ((first_company != pending_company.end() && first_company->first == event.character_id &&
         first_company->second < event.enqueue_sequence) ||
        (first_guild != pending_guild_by_character.end() && first_guild->first == event.character_id &&
         first_guild->second < event.enqueue_sequence)) {
      return false;
    }
    if (component == QuestServiceComponent::company) {
      return !event.company_applied && pending_company.contains(key);
    }
    const auto first_in_guild =
        pending_guild_by_guild.lower_bound({event.effects.expected_guild_id, 0, 0});
    return !event.guild_applied && pending_guild_by_character.contains(key) &&
           pending_guild_by_guild.contains({event.effects.expected_guild_id,
                                            event.enqueue_sequence, event.character_id}) &&
           first_in_guild != pending_guild_by_guild.end() &&
           std::get<0>(*first_in_guild) == event.effects.expected_guild_id &&
           std::get<1>(*first_in_guild) == event.enqueue_sequence &&
           std::get<2>(*first_in_guild) == event.character_id;
  }

  // Settlement must call can_mark_component before publishing its nonthrowing record update.
  // A failed invariant here indicates corrupt in-memory state and terminates rather than
  // publishing an apparently successful but untracked reward.
  void mark_component_applied(QuestServiceEffectEvent &event,
                              QuestServiceComponent component) noexcept {
    if (!can_mark_component(event, component)) {
      std::terminate();
    }
    const auto key = std::make_pair(event.character_id, event.enqueue_sequence);
    if (component == QuestServiceComponent::company) {
      pending_company.erase(key);
      event.company_applied = true;
    } else {
      pending_guild_by_character.erase(key);
      pending_guild_by_guild.erase({event.effects.expected_guild_id, event.enqueue_sequence,
                                     event.character_id});
      event.guild_applied = true;
    }
    if (event.company_applied && event.guild_applied) {
      pending_order.erase({event.world_id, event.enqueue_sequence});
    }
  }

  std::mutex mutex;
  std::map<std::int64_t, int> company_memberships;
  std::map<std::int64_t, std::int64_t> guild_memberships;
  std::map<std::int64_t, std::int64_t> company_generations;
  std::map<std::int64_t, std::int64_t> guild_generations;
  // The rate is part of the key so a configuration change cannot reinterpret a
  // fractional balance that was earned under a different conversion rule.
  std::map<std::tuple<int, std::int64_t, QuestServiceComponent, std::int64_t, std::int64_t>,
           std::int64_t> activity_remainders;
  std::map<std::pair<std::int64_t, std::string>, QuestServiceEffectEvent> events;
  std::map<std::pair<int, std::uint64_t>, std::pair<std::int64_t, std::string>> pending_order;
  std::set<std::pair<std::int64_t, std::uint64_t>> pending_company;
  std::set<std::pair<std::int64_t, std::uint64_t>> pending_guild_by_character;
  std::set<std::tuple<std::int64_t, std::uint64_t, std::int64_t>> pending_guild_by_guild;
  std::uint64_t next_sequence{1};
  std::int64_t owner_epoch{};
};

} // namespace chronicles
