#include "persistence/skill_item_use.hpp"

#include "persistence/loot_claim_internal.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "persistence/quest_service_effect_guards.hpp"
#include "persistence/quest_service_effects.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

bool lowercase_hex(std::string_view value, std::size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

void validate_owner(std::int64_t account_id, std::int64_t character_id, int world_id) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0) {
    throw std::invalid_argument("Invalid skill item owner");
  }
}

std::string sha256(const std::string &input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != 32) {
    throw std::runtime_error("Cannot fingerprint skill item use");
  }
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (unsigned int index = 0; index < length; ++index) {
    result.push_back(hex[digest[index] >> 4]);
    result.push_back(hex[digest[index] & 15]);
  }
  return result;
}

void validate_plan(const SkillItemUsePlan &plan) {
  const auto expected = prepare_skill_item_use(plan.nonce, plan.item_cms_id, plan.before);
  if (expected.after != plan.after || expected.response_json != plan.response_json) {
    throw std::invalid_argument("Skill item use plan differs from its prepared result");
  }
}

bool pending(SkillItemUseState state) {
  return state == SkillItemUseState::reserved || state == SkillItemUseState::committed;
}

const char *state_name(SkillItemUseState state) {
  switch (state) {
  case SkillItemUseState::reserved:
    return "reserved";
  case SkillItemUseState::committed:
    return "committed";
  case SkillItemUseState::completed:
    return "completed";
  case SkillItemUseState::aborted:
    return "aborted";
  }
  throw std::logic_error("Invalid skill item receipt state");
}

SkillItemUseState parse_state(const std::string &state) {
  if (state == "reserved") {
    return SkillItemUseState::reserved;
  }
  if (state == "committed") {
    return SkillItemUseState::committed;
  }
  if (state == "completed") {
    return SkillItemUseState::completed;
  }
  if (state == "aborted") {
    return SkillItemUseState::aborted;
  }
  throw std::runtime_error("Invalid stored skill item receipt state");
}

SkillItemUseReceipt read_receipt(const pqxx::row &row) {
  return {row[0].as<std::int64_t>(), row[1].as<std::int64_t>(), row[2].as<int>(),
          row[3].as<std::int64_t>(), row[4].as<std::string>(), row[5].as<std::string>(),
          row[6].as<int>(), row[7].as<int>(), row[8].as<std::string>(),
          parse_state(row[9].as<std::string>())};
}

pqxx::result postgres_receipt(pqxx::transaction_base &transaction, std::int64_t character_id,
                              bool lock) {
  const std::string query =
      "SELECT account_id,character_id,world_id,sequence,event_id,fingerprint,item_cms_id,"
      "remaining,response_json,state FROM emulator_skill_item_use_receipts "
      "WHERE character_id=$1" + std::string(lock ? " FOR UPDATE" : "");
  return transaction.exec(query, pqxx::params{character_id});
}

bool postgres_lock_owner(pqxx::work &transaction, std::int64_t account_id,
                         std::int64_t character_id, int world_id) {
  return !transaction.exec("SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 "
                           "AND world_id=$3 FOR UPDATE",
                           pqxx::params{account_id, character_id, world_id}).empty();
}

void require_memory_owner(const MemoryDatabase &database, std::int64_t epoch) {
  if (epoch <= 0 || database.monster_reward_owner_epoch != epoch) {
    throw std::runtime_error("Reward owner epoch is stale or missing");
  }
}

bool memory_progress_matches(const WorldProgress &stored, const Character &owner,
                             const WorldProgress &expected) {
  auto actual = stored;
  actual.experience = owner.experience;
  actual.level = owner.level;
  actual.build = owner.build;
  actual.nui_blessing_level = owner.nui_blessing_level;
  actual.dedicated_nui_tears = owner.dedicated_nui_tears;
  return actual == expected;
}

SkillItemUseResult classify_reservation(const SkillItemUseReceipt &receipt,
                                        const SkillItemUsePlan &plan,
                                        const std::string &fingerprint) {
  if (receipt.event_id.substr(16) == plan.nonce) {
    if (receipt.fingerprint != fingerprint) {
      return {SkillItemUseStatus::mismatch, std::nullopt};
    }
    return {SkillItemUseStatus::replay, receipt};
  }
  if (pending(receipt.state)) {
    return {SkillItemUseStatus::conflict, std::nullopt};
  }
  return {SkillItemUseStatus::accepted, std::nullopt};
}

SkillItemUseReceipt make_receipt(std::int64_t account_id, std::int64_t character_id,
                                 int world_id, std::int64_t sequence,
                                 const SkillItemUsePlan &plan, std::string fingerprint) {
  return {account_id, character_id, world_id, sequence,
          skill_item_use_event_id(sequence, plan.nonce), std::move(fingerprint),
          plan.item_cms_id, plan.after.stackables.at(plan.item_cms_id), plan.response_json,
          SkillItemUseState::reserved};
}

SkillItemUseResult classify_commit(const SkillItemUseReceipt &receipt, std::int64_t sequence,
                                   const SkillItemUsePlan &plan, const std::string &fingerprint) {
  if (receipt.sequence != sequence || receipt.event_id != skill_item_use_event_id(sequence, plan.nonce)) {
    return {SkillItemUseStatus::conflict, std::nullopt};
  }
  if (receipt.fingerprint != fingerprint) {
    return {SkillItemUseStatus::mismatch, std::nullopt};
  }
  if (receipt.state == SkillItemUseState::committed || receipt.state == SkillItemUseState::completed) {
    return {SkillItemUseStatus::replay, receipt};
  }
  if (receipt.state != SkillItemUseState::reserved) {
    return {SkillItemUseStatus::conflict, std::nullopt};
  }
  return {SkillItemUseStatus::accepted, std::nullopt};
}

} // namespace

bool spend_skill_item(WorldProgress &progress, int item_cms_id) {
  const auto stack = progress.stackables.find(item_cms_id);
  if (stack == progress.stackables.end() || stack->second <= 0) {
    return false;
  }

  --stack->second;
  for (auto &slot : progress.utility_slots) {
    if (slot.item_cms_id == item_cms_id && slot.count > 0) {
      --slot.count;
      break;
    }
  }
  return true;
}

SkillItemUsePlan prepare_skill_item_use(const std::string &nonce, int item_cms_id,
                                        const WorldProgress &before) {
  if (!lowercase_hex(nonce, 16) || item_cms_id <= 0 ||
      before.revision == (std::numeric_limits<std::int64_t>::max)()) {
    throw std::invalid_argument("Invalid skill item use preparation");
  }
  auto after = before;
  if (!spend_skill_item(after, item_cms_id)) {
    throw std::runtime_error("Skill item is not available");
  }
  ++after.revision;
  const nlohmann::json message{{"stackables", {{{"item_cms_id", item_cms_id},
                                                  {"quantity", after.stackables.at(item_cms_id)}}}},
                               {"utility_bag", nlohmann::json::object()}};
  return {nonce, item_cms_id, before, std::move(after), message.dump()};
}

std::string skill_item_use_event_id(std::int64_t sequence, const std::string &nonce) {
  if (sequence <= 0 || !lowercase_hex(nonce, 16)) {
    throw std::invalid_argument("Invalid skill item event identity");
  }
  std::ostringstream result;
  result << std::hex << std::setfill('0') << std::setw(16) << sequence << nonce;
  return result.str();
}

std::string skill_item_use_fingerprint(std::int64_t account_id, std::int64_t character_id,
                                       int world_id, const SkillItemUsePlan &plan) {
  validate_owner(account_id, character_id, world_id);
  validate_plan(plan);
  detail::validate_loot_trade(plan.before, plan.after, ItemTrade{});
  const auto mutation = quest_mutation_fingerprint(plan.before, plan.after, ItemTrade{},
                                                    QuestServiceEffects{});
  const nlohmann::json document{{"domain", "skill-item-use-v1"}, {"account_id", account_id},
                                {"character_id", character_id}, {"world_id", world_id},
                                {"nonce", plan.nonce}, {"item_cms_id", plan.item_cms_id},
                                {"mutation", mutation}, {"response_json", plan.response_json}};
  return sha256(document.dump());
}

SkillItemUseResult MemoryCharacterStateRepository::reserve_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t expected_sequence,
    const SkillItemUsePlan &plan) {
  auto fingerprint = skill_item_use_fingerprint(account_id, character_id, world_id, plan);
  std::lock_guard lock(database_->mutex);
  require_memory_owner(*database_, monster_reward_owner_epoch_.load());
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return {};
  }
  const auto existing = database_->skill_item_uses.find(character_id);
  std::int64_t sequence = 1;
  if (existing != database_->skill_item_uses.end()) {
    if (expected_sequence == existing->second.sequence) {
      auto result = classify_reservation(existing->second, plan, fingerprint);
      if (result.status == SkillItemUseStatus::accepted) {
        return {};
      }
      return result;
    }
    if (existing->second.sequence == (std::numeric_limits<std::int64_t>::max)()) {
      return {SkillItemUseStatus::exhausted, std::nullopt};
    }
    sequence = existing->second.sequence + 1;
    if (pending(existing->second.state)) {
      return {};
    }
  }
  if (expected_sequence != sequence) {
    return {};
  }
  const auto progress = database_->world_progress.find(character_id);
  if (progress == database_->world_progress.end() ||
      !memory_progress_matches(progress->second, *owner, plan.before)) {
    return {};
  }
  auto receipt = make_receipt(account_id, character_id, world_id, sequence, plan,
                              std::move(fingerprint));
  SkillItemUseResult result{SkillItemUseStatus::accepted, receipt};
  if (existing == database_->skill_item_uses.end()) {
    database_->skill_item_uses.emplace(character_id, std::move(receipt));
  } else {
    existing->second = std::move(receipt);
  }
  return result;
}

SkillItemUseResult MemoryCharacterStateRepository::commit_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t sequence,
    const SkillItemUsePlan &plan) {
  const auto fingerprint = skill_item_use_fingerprint(account_id, character_id, world_id, plan);
  std::lock_guard lock(database_->mutex);
  require_memory_owner(*database_, monster_reward_owner_epoch_.load());
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  const auto receipt = database_->skill_item_uses.find(character_id);
  if (owner == database_->characters.end() || receipt == database_->skill_item_uses.end()) {
    return {};
  }
  auto classification = classify_commit(receipt->second, sequence, plan, fingerprint);
  if (classification.status != SkillItemUseStatus::accepted) {
    return classification;
  }
  auto progress = database_->world_progress.find(character_id);
  if (progress == database_->world_progress.end() ||
      !memory_progress_matches(progress->second, *owner, plan.before)) {
    return {};
  }
  auto prepared = detail::prepare_memory_loot_trade(*database_, *owner, plan.after, ItemTrade{});
  if (!prepared) {
    return {};
  }
  auto committed = receipt->second;
  committed.state = SkillItemUseState::committed;
  SkillItemUseResult result{SkillItemUseStatus::accepted, committed};
  detail::publish_memory_loot_trade(*database_, *owner, progress->second, *prepared);
  receipt->second.state = SkillItemUseState::committed;
  return result;
}

std::optional<SkillItemUseReceipt> MemoryCharacterStateRepository::lookup_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  validate_owner(account_id, character_id, world_id);
  std::lock_guard lock(database_->mutex);
  require_memory_owner(*database_, monster_reward_owner_epoch_.load());
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    return std::nullopt;
  }
  const auto receipt = database_->skill_item_uses.find(character_id);
  return receipt == database_->skill_item_uses.end() ? std::nullopt
                                                     : std::optional{receipt->second};
}

bool MemoryCharacterStateRepository::finish_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t sequence,
    const std::string &event_id, bool committed) {
  validate_owner(account_id, character_id, world_id);
  std::lock_guard lock(database_->mutex);
  require_memory_owner(*database_, monster_reward_owner_epoch_.load());
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    return false;
  }
  const auto receipt = database_->skill_item_uses.find(character_id);
  if (receipt == database_->skill_item_uses.end() || receipt->second.sequence != sequence ||
      receipt->second.event_id != event_id) {
    return false;
  }
  const auto target = committed ? SkillItemUseState::completed : SkillItemUseState::aborted;
  const auto expected = committed ? SkillItemUseState::committed : SkillItemUseState::reserved;
  if (receipt->second.state == target) {
    return true;
  }
  if (receipt->second.state != expected) {
    return false;
  }
  receipt->second.state = target;
  return true;
}

SkillItemUseResult PostgresCharacterStateRepository::reserve_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t expected_sequence,
    const SkillItemUsePlan &plan) {
  auto fingerprint = skill_item_use_fingerprint(account_id, character_id, world_id, plan);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!postgres_lock_owner(transaction, account_id, character_id, world_id)) {
    return {};
  }
  const auto rows = postgres_receipt(transaction, character_id, true);
  std::int64_t sequence = 1;
  if (!rows.empty()) {
    const auto previous = read_receipt(rows.front());
    if (expected_sequence == previous.sequence) {
      auto result = classify_reservation(previous, plan, fingerprint);
      if (result.status == SkillItemUseStatus::accepted) {
        return {};
      }
      return result;
    }
    if (previous.sequence == (std::numeric_limits<std::int64_t>::max)()) {
      return {SkillItemUseStatus::exhausted, std::nullopt};
    }
    sequence = previous.sequence + 1;
    if (pending(previous.state)) {
      return {};
    }
  }
  if (expected_sequence != sequence) {
    return {};
  }
  const auto progress = detail::read_locked_world_progress(transaction, account_id,
                                                            character_id, world_id);
  if (progress != plan.before) {
    return {};
  }
  auto receipt = make_receipt(account_id, character_id, world_id, sequence, plan,
                              std::move(fingerprint));
  SkillItemUseResult result{SkillItemUseStatus::accepted, receipt};
  transaction.exec(
      "INSERT INTO emulator_skill_item_use_receipts "
      "(character_id,account_id,world_id,sequence,event_id,fingerprint,item_cms_id,"
      "remaining,response_json,state) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,'reserved') "
      "ON CONFLICT (character_id) DO UPDATE SET account_id=EXCLUDED.account_id,"
      "world_id=EXCLUDED.world_id,sequence=EXCLUDED.sequence,event_id=EXCLUDED.event_id,"
      "fingerprint=EXCLUDED.fingerprint,item_cms_id=EXCLUDED.item_cms_id,"
      "remaining=EXCLUDED.remaining,response_json=EXCLUDED.response_json,state='reserved'",
      pqxx::params{character_id, account_id, world_id, sequence, receipt.event_id,
                   receipt.fingerprint, receipt.item_cms_id, receipt.remaining,
                   receipt.response_json});
  transaction.commit();
  return result;
}

SkillItemUseResult PostgresCharacterStateRepository::commit_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t sequence,
    const SkillItemUsePlan &plan) {
  const auto fingerprint = skill_item_use_fingerprint(account_id, character_id, world_id, plan);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!postgres_lock_owner(transaction, account_id, character_id, world_id)) {
    return {};
  }
  const auto rows = postgres_receipt(transaction, character_id, true);
  if (rows.empty()) {
    return {};
  }
  auto receipt = read_receipt(rows.front());
  auto classification = classify_commit(receipt, sequence, plan, fingerprint);
  if (classification.status != SkillItemUseStatus::accepted) {
    return classification;
  }
  const auto progress = detail::read_locked_world_progress(transaction, account_id,
                                                            character_id, world_id);
  if (progress != plan.before) {
    return {};
  }
  if (!detail::apply_postgres_loot_trade(transaction, account_id, character_id, world_id,
                                          plan.before, plan.after, ItemTrade{})) {
    return {};
  }
  receipt.state = SkillItemUseState::committed;
  SkillItemUseResult result{SkillItemUseStatus::accepted, receipt};
  transaction.exec("UPDATE emulator_skill_item_use_receipts SET state='committed' "
                   "WHERE character_id=$1", pqxx::params{character_id});
  transaction.commit();
  return result;
}

std::optional<SkillItemUseReceipt> PostgresCharacterStateRepository::lookup_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  validate_owner(account_id, character_id, world_id);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!postgres_lock_owner(transaction, account_id, character_id, world_id)) {
    return std::nullopt;
  }
  const auto rows = postgres_receipt(transaction, character_id, false);
  if (rows.empty()) {
    return std::nullopt;
  }
  return read_receipt(rows.front());
}

bool PostgresCharacterStateRepository::finish_skill_item_use(
    std::int64_t account_id, std::int64_t character_id, int world_id, std::int64_t sequence,
    const std::string &event_id, bool committed) {
  validate_owner(account_id, character_id, world_id);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  if (!postgres_lock_owner(transaction, account_id, character_id, world_id)) {
    return false;
  }
  const auto rows = postgres_receipt(transaction, character_id, true);
  if (rows.empty()) {
    return false;
  }
  const auto receipt = read_receipt(rows.front());
  if (receipt.sequence != sequence || receipt.event_id != event_id) {
    return false;
  }
  const auto target = committed ? SkillItemUseState::completed : SkillItemUseState::aborted;
  const auto expected = committed ? SkillItemUseState::committed : SkillItemUseState::reserved;
  if (receipt.state == target) {
    return true;
  }
  if (receipt.state != expected) {
    return false;
  }
  transaction.exec("UPDATE emulator_skill_item_use_receipts SET state=$2 WHERE character_id=$1",
                   pqxx::params{character_id, state_name(target)});
  transaction.commit();
  return true;
}

} // namespace chronicles
