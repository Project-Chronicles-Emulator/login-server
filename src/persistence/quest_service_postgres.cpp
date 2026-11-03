#include "persistence/postgres_repositories.hpp"
#include "persistence/loot_claim_internal.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace chronicles {
namespace {

bool lowercase_hex(std::string_view value, std::size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}

std::int64_t current_guild(pqxx::transaction_base &transaction, std::int64_t character_id) {
  const auto rows = transaction.exec(
      "SELECT guild_id FROM emulator_guild_members WHERE character_id=$1",
      pqxx::params{character_id});
  return rows.empty() ? 0 : rows.front()[0].as<std::int64_t>();
}

std::optional<QuestServiceMembership> lock_membership(pqxx::work &transaction,
                                                       std::int64_t account_id,
                                                       std::int64_t character_id, int world_id) {
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    return std::nullopt;
  }
  const auto candidate_guild = current_guild(transaction, character_id);
  if (candidate_guild != 0) {
    detail::lock_quest_service_guild(transaction, candidate_guild);
  }
  detail::lock_quest_service_character(transaction, character_id);
  const auto current_owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
      pqxx::params{account_id, character_id, world_id});
  if (current_owner.empty() || current_guild(transaction, character_id) != candidate_guild) {
    return std::nullopt;
  }
  const auto rows = transaction.exec(
      "SELECT COALESCE((SELECT company_type FROM emulator_character_company WHERE character_id=$1),0), "
      "company_generation,guild_generation FROM emulator_quest_service_character_guards "
      "WHERE character_id=$1", pqxx::params{character_id});
  if (rows.size() != 1) {
    throw std::runtime_error("Quest service membership guard is missing");
  }
  return QuestServiceMembership{rows.front()[0].as<int>(), candidate_guild,
                                rows.front()[1].as<std::int64_t>(),
                                rows.front()[2].as<std::int64_t>()};
}

QuestServiceEffectEvent decode_pending_event(const pqxx::row &row) {
  QuestServiceEffectEvent event;
  const auto sequence = row[0].as<std::int64_t>();
  event.character_id = row[1].as<std::int64_t>();
  event.account_id = row[2].as<std::int64_t>();
  event.world_id = row[3].as<int>();
  event.effect_id = row[4].as<std::string>();
  event.mutation_fingerprint = row[5].as<std::string>();
  event.monster_receipt = row[6].as<bool>();
  event.effects = decode_quest_service_effects(nlohmann::json::parse(row[7].as<std::string>()));
  event.granted_items = decode_quest_granted_items(nlohmann::json::parse(row[8].as<std::string>()));
  event.company_applied = row[9].as<bool>();
  event.guild_applied = row[10].as<bool>();
  if (sequence <= 0 || event.character_id <= 0 || event.account_id <= 0 || event.world_id <= 0 ||
      !lowercase_hex(event.effect_id, 32) || !lowercase_hex(event.mutation_fingerprint, 64) ||
      event.effects.expected_company_type != row[11].as<int>() ||
      event.effects.expected_guild_id != row[12].as<std::int64_t>() ||
      event.effects.expected_company_generation != row[13].as<std::int64_t>() ||
      event.effects.expected_guild_generation != row[14].as<std::int64_t>()) {
    throw std::runtime_error("Invalid stored quest service effect identity");
  }
  event.enqueue_sequence = static_cast<std::uint64_t>(sequence);
  return event;
}

QuestMutationResult commit_locked_postgres_quest_mutation(
    pqxx::work &transaction, std::int64_t owner_epoch, std::int64_t account_id,
    std::int64_t character_id, int world_id, const std::string &effect_id,
    const WorldProgress &before, const std::optional<WorldProgress> &after,
    const ItemTrade &trade, const QuestServiceEffects &effects, bool monster_receipt,
    const std::optional<std::string> &combat_source = std::nullopt) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 || !lowercase_hex(effect_id, 32)) {
    throw std::invalid_argument("Invalid quest mutation identity");
  }
  if (after) {
    detail::validate_loot_trade(before, *after, trade);
  } else if (!trade.granted_items.empty() || !trade.removed_item_ids.empty() ||
             !trade.durability_updates.empty()) {
    throw std::invalid_argument("Item changes need a quest progress transition");
  }
  const auto payload = encode_quest_service_effects(effects);
  const auto fingerprint = combat_source
      ? combat_quest_event_fingerprint(*combat_source, before, after, trade, effects)
      : quest_mutation_fingerprint(before, after, trade, effects);
  detail::require_reward_owner_epoch(transaction, owner_epoch);
  const auto membership = lock_membership(transaction, account_id, character_id, world_id);
  if (!membership) {
    return {};
  }
  const auto previous = transaction.exec(
      "SELECT account_id,world_id,mutation_fingerprint,monster_receipt,granted_items "
      "FROM emulator_quest_service_effects WHERE character_id=$1 AND effect_id=$2 FOR UPDATE",
      pqxx::params{character_id, effect_id});
  if (!previous.empty()) {
    const auto &row = previous.front();
    if (row[0].as<std::int64_t>() != account_id || row[1].as<int>() != world_id ||
        row[2].as<std::string>() != fingerprint || row[3].as<bool>() != monster_receipt) {
      return {};
    }
    auto granted = decode_quest_granted_items(nlohmann::json::parse(row[4].as<std::string>()));
    return {QuestMutationStatus::already_processed, std::move(granted)};
  }
  if (membership->company_type != effects.expected_company_type ||
      membership->guild_id != effects.expected_guild_id ||
      membership->company_generation != effects.expected_company_generation ||
      membership->guild_generation != effects.expected_guild_generation) {
    return {};
  }

  // Monster receipt locking follows the existing event -> character -> receipt order.
  if (monster_receipt) {
    const auto event = transaction.exec(
        "SELECT ready,boss_script_published FROM emulator_monster_reward_events WHERE receipt_id=$1 FOR SHARE",
        pqxx::params{effect_id});
    if (event.empty() || !event.front()[0].as<bool>() || !event.front()[1].as<bool>()) {
      return {};
    }
  }
  const auto owner = transaction.exec(
      "SELECT gameplay_revision,player_dead FROM emulator_characters "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 FOR UPDATE",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    return {};
  }
  if (monster_receipt) {
    const auto receipt = transaction.exec(
        "SELECT processed,enqueue_sequence FROM emulator_monster_reward_receipts "
        "WHERE character_id=$1 AND receipt_id=$2 AND account_id=$3 AND world_id=$4 FOR UPDATE",
        pqxx::params{character_id, effect_id, account_id, world_id});
    if (receipt.empty()) {
      return {};
    }
    if (receipt.front()[0].as<bool>()) {
      return {QuestMutationStatus::already_processed, {}};
    }
    const auto earlier = transaction.exec(
        "SELECT 1 FROM emulator_monster_reward_receipts AS receipt "
        "JOIN emulator_monster_reward_events AS event ON event.receipt_id=receipt.receipt_id "
        "WHERE receipt.character_id=$1 AND NOT receipt.processed "
        "AND (NOT event.ready OR NOT event.boss_script_published) "
        "AND receipt.enqueue_sequence < $2 LIMIT 1",
        pqxx::params{character_id, receipt.front()[1].as<std::int64_t>()});
    if (!earlier.empty()) {
      return {};
    }
  }
  if (owner.front()[0].as<std::int64_t>() != before.revision ||
      owner.front()[1].as<bool>() != before.dead) {
    return {};
  }
  if (combat_source &&
      detail::read_locked_world_progress(transaction, account_id, character_id, world_id) != before) {
    return {};
  }
  const auto pending = transaction.exec(
      "SELECT count(*) FROM emulator_quest_service_effects WHERE character_id=$1 "
      "AND (NOT company_applied OR NOT guild_applied)", pqxx::params{character_id});
  if (pending.front()[0].as<std::int64_t>() >=
      static_cast<std::int64_t>(kMaximumPendingQuestServiceEffectsPerCharacter)) {
    return {};
  }
  transaction.exec("INSERT INTO emulator_quest_service_effect_limits(world_id,event_count) "
                   "VALUES ($1,0) ON CONFLICT DO NOTHING", pqxx::params{world_id});
  const auto capacity = transaction.exec(
      "UPDATE emulator_quest_service_effect_limits SET event_count=event_count+1 "
      "WHERE world_id=$1 AND event_count < $2 RETURNING event_count",
      pqxx::params{world_id, kMaximumStoredQuestServiceEvents});
  if (capacity.empty()) {
    return {};
  }

  std::vector<CharacterItem> granted;
  if (after) {
    auto result = detail::apply_postgres_loot_trade(transaction, account_id, character_id,
                                                    world_id, before, *after, trade);
    if (!result) {
      return {};
    }
    granted = std::move(*result);
  }
  const bool company_applied = membership->company_type == 0 ||
      (effects.company_contribution == 0 && effects.company_unlocks.empty() &&
       effects.activity_experience == 0);
  const bool guild_applied = membership->guild_id == 0 ||
      (effects.guild_experience == 0 && effects.activity_experience == 0);
  transaction.exec(
      "INSERT INTO emulator_quest_service_effects "
      "(character_id,account_id,world_id,effect_id,mutation_fingerprint,monster_receipt, "
      "effect_payload,expected_company_type,expected_guild_id,expected_company_generation, "
      "expected_guild_generation,granted_items,company_applied,guild_applied) "
      "VALUES ($1,$2,$3,$4,$5,$6,$7::jsonb,$8,$9,$10,$11,$12::jsonb,$13,$14)",
      pqxx::params{character_id, account_id, world_id, effect_id, fingerprint, monster_receipt,
                   payload.dump(), effects.expected_company_type, effects.expected_guild_id,
                   effects.expected_company_generation, effects.expected_guild_generation,
                   encode_quest_granted_items(granted).dump(), company_applied, guild_applied});
  if (monster_receipt) {
    transaction.exec("UPDATE emulator_monster_reward_receipts SET processed=true "
                     "WHERE character_id=$1 AND receipt_id=$2",
                     pqxx::params{character_id, effect_id});
  }
  return {QuestMutationStatus::committed, std::move(granted)};
}

QuestMutationResult commit_postgres_quest_mutation(
    const std::string &connection_string, std::int64_t owner_epoch, std::int64_t account_id,
    std::int64_t character_id, int world_id, const std::string &effect_id,
    const WorldProgress &before, const std::optional<WorldProgress> &after,
    const ItemTrade &trade, const QuestServiceEffects &effects, bool monster_receipt,
    const std::optional<std::string> &combat_source = std::nullopt) {
  try {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    auto result = commit_locked_postgres_quest_mutation(
        transaction, owner_epoch, account_id, character_id, world_id, effect_id, before,
        after, trade, effects, monster_receipt, combat_source);
    if (result.status != QuestMutationStatus::conflict) {
      transaction.commit();
    }
    return result;
  } catch (const pqxx::unique_violation &) {
    return {};
  }
}

} // namespace

std::optional<QuestServiceMembership> PostgresCharacterStateRepository::read_quest_service_membership(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0) {
    throw std::invalid_argument("Invalid quest service membership owner");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto result = lock_membership(transaction, account_id, character_id, world_id);
  if (!result) {
    return std::nullopt;
  }
  transaction.commit();
  return result;
}

std::vector<QuestServiceEffectEvent> PostgresCharacterStateRepository::pending_quest_service_effects(
    int world_id, const std::optional<std::int64_t> &character_id, std::size_t limit,
    std::uint64_t after_sequence) {
  if (world_id <= 0 || (character_id && *character_id <= 0) || limit == 0 ||
      limit > kQuestServiceEffectPageSize ||
      after_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("Invalid quest service effect page");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto rows = transaction.exec(
      "SELECT enqueue_sequence,character_id,account_id,world_id,effect_id,mutation_fingerprint, "
      "monster_receipt,effect_payload,granted_items,company_applied,guild_applied, "
      "expected_company_type,expected_guild_id,expected_company_generation,expected_guild_generation "
      "FROM (SELECT DISTINCT ON (character_id) * FROM emulator_quest_service_effects "
      "WHERE ($2::bigint=0 OR character_id=$2) "
      "AND (NOT company_applied OR NOT guild_applied) ORDER BY character_id,enqueue_sequence) "
      "AS first_pending WHERE world_id=$1 AND enqueue_sequence > $3 "
      "ORDER BY enqueue_sequence LIMIT $4",
      pqxx::params{world_id, character_id.value_or(0),
                   static_cast<std::int64_t>(after_sequence), limit});
  std::vector<QuestServiceEffectEvent> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    result.push_back(decode_pending_event(row));
  }
  transaction.commit();
  return result;
}

LootClaimResult PostgresCharacterStateRepository::commit_gathering_quest_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &source_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      !lowercase_hex(receipt_id, 32) || !lowercase_hex(source_fingerprint, 64)) {
    throw std::invalid_argument("Invalid gathering claim identity");
  }
  detail::validate_loot_trade(before, after, trade);
  static_cast<void>(encode_quest_service_effects(effects));
  if (source) {
    validate_shared_gathering_source(*source);
  }
  const auto fingerprint = gathering_mutation_fingerprint(source_fingerprint, before, after,
                                                            trade, effects, source);
  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
    const auto membership = lock_membership(transaction, account_id, character_id, world_id);
    if (!membership) {
      return {LootClaimStatus::conflict, {}};
    }
    const auto classify = [&](const pqxx::row &row) -> LootClaimResult {
      if (row[0].as<std::string>() != "gathering" || row[1].as<int>() != world_id ||
          row[2].as<std::string>() != source_fingerprint) {
        return {LootClaimStatus::mismatch, {}};
      }
      if (row[3].as<std::int64_t>() != account_id || row[4].as<std::int64_t>() != character_id) {
        return {LootClaimStatus::claimed_by_other_character, {}};
      }
      if (row[5].is_null() || row[5].as<std::string>() != fingerprint) {
        return {LootClaimStatus::mismatch, {}};
      }
      return {LootClaimStatus::already_claimed_by_same_character,
              decode_quest_granted_items(nlohmann::json::parse(row[6].as<std::string>()))};
    };
    const auto find_receipt = [&] {
      return transaction.exec(
          "SELECT claim_kind,world_id,corpse_fingerprint,account_id,character_id,"
          "mutation_fingerprint,granted_items FROM emulator_loot_claim_receipts "
          "WHERE receipt_id=$1 FOR UPDATE", pqxx::params{receipt_id});
    };
    auto previous = find_receipt();
    if (!previous.empty()) {
      auto result = classify(previous.front());
      transaction.commit();
      return result;
    }
    const bool membership_changed = membership->company_type != effects.expected_company_type ||
        membership->guild_id != effects.expected_guild_id ||
        membership->company_generation != effects.expected_company_generation ||
        membership->guild_generation != effects.expected_guild_generation;

    if (source) {
      std::vector<SharedGatheringSourceTransition> sources{*source};
      if (source->linked) {
        const auto &linked = *source->linked;
        SharedGatheringSourceTransition barrier;
        barrier.map_id = linked.map_id;
        barrier.kind = linked.kind;
        barrier.sector_id = linked.sector_id;
        barrier.local_index = linked.local_index;
        barrier.expected_generation = linked.expected_generation;
        barrier.expected_active = linked.expected_active;
        barrier.expected_phase = linked.expected_phase;
        sources.push_back(std::move(barrier));
      }
      std::ranges::sort(sources, [](const auto &left, const auto &right) {
        return std::tie(left.map_id, left.kind, left.sector_id, left.local_index) <
               std::tie(right.map_id, right.kind, right.sector_id, right.local_index);
      });
      std::size_t initialized_count = 0;
      for (const auto &entry : sources) {
        if (entry.expected_generation == 0) {
          const auto initialized = transaction.exec(
              "INSERT INTO emulator_gathering_sources "
              "(world_id,map_id,source_kind,sector_id,local_index,generation,active,phase) "
              "VALUES ($1,$2,$3,$4,$5,0,$6,NULLIF($7::integer,-1)) "
              "ON CONFLICT DO NOTHING RETURNING world_id",
              pqxx::params{world_id, entry.map_id, entry.kind, entry.sector_id,
                           entry.local_index, entry.expected_active,
                           entry.expected_phase.value_or(-1)});
          if (!initialized.empty()) {
            ++initialized_count;
          }
        }
        const auto rows = transaction.exec(
            "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
            "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
            "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
            "FOR UPDATE", pqxx::params{world_id, entry.map_id, entry.kind,
                                        entry.sector_id, entry.local_index});
        if (rows.empty()) {
          return {LootClaimStatus::source_conflict, {}, std::nullopt};
        }
        auto snapshot = detail::decode_shared_gathering_source(rows.front());
        if (snapshot.generation != entry.expected_generation ||
            snapshot.active != entry.expected_active || snapshot.phase != entry.expected_phase ||
            snapshot.pending_receipt_id ||
            (source->linked && entry.sector_id == source->linked->sector_id &&
             entry.local_index == source->linked->local_index &&
             (snapshot.next_transition_epoch_ticks || snapshot.timed_successor_phase ||
              snapshot.next_transition_delay_ticks))) {
          return {LootClaimStatus::source_conflict, {}, std::move(snapshot)};
        }
      }
      if (initialized_count != 0 &&
          !detail::reserve_shared_source_capacity(transaction, world_id, initialized_count)) {
        return {LootClaimStatus::conflict, {}};
      }
    }
    if (membership_changed) {
      return {LootClaimStatus::conflict, {}};
    }
    const auto inserted = transaction.exec(
        "INSERT INTO emulator_loot_claim_receipts "
        "(receipt_id,account_id,character_id,world_id,corpse_fingerprint,granted_items,"
        "claim_kind,mutation_fingerprint,terminal,shared_source) "
        "VALUES ($1,$2,$3,$4,$5,'[]'::jsonb,'gathering',$6,false,$7) "
        "ON CONFLICT (receipt_id) DO NOTHING RETURNING receipt_id",
        pqxx::params{receipt_id, account_id, character_id, world_id, source_fingerprint,
                     fingerprint, source.has_value()});
    if (inserted.empty()) {
      previous = find_receipt();
      if (previous.empty()) {
        return {LootClaimStatus::conflict, {}};
      }
      // The source row may have been provisionally initialized in this transaction.
      // Roll it back when another transaction already owns the receipt.
      return classify(previous.front());
    }
    transaction.exec("INSERT INTO emulator_loot_claim_limits(world_id,receipt_count) "
                     "VALUES ($1,0) ON CONFLICT DO NOTHING", pqxx::params{world_id});
    const auto capacity = transaction.exec(
        "UPDATE emulator_loot_claim_limits SET receipt_count=receipt_count+1 "
        "WHERE world_id=$1 AND receipt_count < $2 RETURNING receipt_count",
        pqxx::params{world_id, kMaximumRetainedLootClaimsPerWorld});
    if (capacity.empty()) {
      return {LootClaimStatus::conflict, {}};
    }
    const bool has_effect =
        (effects.expected_company_type != 0 &&
         (effects.company_contribution != 0 || effects.activity_experience != 0 ||
          !effects.company_unlocks.empty())) ||
        (effects.expected_guild_id != 0 &&
         (effects.guild_experience != 0 || effects.activity_experience != 0));
    std::vector<CharacterItem> granted;
    if (has_effect) {
      auto saved = commit_locked_postgres_quest_mutation(
          transaction, monster_reward_owner_epoch_.load(), account_id, character_id, world_id,
          receipt_id, before, after, trade, effects, false);
      if (saved.status != QuestMutationStatus::committed) {
        return {LootClaimStatus::conflict, {}};
      }
      granted = std::move(saved.granted_items);
    } else {
      auto saved = detail::apply_postgres_loot_trade(transaction, account_id, character_id,
                                                     world_id, before, after, trade);
      if (!saved) {
        return {LootClaimStatus::conflict, {}};
      }
      granted = std::move(*saved);
    }
    if (source) {
      transaction.exec(
          "UPDATE emulator_gathering_sources SET generation=$6,active=$7,phase=NULLIF($8::integer,-1),"
          "next_transition_epoch_ticks=NULL,timed_successor_phase=NULLIF($9::integer,-1),"
          "pending_receipt_id=$10,next_transition_delay_ticks=NULLIF($11::bigint,-1::bigint) "
          "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
          pqxx::params{world_id, source->map_id, source->kind, source->sector_id,
                       source->local_index, source->expected_generation + 1,
                       source->resulting_active, source->resulting_phase.value_or(-1),
                       source->timed_successor_phase.value_or(-1), receipt_id,
                       source->next_transition_delay_ticks.value_or(-1)});
      if (source->linked) {
        const auto &linked = *source->linked;
        transaction.exec(
            "UPDATE emulator_gathering_sources SET generation=$6,active=false,phase=0,"
            "next_transition_epoch_ticks=NULL,timed_successor_phase=NULL "
            "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
            pqxx::params{world_id, linked.map_id, linked.kind, linked.sector_id,
                         linked.local_index, linked.expected_generation + 1});
      }
    }
    transaction.exec("UPDATE emulator_loot_claim_receipts SET granted_items=$2::jsonb "
                     "WHERE receipt_id=$1",
                     pqxx::params{receipt_id, encode_quest_granted_items(granted).dump()});
    transaction.commit();
    return {LootClaimStatus::committed, std::move(granted)};
  } catch (const pqxx::unique_violation &) {
    return {LootClaimStatus::conflict, {}};
  }
}

GatheringFinalizeResult PostgresCharacterStateRepository::finalize_gathering_quest_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &source_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source,
    std::int64_t confirmation_epoch_ticks) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      !lowercase_hex(receipt_id, 32) || !lowercase_hex(source_fingerprint, 64) ||
      confirmation_epoch_ticks <= 0) {
    throw std::invalid_argument("Invalid gathering finalization identity");
  }
  if (source) {
    validate_shared_gathering_source(*source);
  }
  const auto fingerprint = gathering_mutation_fingerprint(source_fingerprint, before, after,
                                                            trade, effects, source);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!lock_membership(transaction, account_id, character_id, world_id)) {
    return {};
  }
  // The allowlisted barrier key sorts before the cannon; lock sources before the receipt.
  pqxx::result linked_rows;
  if (source && source->linked) {
    const auto &linked = *source->linked;
    linked_rows = transaction.exec(
        "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
        "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
        "FOR UPDATE", pqxx::params{world_id, linked.map_id, linked.kind,
                                    linked.sector_id, linked.local_index});
  }
  pqxx::result source_rows;
  if (source) {
    source_rows = transaction.exec(
        "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
        "timed_successor_phase,next_transition_delay_ticks "
        "FROM emulator_gathering_sources WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 "
        "AND sector_id=$4 AND local_index=$5 FOR UPDATE",
        pqxx::params{world_id, source->map_id, source->kind, source->sector_id,
                     source->local_index});
  }
  const auto rows = transaction.exec(
      "SELECT claim_kind,world_id,corpse_fingerprint,account_id,character_id,"
      "mutation_fingerprint,terminal,armed_deadline_epoch_ticks "
      "FROM emulator_loot_claim_receipts WHERE receipt_id=$1 FOR UPDATE",
      pqxx::params{receipt_id});
  if (rows.empty()) {
    return {};
  }
  const auto &row = rows.front();
  if (row[0].as<std::string>() != "gathering" || row[1].as<int>() != world_id ||
      row[2].as<std::string>() != source_fingerprint || row[3].as<std::int64_t>() != account_id ||
      row[4].as<std::int64_t>() != character_id || row[5].is_null() ||
      row[5].as<std::string>() != fingerprint) {
    return {};
  }
  std::optional<StoredSharedGatheringSource> linked_snapshot;
  if (source && source->linked) {
    if (linked_rows.size() != 1) {
      return {};
    }
    linked_snapshot = detail::decode_shared_gathering_source(linked_rows.front());
    if (linked_snapshot->pending_receipt_id || linked_snapshot->active ||
        linked_snapshot->phase != 0 || linked_snapshot->next_transition_epoch_ticks ||
        linked_snapshot->timed_successor_phase || linked_snapshot->next_transition_delay_ticks) {
      return {};
    }
    if (linked_snapshot->generation >= std::numeric_limits<std::int64_t>::max() - 1) {
      throw std::overflow_error("Linked gathering source generation exhausted");
    }
  }
  if (row[6].as<bool>()) {
    const auto deadline = row[7].is_null() ? std::optional<std::int64_t>{} :
                                             std::optional<std::int64_t>{row[7].as<std::int64_t>()};
    std::optional<StoredSharedGatheringSource> snapshot;
    if (source) {
      if (source_rows.size() != 1) {
        return {};
      }
      snapshot = detail::decode_shared_gathering_source(source_rows.front());
      if (linked_snapshot) {
        if (snapshot->pending_receipt_id ||
            snapshot->generation >= std::numeric_limits<std::int64_t>::max() - 1) {
          return {};
        }
        transaction.exec(
            "UPDATE emulator_gathering_sources SET generation=generation+1 "
            "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND "
            "((sector_id=$4 AND local_index=$5) OR (sector_id=$6 AND local_index=$7))",
            pqxx::params{world_id, source->map_id, source->kind, source->sector_id,
                         source->local_index, source->linked->sector_id,
                         source->linked->local_index});
        ++snapshot->generation;
        ++linked_snapshot->generation;
      }
    }
    transaction.commit();
    return {GatheringFinalizeStatus::already_finalized, deadline, std::move(snapshot),
            std::move(linked_snapshot)};
  }
  if (linked_snapshot) {
    const auto &linked = *source->linked;
    transaction.exec(
        "UPDATE emulator_gathering_sources SET generation=generation+1 "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
        pqxx::params{world_id, linked.map_id, linked.kind, linked.sector_id, linked.local_index});
    ++linked_snapshot->generation;
  }
  std::optional<std::int64_t> deadline;
  std::optional<StoredSharedGatheringSource> snapshot;
  if (source) {
    if (source_rows.size() != 1) {
      return {};
    }
    snapshot = detail::decode_shared_gathering_source(source_rows.front());
    if (snapshot->generation != source->expected_generation + 1 ||
        snapshot->pending_receipt_id != receipt_id ||
        snapshot->next_transition_delay_ticks != source->next_transition_delay_ticks) {
      return {};
    }
    if (source->next_transition_delay_ticks) {
      const auto delay = *source->next_transition_delay_ticks;
      if (confirmation_epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
        throw std::overflow_error("Gathering transition deadline overflow");
      }
      deadline = confirmation_epoch_ticks + delay;
    }
    ++snapshot->generation;
    snapshot->pending_receipt_id.reset();
    snapshot->next_transition_delay_ticks.reset();
    snapshot->next_transition_epoch_ticks = deadline;
    transaction.exec(
        "UPDATE emulator_gathering_sources SET generation=generation+1,pending_receipt_id=NULL,"
        "next_transition_delay_ticks=NULL,next_transition_epoch_ticks=NULLIF($6::bigint,-1::bigint) "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
        pqxx::params{world_id, source->map_id, source->kind, source->sector_id,
                     source->local_index, deadline.value_or(-1)});
  }
  transaction.exec("UPDATE emulator_loot_claim_receipts SET terminal=true, "
                   "armed_deadline_epoch_ticks=NULLIF($2::bigint,-1::bigint) WHERE receipt_id=$1",
                   pqxx::params{receipt_id, deadline.value_or(-1)});
  transaction.commit();
  return {GatheringFinalizeStatus::finalized, deadline, std::move(snapshot),
          std::move(linked_snapshot)};
}

std::size_t PostgresCharacterStateRepository::recover_gathering_claims(
    std::size_t limit, std::int64_t confirmation_epoch_ticks) {
  if (limit == 0 || limit > kMonsterRewardRecoveryPageSize || confirmation_epoch_ticks <= 0) {
    throw std::invalid_argument("Invalid gathering startup recovery page");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto pending = transaction.exec(
      "SELECT receipt_id,world_id,shared_source FROM emulator_loot_claim_receipts "
      "WHERE claim_kind='gathering' AND NOT terminal ORDER BY receipt_id LIMIT $1",
      pqxx::params{limit});
  for (const auto &entry : pending) {
    const auto receipt_id = entry[0].as<std::string>();
    const auto world_id = entry[1].as<int>();
    const bool shared = entry[2].as<bool>();
    const auto sources = transaction.exec(
        "SELECT world_id,generation,next_transition_delay_ticks FROM emulator_gathering_sources "
        "WHERE pending_receipt_id=$1 FOR UPDATE", pqxx::params{receipt_id});
    if (sources.size() != static_cast<std::size_t>(shared) ||
        (!sources.empty() && sources.front()[0].as<int>() != world_id)) {
      throw std::runtime_error("Gathering startup recovery lost its source");
    }
    const auto locked = transaction.exec(
        "SELECT terminal,claim_kind,world_id,shared_source FROM emulator_loot_claim_receipts "
        "WHERE receipt_id=$1 FOR UPDATE", pqxx::params{receipt_id});
    if (locked.size() != 1 || locked.front()[0].as<bool>() ||
        locked.front()[1].as<std::string>() != "gathering" ||
        locked.front()[2].as<int>() != world_id || locked.front()[3].as<bool>() != shared) {
      throw std::runtime_error("Gathering startup recovery receipt changed");
    }
    std::optional<std::int64_t> deadline;
    if (shared) {
      const auto generation = sources.front()[1].as<std::int64_t>();
      if (generation == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("Gathering source generation exhausted");
      }
      if (!sources.front()[2].is_null()) {
        const auto delay = sources.front()[2].as<std::int64_t>();
        if (delay <= 0 || confirmation_epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
          throw std::overflow_error("Gathering startup deadline cannot be armed");
        }
        deadline = confirmation_epoch_ticks + delay;
      }
      transaction.exec(
          "UPDATE emulator_gathering_sources SET generation=generation+1,pending_receipt_id=NULL,"
          "next_transition_delay_ticks=NULL,next_transition_epoch_ticks=NULLIF($2::bigint,-1::bigint) "
          "WHERE pending_receipt_id=$1", pqxx::params{receipt_id, deadline.value_or(-1)});
    }
    transaction.exec("UPDATE emulator_loot_claim_receipts SET terminal=true, "
                     "armed_deadline_epoch_ticks=NULLIF($2::bigint,-1::bigint) WHERE receipt_id=$1",
                     pqxx::params{receipt_id, deadline.value_or(-1)});
  }
  transaction.commit();
  return pending.size();
}

bool PostgresCharacterStateRepository::has_unfinished_gathering_claims() {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const bool pending = !transaction.exec(
      "SELECT 1 FROM emulator_loot_claim_receipts WHERE claim_kind='gathering' "
      "AND NOT terminal LIMIT 1").empty();
  transaction.commit();
  return pending;
}

std::vector<SharedGatheringSourceRecord> PostgresCharacterStateRepository::shared_gathering_sources(
    int world_id, const std::optional<SharedGatheringSourceKey> &after, std::size_t limit) {
  if (world_id <= 0 || limit == 0 || limit > kSharedGatheringSourcePageSize ||
      (after && (after->map_id <= 0 || (after->kind != 0 && after->kind != 1)))) {
    throw std::invalid_argument("Invalid shared gathering hydration page");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const SharedGatheringSourceKey cursor = after.value_or(SharedGatheringSourceKey{});
  const auto rows = transaction.exec(
      "SELECT map_id,source_kind,sector_id,local_index,generation,active,phase,"
      "pending_receipt_id,next_transition_epoch_ticks,timed_successor_phase,next_transition_delay_ticks "
      "FROM emulator_gathering_sources WHERE world_id=$1 "
      "AND (map_id,source_kind,sector_id,local_index)>($2,$3,$4,$5) "
      "ORDER BY map_id,source_kind,sector_id,local_index LIMIT $6",
      pqxx::params{world_id, cursor.map_id, cursor.kind, cursor.sector_id, cursor.local_index, limit});
  std::vector<SharedGatheringSourceRecord> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    SharedGatheringSourceKey key{row[0].as<int>(), row[1].as<int>(),
                                  row[2].as<std::uint32_t>(), row[3].as<std::uint32_t>()};
    result.push_back({key, detail::decode_shared_gathering_source(row, 4)});
  }
  transaction.commit();
  return result;
}

SharedGatheringSourceUpdateResult PostgresCharacterStateRepository::update_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &transition,
    std::int64_t epoch_ticks, bool require_due) {
  if (world_id <= 0 || epoch_ticks <= 0 || transition.linked) {
    throw std::invalid_argument("Invalid shared gathering transition time");
  }
  validate_shared_gathering_source(transition);
  std::optional<std::int64_t> deadline;
  if (transition.next_transition_delay_ticks) {
    const auto delay = *transition.next_transition_delay_ticks;
    if (epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
      throw std::overflow_error("Shared gathering transition deadline overflow");
    }
    deadline = epoch_ticks + delay;
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!require_due && transition.expected_generation == 0) {
    const auto initialized = transaction.exec(
        "INSERT INTO emulator_gathering_sources "
        "(world_id,map_id,source_kind,sector_id,local_index,generation,active,phase) "
        "VALUES ($1,$2,$3,$4,$5,0,$6,NULLIF($7::integer,-1)) "
        "ON CONFLICT DO NOTHING RETURNING world_id",
        pqxx::params{world_id, transition.map_id, transition.kind, transition.sector_id,
                     transition.local_index, transition.expected_active,
                     transition.expected_phase.value_or(-1)});
    if (!initialized.empty() && !detail::reserve_shared_source_capacity(transaction, world_id)) {
      return {};
    }
  }
  const auto rows = transaction.exec(
      "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
      "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
      "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
      "FOR UPDATE", pqxx::params{world_id, transition.map_id, transition.kind,
                                  transition.sector_id, transition.local_index});
  if (rows.empty()) {
    return {};
  }
  auto state = detail::decode_shared_gathering_source(rows.front());
  if (state.generation != transition.expected_generation ||
      state.active != transition.expected_active || state.phase != transition.expected_phase ||
      state.pending_receipt_id ||
      (require_due && (!state.next_transition_epoch_ticks ||
                       *state.next_transition_epoch_ticks > epoch_ticks))) {
    return {false, std::move(state)};
  }
  const auto updated = transaction.exec(
      "UPDATE emulator_gathering_sources SET generation=$6,active=$7,phase=NULLIF($8::integer,-1),"
      "next_transition_epoch_ticks=NULLIF($9::bigint,-1::bigint),"
      "timed_successor_phase=NULLIF($10::integer,-1),next_transition_delay_ticks=NULL "
      "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
      "RETURNING generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
      "timed_successor_phase,next_transition_delay_ticks",
      pqxx::params{world_id, transition.map_id, transition.kind, transition.sector_id,
                   transition.local_index, transition.expected_generation + 1,
                   transition.resulting_active, transition.resulting_phase.value_or(-1),
                   deadline.value_or(-1), transition.timed_successor_phase.value_or(-1)});
  auto result = detail::decode_shared_gathering_source(updated.front());
  transaction.commit();
  return {true, std::move(result)};
}

SharedGatheringSourceUpdateResult PostgresCharacterStateRepository::fence_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &initial) {
  if (world_id <= 0) {
    throw std::invalid_argument("Invalid shared gathering fence world");
  }
  validate_shared_gathering_source(initial);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  std::optional<StoredSharedGatheringSource> linked_snapshot;
  std::size_t initialized_count = 0;
  if (initial.linked) {
    const auto &linked = *initial.linked;
    // This exact unprofiled key sorts before the cannon. A null phase is an
    // explicit non-rendered marker, not a guessed pre-script appearance.
    if (linked.expected_generation == 0) {
      const auto inserted = transaction.exec(
          "INSERT INTO emulator_gathering_sources "
          "(world_id,map_id,source_kind,sector_id,local_index,generation,active,phase) "
          "VALUES ($1,$2,$3,$4,$5,0,false,NULL) ON CONFLICT DO NOTHING RETURNING world_id",
          pqxx::params{world_id, linked.map_id, linked.kind, linked.sector_id,
                       linked.local_index});
      if (!inserted.empty()) {
        ++initialized_count;
      }
    }
    const auto linked_rows = transaction.exec(
        "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
        "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
        "FOR UPDATE", pqxx::params{world_id, linked.map_id, linked.kind,
                                    linked.sector_id, linked.local_index});
    if (linked_rows.empty()) {
      return {};
    }
    linked_snapshot = detail::decode_shared_gathering_source(linked_rows.front());
    if (linked_snapshot->pending_receipt_id || linked_snapshot->active ||
        (linked_snapshot->phase && linked_snapshot->phase != 0) ||
        linked_snapshot->next_transition_epoch_ticks || linked_snapshot->timed_successor_phase ||
        linked_snapshot->next_transition_delay_ticks) {
      return {false, std::nullopt, std::move(linked_snapshot)};
    }
    if (linked_snapshot->generation >= std::numeric_limits<std::int64_t>::max() - 1) {
      throw std::overflow_error("Linked gathering source generation exhausted");
    }
  }
  if (initial.expected_generation == 0) {
    const auto initialized = transaction.exec(
        "INSERT INTO emulator_gathering_sources "
        "(world_id,map_id,source_kind,sector_id,local_index,generation,active,phase) "
        "VALUES ($1,$2,$3,$4,$5,0,$6,NULLIF($7::integer,-1)) "
        "ON CONFLICT DO NOTHING RETURNING world_id",
        pqxx::params{world_id, initial.map_id, initial.kind, initial.sector_id,
                     initial.local_index, initial.expected_active, initial.expected_phase.value_or(-1)});
    if (!initialized.empty()) {
      ++initialized_count;
    }
  }
  const auto rows = transaction.exec(
      "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
      "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
      "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
      "FOR UPDATE", pqxx::params{world_id, initial.map_id, initial.kind,
                                  initial.sector_id, initial.local_index});
  if (rows.empty()) {
    return {};
  }
  auto snapshot = detail::decode_shared_gathering_source(rows.front());
  if (snapshot.pending_receipt_id) {
    return {false, std::move(snapshot), std::move(linked_snapshot)};
  }
  if (snapshot.generation >= std::numeric_limits<std::int64_t>::max() - 1) {
    throw std::overflow_error("Shared gathering source generation exhausted");
  }
  if (initialized_count != 0 &&
      !detail::reserve_shared_source_capacity(transaction, world_id, initialized_count)) {
    return {};
  }
  transaction.exec(
      "UPDATE emulator_gathering_sources SET generation=generation+1 "
      "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
      pqxx::params{world_id, initial.map_id, initial.kind, initial.sector_id,
                   initial.local_index});
  ++snapshot.generation;
  if (linked_snapshot) {
    const auto &linked = *initial.linked;
    transaction.exec(
        "UPDATE emulator_gathering_sources SET generation=generation+1 "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
        pqxx::params{world_id, linked.map_id, linked.kind, linked.sector_id, linked.local_index});
    ++linked_snapshot->generation;
  }
  transaction.commit();
  return {true, std::move(snapshot), std::move(linked_snapshot)};
}

std::optional<std::vector<CharacterItem>>
PostgresCharacterStateRepository::commit_unaffiliated_activity(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const QuestServiceMembership &membership, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      membership.company_type != 0 || membership.guild_id != 0 ||
      membership.company_generation < 0 || membership.guild_generation < 0) {
    throw std::invalid_argument("Invalid unaffiliated activity identity");
  }
  detail::validate_loot_trade(before, after, trade);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto current = lock_membership(transaction, account_id, character_id, world_id);
  if (!current || *current != membership) {
    return std::nullopt;
  }
  auto granted = detail::apply_postgres_loot_trade(transaction, account_id, character_id,
                                                    world_id, before, after, trade);
  if (!granted) {
    return std::nullopt;
  }
  transaction.commit();
  return granted;
}

QuestMutationResult PostgresCharacterStateRepository::commit_quest_mutation(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &effect_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  return commit_postgres_quest_mutation(connection_, monster_reward_owner_epoch_.load(), account_id,
                                         character_id, world_id, effect_id, before, after, trade,
                                         effects, false);
}

QuestMutationResult PostgresCharacterStateRepository::commit_combat_quest_event(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &effect_id, const std::string &source_fingerprint,
    const WorldProgress &before, const std::optional<WorldProgress> &after,
    const ItemTrade &trade, const QuestServiceEffects &effects) {
  static_cast<void>(combat_quest_event_fingerprint(source_fingerprint, before, after, trade,
                                                   effects));
  return commit_postgres_quest_mutation(connection_, monster_reward_owner_epoch_.load(), account_id,
                                         character_id, world_id, effect_id, before, after, trade,
                                         effects, false, source_fingerprint);
}

MonsterRewardCommitResult PostgresCharacterStateRepository::commit_monster_reward_with_effects(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  auto result = commit_postgres_quest_mutation(connection_, monster_reward_owner_epoch_.load(),
                                                account_id, character_id, world_id, receipt_id,
                                                before, after, trade, effects, true);
  switch (result.status) {
  case QuestMutationStatus::committed:
    return {MonsterRewardCommitStatus::committed, std::move(result.granted_items)};
  case QuestMutationStatus::already_processed:
    return {MonsterRewardCommitStatus::already_processed, std::move(result.granted_items)};
  case QuestMutationStatus::conflict:
    return {};
  }
  throw std::logic_error("Unknown quest mutation status");
}

} // namespace chronicles
