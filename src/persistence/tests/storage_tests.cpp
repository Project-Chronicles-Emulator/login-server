// Memory and PostgreSQL persistence tests (Storage suite). Moved verbatim from the
// former aggregated src/testing/tests.cpp (phase 1b-2 Task 6).
#include "apps/startup_reward_recovery.hpp"
#include "data/starter_gear.hpp"
#include "persistence/character_repositories.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres/character_rows.hpp"
#include "persistence/postgres_repositories.hpp"
#include "persistence/quest_service_effects.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>
#include <type_traits>

using namespace chronicles;

namespace {

TEST(Storage, QuestMutationFingerprintIncludesEveryProgressField) {
  const WorldProgress baseline;
  const ItemTrade trade;
  const QuestServiceEffects effects;
  const auto expected = quest_mutation_fingerprint(baseline, baseline, trade, effects);
  ASSERT_EQ(expected.size(), 64U);
  const std::vector<std::function<void(WorldProgress &)>> changes{
      [](auto &p) { ++p.revision; },
      [](auto &p) { p.dead = true; },
      [](auto &p) { ++p.gold; },
      [](auto &p) { ++p.hersander_token; },
      [](auto &p) { ++p.experience; },
      [](auto &p) { ++p.level; },
      [](auto &p) { ++p.level_up_points; },
      [](auto &p) { ++p.weapon_points; },
      [](auto &p) { ++p.nui_blessing_level; },
      [](auto &p) { ++p.dedicated_nui_tears; },
      [](auto &p) { ++p.build.active_weapon_slot; },
      [](auto &p) { p.build.stat_bonuses[1] = 1; },
      [](auto &p) { p.build.perks[1] = 1; },
      [](auto &p) { p.stackables[1] = 1; },
      [](auto &p) { p.adventure_objects[1] = 1; },
      [](auto &p) { p.quests[1].complete = true; },
      [](auto &p) { p.quests[1].success_count = 1; },
      [](auto &p) { p.quests[1].objectives[1] = 1; },
      [](auto &p) { p.quests[1].credited_kills.insert(1); },
      [](auto &p) { p.quests[1].attempt = 1; },
      [](auto &p) { p.quests[1].abandoned = true; },
      [](auto &p) { p.quests[1].started_epoch_ms = 1; },
      [](auto &p) { p.doodad_phases[1] = 1; },
      [](auto &p) { p.utility_slots[0].item_cms_id = 1; },
      [](auto &p) { p.utility_slots[0].count = 1; },
      [](auto &p) { p.shop_purchases[{1, 2}].purchased_count = 1; },
      [](auto &p) { p.shop_purchases[{1, 2}].reset_epoch_ticks = 1; },
      [](auto &p) { p.housing_items[1].quantity = 1; },
      [](auto &p) { p.housing_items[1].installed_count = 1; },
      [](auto &p) { p.trade_vouchers[1].good_cms_id = 1; },
      [](auto &p) { p.trade_vouchers[1].created_epoch_ticks = 1; },
      [](auto &p) { p.trade_vouchers[1].expiration_epoch_ticks = 1; },
      [](auto &p) { p.trade_vouchers[1].redeemed = true; },
      [](auto &p) { p.registered_summons.insert(1); },
      [](auto &p) { p.mates[1].state = 2; },
      [](auto &p) { p.mates[1].housing_lot_number = 1; },
      [](auto &p) { p.mates[1].intimacy = 1; },
      [](auto &p) { p.trade_run.phase = TradeRunPhase::running; },
      [](auto &p) { p.trade_run.start_post_id = 1; },
      [](auto &p) { p.trade_run.parked_post_id = 1; },
      [](auto &p) { p.trade_run.vehicle_cms_id = 1; },
      [](auto &p) { p.trade_run.started_epoch_ticks = 1; },
      [](auto &p) { p.trade_run.goods.push_back({1, 2, 3}); },
      [](auto &p) { p.trade_run.wagon_position = WorldPosition{1, 2, 3, 4, 5}; },
      [](auto &p) { p.trade_run.wagon_health = 1; },
      [](auto &p) { p.trade_run.last_wagon_spawn_epoch_ticks = 1; },
      [](auto &p) { p.loot_first_kills.insert(1); },
      [](auto &p) { p.owned_emotions.insert(1); },
      [](auto &p) { p.learned_crafts.insert(1); },
      [](auto &p) { p.achievements[1].state = 1; },
      [](auto &p) { p.achievements[1].completion_tick = 1; },
      [](auto &p) { p.job_boards[1].day = 1; },
      [](auto &p) { p.job_boards[1].refreshes = 1; },
      [](auto &p) { p.play_records[1] = 1; },
  };
  for (std::size_t index = 0; index < changes.size(); ++index) {
    auto changed = baseline;
    changes[index](changed);
    EXPECT_NE(quest_mutation_fingerprint(changed, baseline, trade, effects), expected) << index;
    EXPECT_NE(quest_mutation_fingerprint(baseline, changed, trade, effects), expected) << index;
  }
  EXPECT_NE(quest_mutation_fingerprint(baseline, std::nullopt, trade, effects), expected);
}

TEST(Storage, QuestMutationFingerprintIncludesItemTradeAndServiceEffects) {
  const WorldProgress progress;
  const ItemTrade baseline_trade;
  const QuestServiceEffects baseline_effects;
  const auto expected = quest_mutation_fingerprint(progress, progress, baseline_trade,
                                                    baseline_effects);
  std::vector<ItemTrade> trades(3);
  trades[0].granted_items.push_back(CharacterItem{});
  trades[1].removed_item_ids.push_back(1);
  trades[2].durability_updates.push_back({1, 2, 3});
  for (const auto &trade : trades) {
    EXPECT_NE(quest_mutation_fingerprint(progress, progress, trade, baseline_effects), expected);
  }
  std::vector<QuestServiceEffects> effects(7);
  effects[0].company_unlocks.push_back({1, 2});
  effects[1].company_contribution = 1;
  effects[2].guild_experience = 1;
  effects[3].expected_company_type = 1;
  effects[4].expected_guild_id = 1;
  effects[5].expected_company_generation = 1;
  effects[6].expected_guild_generation = 1;
  for (const auto &effect : effects) {
    EXPECT_NE(quest_mutation_fingerprint(progress, progress, baseline_trade, effect), expected);
  }

  ItemTrade item_trade;
  item_trade.granted_items.emplace_back();
  const auto item_baseline = quest_mutation_fingerprint(progress, progress, item_trade,
                                                         baseline_effects);
  const std::vector<std::function<void(CharacterItem &)>> item_changes{
      [](auto &item) { item.id = 1; },
      [](auto &item) { item.cms_id = 1; },
      [](auto &item) { item.inventory_slot = 1; },
      [](auto &item) { item.equip_slot = 1; },
      [](auto &item) { item.quantity = 2; },
      [](auto &item) { item.durability = 1; },
      [](auto &item) { item.grade = 1; },
      [](auto &item) { item.polish_step = 1; },
      [](auto &item) { item.created_epoch_ticks = 1; },
  };
  for (std::size_t index = 0; index < item_changes.size(); ++index) {
    auto changed = item_trade;
    item_changes[index](changed.granted_items.front());
    EXPECT_NE(quest_mutation_fingerprint(progress, progress, changed, baseline_effects),
              item_baseline) << index;
  }

  WorldProgress wagon;
  wagon.trade_run.wagon_position = WorldPosition{1, 2, 3, 4, 5};
  wagon.trade_run.goods.push_back({1, 2, 3});
  const auto wagon_baseline = quest_mutation_fingerprint(wagon, wagon, baseline_trade,
                                                          baseline_effects);
  const std::vector<std::function<void(WorldProgress &)>> wagon_changes{
      [](auto &p) { ++p.trade_run.wagon_position->map_id; },
      [](auto &p) { ++p.trade_run.wagon_position->x_mm; },
      [](auto &p) { ++p.trade_run.wagon_position->y_mm; },
      [](auto &p) { ++p.trade_run.wagon_position->z_mm; },
      [](auto &p) { ++p.trade_run.wagon_position->yaw; },
      [](auto &p) { ++p.trade_run.goods.front().good_cms_id; },
      [](auto &p) { ++p.trade_run.goods.front().origin_post_id; },
      [](auto &p) { ++p.trade_run.goods.front().production_epoch_ticks; },
  };
  for (std::size_t index = 0; index < wagon_changes.size(); ++index) {
    auto changed = wagon;
    wagon_changes[index](changed);
    EXPECT_NE(quest_mutation_fingerprint(wagon, changed, baseline_trade, baseline_effects),
              wagon_baseline) << index;
  }
}

TEST(Storage, MemoryQuestMutationAtomicallyStoresEffectsAndReplaysOriginalItemRows) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 12;
  character.name = "QuestEffectOwner";
  character = repositories.characters->create(character);
  const auto initial_item_count = character.items.size();
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    repositories.database->quest_services->company_memberships[character.id] = 2;
    repositories.database->quest_services->guild_memberships[character.id] = 5;
  }
  const auto before = state.load_world_progress(character.account_id, character.id, 1);
  auto after = before;
  ++after.revision;
  after.gold += 10;
  CharacterItem item;
  item.cms_id = 100;
  item.inventory_slot = 10;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  QuestServiceEffects effects;
  effects.expected_company_type = 2;
  effects.expected_guild_id = 5;
  effects.company_contribution = 3;
  effects.guild_experience = 4;
  const std::string event_id(32, 'a');

  const auto committed = state.commit_quest_mutation(character.account_id, character.id, 1,
                                                      event_id, before, after, trade, effects);
  ASSERT_EQ(committed.status, QuestMutationStatus::committed);
  ASSERT_EQ(committed.granted_items.size(), 1U);
  EXPECT_GT(committed.granted_items.front().id, 0);
  EXPECT_EQ(state.load_world_progress(character.account_id, character.id, 1), after);
  const auto replay = state.commit_quest_mutation(character.account_id, character.id, 1,
                                                   event_id, before, after, trade, effects);
  EXPECT_EQ(replay.status, QuestMutationStatus::already_processed);
  EXPECT_EQ(replay.granted_items, committed.granted_items);
  auto changed_effects = effects;
  ++changed_effects.guild_experience;
  EXPECT_EQ(state.commit_quest_mutation(character.account_id, character.id, 1, event_id,
                                         before, after, trade, changed_effects).status,
            QuestMutationStatus::conflict);
  EXPECT_EQ(state.commit_quest_mutation(character.account_id, character.id, 1,
                                         std::string(32, 'b'), before, after, trade, effects).status,
            QuestMutationStatus::conflict);
  const auto characters = repositories.characters->characters(character.account_id, 1);
  ASSERT_EQ(characters.size(), 1U);
  EXPECT_EQ(characters.front().items.size(), initial_item_count + 1);
  EXPECT_EQ(std::ranges::count(characters.front().items, committed.granted_items.front()), 1);
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    const auto &events = repositories.database->quest_services->events;
    ASSERT_EQ(events.size(), 1U);
    EXPECT_FALSE(events.begin()->second.company_applied);
    EXPECT_FALSE(events.begin()->second.guild_applied);
  }
  EXPECT_FALSE(repositories.characters->delete_character(character.id));
  {
    auto &coordinator = *repositories.database->quest_services;
    std::lock_guard lock(coordinator.mutex);
    auto &event = coordinator.events.at({character.id, event_id});
    ASSERT_TRUE(coordinator.can_mark_component(event, QuestServiceComponent::company));
    coordinator.mark_component_applied(event, QuestServiceComponent::company);
    ASSERT_TRUE(coordinator.can_mark_component(event, QuestServiceComponent::guild));
    coordinator.mark_component_applied(event, QuestServiceComponent::guild);
  }
  EXPECT_TRUE(repositories.characters->delete_character(character.id));
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    EXPECT_EQ(repositories.database->quest_services->events.size(), 1U);
  }
}

TEST(Storage, MemoryQuestMembershipCaptureSurvivesDeleteAndRejectsReusedIdentity) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 12;
  character.name = "QuestIdentityOwner";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto captured = state.read_quest_service_membership(12, character.id, 1);
  ASSERT_TRUE(captured);
  EXPECT_EQ(captured->company_generation, 0);
  EXPECT_EQ(captured->guild_generation, 0);
  ASSERT_TRUE(repositories.characters->delete_character(character.id));
  {
    // Simulate a restored numeric ID. Ordinary character creation never reuses an ID.
    std::lock_guard lock(repositories.database->mutex);
    repositories.database->characters.push_back(character);
  }
  const auto restored = state.read_quest_service_membership(12, character.id, 1);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->company_generation, 1);
  EXPECT_EQ(restored->guild_generation, 1);
  const auto before = state.load_world_progress(12, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  QuestServiceEffects stale_effects;
  const auto result = state.commit_quest_mutation(12, character.id, 1, std::string(32, 'e'),
                                                   before, after, ItemTrade{}, stale_effects);
  EXPECT_EQ(result.status, QuestMutationStatus::conflict);
  EXPECT_EQ(state.load_world_progress(12, character.id, 1), before);
}

TEST(Storage, MemoryUnaffiliatedCommitRejectsMembershipChangesAndStaleRevision) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 42;
  character.name = "UnaffiliatedMemory";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto membership = state.read_quest_service_membership(42, character.id, 1);
  ASSERT_TRUE(membership);
  const auto before = state.load_world_progress(42, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.experience;

  {
    auto &coordinator = *repositories.database->quest_services;
    std::lock_guard lock(coordinator.mutex);
    coordinator.company_memberships[character.id] = 2;
    ++coordinator.company_generations[character.id];
  }
  EXPECT_FALSE(state.commit_unaffiliated_activity(42, character.id, 1, *membership,
                                                   before, after, {}).has_value());
  {
    auto &coordinator = *repositories.database->quest_services;
    std::lock_guard lock(coordinator.mutex);
    coordinator.company_memberships[character.id] = 0;
    ++coordinator.company_generations[character.id];
  }
  EXPECT_FALSE(state.commit_unaffiliated_activity(42, character.id, 1, *membership,
                                                   before, after, {}).has_value());
  const auto fresh = state.read_quest_service_membership(42, character.id, 1);
  ASSERT_TRUE(fresh);
  ASSERT_TRUE(state.commit_unaffiliated_activity(42, character.id, 1, *fresh,
                                                  before, after, {}).has_value());
  EXPECT_FALSE(state.commit_unaffiliated_activity(42, character.id, 1, *fresh,
                                                   before, after, {}).has_value());
  EXPECT_EQ(state.load_world_progress(42, character.id, 1).experience, after.experience);
  std::lock_guard lock(repositories.database->quest_services->mutex);
  EXPECT_TRUE(repositories.database->quest_services->events.empty());
}

TEST(Storage, MemoryQuestPendingCursorPreservesActorOrderAndFindsNewArrivals) {
  MemoryRepositories repositories;
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  auto &coordinator = *repositories.database->quest_services;
  const auto add = [&](std::int64_t character_id, int world_id, char digit) {
    QuestServiceEffectEvent event;
    event.account_id = 1;
    event.character_id = character_id;
    event.world_id = world_id;
    event.effect_id = std::string(32, digit);
    event.guild_applied = true;
    std::lock_guard lock(coordinator.mutex);
    EXPECT_TRUE(coordinator.record_event(std::move(event)));
  };
  add(9999, 10, 'a');
  add(9999, 9, 'b');
  for (int index = 0; index < 132; ++index) {
    add(10000 + index, 9, 'a');
  }
  add(10000, 9, 'b');
  const auto first = state.pending_quest_service_effects(9, {}, 128, 0);
  ASSERT_EQ(first.size(), 128U);
  EXPECT_EQ(first.front().character_id, 10000);
  const auto cursor = first.back().enqueue_sequence;
  add(20000, 9, 'a');
  const auto next = state.pending_quest_service_effects(9, {}, 128, cursor);
  ASSERT_EQ(next.size(), 5U);
  EXPECT_EQ(next.back().character_id, 20000);
  EXPECT_TRUE(state.pending_quest_service_effects(9, 10000, 1, cursor).empty());
  EXPECT_TRUE(state.pending_quest_service_effects(9, 9999, 1, 0).empty());
  EXPECT_EQ(state.pending_quest_service_effects(10, 9999, 1, 0).size(), 1U);
  EXPECT_TRUE(state.pending_quest_service_effects(9, {}, 128,
                                                   next.back().enqueue_sequence).empty());
  EXPECT_EQ(state.pending_quest_service_effects(9, {}, 1, 0).front().character_id, 10000);
  {
    std::lock_guard lock(coordinator.mutex);
    auto &event = coordinator.events.at({10000, std::string(32, 'a')});
    ASSERT_TRUE(coordinator.can_mark_component(event, QuestServiceComponent::company));
    coordinator.mark_component_applied(event, QuestServiceComponent::company);
  }
  const auto unblocked = state.pending_quest_service_effects(9, 10000, 1, cursor);
  ASSERT_EQ(unblocked.size(), 1U);
  EXPECT_EQ(unblocked.front().effect_id, std::string(32, 'b'));
}

TEST(Storage, PostgresQuestPendingCursorPreservesActorOrderAndFindsNewArrivals) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  pqxx::connection connection(connection_string);
  const auto add = [&](std::int64_t character_id, int world_id, char digit) {
    pqxx::work transaction(connection);
    transaction.exec(
        "INSERT INTO emulator_quest_service_effects "
        "(character_id,account_id,world_id,effect_id,mutation_fingerprint,monster_receipt,"
        "effect_payload,expected_company_type,expected_guild_id,expected_company_generation,"
        "expected_guild_generation,granted_items,company_applied,guild_applied) "
        "VALUES ($1,1,$2,$3,$4,false,$5::jsonb,0,0,0,0,'[]',false,true)",
        pqxx::params{character_id, world_id, std::string(32, digit), std::string(64, 'f'),
                     encode_quest_service_effects({}).dump()});
    transaction.commit();
  };
  add(9999, 10, 'a');
  add(9999, 9, 'b');
  for (int index = 0; index < 132; ++index) {
    add(10000 + index, 9, 'a');
  }
  add(10000, 9, 'b');
  const auto first = state.pending_quest_service_effects(9, {}, 128, 0);
  ASSERT_EQ(first.size(), 128U);
  EXPECT_EQ(first.front().character_id, 10000);
  const auto cursor = first.back().enqueue_sequence;
  add(20000, 9, 'a');
  const auto next = state.pending_quest_service_effects(9, {}, 128, cursor);
  ASSERT_EQ(next.size(), 5U);
  EXPECT_EQ(next.back().character_id, 20000);
  EXPECT_TRUE(state.pending_quest_service_effects(9, 10000, 1, cursor).empty());
  EXPECT_TRUE(state.pending_quest_service_effects(9, 9999, 1, 0).empty());
  EXPECT_EQ(state.pending_quest_service_effects(10, 9999, 1, 0).size(), 1U);
  EXPECT_TRUE(state.pending_quest_service_effects(9, {}, 128,
                                                   next.back().enqueue_sequence).empty());
  EXPECT_EQ(state.pending_quest_service_effects(9, {}, 1, 0).front().character_id, 10000);
  {
    pqxx::work transaction(connection);
    transaction.exec("UPDATE emulator_quest_service_effects SET company_applied=true "
                     "WHERE character_id=10000 AND effect_id=$1", pqxx::params{std::string(32, 'a')});
    transaction.commit();
  }
  const auto unblocked = state.pending_quest_service_effects(9, 10000, 1, cursor);
  ASSERT_EQ(unblocked.size(), 1U);
  EXPECT_EQ(unblocked.front().effect_id, std::string(32, 'b'));
}

TEST(Storage, PostgresUnaffiliatedCommitSerializesWithJoinAndEntryFence) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  Account account;
  account.name = "unaffiliated_pg";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "UnaffiliatedPg";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.experience;

  pqxx::connection join_connection(connection_string);
  pqxx::work join(join_connection);
  join.exec("SELECT character_id FROM emulator_quest_service_character_guards "
            "WHERE character_id=$1 FOR UPDATE", pqxx::params{character.id});
  join.exec("UPDATE emulator_quest_service_character_guards SET company_generation="
            "company_generation+1 WHERE character_id=$1", pqxx::params{character.id});
  join.exec("INSERT INTO emulator_character_company(character_id,company_type) VALUES ($1,2)",
            pqxx::params{character.id});
  auto pending = std::async(std::launch::async, [&] {
    return state.commit_unaffiliated_activity(owner->id, character.id, 1, *membership,
                                               before, after, {});
  });
  EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  join.commit();
  EXPECT_FALSE(pending.get().has_value());
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1).experience, before.experience);

  {
    pqxx::work leave(join_connection);
    leave.exec("SELECT character_id FROM emulator_quest_service_character_guards "
               "WHERE character_id=$1 FOR UPDATE", pqxx::params{character.id});
    leave.exec("DELETE FROM emulator_character_company WHERE character_id=$1",
               pqxx::params{character.id});
    leave.exec("UPDATE emulator_quest_service_character_guards SET company_generation="
               "company_generation+1 WHERE character_id=$1", pqxx::params{character.id});
    leave.commit();
  }
  EXPECT_FALSE(state.commit_unaffiliated_activity(owner->id, character.id, 1, *membership,
                                                   before, after, {}).has_value());
  const auto fresh = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(fresh);
  ASSERT_TRUE(state.fence_world_entry(owner->id, character.id, 1));
  EXPECT_FALSE(state.commit_unaffiliated_activity(owner->id, character.id, 1, *fresh,
                                                   before, after, {}).has_value());
  const auto fenced = state.load_world_progress(owner->id, character.id, 1);
  EXPECT_EQ(fenced.revision, before.revision + 1);
  EXPECT_EQ(fenced.experience, before.experience);

  auto next = fenced;
  ++next.revision;
  ++next.experience;
  CharacterItem item;
  item.cms_id = 100;
  item.inventory_slot = 20;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  auto commit = std::async(std::launch::async, [&] {
    start.wait();
    return state.commit_unaffiliated_activity(owner->id, character.id, 1, *fresh,
                                                fenced, next, trade);
  });
  auto entry = std::async(std::launch::async, [&] {
    start.wait();
    return state.fence_world_entry(owner->id, character.id, 1);
  });
  start_promise.set_value();
  const auto committed_items = commit.get();
  ASSERT_TRUE(entry.get());
  const auto final_progress = state.load_world_progress(owner->id, character.id, 1);
  const auto final_character = repositories.characters->character_by_id(character.id);
  ASSERT_TRUE(final_character);
  if (committed_items) {
    ASSERT_EQ(committed_items->size(), 1U);
    EXPECT_EQ(final_progress.revision, fenced.revision + 2);
    EXPECT_EQ(final_progress.experience, next.experience);
    EXPECT_EQ(std::ranges::count(final_character->items, committed_items->front()), 1);
  } else {
    EXPECT_EQ(final_progress.revision, fenced.revision + 1);
    EXPECT_EQ(final_progress.experience, fenced.experience);
    EXPECT_EQ(std::ranges::count_if(final_character->items, [&](const CharacterItem &stored) {
      return stored.cms_id == item.cms_id;
    }), 0);
  }
}

TEST(Storage, PostgresQuestMutationReplayAndReusedIdentityAreFenced) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  Account account;
  account.name = "quest_effect_pg";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "QuestEffectPg";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto stale_capture = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(stale_capture);
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("INSERT INTO emulator_character_company(character_id,company_type) VALUES ($1,2)",
                     pqxx::params{character.id});
    transaction.commit();
  }
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  ASSERT_EQ(membership->company_type, 2);
  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;
  effects.company_contribution = 3;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  CharacterItem item;
  item.cms_id = 100;
  item.inventory_slot = 20;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  const std::string event_id(32, '9');
  const auto committed = state.commit_quest_mutation(owner->id, character.id, 1, event_id,
                                                      before, after, trade, effects);
  ASSERT_EQ(committed.status, QuestMutationStatus::committed);
  ASSERT_EQ(committed.granted_items.size(), 1U);
  const auto replay = state.commit_quest_mutation(owner->id, character.id, 1, event_id,
                                                   before, after, trade, effects);
  EXPECT_EQ(replay.status, QuestMutationStatus::already_processed);
  EXPECT_EQ(replay.granted_items, committed.granted_items);
  const auto pending = state.pending_quest_service_effects(1, character.id, 1);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front().effect_id, event_id);
  EXPECT_FALSE(repositories.characters->delete_character(character.id));
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("UPDATE emulator_quest_service_effects SET company_applied=true "
                     "WHERE character_id=$1 AND effect_id=$2", pqxx::params{character.id, event_id});
    transaction.commit();
  }
  ASSERT_TRUE(repositories.characters->delete_character(character.id));
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("INSERT INTO emulator_characters "
                     "(id,account_id,world_id,lobby_slot,name,class_id) OVERRIDING SYSTEM VALUE "
                     "VALUES ($1,$2,1,0,'QuestRestoredPg',1)",
                     pqxx::params{character.id, owner->id});
    transaction.commit();
  }
  const auto restored = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(restored);
  EXPECT_GT(restored->company_generation, stale_capture->company_generation);
  EXPECT_GT(restored->guild_generation, stale_capture->guild_generation);
  const auto restored_before = state.load_world_progress(owner->id, character.id, 1);
  auto restored_after = restored_before;
  ++restored_after.revision;
  ++restored_after.gold;
  QuestServiceEffects stale_effects;
  stale_effects.expected_company_type = stale_capture->company_type;
  stale_effects.expected_guild_id = stale_capture->guild_id;
  stale_effects.expected_company_generation = stale_capture->company_generation;
  stale_effects.expected_guild_generation = stale_capture->guild_generation;
  EXPECT_EQ(state.commit_quest_mutation(owner->id, character.id, 1, std::string(32, '8'),
                                         restored_before, restored_after, ItemTrade{}, stale_effects).status,
            QuestMutationStatus::conflict);
}

void exercise_loot_claim_receipts(const CharacterRepositories &repositories,
                                   const std::string &suffix) {
  Account first_account;
  first_account.name = "loot_receipt_a_" + suffix;
  first_account.salt = "test-salt";
  first_account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(first_account));
  const auto first_owner = repositories.accounts->account(first_account.name);
  ASSERT_TRUE(first_owner);
  Account second_account;
  second_account.name = "loot_receipt_b_" + suffix;
  second_account.salt = "test-salt";
  second_account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(second_account));
  const auto second_owner = repositories.accounts->account(second_account.name);
  ASSERT_TRUE(second_owner);
  Character first;
  first.account_id = first_owner->id;
  first.name = "LootReceiptA" + suffix;
  first = repositories.characters->create(first);
  Character second;
  second.account_id = second_owner->id;
  second.name = "LootReceiptB" + suffix;
  second = repositories.characters->create(second);

  const std::string receipt(32, 'a');
  const std::string fingerprint(64, 'b');
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(first.account_id, first.id, 1);
  auto after = before;
  ++after.revision;
  after.gold += 7;
  ItemTrade trade;
  CharacterItem item;
  item.cms_id = 100;
  item.inventory_slot = 20;
  trade.granted_items.push_back(item);
  EXPECT_EQ(state.lookup_loot_claim(first.account_id, first.id, 1, receipt, fingerprint).status,
            LootClaimStatus::not_found);
  const auto committed = state.commit_loot_claim(first.account_id, first.id, 1, receipt,
                                                  fingerprint, before, after, trade);
  ASSERT_EQ(committed.status, LootClaimStatus::committed);
  ASSERT_EQ(committed.granted_items.size(), 1U);
  const auto replay = state.commit_loot_claim(first.account_id, first.id, 1, receipt,
                                               fingerprint, before, after, trade);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  EXPECT_EQ(replay.granted_items, committed.granted_items);
  EXPECT_EQ(state.load_world_progress(first.account_id, first.id, 1), after);
  EXPECT_EQ(state.lookup_loot_claim(second.account_id, second.id, 1, receipt, fingerprint).status,
            LootClaimStatus::claimed_by_other_character);
  EXPECT_EQ(state.lookup_loot_claim(first.account_id, first.id, 1, receipt,
                                   std::string(64, 'c')).status, LootClaimStatus::mismatch);
  const auto foreign = state.lookup_loot_claim(second.account_id, first.id, 1, receipt, fingerprint);
  EXPECT_EQ(foreign.status, LootClaimStatus::conflict);
  EXPECT_TRUE(foreign.granted_items.empty());
  const auto wrong_world = state.lookup_loot_claim(first.account_id, first.id, 2, receipt, fingerprint);
  EXPECT_EQ(wrong_world.status, LootClaimStatus::conflict);
  EXPECT_TRUE(wrong_world.granted_items.empty());

  const std::string stale_receipt(32, 'd');
  EXPECT_EQ(state.commit_loot_claim(first.account_id, first.id, 1, stale_receipt, fingerprint,
                                     before, after, trade).status, LootClaimStatus::conflict);
  EXPECT_EQ(state.lookup_loot_claim(first.account_id, first.id, 1, stale_receipt, fingerprint).status,
            LootClaimStatus::not_found);
  ASSERT_TRUE(repositories.characters->delete_character(first.id));
  const auto deleted = state.lookup_loot_claim(first.account_id, first.id, 1, receipt, fingerprint);
  EXPECT_EQ(deleted.status, LootClaimStatus::conflict);
  EXPECT_TRUE(deleted.granted_items.empty());
  EXPECT_EQ(state.lookup_loot_claim(second.account_id, second.id, 1, receipt, fingerprint).status,
            LootClaimStatus::claimed_by_other_character);
}

TEST(Storage, MemoryLootClaimReceiptsAreAtomicAndOwnerBound) {
  MemoryRepositories repositories;
  exercise_loot_claim_receipts(repositories.character_repositories(), "memory");
}

void exercise_skill_item_use_receipt(
    CharacterRepositories first, CharacterRepositories second,
    const std::function<void(const Character &, bool)> &tamper_stack) {
  Account account;
  account.name = "skill_item_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(first.accounts->add_account(account));
  const auto owner = first.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "SkillItem" + std::to_string(owner->id);
  character = first.characters->create(character);
  auto &old_state = *first.character_state;
  auto &new_state = *second.character_state;
  const auto old_epoch = old_state.acquire_monster_reward_owner_epoch();
  const auto initial = old_state.load_world_progress(owner->id, character.id, 1);
  auto seeded = initial;
  ++seeded.revision;
  seeded.stackables[901] = 2;
  seeded.utility_slots[0] = {901, 2};
  ASSERT_TRUE(old_state.save_world_progress(owner->id, character.id, 1, initial, seeded));
  const auto plan = prepare_skill_item_use("0123456789abcdef", 901, seeded);
  for (int field = 0; field < 3; ++field) {
    auto incorrect = seeded;
    if (field == 0) {
      ++incorrect.stackables[901];
    } else if (field == 1) {
      --incorrect.utility_slots[0].count;
    } else {
      ++incorrect.gold;
    }
    const auto bad = prepare_skill_item_use("aaaaaaaaaaaaaaaa", 901, incorrect);
    EXPECT_EQ(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, bad).status,
              SkillItemUseStatus::conflict);
    EXPECT_FALSE(old_state.lookup_skill_item_use(owner->id, character.id, 1));
    EXPECT_EQ(old_state.load_world_progress(owner->id, character.id, 1), seeded);
  }
  const auto reserved = old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan);
  ASSERT_EQ(reserved.status, SkillItemUseStatus::accepted);
  ASSERT_TRUE(reserved.receipt);
  EXPECT_EQ(reserved.receipt->sequence, 1);
  EXPECT_EQ(reserved.receipt->event_id, "00000000000000010123456789abcdef");
  EXPECT_EQ(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan).status,
            SkillItemUseStatus::replay);
  tamper_stack(character, true);
  EXPECT_EQ(old_state.commit_skill_item_use(owner->id, character.id, 1, 1, plan).status,
            SkillItemUseStatus::conflict);
  EXPECT_EQ(old_state.lookup_skill_item_use(owner->id, character.id, 1)->state,
            SkillItemUseState::reserved);
  tamper_stack(character, false);
  auto altered_before = seeded;
  ++altered_before.gold;
  const auto altered = prepare_skill_item_use(plan.nonce, 901, altered_before);
  EXPECT_EQ(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, altered).status,
            SkillItemUseStatus::mismatch);
  const auto other = prepare_skill_item_use("1111111111111111", 901, seeded);
  EXPECT_EQ(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, other).status,
            SkillItemUseStatus::conflict);
  EXPECT_FALSE(first.characters->delete_character(character.id));

  const auto new_epoch = new_state.acquire_monster_reward_owner_epoch();
  ASSERT_GT(new_epoch, old_epoch);
  EXPECT_THROW(old_state.commit_skill_item_use(owner->id, character.id, 1, 1, plan),
               std::runtime_error);
  EXPECT_THROW(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan),
               std::runtime_error);
  EXPECT_THROW(old_state.lookup_skill_item_use(owner->id, character.id, 1),
               std::runtime_error);
  EXPECT_THROW(old_state.finish_skill_item_use(owner->id, character.id, 1, 1,
                                              reserved.receipt->event_id, false),
               std::runtime_error);
  const auto pending = new_state.lookup_skill_item_use(owner->id, character.id, 1);
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->event_id, reserved.receipt->event_id);
  EXPECT_EQ(pending->state, SkillItemUseState::reserved);
  old_state.adopt_monster_reward_owner_epoch(new_epoch);
  const auto recovered = old_state.lookup_skill_item_use(owner->id, character.id, 1);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->event_id, pending->event_id);
  EXPECT_EQ(old_state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan).status,
            SkillItemUseStatus::replay);

  auto first_commit = std::async(std::launch::async, [&] {
    return new_state.commit_skill_item_use(owner->id, character.id, 1, 1, plan);
  });
  auto second_commit = std::async(std::launch::async, [&] {
    return new_state.commit_skill_item_use(owner->id, character.id, 1, 1, plan);
  });
  const auto a = first_commit.get();
  const auto b = second_commit.get();
  EXPECT_EQ(static_cast<int>(a.status == SkillItemUseStatus::accepted) +
                static_cast<int>(b.status == SkillItemUseStatus::accepted), 1);
  EXPECT_EQ(static_cast<int>(a.status == SkillItemUseStatus::replay) +
                static_cast<int>(b.status == SkillItemUseStatus::replay), 1);
  const auto committed = new_state.lookup_skill_item_use(owner->id, character.id, 1);
  ASSERT_TRUE(committed);
  EXPECT_EQ(committed->state, SkillItemUseState::committed);
  EXPECT_EQ(committed->response_json, plan.response_json);
  EXPECT_FALSE(first.characters->delete_character(character.id));
  EXPECT_EQ(new_state.load_world_progress(owner->id, character.id, 1), plan.after);
  EXPECT_FALSE(new_state.finish_skill_item_use(owner->id, character.id, 1, 1,
                                               committed->event_id, false));
  EXPECT_TRUE(new_state.finish_skill_item_use(owner->id, character.id, 1, 1,
                                              committed->event_id, true));
  EXPECT_TRUE(new_state.finish_skill_item_use(owner->id, character.id, 1, 1,
                                              committed->event_id, true));
  const auto next = prepare_skill_item_use("fedcba9876543210", 901, plan.after);
  const auto competing = prepare_skill_item_use("2222222222222222", 901, plan.after);
  auto first_reservation = std::async(std::launch::async, [&] {
    return old_state.reserve_skill_item_use(owner->id, character.id, 1, 2, next);
  });
  auto second_reservation = std::async(std::launch::async, [&] {
    return new_state.reserve_skill_item_use(owner->id, character.id, 1, 2, competing);
  });
  const auto reserved_a = first_reservation.get();
  const auto reserved_b = second_reservation.get();
  EXPECT_EQ(static_cast<int>(reserved_a.status == SkillItemUseStatus::accepted) +
                static_cast<int>(reserved_b.status == SkillItemUseStatus::accepted), 1);
  EXPECT_EQ(static_cast<int>(reserved_a.status == SkillItemUseStatus::conflict) +
                static_cast<int>(reserved_b.status == SkillItemUseStatus::conflict), 1);
  const auto &next_reserved = reserved_a.receipt ? reserved_a : reserved_b;
  ASSERT_TRUE(next_reserved.receipt);
  EXPECT_EQ(next_reserved.receipt->sequence, 2);
  EXPECT_EQ(new_state.commit_skill_item_use(owner->id, character.id, 1, 1, plan).status,
            SkillItemUseStatus::conflict);
  EXPECT_EQ(new_state.load_world_progress(owner->id, character.id, 1), plan.after);
  EXPECT_TRUE(new_state.finish_skill_item_use(owner->id, character.id, 1, 2,
                                              next_reserved.receipt->event_id, false));
  const auto action_a = prepare_skill_item_use("aaaaaaaaaaaaaaaa", 901, plan.after);
  const auto action_b = prepare_skill_item_use("bbbbbbbbbbbbbbbb", 901, plan.after);
  EXPECT_EQ(new_state.reserve_skill_item_use(owner->id, character.id, 1, 4, action_a).status,
            SkillItemUseStatus::conflict);
  const auto first_abort = new_state.reserve_skill_item_use(owner->id, character.id, 1, 3,
                                                            action_a);
  ASSERT_EQ(first_abort.status, SkillItemUseStatus::accepted);
  ASSERT_TRUE(first_abort.receipt);
  EXPECT_TRUE(new_state.finish_skill_item_use(owner->id, character.id, 1, 3,
                                              first_abort.receipt->event_id, false));
  const auto second_abort = new_state.reserve_skill_item_use(owner->id, character.id, 1, 4,
                                                             action_b);
  ASSERT_EQ(second_abort.status, SkillItemUseStatus::accepted);
  ASSERT_TRUE(second_abort.receipt);
  EXPECT_TRUE(new_state.finish_skill_item_use(owner->id, character.id, 1, 4,
                                              second_abort.receipt->event_id, false));
  EXPECT_EQ(new_state.reserve_skill_item_use(owner->id, character.id, 1, 3, action_a).status,
            SkillItemUseStatus::conflict);
  const auto terminal = new_state.lookup_skill_item_use(owner->id, character.id, 1);
  ASSERT_TRUE(terminal);
  EXPECT_EQ(terminal->sequence, 4);
  EXPECT_EQ(terminal->state, SkillItemUseState::aborted);
  EXPECT_TRUE(first.characters->delete_character(character.id));
}

TEST(Storage, MemorySkillItemUseReceiptFencesAndReplays) {
  MemoryRepositories repositories;
  exercise_skill_item_use_receipt(repositories.character_repositories(),
                                  make_memory_character_repositories(repositories.database),
                                  [&](const Character &character, bool tamper) {
    std::lock_guard lock(repositories.database->mutex);
    repositories.database->world_progress.at(character.id).stackables.at(901) = tamper ? 3 : 2;
  });
}

void exercise_combat_quest_event(CharacterRepositories first, CharacterRepositories second) {
  Account account;
  account.name = "combat_quest_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(first.accounts->add_account(account));
  const auto owner = first.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "CombatQuest" + std::to_string(owner->id);
  character = first.characters->create(character);

  auto &old_state = *first.character_state;
  auto &new_state = *second.character_state;
  const auto old_epoch = old_state.acquire_monster_reward_owner_epoch();
  const auto before = old_state.load_world_progress(owner->id, character.id, 1);
  const std::string no_op_id(32, 'a');
  const std::string event_id(32, 'b');
  const std::string source(64, 'c');
  const std::string different_source(64, 'd');
  const ItemTrade trade;
  const QuestServiceEffects effects;

  EXPECT_THROW(old_state.commit_combat_quest_event(0, character.id, 1, no_op_id, source,
                                                    before, std::nullopt, trade, effects),
               std::invalid_argument);
  EXPECT_THROW(old_state.commit_combat_quest_event(owner->id, 0, 1, no_op_id, source,
                                                    before, std::nullopt, trade, effects),
               std::invalid_argument);
  EXPECT_THROW(old_state.commit_combat_quest_event(owner->id, character.id, 0, no_op_id, source,
                                                    before, std::nullopt, trade, effects),
               std::invalid_argument);
  EXPECT_THROW(old_state.commit_combat_quest_event(owner->id, character.id, 1, no_op_id,
                                                    "invalid", before, std::nullopt, trade,
                                                    effects), std::invalid_argument);
  const auto no_op = old_state.commit_combat_quest_event(owner->id, character.id, 1, no_op_id,
                                                          source, before, std::nullopt, trade,
                                                          effects);
  EXPECT_EQ(no_op.status, QuestMutationStatus::committed);
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, no_op_id, source,
                                                before, std::nullopt, trade, effects).status,
            QuestMutationStatus::already_processed);
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, no_op_id,
                                                different_source, before, std::nullopt, trade,
                                                effects).status, QuestMutationStatus::conflict);
  EXPECT_EQ(old_state.commit_quest_mutation(owner->id, character.id, 1, no_op_id, before,
                                             std::nullopt, trade, effects).status,
            QuestMutationStatus::conflict);

  auto incorrect_before = before;
  ++incorrect_before.gold;
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, no_op_id, source,
                                                incorrect_before, std::nullopt, trade,
                                                effects).status, QuestMutationStatus::conflict);
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                incorrect_before, std::nullopt, trade,
                                                effects).status, QuestMutationStatus::conflict);
  auto incorrect_nested = before;
  incorrect_nested.stackables[901] = 1;
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                incorrect_nested, std::nullopt, trade,
                                                effects).status, QuestMutationStatus::conflict);
  auto after = before;
  ++after.revision;
  ++after.gold;
  EXPECT_EQ(old_state.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                before, after, trade, effects).status,
            QuestMutationStatus::committed);
  EXPECT_EQ(old_state.load_world_progress(owner->id, character.id, 1), after);
  const auto new_epoch = new_state.acquire_monster_reward_owner_epoch();
  ASSERT_GT(new_epoch, old_epoch);
  EXPECT_THROW(old_state.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                    before, after, trade, effects),
               std::runtime_error);
  EXPECT_EQ(new_state.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                before, after, trade, effects).status,
            QuestMutationStatus::already_processed);
  EXPECT_EQ(new_state.commit_combat_quest_event(owner->id, character.id, 1, event_id,
                                                different_source, before, after, trade,
                                                effects).status, QuestMutationStatus::conflict);
  EXPECT_EQ(new_state.load_world_progress(owner->id, character.id, 1), after);

  auto next = after;
  ++next.revision;
  ++next.gold;
  const std::string concurrent_id(32, 'f');
  auto first_commit = std::async(std::launch::async, [&] {
    return new_state.commit_combat_quest_event(owner->id, character.id, 1, concurrent_id,
                                                source, after, next, trade, effects);
  });
  auto second_commit = std::async(std::launch::async, [&] {
    return new_state.commit_combat_quest_event(owner->id, character.id, 1, concurrent_id,
                                                source, after, next, trade, effects);
  });
  const auto first_result = first_commit.get();
  const auto second_result = second_commit.get();
  EXPECT_EQ(static_cast<int>(first_result.status == QuestMutationStatus::committed) +
                static_cast<int>(second_result.status == QuestMutationStatus::committed), 1);
  EXPECT_EQ(static_cast<int>(first_result.status == QuestMutationStatus::already_processed) +
                static_cast<int>(second_result.status == QuestMutationStatus::already_processed),
            1);
  EXPECT_EQ(new_state.load_world_progress(owner->id, character.id, 1), next);
}

TEST(Storage, MemoryCombatQuestEventReceiptFencesAndReplays) {
  MemoryRepositories repositories;
  exercise_combat_quest_event(repositories.character_repositories(),
                              make_memory_character_repositories(repositories.database));
}

TEST(Storage, PostgresCombatQuestEventReceiptFencesAndReplays) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_combat_quest_event(make_postgres_character_repositories(connection),
                              make_postgres_character_repositories(connection));
}

TEST(Storage, PostgresCombatQuestEventLostAcknowledgement) {
  const auto *direct = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const auto *proxy = std::getenv("CHRONICLES_COMBAT_QUEST_PROXY_URL");
  if (!direct || !proxy) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL commit-acknowledgement proxy";
  }

  auto repositories = make_postgres_character_repositories(direct);
  Account account;
  account.name = "combat_quest_fault";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "CombatQuestFault";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto epoch = state.acquire_monster_reward_owner_epoch();
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  const std::string event_id(32, 'e');
  const std::string source(64, 'f');
  const ItemTrade trade;
  const QuestServiceEffects effects;

  PostgresCharacterStateRepository faulted(proxy);
  faulted.adopt_monster_reward_owner_epoch(epoch);
  EXPECT_THROW(faulted.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                  before, after, trade, effects), std::exception);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);

  PostgresCharacterStateRepository recovered(direct);
  recovered.acquire_monster_reward_owner_epoch();
  EXPECT_EQ(recovered.commit_combat_quest_event(owner->id, character.id, 1, event_id, source,
                                                 before, after, trade, effects).status,
            QuestMutationStatus::already_processed);
  EXPECT_EQ(recovered.commit_combat_quest_event(owner->id, character.id, 1, event_id,
                                                 std::string(64, 'a'), before, after, trade,
                                                 effects).status, QuestMutationStatus::conflict);
  EXPECT_EQ(recovered.load_world_progress(owner->id, character.id, 1), after);
}

TEST(Storage, PostgresSkillItemUseReceiptFencesAndReplays) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_skill_item_use_receipt(make_postgres_character_repositories(connection),
                                  make_postgres_character_repositories(connection),
                                  [&](const Character &character, bool tamper) {
    pqxx::connection database(connection);
    pqxx::work transaction(database);
    transaction.exec("UPDATE emulator_character_stackables SET quantity=$2 "
                     "WHERE character_id=$1 AND item_cms_id=901",
                     pqxx::params{character.id, tamper ? 3 : 2});
    transaction.commit();
  });
}

TEST(Storage, PostgresSkillItemUseLostAcknowledgementStage) {
  const auto *direct = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const auto *proxy = std::getenv("CHRONICLES_SKILL_ITEM_PROXY_URL");
  const auto *stage = std::getenv("CHRONICLES_SKILL_ITEM_FAULT_STAGE");
  if (!direct || !proxy || !stage) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL commit-acknowledgement proxy";
  }
  const std::string phase(stage);
  ASSERT_TRUE(phase == "reserve" || phase == "commit");
  auto repositories = make_postgres_character_repositories(direct);
  Account account;
  account.name = "skill_fault_" + phase;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "SkillFault" + phase;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto epoch = state.acquire_monster_reward_owner_epoch();
  const auto before_seed = state.load_world_progress(owner->id, character.id, 1);
  auto seeded = before_seed;
  ++seeded.revision;
  seeded.stackables[901] = 2;
  ASSERT_TRUE(state.save_world_progress(owner->id, character.id, 1, before_seed, seeded));
  const auto plan = prepare_skill_item_use("1234567890abcdef", 901, seeded);
  if (phase == "commit") {
    ASSERT_EQ(state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan).status,
              SkillItemUseStatus::accepted);
  }

  PostgresCharacterStateRepository faulted(proxy);
  faulted.adopt_monster_reward_owner_epoch(epoch);
  if (phase == "reserve") {
    EXPECT_THROW(faulted.reserve_skill_item_use(owner->id, character.id, 1, 1, plan), std::exception);
  } else {
    EXPECT_THROW(faulted.commit_skill_item_use(owner->id, character.id, 1, 1, plan),
                 std::exception);
  }
  const auto receipt = state.lookup_skill_item_use(owner->id, character.id, 1);
  ASSERT_TRUE(receipt);
  if (phase == "reserve") {
    EXPECT_EQ(state.reserve_skill_item_use(owner->id, character.id, 1, 1, plan).status,
              SkillItemUseStatus::replay);
  } else {
    EXPECT_EQ(state.commit_skill_item_use(owner->id, character.id, 1, 1, plan).status,
              SkillItemUseStatus::replay);
  }
  EXPECT_EQ(receipt->sequence, 1);
  EXPECT_EQ(receipt->event_id, "00000000000000011234567890abcdef");
  EXPECT_EQ(receipt->state, phase == "reserve" ? SkillItemUseState::reserved
                                                 : SkillItemUseState::committed);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1),
            phase == "reserve" ? seeded : plan.after);
}

TEST(Storage, PostgresSkillItemUseRestartLookup) {
  const auto *direct = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!direct || !std::getenv("CHRONICLES_SKILL_ITEM_RESTART")) {
    GTEST_SKIP() << "Requires the separate-process skill item recovery harness";
  }
  auto repositories = make_postgres_character_repositories(direct);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  for (const std::string phase : {"reserve", "commit"}) {
    const auto owner = repositories.accounts->account("skill_fault_" + phase);
    ASSERT_TRUE(owner);
    const auto characters = repositories.characters->characters(owner->id, 1);
    ASSERT_EQ(characters.size(), 1U);
    const auto receipt = state.lookup_skill_item_use(owner->id, characters.front().id, 1);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->event_id, "00000000000000011234567890abcdef");
    EXPECT_EQ(receipt->state, phase == "reserve" ? SkillItemUseState::reserved
                                                   : SkillItemUseState::committed);
    EXPECT_EQ(receipt->remaining, 1);
    const auto progress = state.load_world_progress(owner->id, characters.front().id, 1);
    EXPECT_EQ(progress.stackables.at(901), phase == "reserve" ? 2 : 1);
  }
}

TEST(Storage, PostgresLootClaimReceiptsAreAtomicAndOwnerBound) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_loot_claim_receipts(make_postgres_character_repositories(connection), "postgres");
}

TEST(Storage, GatheringMutationFingerprintBindsEverySourceTransitionField) {
  const WorldProgress before;
  auto after = before;
  ++after.revision;
  const ItemTrade trade;
  const QuestServiceEffects effects;
  SharedGatheringSourceTransition source;
  source.map_id = 10117;
  source.kind = 1;
  source.sector_id = 5;
  source.local_index = 7;
  source.expected_active = true;
  source.expected_phase = 1;
  source.resulting_active = true;
  source.resulting_phase = 2;
  source.next_transition_delay_ticks = 3000;
  source.timed_successor_phase = 1;
  const std::string identity(64, 'a');
  const auto expected = gathering_mutation_fingerprint(identity, before, after, trade, effects,
                                                         source);
  const std::vector<std::function<void(SharedGatheringSourceTransition &)>> changes{
      [](auto &value) { ++value.map_id; },
      [](auto &value) { value.kind = 0; },
      [](auto &value) { ++value.sector_id; },
      [](auto &value) { ++value.local_index; },
      [](auto &value) { ++value.expected_generation; },
      [](auto &value) { value.expected_active = false; },
      [](auto &value) { ++*value.expected_phase; },
      [](auto &value) { value.resulting_active = false; },
      [](auto &value) { ++*value.resulting_phase; },
      [](auto &value) { ++*value.next_transition_delay_ticks; },
      [](auto &value) { ++*value.timed_successor_phase; },
  };
  for (std::size_t index = 0; index < changes.size(); ++index) {
    auto changed = source;
    changes[index](changed);
    EXPECT_NE(gathering_mutation_fingerprint(identity, before, after, trade, effects, changed),
              expected) << index;
  }
  EXPECT_NE(gathering_mutation_fingerprint(identity, before, after, trade, effects, std::nullopt),
            expected);
  EXPECT_NE(gathering_mutation_fingerprint(std::string(64, 'b'), before, after, trade, effects,
                                           source), expected);
}

void exercise_gathering_claim_recovery(const CharacterRepositories &repositories,
                                       const std::string &suffix,
                                       const std::function<void(std::int64_t)> &enroll = {}) {
  Account account;
  account.name = "gathering_claim_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Gather" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  if (enroll) {
    enroll(character.id);
  }
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);

  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;
  if (enroll) {
    effects.company_contribution = 3;
  }
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  CharacterItem item;
  item.cms_id = 100;
  item.inventory_slot = 20;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  SharedGatheringSourceTransition source;
  source.map_id = 10117;
  source.kind = 1;
  source.sector_id = 5;
  source.local_index = 7;
  source.expected_active = true;
  source.expected_phase = 1;
  source.resulting_active = true;
  source.resulting_phase = 2;
  source.next_transition_delay_ticks = 3000;
  source.timed_successor_phase = 1;
  const std::string receipt = enroll ? "ae0123456789abcdef0123456789abcd"
                                     : "ad0123456789abcdef0123456789abcd";
  if (enroll) {
    source.sector_id = 6;
  }
  const std::string fingerprint(64, 'c');

  const auto committed = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, source);
  ASSERT_EQ(committed.status, LootClaimStatus::committed);
  ASSERT_EQ(committed.granted_items.size(), 1U);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);
  const auto replay = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, source);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  EXPECT_EQ(replay.granted_items, committed.granted_items);
  if (enroll) {
    const auto pending = state.pending_quest_service_effects(1, character.id, 1);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().effect_id, receipt);
  }
  auto changed = after;
  ++changed.gold;
  EXPECT_EQ(state.commit_gathering_quest_claim(owner->id, character.id, 1, receipt, fingerprint,
                                                before, changed, trade, effects, source).status,
            LootClaimStatus::mismatch);
  const auto conflict = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, enroll ? "be0123456789abcdef0123456789abcd"
                                        : "bd0123456789abcdef0123456789abcd",
      fingerprint, before, after, trade,
      effects, source);
  ASSERT_EQ(conflict.status, LootClaimStatus::source_conflict);
  ASSERT_TRUE(conflict.source_snapshot);
  EXPECT_EQ(conflict.source_snapshot->pending_receipt_id, receipt);
  EXPECT_FALSE(conflict.source_snapshot->next_transition_epoch_ticks);
  const auto pending_fence = state.fence_shared_gathering_source(1, source);
  EXPECT_FALSE(pending_fence.committed);
  ASSERT_TRUE(pending_fence.source);
  EXPECT_EQ(pending_fence.source->pending_receipt_id, receipt);
  EXPECT_EQ(pending_fence.source->generation, 1);
  EXPECT_TRUE(state.has_unfinished_gathering_claims());

  // The source remains unavailable through a long commit or lost acknowledgement;
  // its cooldown starts only when the result is confirmed.
  constexpr std::int64_t confirmation_ticks = 100000;
  const auto finalized = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, source,
      confirmation_ticks);
  ASSERT_EQ(finalized.status, GatheringFinalizeStatus::finalized);
  EXPECT_EQ(finalized.transition_epoch_ticks, confirmation_ticks + 3000);
  ASSERT_TRUE(finalized.source_snapshot);
  EXPECT_EQ(finalized.source_snapshot->generation, 2);
  EXPECT_EQ(finalized.source_snapshot->phase, 2);
  EXPECT_FALSE(finalized.source_snapshot->pending_receipt_id);
  const auto finalized_again = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, source,
      confirmation_ticks + 9000);
  EXPECT_EQ(finalized_again.status, GatheringFinalizeStatus::already_finalized);
  EXPECT_EQ(finalized_again.transition_epoch_ticks, finalized.transition_epoch_ticks);
  ASSERT_TRUE(finalized_again.source_snapshot);
  EXPECT_EQ(finalized_again.source_snapshot->generation, 2);
  EXPECT_FALSE(state.has_unfinished_gathering_claims());

  auto timer = source;
  timer.expected_generation = 2;
  timer.expected_phase = 2;
  timer.resulting_phase = 1;
  timer.next_transition_delay_ticks.reset();
  timer.timed_successor_phase.reset();
  EXPECT_FALSE(state.update_shared_gathering_source(1, timer, confirmation_ticks + 2999, true)
                   .committed);
  EXPECT_TRUE(state.update_shared_gathering_source(1, timer, confirmation_ticks + 3000, true)
                  .committed);
  const auto stale = state.update_shared_gathering_source(1, timer, confirmation_ticks + 3001, true);
  EXPECT_FALSE(stale.committed);
  ASSERT_TRUE(stale.source);
  EXPECT_EQ(stale.source->generation, 3);
  EXPECT_EQ(stale.source->phase, 1);
  const auto replay_after_timer = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, source,
      confirmation_ticks + 4000);
  ASSERT_EQ(replay_after_timer.status, GatheringFinalizeStatus::already_finalized);
  ASSERT_TRUE(replay_after_timer.source_snapshot);
  EXPECT_EQ(replay_after_timer.source_snapshot->generation, 3);
  EXPECT_EQ(replay_after_timer.source_snapshot->phase, 1);
  const auto records = state.shared_gathering_sources(1, std::nullopt, 128);
  EXPECT_TRUE(std::ranges::any_of(records, [&](const auto &record) {
    return record.key.map_id == source.map_id && record.key.kind == source.kind &&
           record.key.sector_id == source.sector_id && record.key.local_index == source.local_index &&
           record.state.generation == 3;
  }));

  // Startup recovery confirms a commit whose process exited before it armed the cooldown.
  auto restart_source = source;
  ++restart_source.local_index;
  const auto restart_before = state.load_world_progress(owner->id, character.id, 1);
  auto restart_after = restart_before;
  ++restart_after.revision;
  ++restart_after.gold;
  const ItemTrade empty_trade;
  const std::string restart_receipt = enroll ? "ce0123456789abcdef0123456789abcd"
                                             : "cd0123456789abcdef0123456789abcd";
  ASSERT_EQ(state.commit_gathering_quest_claim(owner->id, character.id, 1, restart_receipt,
                                               fingerprint, restart_before, restart_after,
                                               empty_trade, effects, restart_source).status,
            LootClaimStatus::committed);
  ASSERT_TRUE(state.has_unfinished_gathering_claims());
  state.acquire_monster_reward_owner_epoch();
  constexpr std::int64_t restart_ticks = 200000;
  EXPECT_EQ(state.recover_gathering_claims(128, restart_ticks), 1U);
  EXPECT_EQ(state.recover_gathering_claims(128, restart_ticks + 1), 0U);
  EXPECT_FALSE(state.has_unfinished_gathering_claims());
  const auto recovered = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, restart_receipt, fingerprint, restart_before, restart_after,
      empty_trade, effects, restart_source, restart_ticks + 9000);
  EXPECT_EQ(recovered.status, GatheringFinalizeStatus::already_finalized);
  EXPECT_EQ(recovered.transition_epoch_ticks, restart_ticks + 3000);

  auto rejected_source = source;
  rejected_source.local_index += 2;
  const auto rejected_before = state.load_world_progress(owner->id, character.id, 1);
  auto rejected_after = rejected_before;
  ++rejected_after.revision;
  ++rejected_after.gold;
  const auto rejected = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, "de0123456789abcdef0123456789abcd", fingerprint,
      rejected_before, rejected_after, trade, effects, rejected_source);
  EXPECT_EQ(rejected.status, LootClaimStatus::conflict);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), rejected_before);
  EXPECT_FALSE(state.has_unfinished_gathering_claims());
  const auto after_rejection = state.shared_gathering_sources(1, std::nullopt, 128);
  EXPECT_FALSE(std::ranges::any_of(after_rejection, [&](const auto &record) {
    return record.key.map_id == rejected_source.map_id &&
           record.key.kind == rejected_source.kind &&
           record.key.sector_id == rejected_source.sector_id &&
           record.key.local_index == rejected_source.local_index;
  }));
}

TEST(Storage, MemoryGatheringClaimRecoveryAndSourceTransitions) {
  MemoryRepositories repositories;
  exercise_gathering_claim_recovery(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresGatheringClaimRecoveryAndSourceTransitions) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_gathering_claim_recovery(make_postgres_character_repositories(connection), "postgres");
}

TEST(Storage, MemoryGatheringClaimAlsoEnqueuesQuestEffects) {
  MemoryRepositories repositories;
  exercise_gathering_claim_recovery(repositories.character_repositories(), "memfx",
      [&](std::int64_t character_id) {
        std::lock_guard lock(repositories.database->quest_services->mutex);
        repositories.database->quest_services->company_memberships[character_id] = 2;
      });
}

TEST(Storage, PostgresGatheringClaimAlsoEnqueuesQuestEffects) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_gathering_claim_recovery(make_postgres_character_repositories(connection_string), "pgfx",
      [&](std::int64_t character_id) {
        pqxx::connection connection(connection_string);
        pqxx::work transaction(connection);
        transaction.exec("INSERT INTO emulator_character_company(character_id,company_type) "
                         "VALUES ($1,2)", pqxx::params{character_id});
        transaction.commit();
      });
}

void exercise_gathering_source_capacity(CharacterStateRepository &state) {
  constexpr int kTestWorld = 909;
  SharedGatheringSourceTransition source;
  source.map_id = 10117;
  source.kind = 0;
  source.sector_id = 1;
  source.local_index = 1;
  source.expected_active = true;
  source.resulting_active = false;
  source.next_transition_delay_ticks = 100;
  const auto last = state.update_shared_gathering_source(kTestWorld, source, 1000, false);
  ASSERT_TRUE(last.committed);
  ASSERT_TRUE(last.source);
  EXPECT_EQ(last.source->generation, 1);

  auto overflow = source;
  ++overflow.local_index;
  EXPECT_FALSE(state.update_shared_gathering_source(kTestWorld, overflow, 1000, false).committed);
  auto existing = source;
  existing.expected_generation = 1;
  existing.expected_active = false;
  existing.resulting_active = true;
  existing.next_transition_delay_ticks.reset();
  EXPECT_TRUE(state.update_shared_gathering_source(kTestWorld, existing, 1100, false).committed);
  const auto records = state.shared_gathering_sources(kTestWorld, std::nullopt, 128);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records.front().key.local_index, 1U);
  EXPECT_EQ(records.front().state.generation, 2);
}

TEST(Storage, MemoryGatheringSourceCapacityAllowsExistingTransitionsAtLimit) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  {
    std::lock_guard lock(repositories.database->mutex);
    // Seed the counter to exercise the boundary without allocating a million rows.
    repositories.database->gathering_source_counts[909] =
        kMaximumSharedGatheringSourcesPerWorld - 1;
  }
  exercise_gathering_source_capacity(*repositories.character_state);
}

TEST(Storage, PostgresGatheringSourceCapacityAllowsExistingTransitionsAtLimit) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  repositories.character_state->acquire_monster_reward_owner_epoch();
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    // This isolated world seeds the counter only to exercise the boundary efficiently.
    transaction.exec("INSERT INTO emulator_gathering_source_limits(world_id,source_count) "
                     "VALUES (909,$1)",
                     pqxx::params{kMaximumSharedGatheringSourcesPerWorld - 1});
    transaction.commit();
  }
  exercise_gathering_source_capacity(*repositories.character_state);
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    EXPECT_EQ(transaction.exec1("SELECT source_count FROM emulator_gathering_source_limits "
                                "WHERE world_id=909")[0].as<std::int64_t>(),
              static_cast<std::int64_t>(kMaximumSharedGatheringSourcesPerWorld));
    transaction.exec("DELETE FROM emulator_gathering_sources WHERE world_id=909");
    transaction.exec("DELETE FROM emulator_gathering_source_limits WHERE world_id=909");
    transaction.commit();
  }
}

void exercise_gathering_source_fence(CharacterStateRepository &state) {
  constexpr int kTestWorld = 910;
  EXPECT_NO_THROW(state.shared_gathering_sources(kTestWorld, std::nullopt,
                                                 kSharedGatheringSourcePageSize));
  EXPECT_THROW(state.shared_gathering_sources(kTestWorld, std::nullopt,
                                               kSharedGatheringSourcePageSize + 1),
               std::invalid_argument);
  SharedGatheringSourceTransition initial;
  initial.map_id = 10117;
  initial.kind = 1;
  initial.sector_id = 1;
  initial.local_index = 1;
  initial.expected_active = true;
  initial.expected_phase = 1;
  initial.resulting_active = true;
  initial.resulting_phase = 2;
  initial.next_transition_delay_ticks = 3000;
  initial.timed_successor_phase = 1;
  const auto fence = state.fence_shared_gathering_source(kTestWorld, initial);
  ASSERT_TRUE(fence.committed);
  ASSERT_TRUE(fence.source);
  EXPECT_EQ(fence.source->generation, 1);
  EXPECT_EQ(fence.source->phase, 1);
  EXPECT_FALSE(state.update_shared_gathering_source(kTestWorld, initial, 1000, false).committed);

  auto valid = initial;
  valid.expected_generation = 1;
  const auto changed = state.update_shared_gathering_source(kTestWorld, valid, 1000, false);
  ASSERT_TRUE(changed.committed);
  ASSERT_TRUE(changed.source);
  EXPECT_EQ(changed.source->generation, 2);
  EXPECT_EQ(changed.source->next_transition_epoch_ticks, 4000);
  const auto second_fence = state.fence_shared_gathering_source(kTestWorld, initial);
  ASSERT_TRUE(second_fence.committed);
  ASSERT_TRUE(second_fence.source);
  EXPECT_EQ(second_fence.source->generation, 3);
  EXPECT_EQ(second_fence.source->phase, 2);
  EXPECT_EQ(second_fence.source->next_transition_epoch_ticks, 4000);

  auto stale_timer = initial;
  stale_timer.expected_generation = 2;
  stale_timer.expected_phase = 2;
  stale_timer.resulting_phase = 1;
  stale_timer.next_transition_delay_ticks.reset();
  stale_timer.timed_successor_phase.reset();
  EXPECT_FALSE(state.update_shared_gathering_source(kTestWorld, stale_timer, 4000, true).committed);
  auto actual_timer = stale_timer;
  actual_timer.expected_generation = 3;
  ASSERT_TRUE(state.update_shared_gathering_source(kTestWorld, actual_timer, 4000, true).committed);
  EXPECT_FALSE(state.update_shared_gathering_source(kTestWorld, valid, 5000, false).committed);
}

TEST(Storage, MemoryGatheringSourceFenceRejectsQueuedAndAbaTransitions) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  exercise_gathering_source_fence(*repositories.character_state);
}

TEST(Storage, PostgresGatheringSourceFenceRejectsQueuedAndAbaTransitions) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  repositories.character_state->acquire_monster_reward_owner_epoch();
  exercise_gathering_source_fence(*repositories.character_state);
}

TEST(Storage, PostgresGatheringSourceFenceWaitsForAnEarlierTransition) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  SharedGatheringSourceTransition source;
  source.map_id = 10117;
  source.kind = 1;
  source.sector_id = 1;
  source.local_index = 1;
  source.expected_active = true;
  source.expected_phase = 1;
  source.resulting_active = true;
  source.resulting_phase = 2;
  source.next_transition_delay_ticks = 3000;
  source.timed_successor_phase = 1;
  constexpr int kTestWorld = 911;
  ASSERT_TRUE(state.fence_shared_gathering_source(kTestWorld, source).committed);

  pqxx::connection connection(connection_string);
  pqxx::work earlier(connection);
  const auto changed = earlier.exec(
      "UPDATE emulator_gathering_sources SET generation=2,phase=2,"
      "next_transition_epoch_ticks=4000,timed_successor_phase=1 "
      "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 "
      "AND local_index=$5 AND generation=1 RETURNING generation",
      pqxx::params{kTestWorld, source.map_id, source.kind, source.sector_id, source.local_index});
  ASSERT_EQ(changed.size(), 1U);
  auto fence = std::async(std::launch::async, [&] {
    return state.fence_shared_gathering_source(kTestWorld, source);
  });
  EXPECT_EQ(fence.wait_for(std::chrono::milliseconds(150)), std::future_status::timeout);
  earlier.commit();
  const auto result = fence.get();
  ASSERT_TRUE(result.committed);
  ASSERT_TRUE(result.source);
  EXPECT_EQ(result.source->generation, 3);
  EXPECT_EQ(result.source->phase, 2);
  EXPECT_EQ(result.source->next_transition_epoch_ticks, 4000);
  auto stale = source;
  stale.expected_generation = 1;
  EXPECT_FALSE(state.update_shared_gathering_source(kTestWorld, stale, 5000, false).committed);
}

void exercise_gathering_hydration_owner_takeover(CharacterStateRepository &old_owner,
                                                CharacterStateRepository &new_owner) {
  old_owner.acquire_monster_reward_owner_epoch();
  EXPECT_NO_THROW(old_owner.shared_gathering_sources(1, std::nullopt,
                                                     kSharedGatheringSourcePageSize));
  new_owner.acquire_monster_reward_owner_epoch();
  EXPECT_THROW(old_owner.shared_gathering_sources(1, std::nullopt,
                                                  kSharedGatheringSourcePageSize),
               std::exception);
  EXPECT_NO_THROW(new_owner.shared_gathering_sources(1, std::nullopt,
                                                     kSharedGatheringSourcePageSize));
}

TEST(Storage, MemoryGatheringHydrationRejectsOwnerTakeoverBetweenPages) {
  MemoryRepositories repositories;
  MemoryCharacterStateRepository next_owner(repositories.database);
  exercise_gathering_hydration_owner_takeover(*repositories.character_state, next_owner);
}

TEST(Storage, PostgresGatheringHydrationRejectsOwnerTakeoverBetweenPages) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto old_owner = make_postgres_character_repositories(connection_string);
  auto new_owner = make_postgres_character_repositories(connection_string);
  exercise_gathering_hydration_owner_takeover(*old_owner.character_state,
                                              *new_owner.character_state);
}

void exercise_linked_gathering_claim(const CharacterRepositories &repositories,
                                     const std::string &suffix) {
  Account account;
  account.name = "linked_gather_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Link" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  QuestServiceEffects effects;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;

  SharedGatheringSourceTransition cannon;
  cannon.map_id = 30104;
  cannon.kind = 1;
  cannon.sector_id = 171 | (290 << 14);
  cannon.local_index = 1;
  cannon.expected_active = true;
  cannon.expected_phase = 1;
  cannon.resulting_active = true;
  cannon.resulting_phase = 2;
  cannon.next_transition_delay_ticks = 5000;
  cannon.timed_successor_phase = 3;
  SharedGatheringLinkedTransition barrier;
  barrier.map_id = 30104;
  barrier.kind = 1;
  barrier.sector_id = 171 | (289 << 14);
  barrier.local_index = 2;
  cannon.linked = barrier;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  const ItemTrade trade;
  const std::string receipt = "ee0123456789abcdef0123456789abcd";
  const std::string fingerprint(64, 'f');

  // An absent, unverified barrier is represented by a non-rendered generation marker.
  const auto unknown_fence = state.fence_shared_gathering_source(1, cannon);
  ASSERT_TRUE(unknown_fence.committed);
  ASSERT_TRUE(unknown_fence.source);
  ASSERT_TRUE(unknown_fence.linked_source_snapshot);
  EXPECT_EQ(unknown_fence.source->generation, 1);
  EXPECT_EQ(unknown_fence.linked_source_snapshot->generation, 1);
  EXPECT_FALSE(unknown_fence.linked_source_snapshot->active);
  EXPECT_FALSE(unknown_fence.linked_source_snapshot->phase);
  auto unsupported_marker = cannon;
  ++unsupported_marker.linked->local_index;
  EXPECT_THROW(state.fence_shared_gathering_source(1, unsupported_marker),
               std::invalid_argument);
  auto stale_initial = cannon;
  stale_initial.linked.reset();
  EXPECT_FALSE(state.update_shared_gathering_source(1, stale_initial, 900, false).committed);
  cannon.expected_generation = 1;
  cannon.linked->expected_generation = 1;
  constexpr auto kLastLocalKey = std::numeric_limits<std::uint32_t>::max();
  const auto unknown_sources = state.shared_gathering_sources(
      1, SharedGatheringSourceKey{30103, 1, kLastLocalKey, kLastLocalKey}, 2);
  ASSERT_EQ(unknown_sources.size(), 2U);
  EXPECT_FALSE(unknown_sources.front().state.phase);
  state.acquire_monster_reward_owner_epoch();
  const auto after_owner_restart = state.shared_gathering_sources(
      1, SharedGatheringSourceKey{30103, 1, kLastLocalKey, kLastLocalKey}, 2);
  ASSERT_EQ(after_owner_restart.size(), 2U);
  EXPECT_EQ(after_owner_restart.front().state.generation, 1);
  EXPECT_FALSE(after_owner_restart.front().state.phase);
  const auto committed = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, cannon);
  ASSERT_EQ(committed.status, LootClaimStatus::committed);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);
  const auto sources = state.shared_gathering_sources(
      1, SharedGatheringSourceKey{30103, 1, kLastLocalKey, kLastLocalKey}, 2);
  ASSERT_EQ(sources.size(), 2U);
  EXPECT_EQ(sources[0].key.sector_id, barrier.sector_id);
  EXPECT_FALSE(sources[0].state.active);
  EXPECT_EQ(sources[0].state.phase, 0);
  EXPECT_EQ(sources[1].state.pending_receipt_id, receipt);
  const auto pending_fence = state.fence_shared_gathering_source(1, cannon);
  EXPECT_FALSE(pending_fence.committed);
  ASSERT_TRUE(pending_fence.source);
  EXPECT_EQ(pending_fence.source->pending_receipt_id, receipt);
  const auto replay = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, cannon);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  auto changed = cannon;
  changed.linked->expected_generation = 2;
  changed.linked->expected_phase = 0;
  EXPECT_EQ(state.commit_gathering_quest_claim(owner->id, character.id, 1, receipt, fingerprint,
                                               before, after, trade, effects, changed).status,
            LootClaimStatus::mismatch);
  const auto final = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, receipt, fingerprint, before, after, trade, effects, cannon, 1000);
  ASSERT_EQ(final.status, GatheringFinalizeStatus::finalized);
  ASSERT_TRUE(final.source_snapshot);
  ASSERT_TRUE(final.linked_source_snapshot);
  EXPECT_EQ(final.source_snapshot->generation, 3);
  EXPECT_EQ(final.linked_source_snapshot->generation, 3);
  EXPECT_FALSE(final.linked_source_snapshot->active);
  EXPECT_EQ(final.linked_source_snapshot->phase, 0);
  const auto fenced = state.fence_shared_gathering_source(1, cannon);
  ASSERT_TRUE(fenced.committed);
  ASSERT_TRUE(fenced.source);
  ASSERT_TRUE(fenced.linked_source_snapshot);
  EXPECT_EQ(fenced.source->generation, 4);
  EXPECT_EQ(fenced.linked_source_snapshot->generation, 4);
  auto stale = cannon;
  stale.linked.reset();
  stale.expected_generation = 3;
  stale.expected_phase = 2;
  EXPECT_FALSE(state.update_shared_gathering_source(1, stale, 6000, true).committed);

  // A later life commits both sources, then the process dies before confirmation.
  auto reset = cannon;
  reset.linked.reset();
  reset.expected_generation = 4;
  reset.expected_phase = 2;
  reset.resulting_phase = 1;
  reset.next_transition_delay_ticks.reset();
  reset.timed_successor_phase.reset();
  ASSERT_TRUE(state.update_shared_gathering_source(1, reset, 7000, false).committed);
  auto next_cannon = cannon;
  next_cannon.expected_generation = 5;
  next_cannon.linked->expected_generation = 4;
  next_cannon.linked->expected_phase = 0;
  const auto next_before = state.load_world_progress(owner->id, character.id, 1);
  auto next_after = next_before;
  ++next_after.revision;
  ++next_after.gold;
  const std::string next_receipt = "ef0123456789abcdef0123456789abcd";
  ASSERT_EQ(state.commit_gathering_quest_claim(owner->id, character.id, 1, next_receipt,
                                               fingerprint, next_before, next_after, trade,
                                               effects, next_cannon).status,
            LootClaimStatus::committed);
  state.acquire_monster_reward_owner_epoch();
  ASSERT_EQ(state.recover_gathering_claims(128, 8000), 1U);
  const auto recovered = state.finalize_gathering_quest_claim(
      owner->id, character.id, 1, next_receipt, fingerprint, next_before, next_after, trade,
      effects, next_cannon, 9000);
  ASSERT_EQ(recovered.status, GatheringFinalizeStatus::already_finalized);
  ASSERT_TRUE(recovered.source_snapshot);
  ASSERT_TRUE(recovered.linked_source_snapshot);
  EXPECT_EQ(recovered.source_snapshot->generation, 8);
  EXPECT_EQ(recovered.linked_source_snapshot->generation, 6);
  EXPECT_FALSE(recovered.linked_source_snapshot->active);
  EXPECT_EQ(recovered.linked_source_snapshot->phase, 0);

  auto prepare_third = cannon;
  prepare_third.linked.reset();
  prepare_third.expected_generation = 8;
  prepare_third.expected_phase = 2;
  prepare_third.resulting_phase = 1;
  prepare_third.next_transition_delay_ticks.reset();
  prepare_third.timed_successor_phase.reset();
  ASSERT_TRUE(state.update_shared_gathering_source(1, prepare_third, 10000, false).committed);
  auto stale_barrier = cannon;
  stale_barrier.expected_generation = 9;
  stale_barrier.linked->expected_generation = 5;
  stale_barrier.linked->expected_phase = 0;
  const auto conflict_before = state.load_world_progress(owner->id, character.id, 1);
  auto conflict_after = conflict_before;
  ++conflict_after.revision;
  ++conflict_after.gold;
  const auto rejected = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, "fa0123456789abcdef0123456789abcd", fingerprint,
      conflict_before, conflict_after, trade, effects, stale_barrier);
  EXPECT_EQ(rejected.status, LootClaimStatus::source_conflict);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), conflict_before);
  EXPECT_FALSE(state.has_unfinished_gathering_claims());

  SharedGatheringSourceTransition unexpected_timer;
  unexpected_timer.map_id = barrier.map_id;
  unexpected_timer.kind = barrier.kind;
  unexpected_timer.sector_id = barrier.sector_id;
  unexpected_timer.local_index = barrier.local_index;
  unexpected_timer.expected_generation = 6;
  unexpected_timer.expected_active = false;
  unexpected_timer.expected_phase = 0;
  unexpected_timer.resulting_active = false;
  unexpected_timer.resulting_phase = 0;
  unexpected_timer.next_transition_delay_ticks = 100;
  unexpected_timer.timed_successor_phase = 1;
  ASSERT_TRUE(state.update_shared_gathering_source(1, unexpected_timer, 11000, false).committed);
  auto timed_barrier = stale_barrier;
  timed_barrier.linked->expected_generation = 7;
  const auto timed_rejection = state.commit_gathering_quest_claim(
      owner->id, character.id, 1, "fb0123456789abcdef0123456789abcd", fingerprint,
      conflict_before, conflict_after, trade, effects, timed_barrier);
  EXPECT_EQ(timed_rejection.status, LootClaimStatus::source_conflict);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), conflict_before);
}

TEST(Storage, MemoryLinkedGatheringClaimCommitsBothSources) {
  MemoryRepositories repositories;
  exercise_linked_gathering_claim(repositories.character_repositories(), "mem");
}

TEST(Storage, PostgresLinkedGatheringClaimCommitsBothSources) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_linked_gathering_claim(make_postgres_character_repositories(connection_string), "pg");
}

TEST(Storage, PostgresLinkedGatheringLostAckStage) {
  const auto *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const auto *proxy_url = std::getenv("CHRONICLES_LOOT_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  Account account;
  account.name = "linked_lost_ack";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "LinkedLostAck";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto epoch = state.acquire_monster_reward_owner_epoch();
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  const ItemTrade trade;
  SharedGatheringSourceTransition cannon;
  cannon.map_id = 30104;
  cannon.kind = 1;
  cannon.sector_id = 171 | (290 << 14);
  cannon.local_index = 1;
  cannon.expected_active = true;
  cannon.expected_phase = 1;
  cannon.resulting_active = true;
  cannon.resulting_phase = 2;
  cannon.next_transition_delay_ticks = 5000;
  cannon.timed_successor_phase = 3;
  SharedGatheringLinkedTransition barrier;
  barrier.map_id = 30104;
  barrier.kind = 1;
  barrier.sector_id = 171 | (289 << 14);
  barrier.local_index = 2;
  cannon.linked = barrier;
  const auto marker = state.fence_shared_gathering_source(2, cannon);
  ASSERT_TRUE(marker.committed);
  ASSERT_TRUE(marker.linked_source_snapshot);
  EXPECT_FALSE(marker.linked_source_snapshot->phase);
  const std::string receipt = "ac0123456789abcdef0123456789abcd";
  const std::string fingerprint(64, 'f');
  PostgresCharacterStateRepository faulted(proxy_url);
  faulted.adopt_monster_reward_owner_epoch(epoch);
  EXPECT_THROW(faulted.commit_gathering_quest_claim(owner->id, character.id, 1, receipt,
                                                     fingerprint, before, after, trade, effects,
                                                     cannon), std::exception);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);
  const auto replay = state.commit_gathering_quest_claim(owner->id, character.id, 1, receipt,
                                                          fingerprint, before, after, trade,
                                                          effects, cannon);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  const auto rows = state.shared_gathering_sources(1, std::nullopt, 10);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].key.sector_id, barrier.sector_id);
  EXPECT_FALSE(rows[0].state.active);
  EXPECT_EQ(rows[0].state.phase, 0);
  EXPECT_EQ(rows[1].state.pending_receipt_id, receipt);
  EXPECT_TRUE(state.has_unfinished_gathering_claims());
}

TEST(Storage, PostgresLinkedGatheringRestartStage) {
  const auto *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!direct_url || !std::getenv("CHRONICLES_LINKED_RESTART_STAGE")) {
    GTEST_SKIP() << "Requires the isolated two-process PostgreSQL harness";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto owner = repositories.accounts->account("linked_lost_ack");
  ASSERT_TRUE(owner);
  const auto characters = repositories.characters->characters(owner->id, 1);
  ASSERT_EQ(characters.size(), 1U);
  const auto character = characters.front();
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;
  const auto after = state.load_world_progress(owner->id, character.id, 1);
  ASSERT_GT(after.revision, 0);
  ASSERT_GT(after.gold, 0);
  auto before = after;
  --before.revision;
  --before.gold;
  const ItemTrade trade;
  SharedGatheringSourceTransition cannon;
  cannon.map_id = 30104;
  cannon.kind = 1;
  cannon.sector_id = 171 | (290 << 14);
  cannon.local_index = 1;
  cannon.expected_active = true;
  cannon.expected_phase = 1;
  cannon.resulting_active = true;
  cannon.resulting_phase = 2;
  cannon.next_transition_delay_ticks = 5000;
  cannon.timed_successor_phase = 3;
  SharedGatheringLinkedTransition barrier;
  barrier.map_id = 30104;
  barrier.kind = 1;
  barrier.sector_id = 171 | (289 << 14);
  barrier.local_index = 2;
  cannon.linked = barrier;
  const auto marker_rows = state.shared_gathering_sources(2, std::nullopt, 8);
  ASSERT_EQ(marker_rows.size(), 2U);
  EXPECT_EQ(marker_rows.front().state.generation, 1);
  EXPECT_FALSE(marker_rows.front().state.phase);
  auto stale_marker = cannon;
  stale_marker.linked.reset();
  EXPECT_FALSE(state.update_shared_gathering_source(2, stale_marker, 1000, false).committed);
  const std::string receipt = "ac0123456789abcdef0123456789abcd";
  const std::string fingerprint(64, 'f');
  ASSERT_TRUE(state.has_unfinished_gathering_claims());
  ASSERT_EQ(state.recover_gathering_claims(128, 10000), 1U);
  const auto replay = state.commit_gathering_quest_claim(owner->id, character.id, 1, receipt,
                                                          fingerprint, before, after, trade,
                                                          effects, cannon);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  const auto final = state.finalize_gathering_quest_claim(owner->id, character.id, 1, receipt,
      fingerprint, before, after, trade, effects, cannon, 11000);
  ASSERT_EQ(final.status, GatheringFinalizeStatus::already_finalized);
  ASSERT_TRUE(final.source_snapshot);
  ASSERT_TRUE(final.linked_source_snapshot);
  EXPECT_EQ(final.transition_epoch_ticks, 15000);
  EXPECT_FALSE(final.linked_source_snapshot->active);
  EXPECT_EQ(final.linked_source_snapshot->phase, 0);
  EXPECT_FALSE(state.has_unfinished_gathering_claims());
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);
}

void exercise_boss_script_event(CharacterStateRepository &state) {
  state.acquire_monster_reward_owner_epoch();
  BossScriptIntent elite{"bb0123456789abcdef0123456789abcd", 1, 30104, 5001510, "shared"};
  EXPECT_THROW(state.enqueue_monster_rewards_with_script({}, std::nullopt), std::invalid_argument);
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({}, elite));
  EXPECT_TRUE(state.enqueue_monster_rewards_with_script({}, elite));
  auto changed = elite;
  changed.npc_cms_id = 5001104;
  EXPECT_FALSE(state.enqueue_monster_rewards_with_script({}, changed));
  EXPECT_TRUE(state.pending_boss_scripts(1, std::nullopt, 128).empty());
  EXPECT_FALSE(state.settle_shared_boss_script(elite).committed);
  ASSERT_TRUE(state.mark_monster_reward_ready(elite.event_id));
  EXPECT_FALSE(state.mark_boss_script_published(elite));
  const auto pending = state.pending_boss_scripts(1, std::nullopt, 128);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front(), elite);
  const auto settled = state.settle_shared_boss_script(elite);
  ASSERT_TRUE(settled.committed);
  EXPECT_EQ(settled.event_id, elite.event_id);
  ASSERT_EQ(settled.sources.size(), 2U);
  EXPECT_EQ(settled.sources[0].key.local_index, 5U);
  EXPECT_EQ(settled.sources[0].state.phase, 2);
  EXPECT_EQ(settled.sources[1].key.local_index, 8U);
  EXPECT_EQ(settled.sources[1].state.phase, 1);
  EXPECT_EQ(state.pending_boss_scripts(1, std::nullopt, 128).size(), 1U);
  const auto replay = state.settle_shared_boss_script(elite);
  ASSERT_TRUE(replay.committed);
  EXPECT_EQ(replay.event_id, elite.event_id);
  ASSERT_EQ(replay.sources.size(), 2U);
  EXPECT_GT(replay.sources[1].state.generation, settled.sources[1].state.generation);
  EXPECT_TRUE(state.mark_boss_script_published(elite));
  EXPECT_TRUE(state.mark_boss_script_published(elite));
  EXPECT_TRUE(state.pending_boss_scripts(1, std::nullopt, 128).empty());

  SharedGatheringSourceTransition consumed;
  consumed.map_id = elite.map_id;
  consumed.kind = 1;
  consumed.sector_id = replay.sources[1].key.sector_id;
  consumed.local_index = replay.sources[1].key.local_index;
  consumed.expected_generation = replay.sources[1].state.generation;
  consumed.expected_active = true;
  consumed.expected_phase = 1;
  consumed.resulting_active = true;
  consumed.resulting_phase = 2;
  ASSERT_TRUE(state.update_shared_gathering_source(1, consumed, 1000, false).committed);
  const auto progressed = state.settle_shared_boss_script(elite);
  ASSERT_TRUE(progressed.committed);
  EXPECT_EQ(progressed.sources[1].state.phase, 2);

  BossScriptIntent private_script{"bc0123456789abcdef0123456789abcd", 1, 30104, 5001104,
                                  std::string(32, 'a') + ":1"};
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({}, private_script));
  ASSERT_TRUE(state.mark_monster_reward_ready(private_script.event_id));
  EXPECT_THROW(state.settle_shared_boss_script(private_script), std::invalid_argument);
  EXPECT_TRUE(state.mark_private_boss_script_applied(private_script));
  EXPECT_TRUE(state.mark_private_boss_script_applied(private_script));
  EXPECT_TRUE(state.pending_boss_scripts(1, std::nullopt, 128).empty());
}

TEST(Storage, MemoryBossScriptEventIsStagedAndIdempotent) {
  MemoryRepositories repositories;
  exercise_boss_script_event(*repositories.character_state);
}

TEST(Storage, MemoryBossScriptIndexSkipsHistoryAndPaginatesByWorld) {
  MemoryRepositories repositories;
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  {
    std::lock_guard lock(repositories.database->mutex);
    for (int index = 0; index < 50000; ++index) {
      std::ostringstream id;
      id << std::hex << std::setw(32) << std::setfill('0') << index;
      repositories.database->monster_reward_batches.emplace(
          id.str(), MemoryDatabase::StoredMonsterRewardBatch{{}, true, std::nullopt, true});
    }
  }
  const std::string copy_scope = std::string(32, 'a') + ":1";
  const BossScriptIntent first{"f10123456789abcdef0123456789abcd", 1, 30104, 5001104,
                               copy_scope};
  const BossScriptIntent second{"f20123456789abcdef0123456789abcd", 1, 30104, 5001104,
                                copy_scope};
  const BossScriptIntent other{"f30123456789abcdef0123456789abcd", 2, 30104, 5001104,
                               copy_scope};
  for (const auto &intent : {first, second, other}) {
    ASSERT_TRUE(state.enqueue_monster_rewards_with_script({}, intent));
    ASSERT_TRUE(state.mark_monster_reward_ready(intent.event_id));
  }
  const auto page = state.pending_boss_scripts(1, std::nullopt, 1);
  ASSERT_EQ(page.size(), 1U);
  EXPECT_EQ(page.front(), first);
  const auto next = state.pending_boss_scripts(1, first.event_id, 1);
  ASSERT_EQ(next.size(), 1U);
  EXPECT_EQ(next.front(), second);
  const auto other_world = state.pending_boss_scripts(2, std::nullopt, 2);
  ASSERT_EQ(other_world.size(), 1U);
  EXPECT_EQ(other_world.front(), other);
  ASSERT_TRUE(state.mark_private_boss_script_applied(first));
  const auto remaining = state.pending_boss_scripts(1, std::nullopt, 2);
  ASSERT_EQ(remaining.size(), 1U);
  EXPECT_EQ(remaining.front(), second);
}

TEST(Storage, PostgresBossScriptEventIsStagedAndIdempotent) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  exercise_boss_script_event(*repositories.character_state);
}

void exercise_boss_script_reward_order(const CharacterRepositories &repositories,
                                       const std::string &suffix) {
  Account account;
  account.name = "boss_order_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Boss" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  const std::string event_id = "bd0123456789abcdef0123456789abcd";
  BossScriptIntent intent{event_id, 1, 30104, 5001104, "shared"};
  MonsterRewardReceipt receipt;
  receipt.account_id = owner->id;
  receipt.character_id = character.id;
  receipt.world_id = 1;
  receipt.receipt_id = event_id;
  receipt.credited_unit_id = 123;
  receipt.npc_cms_id = intent.npc_cms_id;
  receipt.position.map_id = intent.map_id;
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({receipt}, intent));
  EXPECT_FALSE(state.enqueue_monster_rewards({receipt}));
  ASSERT_TRUE(state.mark_monster_reward_ready(event_id));
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, event_id, before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::conflict);
  const auto membership = state.read_quest_service_membership(owner->id, character.id, 1);
  ASSERT_TRUE(membership);
  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_generation = membership->guild_generation;
  EXPECT_EQ(state.commit_monster_reward_with_effects(owner->id, character.id, 1, event_id,
                                                      before, std::nullopt, {}, effects).status,
            MonsterRewardCommitStatus::conflict);
  ASSERT_TRUE(state.settle_shared_boss_script(intent).committed);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, event_id, before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::conflict);
  ASSERT_TRUE(state.mark_boss_script_published(intent));
  const auto pending = state.pending_monster_rewards(owner->id, character.id, 1);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front().receipt_id, event_id);
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, event_id, before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::committed);
}

TEST(Storage, MemoryBossScriptSettlesBeforeRecipientRewards) {
  MemoryRepositories repositories;
  exercise_boss_script_reward_order(repositories.character_repositories(), "mem");
}

TEST(Storage, PostgresBossScriptSettlesBeforeRecipientRewards) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_boss_script_reward_order(make_postgres_character_repositories(connection_string), "pg");
}

TEST(Storage, PostgresBossRewardWaitsWhilePublicationAcknowledgementIsBlocked) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  Account account;
  account.name = "boss_publish_race";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "BossPubRace";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  BossScriptIntent intent{"ba0123456789abcdef0123456789abcd", 1, 30104, 5001104, "shared"};
  MonsterRewardReceipt receipt;
  receipt.account_id = owner->id;
  receipt.character_id = character.id;
  receipt.world_id = 1;
  receipt.receipt_id = intent.event_id;
  receipt.credited_unit_id = 123;
  receipt.npc_cms_id = intent.npc_cms_id;
  receipt.position.map_id = intent.map_id;
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({receipt}, intent));
  ASSERT_TRUE(state.mark_monster_reward_ready(intent.event_id));
  ASSERT_TRUE(state.settle_shared_boss_script(intent).committed);

  pqxx::connection connection(connection_string);
  pqxx::work hold(connection);
  hold.exec("SELECT receipt_id FROM emulator_monster_reward_events WHERE receipt_id=$1 FOR UPDATE",
            pqxx::params{intent.event_id});
  auto publication = std::async(std::launch::async, [&] {
    return state.mark_boss_script_published(intent);
  });
  EXPECT_EQ(publication.wait_for(std::chrono::milliseconds(150)), std::future_status::timeout);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  hold.commit();
  EXPECT_TRUE(publication.get());
  EXPECT_EQ(state.pending_monster_rewards(owner->id, character.id, 1).size(), 1U);
}

void exercise_boss_script_capacity(CharacterStateRepository &state) {
  constexpr int kTestWorld = 912;
  BossScriptIntent elite{"bf0123456789abcdef0123456789abcd", kTestWorld, 30104, 5001510,
                         "shared"};
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({}, elite));
  ASSERT_TRUE(state.mark_monster_reward_ready(elite.event_id));
  EXPECT_FALSE(state.settle_shared_boss_script(elite).committed);
  EXPECT_TRUE(state.shared_gathering_sources(kTestWorld, std::nullopt, 2).empty());
  const auto pending = state.pending_boss_scripts(kTestWorld, std::nullopt, 2);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front(), elite);
}

TEST(Storage, MemoryBossScriptCapacityDoesNotPartiallyInitializeTargets) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  {
    std::lock_guard lock(repositories.database->mutex);
    repositories.database->gathering_source_counts[912] =
        kMaximumSharedGatheringSourcesPerWorld - 1;
  }
  exercise_boss_script_capacity(*repositories.character_state);
}

TEST(Storage, PostgresBossScriptCapacityDoesNotPartiallyInitializeTargets) {
  const auto *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  repositories.character_state->acquire_monster_reward_owner_epoch();
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("INSERT INTO emulator_gathering_source_limits(world_id,source_count) "
                     "VALUES (912,$1)",
                     pqxx::params{kMaximumSharedGatheringSourcesPerWorld - 1});
    transaction.commit();
  }
  exercise_boss_script_capacity(*repositories.character_state);
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    EXPECT_EQ(transaction.exec1("SELECT source_count FROM emulator_gathering_source_limits "
                                "WHERE world_id=912")[0].as<std::int64_t>(),
              static_cast<std::int64_t>(kMaximumSharedGatheringSourcesPerWorld - 1));
    transaction.exec("DELETE FROM emulator_monster_reward_events WHERE world_id=912");
    transaction.exec("DELETE FROM emulator_monster_reward_limits WHERE world_id=912");
    transaction.exec("DELETE FROM emulator_gathering_source_limits WHERE world_id=912");
    transaction.commit();
  }
}

TEST(Storage, PostgresBossScriptLostAckStage) {
  const auto *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const auto *proxy_url = std::getenv("CHRONICLES_LOOT_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  Account account;
  account.name = "boss_lost_ack";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "BossLostAck";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto epoch = state.acquire_monster_reward_owner_epoch();
  BossScriptIntent intent{"ab0123456789abcdef0123456789abcd", 1, 30104, 5001510, "shared"};
  MonsterRewardReceipt receipt;
  receipt.account_id = owner->id;
  receipt.character_id = character.id;
  receipt.world_id = 1;
  receipt.receipt_id = intent.event_id;
  receipt.credited_unit_id = 123;
  receipt.npc_cms_id = intent.npc_cms_id;
  receipt.position.map_id = intent.map_id;
  ASSERT_TRUE(state.enqueue_monster_rewards_with_script({receipt}, intent));
  ASSERT_TRUE(state.mark_monster_reward_ready(intent.event_id));
  PostgresCharacterStateRepository faulted(proxy_url);
  faulted.adopt_monster_reward_owner_epoch(epoch);
  EXPECT_THROW(faulted.settle_shared_boss_script(intent), std::exception);
  const auto rows = state.shared_gathering_sources(1, std::nullopt, 8);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].state.phase, 2);
  EXPECT_EQ(rows[1].state.phase, 1);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  const auto pending = state.pending_boss_scripts(1, std::nullopt, 8);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front(), intent);
}

TEST(Storage, PostgresBossScriptRestartStage) {
  const auto *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!direct_url || !std::getenv("CHRONICLES_BOSS_RESTART_STAGE")) {
    GTEST_SKIP() << "Requires the isolated two-process PostgreSQL harness";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto owner = repositories.accounts->account("boss_lost_ack");
  ASSERT_TRUE(owner);
  const auto characters = repositories.characters->characters(owner->id, 1);
  ASSERT_EQ(characters.size(), 1U);
  const auto character = characters.front();
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  BossScriptIntent intent{"ab0123456789abcdef0123456789abcd", 1, 30104, 5001510, "shared"};
  const auto pending = state.pending_boss_scripts(1, std::nullopt, 8);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front(), intent);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  const auto settled = state.settle_shared_boss_script(intent);
  ASSERT_TRUE(settled.committed);
  ASSERT_EQ(settled.sources.size(), 2U);
  EXPECT_EQ(settled.event_id, intent.event_id);
  EXPECT_EQ(settled.sources[0].state.phase, 2);
  EXPECT_EQ(settled.sources[1].state.phase, 1);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  // This storage-only stage models completed hydration; native startup is tested separately.
  ASSERT_TRUE(state.mark_boss_script_published(intent));
  EXPECT_TRUE(state.mark_boss_script_published(intent));
  const auto rewards = state.pending_monster_rewards(owner->id, character.id, 1);
  ASSERT_EQ(rewards.size(), 1U);
  EXPECT_EQ(rewards.front().receipt_id, intent.event_id);
}

void exercise_concurrent_loot_claim(const CharacterRepositories &repositories,
                                    const std::string &suffix) {
  std::vector<Character> characters;
  for (int index = 0; index < 2; ++index) {
    Account account;
    account.name = "loot_race_" + suffix + std::to_string(index);
    account.salt = "test-salt";
    account.hash = "test-hash";
    ASSERT_TRUE(repositories.accounts->add_account(account));
    const auto owner = repositories.accounts->account(account.name);
    ASSERT_TRUE(owner);
    Character character;
    character.account_id = owner->id;
    character.name = "LootRace" + suffix + std::to_string(index);
    characters.push_back(repositories.characters->create(character));
  }
  auto &state = *repositories.character_state;
  std::vector<WorldProgress> before;
  for (const auto &character : characters) {
    before.push_back(state.load_world_progress(character.account_id, character.id, 1));
  }
  std::promise<void> ready;
  const auto start = ready.get_future().share();
  const auto claim = [&](std::size_t index) {
    start.wait();
    auto after = before[index];
    ++after.revision;
    ++after.gold;
    return state.commit_loot_claim(characters[index].account_id, characters[index].id, 1,
                                   std::string(32, 'e'), std::string(64, 'f'),
                                   before[index], after, ItemTrade{});
  };
  auto first = std::async(std::launch::async, claim, 0);
  auto second = std::async(std::launch::async, claim, 1);
  ready.set_value();
  const auto first_result = first.get();
  const auto second_result = second.get();
  const auto committed = int(first_result.status == LootClaimStatus::committed) +
                         int(second_result.status == LootClaimStatus::committed);
  const auto lost = int(first_result.status == LootClaimStatus::claimed_by_other_character) +
                    int(second_result.status == LootClaimStatus::claimed_by_other_character);
  EXPECT_EQ(committed, 1);
  EXPECT_EQ(lost, 1);
  for (std::size_t index = 0; index < characters.size(); ++index) {
    const auto after = state.load_world_progress(characters[index].account_id, characters[index].id, 1);
    const auto &result = index == 0 ? first_result : second_result;
    EXPECT_EQ(after.gold, before[index].gold + (result.status == LootClaimStatus::committed ? 1 : 0));
  }
}

TEST(Storage, MemoryConcurrentLootClaimHasOneWinner) {
  MemoryRepositories repositories;
  exercise_concurrent_loot_claim(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresConcurrentLootClaimHasOneWinner) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires a disposable PostgreSQL database";
  }
  exercise_concurrent_loot_claim(make_postgres_character_repositories(connection), "postgres");
}

TEST(Storage, PostgresLootClaimReplaysAfterActualLostCommitAcknowledgement) {
  const auto *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const auto *proxy_url = std::getenv("CHRONICLES_LOOT_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  const auto repositories = make_postgres_character_repositories(direct_url);
  Account account;
  account.name = "loot_real_lost_ack";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "LootLostAck";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(character.account_id, character.id, 1);
  auto after = before;
  ++after.revision;
  after.gold += 7;
  std::set<int> used_slots;
  for (const auto &item : character.items) {
    used_slots.insert(item.inventory_slot);
  }
  int free_slot = 0;
  while (used_slots.contains(free_slot)) {
    ++free_slot;
  }
  CharacterItem item;
  item.cms_id = 901;
  item.inventory_slot = free_slot;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  const std::string receipt(32, '9');
  const std::string fingerprint(64, '8');
  PostgresCharacterStateRepository faulted(proxy_url);
  EXPECT_THROW(faulted.commit_loot_claim(character.account_id, character.id, 1, receipt,
                                          fingerprint, before, after, trade), std::exception);
  const auto lookup = state.lookup_loot_claim(character.account_id, character.id, 1, receipt,
                                               fingerprint);
  ASSERT_EQ(lookup.status, LootClaimStatus::already_claimed_by_same_character);
  ASSERT_EQ(lookup.granted_items.size(), 1U);
  const auto replay = state.commit_loot_claim(character.account_id, character.id, 1, receipt,
                                               fingerprint, before, after, trade);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  EXPECT_EQ(replay.granted_items, lookup.granted_items);
  EXPECT_EQ(state.load_world_progress(character.account_id, character.id, 1).gold, after.gold);
  const auto reloaded = repositories.characters->character_by_id(character.id);
  ASSERT_TRUE(reloaded);
  EXPECT_EQ(std::ranges::count(reloaded->items, lookup.granted_items.front()), 1);
}

TEST(Storage, PostgresLootClaimAndEntryFenceSerializeInBothOrders) {
  const auto *connection_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_url) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL loot harness";
  }
  {
    pqxx::connection connection(connection_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "loot_claim_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  const auto repositories = make_postgres_character_repositories(connection_url);
  auto &state = *repositories.character_state;
  for (int index = 0; index < 12; ++index) {
    Account account;
    account.name = "loot_fence_" + std::to_string(index);
    account.salt = "test-salt";
    account.hash = "test-hash";
    ASSERT_TRUE(repositories.accounts->add_account(account));
    const auto owner = repositories.accounts->account(account.name);
    ASSERT_TRUE(owner);
    Character character;
    character.account_id = owner->id;
    character.name = "LootFence" + std::to_string(index);
    character = repositories.characters->create(character);
    const auto before = state.load_world_progress(character.account_id, character.id, 1);
    auto after = before;
    ++after.revision;
    after.gold += 1;
    std::ostringstream receipt_stream;
    receipt_stream << std::hex << std::setw(32) << std::setfill('0') << (index + 1);
    const auto receipt = receipt_stream.str();
    const std::string fingerprint(64, 'd');
    LootClaimResult result;
    bool fenced{};
    if (index == 0) {
      fenced = state.fence_world_entry(character.account_id, character.id, 1);
      result = state.commit_loot_claim(character.account_id, character.id, 1, receipt,
                                       fingerprint, before, after, ItemTrade{});
      EXPECT_EQ(result.status, LootClaimStatus::conflict);
    } else if (index == 1) {
      result = state.commit_loot_claim(character.account_id, character.id, 1, receipt,
                                       fingerprint, before, after, ItemTrade{});
      fenced = state.fence_world_entry(character.account_id, character.id, 1);
      EXPECT_EQ(result.status, LootClaimStatus::committed);
    } else {
      std::promise<void> start_promise;
      const auto start = start_promise.get_future().share();
      auto claim = std::async(std::launch::async, [&] {
        start.wait();
        return state.commit_loot_claim(character.account_id, character.id, 1, receipt,
                                       fingerprint, before, after, ItemTrade{});
      });
      auto fence = std::async(std::launch::async, [&] {
        start.wait();
        return state.fence_world_entry(character.account_id, character.id, 1);
      });
      start_promise.set_value();
      result = claim.get();
      fenced = fence.get();
    }
    ASSERT_TRUE(fenced);
    ASSERT_TRUE(result.status == LootClaimStatus::committed ||
                result.status == LootClaimStatus::conflict);
    const auto final_progress = state.load_world_progress(character.account_id, character.id, 1);
    const auto lookup = state.lookup_loot_claim(character.account_id, character.id, 1, receipt,
                                                 fingerprint);
    if (result.status == LootClaimStatus::committed) {
      EXPECT_EQ(final_progress.revision, before.revision + 2);
      EXPECT_EQ(final_progress.gold, before.gold + 1);
      EXPECT_EQ(lookup.status, LootClaimStatus::already_claimed_by_same_character);
    } else {
      EXPECT_EQ(final_progress.revision, before.revision + 1);
      EXPECT_EQ(final_progress.gold, before.gold);
      EXPECT_EQ(lookup.status, LootClaimStatus::not_found);
    }
  }
}

TEST(Storage, ConcurrentSlotCreation) {
  MemoryRepositories repositories;
  std::atomic_int successful_creations{};
  std::atomic_int slot_conflicts{};
  std::vector<std::thread> threads;

  for (int i = 0; i < 12; ++i) {
    threads.emplace_back([&, i] {
      Character character;
      character.account_id = 1;
      character.name = "Name" + std::to_string(i);
      try {
        repositories.characters->create(character);
        ++successful_creations;
      } catch (const CreateError &) {
        ++slot_conflicts;
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(successful_creations, 1);
  EXPECT_EQ(slot_conflicts, 11);
}

TEST(Storage, LearnedRecipesAreOwnerScopedMonotonicAndRevisionGuarded) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 7;
  character.world_id = 2;
  character.name = "RecipeOwner";
  const auto created = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(7, created.id, 2);
  auto learned = before;
  ++learned.revision;
  learned.learned_crafts.insert(60203031);
  EXPECT_FALSE(state.save_world_progress(8, created.id, 2, before, learned));
  ASSERT_TRUE(state.save_world_progress(7, created.id, 2, before, learned));
  EXPECT_FALSE(state.save_world_progress(7, created.id, 2, before, learned));
  EXPECT_EQ(state.load_world_progress(7, created.id, 2).learned_crafts, (std::set<int>{60203031}));

  auto erased = learned;
  ++erased.revision;
  erased.learned_crafts.clear();
  EXPECT_THROW(state.save_world_progress(7, created.id, 2, learned, erased), std::invalid_argument);
  auto invalid = learned;
  ++invalid.revision;
  invalid.learned_crafts.insert(0);
  EXPECT_THROW(state.save_world_progress(7, created.id, 2, learned, invalid),
               std::invalid_argument);
  EXPECT_EQ(state.load_world_progress(7, created.id, 2), learned);
}

TEST(Storage, DoodadPhaseZeroPersistsAndNegativePhaseIsRejected) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 7;
  character.world_id = 2;
  character.name = "DoodadOwner";
  const auto created = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(7, created.id, 2);

  auto changed = before;
  ++changed.revision;
  changed.doodad_phases[12345] = 0;
  ASSERT_TRUE(state.save_world_progress(7, created.id, 2, before, changed));
  EXPECT_EQ(state.load_world_progress(7, created.id, 2).doodad_phases.at(12345), 0);

  auto invalid = changed;
  ++invalid.revision;
  invalid.doodad_phases[12345] = -1;
  EXPECT_THROW(state.save_world_progress(7, created.id, 2, changed, invalid),
               std::invalid_argument);
  EXPECT_EQ(state.load_world_progress(7, created.id, 2), changed);
}

TEST(Storage, AchievementReceiptsCannotLoseClaimsOrChangeCompletionTime) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 7;
  character.world_id = 2;
  character.name = "AchievementOwner";
  const auto created = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(7, created.id, 2);
  auto completed = before;
  ++completed.revision;
  completed.achievements[1000] = {1, 17000000000000000};
  ASSERT_TRUE(state.save_world_progress(7, created.id, 2, before, completed));
  auto claimed = completed;
  ++claimed.revision;
  claimed.gold += 100;
  claimed.achievements.at(1000).state = 2;
  ASSERT_TRUE(state.save_world_progress(7, created.id, 2, completed, claimed));
  EXPECT_FALSE(state.save_world_progress(7, created.id, 2, completed, claimed));
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto invalid = claimed;
    ++invalid.revision;
    if (mutation == 0) {
      invalid.achievements.clear();
    } else if (mutation == 1) {
      invalid.achievements.at(1000).state = 1;
    } else {
      ++invalid.achievements.at(1000).completion_tick;
    }
    EXPECT_THROW(state.save_world_progress(7, created.id, 2, claimed, invalid),
                 std::invalid_argument);
  }
  EXPECT_EQ(state.load_world_progress(7, created.id, 2), claimed);
}

// Schema versions 35 and 36: learned recipes and achievement receipts persist in their own
// tables, commit atomically with the rest of the progress row and survive a reload.
void exercise_recipe_and_achievement_receipts(const CharacterRepositories &repositories,
                                              const std::string &suffix) {
  Account owner;
  owner.name = "receipts_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);

  Character character;
  character.account_id = owned_account->id;
  character.name = "Rcpt" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto load = [&] { return state.load_world_progress(owned_account->id, character.id, 1); };

  // Learning a recipe spends its item in the same commit.
  const auto before = load();
  auto learned = before;
  ++learned.revision;
  learned.stackables[60203031] = 1;
  learned.learned_crafts.insert(60203031);
  ASSERT_TRUE(state.save_world_progress(owned_account->id, character.id, 1, before, learned));
  EXPECT_FALSE(state.save_world_progress(owned_account->id, character.id, 1, before, learned));

  // Completion and its reward claim each commit with the progress row.
  auto completed = load();
  ASSERT_EQ(completed, learned);
  ++completed.revision;
  completed.achievements[1000] = {1, 17000000000000000};
  ASSERT_TRUE(state.save_world_progress(owned_account->id, character.id, 1, learned, completed));
  auto claimed = completed;
  ++claimed.revision;
  claimed.gold += 100;
  claimed.achievements.at(1000).state = 2;
  ASSERT_TRUE(state.save_world_progress(owned_account->id, character.id, 1, completed, claimed));

  // A stale retry of the claim cannot pay again, and receipts cannot be removed or retimed.
  EXPECT_FALSE(state.save_world_progress(owned_account->id, character.id, 1, completed, claimed));
  auto retimed = claimed;
  ++retimed.revision;
  ++retimed.achievements.at(1000).completion_tick;
  EXPECT_THROW(state.save_world_progress(owned_account->id, character.id, 1, claimed, retimed),
               std::invalid_argument);
  auto forgotten = claimed;
  ++forgotten.revision;
  forgotten.learned_crafts.clear();
  EXPECT_THROW(state.save_world_progress(owned_account->id, character.id, 1, claimed, forgotten),
               std::invalid_argument);

  const auto reloaded = load();
  EXPECT_EQ(reloaded, claimed);
  EXPECT_EQ(reloaded.learned_crafts, (std::set<int>{60203031}));
  ASSERT_EQ(reloaded.achievements.size(), 1U);
  EXPECT_EQ(reloaded.achievements.at(1000).state, 2);
  EXPECT_EQ(reloaded.achievements.at(1000).completion_tick, 17000000000000000);
  EXPECT_EQ(reloaded.gold, before.gold + 100);
}

TEST(Storage, MemoryStoreRecipeAndAchievementReceiptsRoundTrip) {
  MemoryRepositories repositories;
  exercise_recipe_and_achievement_receipts(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresRecipeAndAchievementReceiptsRoundTrip) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_recipe_and_achievement_receipts(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void exercise_monster_reward_receipts(const CharacterRepositories &repositories,
                                      const std::string &suffix) {
  Account account;
  account.name = "outbox_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);

  Character first;
  first.account_id = owner->id;
  first.name = "OutboxA" + suffix;
  first.slot = 0;
  first = repositories.characters->create(first);
  Character second;
  second.account_id = owner->id;
  second.name = "OutboxB" + suffix;
  second.slot = 1;
  second = repositories.characters->create(second);

  const std::string first_event(32, 'a');
  const std::string next_event(32, 'b');
  MonsterRewardReceipt first_receipt{owner->id, first.id, 1, first_event, 77, 500,
                                      {9012, 10, 20, 30, 0.0F}, true};
  MonsterRewardReceipt second_receipt{owner->id, second.id, 1, first_event, 77, 500,
                                       {9012, 40, 50, 60, 0.0F}, false};
  first_receipt.quest_attempts[42] = {2, 123, false, false, {{1, 3}}};
  second_receipt.quest_attempts[43] = {1, 456, true, false, {{1, 2}, {2, 5}}};
  second_receipt.allow_monster_offered_quests = true;
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();

  auto wrong_owner = second_receipt;
  ++wrong_owner.account_id;
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt, wrong_owner}));
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, first.id, 1).empty());
  ASSERT_TRUE(state.enqueue_monster_rewards({first_receipt, second_receipt}));
  EXPECT_TRUE(state.enqueue_monster_rewards({first_receipt, second_receipt}));
  EXPECT_TRUE(state.enqueue_monster_rewards({second_receipt, first_receipt}));
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt}));
  auto conflicting = second_receipt;
  ++conflicting.position.x_mm;
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt, conflicting}));
  conflicting = second_receipt;
  ++conflicting.quest_attempts.at(43).attempt;
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt, conflicting}));
  conflicting = second_receipt;
  conflicting.quest_attempts.at(43).complete = false;
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt, conflicting}));
  conflicting = second_receipt;
  ++conflicting.quest_attempts.at(43).objectives.at(1);
  EXPECT_FALSE(state.enqueue_monster_rewards({first_receipt, conflicting}));
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, second.id, 1).empty());
  const auto staged_before = state.load_world_progress(owner->id, first.id, 1);
  EXPECT_EQ(state.commit_monster_reward(owner->id, first.id, 1, first_event, staged_before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::conflict);
  EXPECT_FALSE(state.mark_monster_reward_ready(std::string(32, 'c')));
  ASSERT_TRUE(state.mark_monster_reward_ready(first_event));
  EXPECT_TRUE(state.mark_monster_reward_ready(first_event));
  EXPECT_EQ(state.pending_monster_rewards(owner->id, second.id, 1),
            (std::vector<MonsterRewardReceipt>{second_receipt}));

  const auto before = state.load_world_progress(owner->id, first.id, 1);
  auto after = before;
  ++after.revision;
  after.gold += 10;
  ItemTrade trade;
  CharacterItem item;
  item.cms_id = 123;
  item.inventory_slot = 100;
  trade.granted_items.push_back(item);
  auto stale = before;
  ++stale.revision;
  auto stale_after = stale;
  ++stale_after.revision;
  EXPECT_EQ(state.commit_monster_reward(owner->id, first.id, 1, first_event, stale, stale_after,
                                        trade).status,
            MonsterRewardCommitStatus::conflict);
  EXPECT_EQ(state.pending_monster_rewards(owner->id, first.id, 1).size(), 1U);

  const auto applied = state.commit_monster_reward(owner->id, first.id, 1, first_event, before,
                                                    after, trade);
  EXPECT_EQ(applied.status, MonsterRewardCommitStatus::committed);
  ASSERT_EQ(applied.granted_items.size(), 1U);
  EXPECT_EQ(state.load_world_progress(owner->id, first.id, 1), after);
  EXPECT_EQ(state.commit_monster_reward(owner->id, first.id, 1, first_event, before, after,
                                        trade).status,
            MonsterRewardCommitStatus::already_processed);
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, first.id, 1).empty());

  const auto second_before = state.load_world_progress(owner->id, second.id, 1);
  EXPECT_EQ(state.commit_monster_reward(owner->id, second.id, 1, first_event, second_before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::committed);
  EXPECT_EQ(state.load_world_progress(owner->id, second.id, 1), second_before);
  ASSERT_TRUE(repositories.characters->delete_character(second.id));
  // The committed envelope is authoritative even if its second recipient was deleted after
  // an acknowledgement was lost. A retry must not recreate that character's receipt.
  EXPECT_TRUE(state.enqueue_monster_rewards({first_receipt, second_receipt}));

  auto next_receipt = first_receipt;
  next_receipt.receipt_id = next_event;
  next_receipt.credited_unit_id = 78;
  ASSERT_TRUE(state.enqueue_monster_rewards({next_receipt}));
  ASSERT_TRUE(state.mark_monster_reward_ready(next_event));
  auto next_after = after;
  ++next_after.revision;
  EXPECT_EQ(state.commit_monster_reward(owner->id, first.id, 1, next_event, after, next_after,
                                        trade).status,
            MonsterRewardCommitStatus::conflict);
  EXPECT_EQ(state.load_world_progress(owner->id, first.id, 1), after);
  ASSERT_EQ(state.pending_monster_rewards(owner->id, first.id, 1).size(), 1U);
  EXPECT_EQ(state.commit_monster_reward(owner->id, first.id, 1, next_event, after, next_after,
                                        {}).status,
            MonsterRewardCommitStatus::committed);
}

void exercise_monster_reward_staging(const CharacterRepositories &repositories,
                                     const std::string &suffix) {
  Account account;
  account.name = "outbox_stage_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Stage" + suffix;
  character = repositories.characters->create(character);

  auto &state = *repositories.character_state;
  const auto old_epoch = state.acquire_monster_reward_owner_epoch();
  while (state.recover_staged_monster_rewards(kMonsterRewardRecoveryPageSize) != 0) {
  }
  EXPECT_FALSE(state.has_staged_monster_rewards());
  EXPECT_THROW(state.recover_staged_monster_rewards(0), std::invalid_argument);
  EXPECT_THROW(state.recover_staged_monster_rewards(kMonsterRewardRecoveryPageSize + 1),
               std::invalid_argument);

  MonsterRewardReceipt first{owner->id, character.id, 1, std::string(32, '2'), 501, 500,
                              {9012, 0, 0, 0, 0.0F}, true};
  auto second = first;
  second.receipt_id = std::string(32, '3');
  ++second.credited_unit_id;
  ASSERT_TRUE(state.enqueue_monster_rewards({first}));
  ASSERT_TRUE(state.enqueue_monster_rewards({second}));
  ASSERT_TRUE(state.has_staged_monster_rewards());
  ASSERT_TRUE(state.mark_monster_reward_ready(second.receipt_id));
  EXPECT_TRUE(state.has_staged_monster_rewards());
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, second.receipt_id, before,
                                        std::nullopt, {}).status,
            MonsterRewardCommitStatus::conflict);
  ASSERT_EQ(state.recover_staged_monster_rewards(1), 1U);
  EXPECT_FALSE(state.has_staged_monster_rewards());
  EXPECT_EQ(state.recover_staged_monster_rewards(1), 0U);
  const auto pending = state.pending_monster_rewards(owner->id, character.id, 1);
  ASSERT_EQ(pending.size(), 2U);
  EXPECT_EQ(pending[0], first);
  EXPECT_EQ(pending[1], second);
  EXPECT_TRUE(state.mark_monster_reward_ready(first.receipt_id));

  const auto new_epoch = state.acquire_monster_reward_owner_epoch();
  EXPECT_GT(new_epoch, old_epoch);
  state.adopt_monster_reward_owner_epoch(old_epoch);
  EXPECT_THROW(state.enqueue_monster_rewards({first}), std::runtime_error);
  EXPECT_THROW(state.mark_monster_reward_ready(first.receipt_id), std::runtime_error);
  EXPECT_THROW(state.recover_staged_monster_rewards(1), std::runtime_error);
  EXPECT_THROW(state.has_staged_monster_rewards(), std::runtime_error);
  EXPECT_THROW(state.pending_monster_rewards(owner->id, character.id, 1), std::runtime_error);
  EXPECT_THROW(state.commit_monster_reward(owner->id, character.id, 1, first.receipt_id, before,
                                           std::nullopt, {}), std::runtime_error);
  state.adopt_monster_reward_owner_epoch(new_epoch);
  EXPECT_TRUE(state.mark_monster_reward_ready(first.receipt_id));
}

TEST(Storage, MemoryMonsterRewardStagingAndEpochFence) {
  MemoryRepositories repositories;
  exercise_monster_reward_staging(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresMonsterRewardStagingAndEpochFence) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_monster_reward_staging(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void exercise_startup_reward_recovery_limit(const CharacterRepositories &repositories,
                                            const std::string &suffix) {
  Account account;
  account.name = "outbox_recovery_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Recovery" + suffix;
  character = repositories.characters->create(character);

  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  while (state.recover_staged_monster_rewards(kMonsterRewardRecoveryPageSize) != 0) {
  }
  const auto stage = [&](std::size_t number) {
    std::ostringstream event_id;
    event_id << std::hex << std::setw(32) << std::setfill('0') << 4096 + number;
    MonsterRewardReceipt receipt{owner->id, character.id, 1, event_id.str(), 9000 + number, 500,
                                 {9012, 0, 0, 0, 0.0F}, true};
    return state.enqueue_monster_rewards({receipt});
  };
  const auto recover = [&state](std::size_t limit) {
    return state.recover_staged_monster_rewards(limit);
  };
  const auto has_staged = [&state] {
    return state.has_staged_monster_rewards();
  };

  for (std::size_t index = 0; index < 5; ++index) {
    ASSERT_TRUE(stage(index));
  }
  EXPECT_EQ(recover_startup_rewards(2, 5, recover, has_staged), 5U);
  EXPECT_FALSE(state.has_staged_monster_rewards());

  for (std::size_t index = 5; index < 11; ++index) {
    ASSERT_TRUE(stage(index));
  }
  EXPECT_THROW(recover_startup_rewards(2, 5, recover, has_staged), std::runtime_error);
  EXPECT_TRUE(state.has_staged_monster_rewards());
  EXPECT_EQ(state.recover_staged_monster_rewards(2), 1U);
  EXPECT_FALSE(state.has_staged_monster_rewards());
}

TEST(Storage, MemoryMonsterRewardStartupRecoveryLimit) {
  MemoryRepositories repositories;
  exercise_startup_reward_recovery_limit(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresMonsterRewardStartupRecoveryLimit) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_startup_reward_recovery_limit(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(Storage, MemoryMonsterRewardReceiptsAreAtomicAndIdempotent) {
  MemoryRepositories repositories;
  exercise_monster_reward_receipts(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresMonsterRewardReceiptsAreAtomicAndIdempotent) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_monster_reward_receipts(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void exercise_monster_reward_backpressure(const CharacterRepositories &repositories,
                                         const std::string &suffix) {
  Account account;
  account.name = "outbox_limit_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Limit" + suffix;
  character = repositories.characters->create(character);

  const auto make_receipt = [&](std::size_t number) {
    std::ostringstream id;
    id << std::hex << std::setw(32) << std::setfill('0') << number + 1;
    return MonsterRewardReceipt{owner->id, character.id, 1, id.str(), number + 1, 500,
                                {9012, 0, 0, 0, 0.0F}, true};
  };
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  for (std::size_t index = 0; index < kMaximumPendingMonsterRewardsPerCharacter; ++index) {
    ASSERT_TRUE(state.enqueue_monster_rewards({make_receipt(index)})) << index;
  }
  EXPECT_TRUE(state.pending_monster_rewards(owner->id, character.id, 1).empty());
  EXPECT_TRUE(state.has_staged_monster_rewards());
  std::size_t recovered = 0;
  while (const auto count = state.recover_staged_monster_rewards(kMonsterRewardRecoveryPageSize)) {
    recovered += count;
  }
  EXPECT_EQ(recovered, kMaximumPendingMonsterRewardsPerCharacter);
  EXPECT_FALSE(state.has_staged_monster_rewards());
  const auto page = state.pending_monster_rewards(owner->id, character.id, 1);
  ASSERT_EQ(page.size(), kMonsterRewardPageSize);
  EXPECT_EQ(page.front(), make_receipt(0));
  EXPECT_EQ(page.back(), make_receipt(kMonsterRewardPageSize - 1));
  const auto overflow = make_receipt(kMaximumPendingMonsterRewardsPerCharacter);
  EXPECT_FALSE(state.enqueue_monster_rewards({overflow}));
  EXPECT_TRUE(state.enqueue_monster_rewards({make_receipt(0)}));

  const auto before = state.load_world_progress(owner->id, character.id, 1);
  ASSERT_EQ(state.commit_monster_reward(owner->id, character.id, 1, page.front().receipt_id,
                                       before, std::nullopt, {}).status,
            MonsterRewardCommitStatus::committed);
  EXPECT_TRUE(state.enqueue_monster_rewards({overflow}));
  const auto next_page = state.pending_monster_rewards(owner->id, character.id, 1);
  ASSERT_EQ(next_page.size(), kMonsterRewardPageSize);
  EXPECT_EQ(next_page.front(), make_receipt(1));
}

TEST(Storage, MemoryMonsterRewardBackpressureUsesOrderedPages) {
  MemoryRepositories repositories;
  exercise_monster_reward_backpressure(repositories.character_repositories(), "memory");
}

TEST(Storage, MemoryMonsterRewardEventLimitIsPerWorldAndDoesNotRejectReplays) {
  MemoryRepositories repositories;
  Character first;
  first.account_id = 1;
  first.world_id = 1;
  first.name = "LimitedWorld";
  first = repositories.characters->create(first);
  Character second;
  second.account_id = 1;
  second.world_id = 2;
  second.name = "OtherWorld";
  second = repositories.characters->create(second);
  repositories.database->monster_reward_event_counts[1] = kMaximumStoredMonsterRewardEvents;

  const MonsterRewardReceipt full{1, first.id, 1, std::string(32, '7'), 81, 500,
                                   {9012, 0, 0, 0, 0.0F}, true};
  const MonsterRewardReceipt other{1, second.id, 2, std::string(32, '8'), 82, 500,
                                    {9012, 0, 0, 0, 0.0F}, true};
  repositories.character_state->acquire_monster_reward_owner_epoch();
  EXPECT_FALSE(repositories.character_state->enqueue_monster_rewards({full}));
  ASSERT_TRUE(repositories.character_state->enqueue_monster_rewards({other}));
  ASSERT_TRUE(repositories.character_state->mark_monster_reward_ready(other.receipt_id));
  repositories.database->monster_reward_event_counts[2] = kMaximumStoredMonsterRewardEvents;
  EXPECT_TRUE(repositories.character_state->enqueue_monster_rewards({other}));
  EXPECT_EQ(repositories.character_state->pending_monster_rewards(1, second.id, 2).size(), 1U);
}

TEST(Storage, PostgresMonsterRewardBackpressureUsesOrderedPages) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_monster_reward_backpressure(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(Storage, PostgresMonsterRewardBlockedRecipientDoesNotBlockAnotherDefeat) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const std::string connection(connection_string);
  auto repositories = make_postgres_character_repositories(connection);
  Account account;
  account.name = "outbox_contention_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character first;
  first.account_id = owner->id;
  first.name = "OutboxBlock" + std::to_string(owner->id);
  first.slot = 0;
  first = repositories.characters->create(first);
  Character second;
  second.account_id = owner->id;
  second.name = "OutboxOther" + std::to_string(owner->id);
  second.slot = 1;
  second = repositories.characters->create(second);

  const MonsterRewardReceipt blocked{owner->id, first.id, 1, std::string(32, 'd'), 777, 500,
                                     {9012, 0, 0, 0, 0.0F}, true};
  const MonsterRewardReceipt other{owner->id, second.id, 1, std::string(32, 'e'), 778, 500,
                                    {9012, 0, 0, 0, 0.0F}, true};
  const auto owner_epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  pqxx::connection locked_connection(connection);
  pqxx::work locked_transaction(locked_connection);
  locked_transaction.exec("SELECT id FROM emulator_characters WHERE id=$1 FOR UPDATE",
                          pqxx::params{first.id});
  auto blocked_repository = make_postgres_character_repositories(
      connection + " application_name=chronicles_outbox_contention");
  blocked_repository.character_state->adopt_monster_reward_owner_epoch(owner_epoch);
  auto waiting = std::async(std::launch::async, [&] {
    return blocked_repository.character_state->enqueue_monster_rewards({blocked});
  });

  bool observed_wait = false;
  pqxx::connection monitor_connection(connection);
  for (int retry = 0; retry < 100 && !observed_wait; ++retry) {
    pqxx::read_transaction monitor(monitor_connection);
    const auto rows = monitor.exec(
        "SELECT count(*) FROM pg_stat_activity WHERE application_name="
        "'chronicles_outbox_contention' AND wait_event_type='Lock'");
    observed_wait = rows.front()[0].as<int>() != 0;
    if (!observed_wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  auto unrelated = std::async(std::launch::async, [&] {
    return repositories.character_state->enqueue_monster_rewards({other});
  });
  const bool unrelated_finished = unrelated.wait_for(std::chrono::seconds(3)) ==
                                  std::future_status::ready;
  locked_transaction.abort();
  const bool blocked_result = waiting.get();
  const bool unrelated_result = unrelated.get();
  EXPECT_TRUE(observed_wait);
  EXPECT_TRUE(unrelated_finished);
  EXPECT_TRUE(blocked_result);
  EXPECT_TRUE(unrelated_result);

  {
    pqxx::work transaction(monitor_connection);
    transaction.exec("UPDATE emulator_monster_reward_limits SET event_count=$1 WHERE world_id=1",
                     pqxx::params{kMaximumStoredMonsterRewardEvents});
    transaction.commit();
  }
  auto over_capacity = other;
  over_capacity.receipt_id = std::string(32, '9');
  ++over_capacity.credited_unit_id;
  EXPECT_FALSE(repositories.character_state->enqueue_monster_rewards({over_capacity}));
  EXPECT_TRUE(repositories.character_state->enqueue_monster_rewards({other}));
  {
    pqxx::work transaction(monitor_connection);
    const auto leaked = transaction.exec(
        "SELECT count(*) FROM emulator_monster_reward_events WHERE receipt_id=$1",
        pqxx::params{over_capacity.receipt_id});
    EXPECT_EQ(leaked.front()[0].as<int>(), 0);
    transaction.exec("UPDATE emulator_monster_reward_limits SET event_count="
                     "(SELECT count(*) FROM emulator_monster_reward_events WHERE world_id=1) "
                     "WHERE world_id=1");
    transaction.commit();
  }
}

TEST(Storage, PostgresMonsterRewardEpochTakeoverWaitsForOldTransactions) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const std::string connection(connection_string);
  auto old_repository = make_postgres_character_repositories(connection);
  auto new_repository = make_postgres_character_repositories(
      connection + " application_name=chronicles_reward_epoch_takeover");
  const auto old_epoch = old_repository.character_state->acquire_monster_reward_owner_epoch();

  pqxx::connection holding_connection(connection);
  pqxx::work holding_transaction(holding_connection);
  holding_transaction.exec("SELECT epoch FROM emulator_monster_reward_server_owner "
                           "WHERE singleton FOR SHARE");
  auto takeover = std::async(std::launch::async, [&] {
    return new_repository.character_state->acquire_monster_reward_owner_epoch();
  });

  bool observed_wait = false;
  pqxx::connection monitor_connection(connection);
  for (int retry = 0; retry < 100 && !observed_wait; ++retry) {
    pqxx::read_transaction monitor(monitor_connection);
    const auto rows = monitor.exec(
        "SELECT count(*) FROM pg_stat_activity WHERE application_name="
        "'chronicles_reward_epoch_takeover' AND wait_event_type='Lock'");
    observed_wait = rows.front()[0].as<int>() != 0;
    if (!observed_wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  const bool takeover_blocked = takeover.wait_for(std::chrono::milliseconds(20)) ==
                                std::future_status::timeout;
  holding_transaction.abort();
  const auto new_epoch = takeover.get();
  EXPECT_TRUE(observed_wait);
  EXPECT_TRUE(takeover_blocked);
  EXPECT_GT(new_epoch, old_epoch);
  EXPECT_THROW(old_repository.character_state->recover_staged_monster_rewards(1),
               std::runtime_error);
  EXPECT_NO_THROW(new_repository.character_state->recover_staged_monster_rewards(1));
}

TEST(Storage, PostgresMonsterRewardRollsBackProgressAndItemsOnMarkerFailure) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const std::string connection(connection_string);
  auto repositories = make_postgres_character_repositories(connection);
  Account account;
  account.name = "outbox_fault_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "OutboxFault" + std::to_string(owner->id);
  character = repositories.characters->create(character);
  const MonsterRewardReceipt receipt{owner->id, character.id, 1, std::string(32, 'f'), 779, 500,
                                      {9012, 0, 0, 0, 0.0F}, true};
  auto &state = *repositories.character_state;
  state.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(state.enqueue_monster_rewards({receipt}));
  ASSERT_TRUE(state.mark_monster_reward_ready(receipt.receipt_id));
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  after.gold += 10;
  CharacterItem item;
  item.cms_id = 123;
  item.inventory_slot = 100;
  ItemTrade trade;
  trade.granted_items.push_back(item);

  pqxx::connection database(connection);
  {
    pqxx::work transaction(database);
    transaction.exec(
        "CREATE FUNCTION reject_outbox_marker() RETURNS trigger LANGUAGE plpgsql AS $$ "
        "BEGIN RAISE EXCEPTION 'injected marker failure'; END $$");
    transaction.exec(
        "CREATE TRIGGER reject_outbox_marker BEFORE UPDATE OF processed "
        "ON emulator_monster_reward_receipts FOR EACH ROW EXECUTE FUNCTION reject_outbox_marker()");
    transaction.commit();
  }
  EXPECT_THROW(state.commit_monster_reward(owner->id, character.id, 1, receipt.receipt_id,
                                           before, after, trade), pqxx::sql_error);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), before);
  EXPECT_EQ(state.pending_monster_rewards(owner->id, character.id, 1).size(), 1U);
  const auto before_retry = repositories.characters->characters(owner->id, 1);
  ASSERT_EQ(before_retry.size(), 1U);
  EXPECT_TRUE(std::ranges::none_of(before_retry.front().items, [](const CharacterItem &entry) {
    return entry.inventory_slot == 100;
  }));
  {
    pqxx::work transaction(database);
    transaction.exec("DROP TRIGGER reject_outbox_marker ON emulator_monster_reward_receipts");
    transaction.exec("DROP FUNCTION reject_outbox_marker()");
    transaction.commit();
  }
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, receipt.receipt_id,
                                        before, after, trade).status,
            MonsterRewardCommitStatus::committed);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), after);
  EXPECT_EQ(state.commit_monster_reward(owner->id, character.id, 1, receipt.receipt_id,
                                        before, after, trade).status,
            MonsterRewardCommitStatus::already_processed);
}

void exercise_atomic_region_checkpoint(const CharacterRepositories &repositories,
                                        const std::string &suffix) {
  Account account;
  account.name = "region_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);

  Character character;
  character.account_id = owner->id;
  character.name = "Region" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const WorldPosition origin{9012, 10, 20, 30, 0.0F};
  const WorldPosition destination{9021, 40, 50, 60, 15.0F};
  ASSERT_TRUE(repositories.characters->save_world_position(owner->id, character.id, 1, origin));

  const auto initial = state.load_world_progress(owner->id, character.id, 1);
  auto running = initial;
  ++running.revision;
  running.trade_run.phase = TradeRunPhase::running;
  running.trade_run.vehicle_cms_id = 200;
  running.trade_run.wagon_position = origin;
  running.trade_run.wagon_health = 1000;
  running.trade_run.goods.push_back({1102, 1100, 1});
  ASSERT_TRUE(state.save_world_progress(owner->id, character.id, 1, initial, running));

  auto moved = running;
  ++moved.revision;
  moved.trade_run.wagon_position = destination;
  EXPECT_FALSE(state.commit_world_progress_and_position(owner->id + 1, character.id, 1,
                                                         running, moved, destination));
  ASSERT_TRUE(state.commit_world_progress_and_position(owner->id, character.id, 1,
                                                        running, moved, destination));
  EXPECT_FALSE(state.commit_world_progress_and_position(owner->id, character.id, 1,
                                                         running, moved, origin));
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), moved);
  const auto saved = repositories.characters->characters(owner->id, 1);
  ASSERT_EQ(saved.size(), 1U);
  ASSERT_TRUE(saved.front().world_position);
  EXPECT_EQ(*saved.front().world_position, destination);
  ASSERT_TRUE(moved.trade_run.wagon_position);
  EXPECT_EQ(*moved.trade_run.wagon_position, destination);
  EXPECT_EQ(moved.trade_run.goods, running.trade_run.goods);

  auto invalid = moved;
  ++invalid.revision;
  EXPECT_THROW(state.commit_world_progress_and_position(owner->id, character.id, 1, moved,
                                                         invalid, WorldPosition{}),
               std::invalid_argument);
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), moved);
}

TEST(Storage, MemoryRegionCheckpointCommitsPlayerAndWagonTogether) {
  MemoryRepositories repositories;
  exercise_atomic_region_checkpoint(repositories.character_repositories(), "memory");
}

TEST(Storage, MemoryWorldEntryFenceRejectsAnEarlierCheckpoint) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 7;
  character.world_id = 2;
  character.name = "FencedEntry";
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(7, character.id, 2);
  auto after = before;
  ++after.revision;
  const WorldPosition destination{9021, 40, 50, 60, 15.0F};

  EXPECT_FALSE(state.fence_world_entry(8, character.id, 2));
  ASSERT_TRUE(state.fence_world_entry(7, character.id, 2));
  EXPECT_FALSE(state.commit_world_progress_and_position(7, character.id, 2, before, after,
                                                         destination));
  EXPECT_EQ(state.load_world_progress(7, character.id, 2).revision, after.revision);
}

TEST(Storage, PostgresWorldEntryFenceWaitsAndRejectsAnEarlierCheckpoint) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "entry_fence_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "EntryFence" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);

  pqxx::connection connection(connection_string);
  pqxx::work pending(connection);
  pending.exec("UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
               "WHERE id=$1", pqxx::params{character.id});
  std::promise<void> started;
  auto ready = started.get_future();
  auto result = std::async(std::launch::async, [&] {
    started.set_value();
    return state.fence_world_entry(owner->id, character.id, 1);
  });
  ready.wait();
  EXPECT_EQ(result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  pending.commit();
  ASSERT_TRUE(result.get());
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1).revision, before.revision + 2);

  auto stale = before;
  ++stale.revision;
  const WorldPosition destination{9021, 40, 50, 60, 15.0F};
  EXPECT_FALSE(state.commit_world_progress_and_position(owner->id, character.id, 1, before, stale,
                                                         destination));
}

TEST(Storage, PostgresWorldEntryFenceFailsClosedOnLockTimeout) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "entry_timeout_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "EntryTimeout" + suffix;
  character = repositories.characters->create(character);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);

  pqxx::connection connection(connection_string);
  pqxx::work pending(connection);
  pending.exec("UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
               "WHERE id=$1", pqxx::params{character.id});
  auto result = std::async(std::launch::async, [&] {
    try {
      return state.fence_world_entry(owner->id, character.id, 1);
    } catch (const pqxx::sql_error &) {
      return false;
    }
  });
  EXPECT_EQ(result.wait_for(std::chrono::seconds(7)), std::future_status::ready);
  if (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    pending.abort();
    result.wait();
    FAIL() << "The entry fence did not honor its PostgreSQL lock timeout";
  }
  EXPECT_FALSE(result.get());
  pending.abort();
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), before);
}

TEST(Storage, PostgresRegionCheckpointCommitsPlayerAndWagonTogether) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_atomic_region_checkpoint(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(Storage, PostgresRegionCheckpointRollsBackWhenPositionWriteFails) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const auto repositories = make_postgres_character_repositories(connection_string);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "region_fault_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Fault" + suffix;
  character = repositories.characters->create(character);
  const WorldPosition origin{9012, 10, 20, 30, 0.0F};
  const WorldPosition destination{9021, 40, 50, 60, 15.0F};
  ASSERT_TRUE(repositories.characters->save_world_position(owner->id, character.id, 1, origin));
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  auto after = before;
  ++after.revision;
  after.trade_run.phase = TradeRunPhase::running;
  after.trade_run.vehicle_cms_id = 200;
  after.trade_run.wagon_position = destination;
  after.trade_run.wagon_health = 1000;
  after.trade_run.goods.push_back({1102, 1100, 1});

  pqxx::connection connection(connection_string);
  {
    pqxx::work setup(connection);
    setup.exec("CREATE FUNCTION test_reject_region_position() RETURNS trigger LANGUAGE plpgsql "
               "AS $$ BEGIN RAISE EXCEPTION 'injected position failure'; END $$");
    setup.exec("CREATE TRIGGER test_reject_region_position BEFORE UPDATE OF world_map_id "
               "ON emulator_characters FOR EACH ROW EXECUTE FUNCTION test_reject_region_position()");
    setup.commit();
  }
  EXPECT_THROW(state.commit_world_progress_and_position(owner->id, character.id, 1, before,
                                                         after, destination),
               pqxx::sql_error);
  {
    pqxx::work cleanup(connection);
    cleanup.exec("DROP TRIGGER test_reject_region_position ON emulator_characters");
    cleanup.exec("DROP FUNCTION test_reject_region_position()");
    cleanup.commit();
  }
  EXPECT_EQ(state.load_world_progress(owner->id, character.id, 1), before);
  const auto saved = repositories.characters->characters(owner->id, 1);
  ASSERT_EQ(saved.size(), 1U);
  ASSERT_TRUE(saved.front().world_position);
  EXPECT_EQ(*saved.front().world_position, origin);
}

TEST(Storage, CharacterItemsAndProgressRemainOwnerScoped) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 7;
  character.world_id = 2;
  character.name = "PersistentWild";
  character.class_id = kWildCharacterClass;

  const auto created = repositories.characters->create(character);
  ASSERT_EQ(created.items.size(), kWildStarterGear.size());
  EXPECT_EQ(created.items.front().cms_id, 11102302);
  EXPECT_EQ(created.items.front().equip_slot, 0);
  EXPECT_NE(created.items.front().id, 0);

  const WorldPosition position{8000, -100, 200, 300, 75.0F};
  EXPECT_FALSE(repositories.characters->save_world_position(8, created.id, 2, position));
  EXPECT_FALSE(repositories.characters->save_progress(8, created.id, 2, 4, 900));
  EXPECT_TRUE(repositories.characters->save_world_position(7, created.id, 2, position));
  EXPECT_TRUE(repositories.characters->save_progress(7, created.id, 2, 4, 900));

  const auto loaded = repositories.characters->characters(7, 2);
  ASSERT_EQ(loaded.size(), 1);
  ASSERT_TRUE(loaded.front().world_position);
  EXPECT_EQ(loaded.front().world_position->x_mm, -100);
  EXPECT_EQ(loaded.front().level, 4);
  EXPECT_EQ(loaded.front().experience, 900);
  ASSERT_EQ(loaded.front().items.size(), kWildStarterGear.size());
  EXPECT_EQ(loaded.front().items.front().id, created.items.front().id);

  CharacterItem new_item;
  new_item.cms_id = 12345;
  new_item.inventory_slot = 9;
  EXPECT_FALSE(repositories.character_state->grant_item(8, created.id, 2, new_item));

  const auto granted = repositories.character_state->grant_item(7, created.id, 2, new_item);
  ASSERT_TRUE(granted);
  EXPECT_NE(granted->id, created.items.front().id);
  EXPECT_FALSE(repositories.character_state->grant_item(7, created.id, 2, new_item));

  auto changed_item = *granted;
  changed_item.quantity = 2;
  changed_item.inventory_slot = 10;
  changed_item.equip_slot = 1;
  EXPECT_FALSE(repositories.character_state->update_item(8, created.id, 2, changed_item));
  EXPECT_TRUE(repositories.character_state->update_item(7, created.id, 2, changed_item));
  EXPECT_FALSE(repositories.character_state->remove_item(8, created.id, 2, changed_item.id));
  EXPECT_TRUE(repositories.character_state->remove_item(7, created.id, 2, changed_item.id));
  EXPECT_FALSE(repositories.character_state->remove_item(7, created.id, 2, changed_item.id));

  EXPECT_THROW(repositories.characters->save_progress(7, created.id, 2, 0, 900),
               std::invalid_argument);
  EXPECT_THROW(repositories.characters->save_world_position(7, created.id, 2, WorldPosition{}),
               std::invalid_argument);
}

// Repeatable and abandonable quests: a reset is accepted only with a higher attempt, and job
// board periods and play records never move backwards.
TEST(Storage, QuestRestartsNeedAHigherAttemptAndCountersNeverMoveBackwards) {
  using detail::validate_world_progress_transition;
  WorldProgress before;
  before.revision = 4;
  QuestProgress completed;
  completed.complete = true;
  completed.success_count = 1;
  completed.objectives = {{1, 1}};
  completed.credited_kills = {77};
  before.quests.emplace(601, completed);
  before.job_boards.emplace(101, JobBoardRecord{1500, 1});
  before.play_records.emplace(30, 2);

  auto restarted = before;
  restarted.revision = 5;
  QuestProgress tombstone;
  tombstone.attempt = 1;
  tombstone.success_count = 1;
  tombstone.abandoned = true;
  tombstone.objectives = {{1, 0}};
  restarted.quests.at(601) = tombstone;
  EXPECT_NO_THROW(validate_world_progress_transition(before, restarted));

  auto same_attempt = restarted;
  same_attempt.quests.at(601).attempt = 0;
  EXPECT_THROW(validate_world_progress_transition(before, same_attempt), std::invalid_argument);

  auto abandoned_complete = restarted;
  abandoned_complete.quests.at(601).complete = true;
  EXPECT_THROW(validate_world_progress_transition(before, abandoned_complete),
               std::invalid_argument);

  auto earlier_period = restarted;
  earlier_period.job_boards.at(101) = JobBoardRecord{1499, 5};
  EXPECT_THROW(validate_world_progress_transition(before, earlier_period), std::invalid_argument);

  auto fewer_refreshes = restarted;
  fewer_refreshes.job_boards.at(101) = JobBoardRecord{1500, 0};
  EXPECT_THROW(validate_world_progress_transition(before, fewer_refreshes),
               std::invalid_argument);

  auto next_period = restarted;
  next_period.job_boards.at(101) = JobBoardRecord{1501, 0};
  EXPECT_NO_THROW(validate_world_progress_transition(before, next_period));

  auto fewer_kills = restarted;
  fewer_kills.play_records.at(30) = 1;
  EXPECT_THROW(validate_world_progress_transition(before, fewer_kills), std::invalid_argument);
}

TEST(Storage, SupportedClassesStartWithVerifiedEquippedOutfits) {
  MemoryRepositories repositories;
  for (const int class_id : {1, 2, 3, 5}) {
    Character character;
    character.account_id = class_id;
    character.name = "Outfit" + std::to_string(class_id);
    character.class_id = class_id;

    const auto created = repositories.characters->create(character);
    const auto gear = starter_gear_for_class(class_id);
    ASSERT_EQ(created.items.size(), gear.size());
    EXPECT_TRUE(repositories.characters->characters(class_id + 100, character.world_id).empty());

    for (std::size_t index = 0; index < gear.size(); ++index) {
      EXPECT_EQ(created.items[index].cms_id, gear[index].cms_id);
      EXPECT_EQ(created.items[index].equip_slot, gear[index].slot_index);
      EXPECT_NE(created.items[index].id, 0);
    }

    const auto reloaded = repositories.characters->characters(class_id, character.world_id);
    ASSERT_EQ(reloaded.size(), 1);
    EXPECT_EQ(reloaded.front().items.front().id, created.items.front().id);
  }

  for (const int class_id : {0, 4, 6, 7, 8, 9, 10, 11}) {
    EXPECT_TRUE(starter_gear_for_class(class_id).empty()) << class_id;
  }
}

TEST(Storage, StarterBackfillPreservesExistingEquipmentAndDoesNotDuplicateItems) {
  Character existing;
  existing.class_id = 1;
  existing.items = {
      CharacterItem{1, 900001, 0, 0},
      CharacterItem{2, 10130102, 1, 7},
      CharacterItem{3, 900002, 2, 8},
  };

  const auto missing = missing_starter_items(existing);
  ASSERT_EQ(missing.size(), 2);
  EXPECT_EQ(missing[0].cms_id, 10130104);
  EXPECT_EQ(missing[0].inventory_slot, 3);
  EXPECT_EQ(missing[0].equip_slot, 9);
  EXPECT_EQ(missing[1].cms_id, 11700201);
  EXPECT_EQ(missing[1].inventory_slot, 4);
  EXPECT_EQ(missing[1].equip_slot, 24);

  existing.items.insert(existing.items.end(), missing.begin(), missing.end());
  EXPECT_TRUE(missing_starter_items(existing).empty());
}

void exercise_hersander_token(const CharacterRepositories &repositories,
                              const std::string &suffix) {
  constexpr std::int64_t kLimit = 9000000000000000;
  Account owner;
  owner.name = "hersander_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);
  Character character;
  character.account_id = owned_account->id;
  character.name = "Hers" + suffix;
  character = repositories.characters->create(character);
  const auto load = [&] {
    return repositories.character_state->load_world_progress(owned_account->id, character.id, 1);
  };

  const auto before = load();
  auto first = before;
  first.revision = before.revision + 1;
  first.hersander_token = 42;
  ASSERT_TRUE(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       before, first));
  EXPECT_EQ(load().hersander_token, 42);

  auto competing = before;
  competing.revision = before.revision + 1;
  competing.hersander_token = 77;
  EXPECT_FALSE(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       before, competing));
  EXPECT_EQ(load().hersander_token, 42);

  auto at_limit = first;
  at_limit.revision = first.revision + 1;
  at_limit.hersander_token = kLimit;
  ASSERT_TRUE(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       first, at_limit));
  EXPECT_EQ(load().hersander_token, kLimit);

  auto above = at_limit;
  above.revision = at_limit.revision + 1;
  above.hersander_token = kLimit + 1;
  EXPECT_THROW(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       at_limit, above),
      std::invalid_argument);
  auto negative = at_limit;
  negative.revision = at_limit.revision + 1;
  negative.hersander_token = -1;
  EXPECT_THROW(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       at_limit, negative),
      std::invalid_argument);
  const auto reloaded = load();
  EXPECT_EQ(reloaded.hersander_token, kLimit);
  EXPECT_EQ(reloaded.revision, at_limit.revision);

  // Gold shares the catalog limit: the limit is accepted, limit+1 is rejected unchanged.
  auto gold_at_limit = at_limit;
  gold_at_limit.revision = at_limit.revision + 1;
  gold_at_limit.gold = kLimit;
  ASSERT_TRUE(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       at_limit, gold_at_limit));
  EXPECT_EQ(load(), gold_at_limit);

  auto gold_above = gold_at_limit;
  gold_above.revision = gold_at_limit.revision + 1;
  gold_above.gold = kLimit + 1;
  EXPECT_THROW(
      repositories.character_state->save_world_progress(owned_account->id, character.id, 1,
                                                       gold_at_limit, gold_above),
      std::invalid_argument);
  EXPECT_EQ(load(), gold_at_limit);

  // Death and revival never name the token: a nonzero balance survives both and reloads intact.
  ASSERT_TRUE(
      repositories.character_state->commit_world_death(owned_account->id, character.id, 1,
                                                      gold_at_limit.revision, {}));
  const auto dead = load();
  EXPECT_TRUE(dead.dead);
  EXPECT_EQ(dead.hersander_token, kLimit);
  EXPECT_EQ(dead.gold, kLimit);

  const WorldPosition shrine{8000, 100, 200, 300, 45.0F};
  ASSERT_TRUE(
      repositories.character_state->commit_world_revival(owned_account->id, character.id, 1,
                                                        dead.revision, shrine));
  const auto alive = load();
  EXPECT_FALSE(alive.dead);
  EXPECT_EQ(alive.revision, dead.revision + 1);
  EXPECT_EQ(alive.hersander_token, kLimit);
  EXPECT_EQ(alive.gold, kLimit);
}

TEST(Storage, MemoryStoreHersanderTokenBoundsAndRevisionConflict) {
  MemoryRepositories repositories;
  exercise_hersander_token(repositories.character_repositories(), "memory");
}

TEST(Storage, PostgresHersanderTokenBoundsAndRevisionConflict) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_hersander_token(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// Schema version 11: the gold column itself rejects values above the catalog limit, independent
// of the C++ validation. Needs the migrated disposable cluster (never port 5433).
TEST(Storage, PostgresGoldColumnRejectsValuesAboveCatalogLimit) {
  constexpr std::int64_t kGoldLimit = 9000000000000000;
  const char *connection_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_url) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  const auto repositories = make_postgres_character_repositories(connection_url);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "gold_check_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);
  Character character;
  character.account_id = owned_account->id;
  character.name = "Gold" + suffix.substr(suffix.size() - 8);
  character = repositories.characters->create(character);

  const auto write_gold = [&](std::int64_t gold) {
    pqxx::connection connection(connection_url);
    pqxx::work transaction(connection);
    transaction.exec("UPDATE emulator_characters SET gold=$1 WHERE id=$2",
                     pqxx::params{gold, character.id});
    transaction.commit();
  };
  const auto read_gold = [&] {
    pqxx::connection connection(connection_url);
    pqxx::read_transaction transaction(connection);
    return transaction
        .exec("SELECT gold FROM emulator_characters WHERE id=$1", pqxx::params{character.id})
        .front()[0]
        .as<std::int64_t>();
  };

  write_gold(kGoldLimit);
  EXPECT_EQ(read_gold(), kGoldLimit);
  EXPECT_THROW(write_gold(kGoldLimit + 1), pqxx::check_violation);
  EXPECT_EQ(read_gold(), kGoldLimit);
  EXPECT_THROW(write_gold(-1), pqxx::check_violation);
  EXPECT_EQ(read_gold(), kGoldLimit);
}

// One polish step must move the item row, the material stackable and gold together under the
// character revision. Runs against both backends.
void exercise_item_polish(const CharacterRepositories &repositories,
                          const std::string &suffix) {
  Account owner;
  owner.name = "polish_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);
  Character character;
  character.account_id = owned_account->id;
  character.name = "Pol" + suffix.substr(suffix.size() - 8);
  character = repositories.characters->create(character);
  const auto account_id = owned_account->id;

  const auto weapon =
      repositories.character_state->grant_item(account_id, character.id, 1,
                                               CharacterItem{0, 13102102, 600, std::nullopt, 1,
                                                             9908, 3});
  ASSERT_TRUE(weapon);
  EXPECT_EQ(weapon->polish_step, 0);

  const auto load_progress = [&] {
    return repositories.character_state->load_world_progress(account_id, character.id, 1);
  };
  const auto load_weapon = [&] {
    const auto characters = repositories.characters->characters(account_id, 1);
    const auto &items = characters.front().items;
    return *std::ranges::find_if(items, [&](const CharacterItem &item) {
      return item.id == weapon->id;
    });
  };
  // A fully spent stack may be stored as 0 or absent; both read as zero here.
  const auto material_count = [&] {
    const auto stacks = load_progress().stackables;
    const auto found = stacks.find(90211101);
    return found == stacks.end() ? 0 : found->second;
  };
  const auto funded = load_progress();
  auto stocked = funded;
  stocked.revision = funded.revision + 1;
  stocked.gold = 5000;
  stocked.stackables[90211101] = 2;
  ASSERT_TRUE(repositories.character_state->save_world_progress(account_id, character.id, 1,
                                                              funded, stocked));

  const ItemPolishChange first{weapon->id, weapon->cms_id, 0, 90211101, 1, 1000};
  const auto revision = load_progress().revision;
  ASSERT_TRUE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                             revision, first));
  EXPECT_EQ(load_weapon().polish_step, 1);
  EXPECT_EQ(load_weapon().durability, 9908);
  EXPECT_EQ(load_progress().gold, 4000);
  EXPECT_EQ(material_count(), 1);
  EXPECT_EQ(load_progress().revision, revision + 1);

  // A retry carrying the old revision (or the old step) changes nothing: one charge only.
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision, first));
  const ItemPolishChange stale_step{weapon->id, weapon->cms_id, 0, 90211101, 1, 1000};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 1, stale_step));
  EXPECT_EQ(load_weapon().polish_step, 1);
  EXPECT_EQ(load_progress().gold, 4000);
  EXPECT_EQ(material_count(), 1);
  EXPECT_EQ(load_progress().revision, revision + 1);

  // Not enough material or gold, or an unknown item: refused with no partial change.
  const ItemPolishChange short_material{weapon->id, weapon->cms_id, 1, 90211101, 2, 1425};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 1, short_material));
  const ItemPolishChange short_gold{weapon->id, weapon->cms_id, 1, 90211101, 1, 4001};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 1, short_gold));
  const ItemPolishChange unknown_item{weapon->id + 1000000, weapon->cms_id, 1, 90211101, 1, 10};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 1, unknown_item));
  EXPECT_EQ(load_weapon().polish_step, 1);
  EXPECT_EQ(load_progress().gold, 4000);
  EXPECT_EQ(material_count(), 1);
  EXPECT_EQ(load_progress().revision, revision + 1);

  // The exact remaining balance is accepted and leaves a zero stack and zero gold.
  const ItemPolishChange exact{weapon->id, weapon->cms_id, 1, 90211101, 1, 4000};
  ASSERT_TRUE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                             revision + 1, exact));
  EXPECT_EQ(load_weapon().polish_step, 2);
  EXPECT_EQ(load_progress().gold, 0);
  EXPECT_EQ(material_count(), 0);

  // Another account cannot polish this character's item.
  const ItemPolishChange foreign{weapon->id, weapon->cms_id, 2, 90211101, 1, 0};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id + 1000000, character.id,
                                                              1, revision + 2, foreign));
}

TEST(Storage, MemoryStoreItemPolishIsAtomicAndRetrySafe) {
  MemoryRepositories repositories;
  exercise_item_polish(
      repositories.character_repositories(),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// Fail keeps the step, destruction deletes the unequipped item, and both charge the stone, the
// optional additive and gold under one revision. Runs against both backends.
void exercise_item_polish_outcomes(const CharacterRepositories &repositories,
                                   const std::string &suffix) {
  constexpr int kStone = 90211101;
  constexpr int kAdditive = 90552001;

  Account owner;
  owner.name = "polish_outcome_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);
  Character character;
  character.account_id = owned_account->id;
  character.name = "Out" + suffix.substr(suffix.size() - 8);
  character = repositories.characters->create(character);
  const auto account_id = owned_account->id;

  const auto kept =
      repositories.character_state->grant_item(account_id, character.id, 1,
                                               CharacterItem{0, 13102102, 600, std::nullopt, 1,
                                                             9908, 3});
  const auto doomed =
      repositories.character_state->grant_item(account_id, character.id, 1,
                                               CharacterItem{0, 11102302, 601, std::nullopt, 1,
                                                             10000, 0});
  // Worn in EX5EquipSlotType_ArmorSuit (20).
  const auto equipped =
      repositories.character_state->grant_item(account_id, character.id, 1,
                                               CharacterItem{0, 12401201, 602, 20, 1, 10000, 1});
  ASSERT_TRUE(kept);
  ASSERT_TRUE(doomed);
  ASSERT_TRUE(equipped);

  const auto load_progress = [&] {
    return repositories.character_state->load_world_progress(account_id, character.id, 1);
  };
  const auto find_item = [&](std::int64_t item_id) -> std::optional<CharacterItem> {
    const auto characters = repositories.characters->characters(account_id, 1);
    for (const auto &item : characters.front().items) {
      if (item.id == item_id) {
        return item;
      }
    }
    return std::nullopt;
  };
  const auto stack = [&](int cms_id) {
    const auto stacks = load_progress().stackables;
    const auto found = stacks.find(cms_id);
    return found == stacks.end() ? 0 : found->second;
  };

  const auto funded = load_progress();
  auto stocked = funded;
  stocked.revision = funded.revision + 1;
  stocked.gold = 5000;
  stocked.stackables[kStone] = 4;
  stocked.stackables[kAdditive] = 1;
  ASSERT_TRUE(repositories.character_state->save_world_progress(account_id, character.id, 1,
                                                              funded, stocked));
  const auto revision = load_progress().revision;

  // Fail: the step stays 0, but the stone, the additive and gold are charged.
  const ItemPolishChange failed{kept->id, kept->cms_id, 0,         kStone, 1, 1000,
                                kAdditive, PolishOutcome::fail};
  ASSERT_TRUE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                             revision, failed));
  EXPECT_EQ(find_item(kept->id)->polish_step, 0);
  EXPECT_EQ(stack(kStone), 3);
  EXPECT_EQ(stack(kAdditive), 0);
  EXPECT_EQ(load_progress().gold, 4000);
  EXPECT_EQ(load_progress().revision, revision + 1);

  // The spent additive refuses the next attempt that names it, with no partial change.
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 1, failed));
  EXPECT_EQ(stack(kStone), 3);
  EXPECT_EQ(load_progress().gold, 4000);

  // Destruction deletes the unequipped item and charges as usual.
  const ItemPolishChange destroyed{doomed->id, doomed->cms_id, 0, kStone, 2, 500,
                                   0,          PolishOutcome::destruction};
  ASSERT_TRUE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                             revision + 1, destroyed));
  EXPECT_FALSE(find_item(doomed->id));
  EXPECT_EQ(stack(kStone), 1);
  EXPECT_EQ(load_progress().gold, 3500);
  EXPECT_EQ(load_progress().revision, revision + 2);

  // An equipped item is never destroyed: the whole attempt is refused.
  const ItemPolishChange equipped_destruction{equipped->id, equipped->cms_id, 0, kStone, 1, 0,
                                              0,            PolishOutcome::destruction};
  EXPECT_FALSE(repositories.character_state->apply_item_polish(account_id, character.id, 1,
                                                              revision + 2,
                                                              equipped_destruction));
  EXPECT_TRUE(find_item(equipped->id));
  EXPECT_EQ(stack(kStone), 1);
  EXPECT_EQ(load_progress().revision, revision + 2);
}

TEST(Storage, MemoryStoreItemPolishOutcomesChargeEveryAttempt) {
  MemoryRepositories repositories;
  exercise_item_polish_outcomes(
      repositories.character_repositories(),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// Needs the disposable cluster migrated to schema version 12 (never port 5433).
TEST(Storage, PostgresItemPolishOutcomesChargeEveryAttempt) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_item_polish_outcomes(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// Needs the disposable cluster migrated to schema version 12 (never port 5433).
TEST(Storage, PostgresItemPolishRoundTripIsAtomicAndRetrySafe) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_item_polish(
      make_postgres_character_repositories(connection),
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(Storage, PostgresEquipmentSwapCommitsAndFailedSwapRollsBack) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  const auto repositories = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "equip_owner_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);

  Character character;
  character.account_id = owned_account->id;
  character.name = "Equip" + suffix;
  character = repositories.characters->create(character);
  const auto initial_weapon = character.items.front();
  ASSERT_EQ(initial_weapon.equip_slot, 0);

  CharacterItem replacement;
  replacement.cms_id = 11102302;
  replacement.inventory_slot = 10;
  const auto granted =
      repositories.character_state->grant_item(owned_account->id, character.id, 1, replacement);
  ASSERT_TRUE(granted);
  replacement = *granted;
  const auto initial_progress =
      repositories.character_state->load_world_progress(owned_account->id, character.id, 1);

  auto vacated_weapon = initial_weapon;
  vacated_weapon.equip_slot.reset();
  replacement.equip_slot = 0;
  ASSERT_TRUE(repositories.character_state->update_items(owned_account->id, character.id, 1,
                                                       initial_progress.revision,
                                                       {replacement, vacated_weapon}));
  const auto swapped_progress =
      repositories.character_state->load_world_progress(owned_account->id, character.id, 1);
  EXPECT_EQ(swapped_progress.revision, initial_progress.revision + 1);
  EXPECT_FALSE(repositories.character_state->update_items(owned_account->id, character.id, 1,
                                                        initial_progress.revision,
                                                        {replacement, vacated_weapon}));

  const auto reconnected = make_postgres_character_repositories(connection);
  const auto reloaded = reconnected.characters->characters(owned_account->id, 1);
  ASSERT_EQ(reloaded.size(), 1);
  const auto find_item = [](const Character &source, std::int64_t id) {
    return std::find_if(source.items.begin(), source.items.end(),
                        [id](const CharacterItem &item) { return item.id == id; });
  };
  const auto equipped = find_item(reloaded.front(), replacement.id);
  const auto unequipped = find_item(reloaded.front(), initial_weapon.id);
  ASSERT_NE(equipped, reloaded.front().items.end());
  ASSERT_NE(unequipped, reloaded.front().items.end());
  EXPECT_EQ(equipped->equip_slot, 0);
  EXPECT_FALSE(unequipped->equip_slot);

  Account stranger;
  stranger.name = "equip_other_" + suffix;
  stranger.salt = "test-salt";
  stranger.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(stranger));
  const auto other_account = repositories.accounts->account(stranger.name);
  ASSERT_TRUE(other_account);
  Character other_character;
  other_character.account_id = other_account->id;
  other_character.name = "Other" + suffix;
  other_character = repositories.characters->create(other_character);

  auto foreign_weapon = other_character.items.front();
  foreign_weapon.equip_slot = 0;
  auto released_replacement = replacement;
  released_replacement.equip_slot.reset();
  EXPECT_FALSE(repositories.character_state->update_items(owned_account->id, character.id, 1,
                                                        swapped_progress.revision,
                                                        {released_replacement, foreign_weapon}));

  const auto after_rejected_swap = reconnected.characters->characters(owned_account->id, 1);
  ASSERT_EQ(after_rejected_swap.size(), 1);
  const auto still_equipped = find_item(after_rejected_swap.front(), replacement.id);
  ASSERT_NE(still_equipped, after_rejected_swap.front().items.end());
  EXPECT_EQ(still_equipped->equip_slot, 0);

  auto conflicting_weapon = vacated_weapon;
  conflicting_weapon.equip_slot = 0;
  EXPECT_FALSE(repositories.character_state->update_items(owned_account->id, character.id, 1,
                                                        swapped_progress.revision,
                                                        {conflicting_weapon}));
  const auto after_conflict = reconnected.characters->characters(owned_account->id, 1);
  ASSERT_EQ(after_conflict.size(), 1);
  const auto original = find_item(after_conflict.front(), initial_weapon.id);
  ASSERT_NE(original, after_conflict.front().items.end());
  EXPECT_FALSE(original->equip_slot);
  EXPECT_EQ(reconnected.character_state->load_world_progress(owned_account->id, character.id, 1),
            swapped_progress);
}

TEST(Storage, PostgresDiscardBatchCommitsOrRollsBack) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  const auto repositories = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "discard_owner_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);

  Character character;
  character.account_id = owned_account->id;
  character.name = "Discard" + suffix;
  character = repositories.characters->create(character);
  CharacterItem bag_item;
  bag_item.cms_id = 90001;
  bag_item.inventory_slot = 10;
  const auto granted =
      repositories.character_state->grant_item(owned_account->id, character.id, 1, bag_item);
  ASSERT_TRUE(granted);

  auto initial =
      repositories.character_state->load_world_progress(owned_account->id, character.id, 1);
  auto stocked = initial;
  stocked.revision = initial.revision + 1;
  stocked.stackables[90003] = 5;
  ASSERT_TRUE(repositories.character_state->save_world_progress(owned_account->id, character.id,
                                                              1, initial, stocked));

  Account stranger;
  stranger.name = "discard_other_" + suffix;
  stranger.salt = "test-salt";
  stranger.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(stranger));
  const auto other_account = repositories.accounts->account(stranger.name);
  ASSERT_TRUE(other_account);
  Character foreign;
  foreign.account_id = other_account->id;
  foreign.name = "Foreign" + suffix;
  foreign = repositories.characters->create(foreign);
  ASSERT_FALSE(foreign.items.empty());

  EXPECT_FALSE(repositories.character_state->discard_items(
      owned_account->id, character.id, 1, stocked.revision,
      {granted->id, foreign.items.front().id}, {{90003, 2}}));
  const auto reconnected = make_postgres_character_repositories(connection);
  const auto after_failure = reconnected.characters->characters(owned_account->id, 1);
  ASSERT_EQ(after_failure.size(), 1);
  EXPECT_TRUE(std::ranges::any_of(after_failure.front().items, [&](const CharacterItem &item) {
    return item.id == granted->id;
  }));
  EXPECT_EQ(reconnected.character_state->load_world_progress(owned_account->id, character.id, 1),
            stocked);

  ASSERT_TRUE(repositories.character_state->discard_items(owned_account->id, character.id, 1,
                                                        stocked.revision, {granted->id},
                                                        {{90003, 2}}));
  const auto after_commit = reconnected.characters->characters(owned_account->id, 1);
  ASSERT_EQ(after_commit.size(), 1);
  EXPECT_FALSE(std::ranges::any_of(after_commit.front().items, [&](const CharacterItem &item) {
    return item.id == granted->id;
  }));
  const auto progress =
      reconnected.character_state->load_world_progress(owned_account->id, character.id, 1);
  EXPECT_EQ(progress.revision, stocked.revision + 1);
  EXPECT_EQ(progress.stackables.at(90003), 3);
}

TEST(Storage, DeathAndRevivalCommitOnceWithAtomicEquippedWear) {
  MemoryRepositories repositories;
  Character owner;
  owner.account_id = 7;
  owner.world_id = 2;
  owner.name = "DeathTransaction";
  owner.class_id = kWildCharacterClass;
  const auto character = repositories.characters->create(owner);
  ASSERT_GE(character.items.size(), 2);
  const auto first = character.items[0];
  const auto second = character.items[1];
  ASSERT_TRUE(first.equip_slot);
  ASSERT_TRUE(second.equip_slot);

  const EquippedDurabilityChange first_wear{
      first.id, first.cms_id, *first.equip_slot, first.durability, first.durability - 300};
  const EquippedDurabilityChange second_wear{
      second.id, second.cms_id, *second.equip_slot, second.durability, second.durability - 60};
  const auto before =
      repositories.character_state->load_world_progress(7, character.id, 2);
  EXPECT_FALSE(before.dead);

  auto invalid_wear = second_wear;
  invalid_wear.cms_id += 1;
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              before.revision,
                                                              {first_wear, invalid_wear}));
  EXPECT_FALSE(repositories.character_state->commit_world_death(8, character.id, 2,
                                                              before.revision,
                                                              {first_wear, second_wear}));
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              before.revision,
                                                              {first_wear, first_wear}));
  auto increasing_wear = first_wear;
  increasing_wear.durability = increasing_wear.expected_durability + 1;
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              before.revision,
                                                              {increasing_wear}));
  invalid_wear = second_wear;
  invalid_wear.equip_slot = *first.equip_slot;
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              before.revision,
                                                              {invalid_wear}));
  EXPECT_EQ(repositories.character_state->load_world_progress(7, character.id, 2), before);
  EXPECT_EQ(repositories.characters->characters(7, 2).front().items[0].durability,
            first.durability);

  // Legacy item updates do not advance the gameplay revision: compare old durability too.
  auto changed_second = second;
  changed_second.durability -= 10;
  ASSERT_TRUE(repositories.character_state->update_item(7, character.id, 2, changed_second));
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              before.revision,
                                                              {first_wear, second_wear}));
  EXPECT_EQ(repositories.character_state->load_world_progress(7, character.id, 2), before);
  EXPECT_EQ(repositories.characters->characters(7, 2).front().items[0].durability,
            first.durability);

  invalid_wear = second_wear;
  invalid_wear.expected_durability = changed_second.durability;
  invalid_wear.durability = changed_second.durability - 60;
  ASSERT_TRUE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                             before.revision,
                                                             {first_wear, invalid_wear}));
  const auto dead = repositories.character_state->load_world_progress(7, character.id, 2);
  EXPECT_TRUE(dead.dead);
  EXPECT_EQ(dead.revision, before.revision + 1);
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              dead.revision,
                                                              {first_wear, invalid_wear}));
  auto after_death_items = repositories.characters->characters(7, 2).front().items;
  EXPECT_EQ(after_death_items[0].durability, first_wear.durability);
  EXPECT_EQ(after_death_items[1].durability, invalid_wear.durability);

  auto bypass = dead;
  bypass.dead = false;
  ++bypass.revision;
  EXPECT_THROW(repositories.character_state->save_world_progress(7, character.id, 2, dead,
                                                               bypass),
               std::invalid_argument);
  const WorldPosition shrine{8000, 100, 200, 300, 45.0F};
  EXPECT_FALSE(repositories.character_state->commit_world_revival(7, character.id, 2,
                                                                before.revision, shrine));
  EXPECT_FALSE(repositories.character_state->commit_world_revival(8, character.id, 2,
                                                                dead.revision, shrine));
  EXPECT_THROW(repositories.character_state->commit_world_revival(7, character.id, 2,
                                                                dead.revision,
                                                                WorldPosition{}),
               std::invalid_argument);
  EXPECT_EQ(repositories.character_state->load_world_progress(7, character.id, 2), dead);

  ASSERT_TRUE(repositories.character_state->commit_world_revival(7, character.id, 2,
                                                               dead.revision, shrine));
  const auto alive = repositories.character_state->load_world_progress(7, character.id, 2);
  EXPECT_FALSE(alive.dead);
  EXPECT_EQ(alive.revision, dead.revision + 1);
  EXPECT_FALSE(repositories.character_state->commit_world_revival(7, character.id, 2,
                                                                alive.revision, shrine));
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              dead.revision,
                                                              {first_wear, invalid_wear}));
  EXPECT_FALSE(repositories.character_state->commit_world_death(7, character.id, 2,
                                                              alive.revision,
                                                              {first_wear, invalid_wear}));
  EXPECT_EQ(repositories.character_state->load_world_progress(7, character.id, 2), alive);
  const auto reloaded = repositories.characters->characters(7, 2).front();
  ASSERT_TRUE(reloaded.world_position);
  EXPECT_EQ(reloaded.world_position->map_id, shrine.map_id);
  EXPECT_EQ(reloaded.world_position->x_mm, shrine.x_mm);
  EXPECT_EQ(reloaded.items[0].durability, first_wear.durability);
  EXPECT_EQ(reloaded.items[1].durability, invalid_wear.durability);
}

void exercise_death_reconciliation(const CharacterRepositories &repositories,
                                   const std::string &suffix) {
  Account account;
  account.name = "death_reconcile_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "Reconcile" + suffix;
  character = repositories.characters->create(character);
  ASSERT_FALSE(character.items.empty());
  const auto equipped = character.items.front();
  ASSERT_TRUE(equipped.equip_slot);
  const EquippedDurabilityChange wear{
      equipped.id, equipped.cms_id, *equipped.equip_slot, equipped.durability,
      equipped.durability - 100};
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  EXPECT_FALSE(state.fence_world_death_reconciliation(owner->id + 1, character.id, 1));
  EXPECT_FALSE(state.fence_world_death_reconciliation(owner->id, character.id, 2));
  ASSERT_EQ(state.load_world_progress(owner->id, character.id, 1), before);

  const auto alive = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  ASSERT_TRUE(alive);
  EXPECT_FALSE(alive->progress.dead);
  EXPECT_EQ(alive->progress.revision, before.revision + 1);
  ASSERT_FALSE(alive->equipped_items.empty());
  EXPECT_EQ(alive->equipped_items.front(), equipped);
  EXPECT_FALSE(state.commit_world_death(owner->id, character.id, 1, before.revision, {wear}));
  ASSERT_TRUE(state.commit_world_death(owner->id, character.id, 1, alive->progress.revision,
                                       {wear}));

  const auto dead = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  ASSERT_TRUE(dead);
  EXPECT_TRUE(dead->progress.dead);
  EXPECT_EQ(dead->progress.revision, before.revision + 3);
  ASSERT_FALSE(dead->equipped_items.empty());
  EXPECT_EQ(dead->equipped_items.front().durability, wear.durability);
  auto expected = before;
  expected.dead = true;
  expected.revision = dead->progress.revision;
  EXPECT_EQ(dead->progress, expected);
  const auto retry = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  ASSERT_TRUE(retry);
  EXPECT_TRUE(retry->progress.dead);
  EXPECT_EQ(retry->progress.revision, dead->progress.revision + 1);
  EXPECT_EQ(retry->equipped_items, dead->equipped_items);

  for (auto item : retry->equipped_items) {
    item.equip_slot.reset();
    ASSERT_TRUE(state.update_item(owner->id, character.id, 1, item));
  }
  const auto empty = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->progress.dead);
  EXPECT_TRUE(empty->equipped_items.empty());
}

TEST(Storage, MemoryDeathReconciliationFencesAndReadsOneSnapshot) {
  MemoryRepositories repositories;
  exercise_death_reconciliation(repositories.character_repositories(), "mem");
}

TEST(Storage, PostgresDeathReconciliationFencesAndReadsOneSnapshot) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_death_reconciliation(make_postgres_character_repositories(connection), "pg");
}

TEST(Storage, PostgresDeathReconciliationWaitsForEarlierDeathAndRejectsLateDeath) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  const auto repositories = make_postgres_character_repositories(connection);
  Account account;
  account.name = "death_fence_race";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "DeathFenceRace";
  character = repositories.characters->create(character);
  ASSERT_FALSE(character.items.empty());
  const auto item = character.items.front();
  ASSERT_TRUE(item.equip_slot);
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  const EquippedDurabilityChange wear{
      item.id, item.cms_id, *item.equip_slot, item.durability, item.durability - 100};

  pqxx::connection prior_connection(connection);
  pqxx::work prior(prior_connection);
  prior.exec("UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1, "
             "player_dead=true WHERE id=$1", pqxx::params{character.id});
  prior.exec("UPDATE emulator_character_items SET durability=$2 WHERE id=$1",
             pqxx::params{item.id, wear.durability});
  std::promise<void> started;
  auto ready = started.get_future();
  auto reconciliation = std::async(std::launch::async, [&] {
    started.set_value();
    try {
      return state.fence_world_death_reconciliation(owner->id, character.id, 1);
    } catch (const pqxx::sql_error &error) {
      if (error.sqlstate() != "40001") {
        throw;
      }
      return std::optional<DeathReconciliationSnapshot>{};
    }
  });
  ready.wait();
  EXPECT_EQ(reconciliation.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  prior.commit();
  auto snapshot = reconciliation.get();
  if (!snapshot) {
    snapshot = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  }
  ASSERT_TRUE(snapshot);
  EXPECT_TRUE(snapshot->progress.dead);
  EXPECT_EQ(snapshot->progress.revision, before.revision + 2);
  ASSERT_FALSE(snapshot->equipped_items.empty());
  EXPECT_EQ(snapshot->equipped_items.front().durability, wear.durability);
  EXPECT_FALSE(state.commit_world_death(owner->id, character.id, 1, before.revision, {wear}));
}

void exercise_death_reconciliation_lost_ack(bool drop_death_ack) {
  const char *direct = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const char *proxy = std::getenv("CHRONICLES_LOOT_PROXY_TEST_DATABASE_URL");
  if (!direct || !proxy) {
    GTEST_SKIP() << "Requires an isolated PostgreSQL lost-ack proxy";
  }
  const auto repositories = make_postgres_character_repositories(direct);
  Account account;
  account.name = drop_death_ack ? "lost_death_ack" : "lost_death_fence_ack";
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = drop_death_ack ? "LostDeathAck" : "LostDeathFenceAck";
  character = repositories.characters->create(character);
  ASSERT_FALSE(character.items.empty());
  const auto item = character.items.front();
  ASSERT_TRUE(item.equip_slot);
  const EquippedDurabilityChange wear{
      item.id, item.cms_id, *item.equip_slot, item.durability, item.durability - 100};
  auto &state = *repositories.character_state;
  const auto before = state.load_world_progress(owner->id, character.id, 1);
  PostgresCharacterStateRepository faulted(proxy);
  if (drop_death_ack) {
    EXPECT_THROW(faulted.commit_world_death(owner->id, character.id, 1, before.revision, {wear}),
                 std::exception);
  } else {
    EXPECT_THROW(faulted.fence_world_death_reconciliation(owner->id, character.id, 1),
                 std::exception);
  }

  const auto snapshot = state.fence_world_death_reconciliation(owner->id, character.id, 1);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->progress.revision, before.revision + 2);
  EXPECT_EQ(snapshot->progress.dead, drop_death_ack);
  ASSERT_FALSE(snapshot->equipped_items.empty());
  EXPECT_EQ(snapshot->equipped_items.front().durability,
            drop_death_ack ? wear.durability : item.durability);
  EXPECT_FALSE(state.commit_world_death(owner->id, character.id, 1, before.revision, {wear}));
}

TEST(Storage, PostgresDeathReconciliationLostDeathAcknowledgement) {
  exercise_death_reconciliation_lost_ack(true);
}

TEST(Storage, PostgresDeathReconciliationLostFenceAcknowledgement) {
  exercise_death_reconciliation_lost_ack(false);
}

TEST(Storage, RevisionGuardedItemUpdateConflictsWithDeath) {
  MemoryRepositories repositories;
  Character owner;
  owner.account_id = 11;
  owner.name = "RevisionGuardedEquip";
  const auto character = repositories.characters->create(owner);
  ASSERT_FALSE(character.items.empty());
  auto weapon = character.items.front();
  ASSERT_TRUE(weapon.equip_slot);
  const auto initial =
      repositories.character_state->load_world_progress(11, character.id, 1);

  weapon.durability -= 10;
  ASSERT_TRUE(repositories.character_state->update_items(11, character.id, 1,
                                                       initial.revision, {weapon}));
  EXPECT_FALSE(repositories.character_state->update_items(11, character.id, 1,
                                                        initial.revision, {weapon}));
  const EquippedDurabilityChange stale_wear{
      weapon.id, weapon.cms_id, *weapon.equip_slot, weapon.durability + 10,
      weapon.durability - 300};
  EXPECT_FALSE(repositories.character_state->commit_world_death(11, character.id, 1,
                                                              initial.revision,
                                                              {stale_wear}));
  const auto changed =
      repositories.character_state->load_world_progress(11, character.id, 1);
  EXPECT_EQ(changed.revision, initial.revision + 1);
  EXPECT_FALSE(changed.dead);

  const auto current_wear = EquippedDurabilityChange{
      weapon.id, weapon.cms_id, *weapon.equip_slot, weapon.durability, weapon.durability - 300};
  ASSERT_TRUE(repositories.character_state->commit_world_death(11, character.id, 1,
                                                             changed.revision,
                                                             {current_wear}));
  auto stale_update = weapon;
  stale_update.durability -= 20;
  EXPECT_FALSE(repositories.character_state->update_items(11, character.id, 1,
                                                        changed.revision, {stale_update}));
  EXPECT_EQ(repositories.characters->characters(11, 1).front().items.front().durability,
            current_wear.durability);
}

TEST(Storage, PostgresDeathAndRevivalRollbackAndReconnect) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  const auto repositories = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "death_owner_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(account);

  Character character;
  character.account_id = account->id;
  character.class_id = kWildCharacterClass;
  character.name = "Death" + suffix;
  character = repositories.characters->create(character);
  ASSERT_GE(character.items.size(), 2);
  const auto first = character.items[0];
  const auto second = character.items[1];
  ASSERT_TRUE(first.equip_slot);
  ASSERT_TRUE(second.equip_slot);
  const EquippedDurabilityChange first_wear{
      first.id, first.cms_id, *first.equip_slot, first.durability, first.durability - 300};
  auto second_wear = EquippedDurabilityChange{
      second.id, second.cms_id, *second.equip_slot, second.durability, second.durability - 60};
  const auto initial =
      repositories.character_state->load_world_progress(account->id, character.id, 1);

  auto wrong_item = second_wear;
  wrong_item.item_id += 90000000;
  EXPECT_FALSE(repositories.character_state->commit_world_death(account->id, character.id, 1,
                                                              initial.revision,
                                                              {first_wear, wrong_item}));
  const auto reconnected = make_postgres_character_repositories(connection);
  EXPECT_EQ(reconnected.character_state->load_world_progress(account->id, character.id, 1),
            initial);
  EXPECT_EQ(reconnected.characters->characters(account->id, 1).front().items[0].durability,
            first.durability);
  auto worn_before_death = second;
  worn_before_death.durability -= 10;
  ASSERT_TRUE(repositories.character_state->update_item(account->id, character.id, 1,
                                                      worn_before_death));
  EXPECT_FALSE(repositories.character_state->commit_world_death(account->id, character.id, 1,
                                                              initial.revision,
                                                              {first_wear, second_wear}));
  EXPECT_EQ(reconnected.character_state->load_world_progress(account->id, character.id, 1),
            initial);
  EXPECT_EQ(reconnected.characters->characters(account->id, 1).front().items[0].durability,
            first.durability);
  second_wear.expected_durability = worn_before_death.durability;
  second_wear.durability = worn_before_death.durability - 60;
  ASSERT_TRUE(repositories.character_state->commit_world_death(account->id, character.id, 1,
                                                             initial.revision,
                                                             {first_wear, second_wear}));
  const auto dead =
      reconnected.character_state->load_world_progress(account->id, character.id, 1);
  EXPECT_TRUE(dead.dead);
  EXPECT_EQ(dead.revision, initial.revision + 1);
  EXPECT_FALSE(repositories.character_state->commit_world_death(account->id, character.id, 1,
                                                              dead.revision,
                                                              {first_wear, second_wear}));
  const auto worn = reconnected.characters->characters(account->id, 1);
  ASSERT_EQ(worn.size(), 1);
  EXPECT_EQ(worn.front().items[0].durability, first_wear.durability);
  EXPECT_EQ(worn.front().items[1].durability, second_wear.durability);

  const auto migration_path = std::filesystem::path(CHRONICLES_PROTOCOL_DIR).parent_path() /
                              "migrations" / "001_accounts_characters.sql";
  std::ifstream migration_file(migration_path);
  ASSERT_TRUE(migration_file);
  const std::string migration_sql(std::istreambuf_iterator<char>{migration_file},
                                  std::istreambuf_iterator<char>{});
  migrate_postgres(connection, migration_sql);
  migrate_postgres(connection, migration_sql);
  EXPECT_EQ(reconnected.character_state->load_world_progress(account->id, character.id, 1),
            dead);
  const auto after_migration = reconnected.characters->characters(account->id, 1);
  ASSERT_EQ(after_migration.size(), 1);
  EXPECT_EQ(after_migration.front().items[0].durability, first_wear.durability);
  EXPECT_EQ(after_migration.front().items[1].durability, second_wear.durability);

  const WorldPosition destination{8000, 10, 20, 30, 15.0F};
  EXPECT_FALSE(repositories.character_state->commit_world_revival(account->id, character.id, 1,
                                                                initial.revision,
                                                                destination));
  ASSERT_TRUE(repositories.character_state->commit_world_revival(account->id, character.id, 1,
                                                               dead.revision,
                                                               destination));
  const auto alive =
      reconnected.character_state->load_world_progress(account->id, character.id, 1);
  EXPECT_FALSE(alive.dead);
  EXPECT_EQ(alive.revision, dead.revision + 1);
  EXPECT_FALSE(repositories.character_state->commit_world_revival(account->id, character.id, 1,
                                                                alive.revision,
                                                                destination));
  const auto resurrected = reconnected.characters->characters(account->id, 1);
  ASSERT_EQ(resurrected.size(), 1);
  ASSERT_TRUE(resurrected.front().world_position);
  EXPECT_EQ(resurrected.front().world_position->map_id, destination.map_id);
  EXPECT_EQ(resurrected.front().world_position->z_mm, destination.z_mm);
  EXPECT_EQ(resurrected.front().items[0].durability, first_wear.durability);
}

void exercise_shrine_revival_transaction(const CharacterRepositories &repositories) {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "shrine_owner_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(account);
  Character character;
  character.account_id = account->id;
  character.name = "Shrine" + suffix;
  character = repositories.characters->create(character);
  const WorldPosition origin{8000, 100, 200, 300, 5.0F};
  const WorldPosition destination{8000, 900, 800, 700, 45.0F};
  ASSERT_TRUE(repositories.characters->save_world_position(account->id, character.id, 1,
                                                         origin));
  const auto initial =
      repositories.character_state->load_world_progress(account->id, character.id, 1);
  ASSERT_TRUE(repositories.character_state->commit_world_death(account->id, character.id, 1,
                                                             initial.revision, {}));
  const auto dead =
      repositories.character_state->load_world_progress(account->id, character.id, 1);
  auto revived = dead;
  ++revived.revision;
  revived.dead = false;
  revived.stackables[40101101] = 5;
  revived.utility_slots[0] = {40101101, 5};
  revived.nui_blessing_level = 1;

  EXPECT_FALSE(repositories.character_state->commit_world_shrine_revival(
      account->id + 1, character.id, 1, dead, revived, destination));
  EXPECT_FALSE(repositories.character_state->commit_world_shrine_revival(
      account->id, character.id, 2, dead, revived, destination));
  auto invalid_before = dead;
  invalid_before.dead = false;
  EXPECT_THROW(repositories.character_state->commit_world_shrine_revival(
                   account->id, character.id, 1, invalid_before, revived, destination),
               std::invalid_argument);
  EXPECT_THROW(repositories.character_state->commit_world_shrine_revival(
                   account->id, character.id, 1, dead, revived, WorldPosition{}),
               std::invalid_argument);
  auto invalid = revived;
  invalid.dead = true;
  EXPECT_THROW(repositories.character_state->commit_world_shrine_revival(
                   account->id, character.id, 1, dead, invalid, destination),
               std::invalid_argument);
  invalid = revived;
  ++invalid.revision;
  EXPECT_THROW(repositories.character_state->commit_world_shrine_revival(
                   account->id, character.id, 1, dead, invalid, destination),
               std::invalid_argument);
  invalid = revived;
  invalid.stackables[40101101] = -1;
  EXPECT_THROW(repositories.character_state->commit_world_shrine_revival(
                   account->id, character.id, 1, dead, invalid, destination),
               std::invalid_argument);
  EXPECT_EQ(repositories.character_state->load_world_progress(account->id, character.id, 1),
            dead);
  EXPECT_EQ(repositories.characters->characters(account->id, 1)
                .front()
                .world_position->x_mm,
            origin.x_mm);

  auto competing = dead;
  ++competing.revision;
  competing.gold = 7;
  ASSERT_TRUE(repositories.character_state->save_world_progress(account->id, character.id, 1,
                                                              dead, competing));
  EXPECT_FALSE(repositories.character_state->commit_world_shrine_revival(
      account->id, character.id, 1, dead, revived, destination));
  EXPECT_EQ(repositories.character_state->load_world_progress(account->id, character.id, 1),
            competing);
  const auto conflicted = repositories.characters->characters(account->id, 1).front();
  ASSERT_TRUE(conflicted.world_position);
  EXPECT_EQ(conflicted.world_position->x_mm, origin.x_mm);
  EXPECT_EQ(conflicted.items, character.items);

  revived = competing;
  ++revived.revision;
  revived.dead = false;
  revived.stackables[40101101] = 5;
  revived.utility_slots[0] = {40101101, 5};
  revived.nui_blessing_level = 1;
  ASSERT_TRUE(repositories.character_state->commit_world_shrine_revival(
      account->id, character.id, 1, competing, revived, destination));
  EXPECT_EQ(repositories.character_state->load_world_progress(account->id, character.id, 1),
            revived);
  const auto reloaded = repositories.characters->characters(account->id, 1).front();
  ASSERT_TRUE(reloaded.world_position);
  EXPECT_EQ(reloaded.world_position->x_mm, destination.x_mm);
  EXPECT_EQ(reloaded.world_position->z_mm, destination.z_mm);
  EXPECT_EQ(reloaded.nui_blessing_level, 1);
  EXPECT_EQ(reloaded.items, character.items);
  EXPECT_FALSE(repositories.character_state->commit_world_shrine_revival(
      account->id, character.id, 1, competing, revived, origin));
  EXPECT_EQ(repositories.character_state->load_world_progress(account->id, character.id, 1),
            revived);
  EXPECT_EQ(repositories.characters->characters(account->id, 1)
                .front()
                .world_position->x_mm,
            destination.x_mm);
}

TEST(Storage, ShrineRevivalCommitsSupplyPositionAndLifeAtomically) {
  MemoryRepositories repositories;
  exercise_shrine_revival_transaction(repositories.character_repositories());
}

TEST(Storage, PostgresShrineRevivalCommitsSupplyPositionAndLifeAtomically) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_shrine_revival_transaction(make_postgres_character_repositories(connection));
}

TEST(Storage, PostgresShopTradeCommitsOrRollsBack) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  const auto repositories = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "shop_owner_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(repositories.accounts->add_account(owner));
  const auto owned_account = repositories.accounts->account(owner.name);
  ASSERT_TRUE(owned_account);

  Character character;
  character.account_id = owned_account->id;
  character.name = "Shop" + suffix;
  character = repositories.characters->create(character);
  CharacterItem sold_item;
  sold_item.cms_id = 11200301;
  sold_item.inventory_slot = 30;
  const auto sellable =
      repositories.character_state->grant_item(owned_account->id, character.id, 1, sold_item);
  CharacterItem worn_item;
  worn_item.cms_id = 11401101;
  worn_item.inventory_slot = 32;
  worn_item.durability = 9000;
  const auto worn =
      repositories.character_state->grant_item(owned_account->id, character.id, 1, worn_item);
  ASSERT_TRUE(sellable && worn);
  const auto equipped = std::ranges::find_if(character.items, [](const CharacterItem &item) {
    return item.equip_slot.has_value();
  });
  ASSERT_NE(equipped, character.items.end());

  const auto before =
      repositories.character_state->load_world_progress(owned_account->id, character.id, 1);
  auto after = before;
  after.revision = before.revision + 1;
  after.gold = before.gold + 80;
  after.hersander_token = 5;
  after.stackables[15010403] = 2;
  after.shop_purchases[{1001, 70101009}] = {3, 1234567};
  after.housing_items[1900000001] = {2, 1};
  after.trade_vouchers[77] = {100, 10, 20, false};
  CharacterItem bought_item;
  bought_item.cms_id = 11200201;
  bought_item.inventory_slot = 31;
  bought_item.durability = 2000;
  bought_item.created_epoch_ticks = 17929916101534738;
  ItemTrade trade;
  trade.granted_items = {bought_item};
  trade.removed_item_ids = {sellable->id};
  trade.durability_updates = {{worn->id, 9000, 10000}};

  // An equipped removal aborts the whole transaction, including the progress update, the
  // repaired item and the inserted item.
  auto equipped_removal = trade;
  equipped_removal.removed_item_ids = {sellable->id, equipped->id};
  EXPECT_FALSE(repositories.character_state->commit_item_trade(owned_account->id, character.id,
                                                             1, before, after,
                                                             equipped_removal));
  // Wear that changed since the plan (for example by a death) also aborts everything.
  auto stale_wear = trade;
  stale_wear.durability_updates = {{worn->id, 8000, 10000}};
  EXPECT_FALSE(repositories.character_state->commit_item_trade(owned_account->id, character.id,
                                                             1, before, after,
                                                             stale_wear));
  const auto reconnected = make_postgres_character_repositories(connection);
  EXPECT_EQ(reconnected.character_state->load_world_progress(owned_account->id, character.id,
                                                           1),
            before);
  const auto after_failure =
      reconnected.characters->characters(owned_account->id, 1).front().items;
  EXPECT_TRUE(std::ranges::any_of(after_failure, [&](const CharacterItem &item) {
    return item.id == sellable->id;
  }));
  EXPECT_TRUE(std::ranges::any_of(after_failure, [&](const CharacterItem &item) {
    return item.id == worn->id && item.durability == 9000;
  }));
  EXPECT_FALSE(std::ranges::any_of(after_failure, [](const CharacterItem &item) {
    return item.inventory_slot == 31;
  }));

  const auto committed =
      repositories.character_state->commit_item_trade(owned_account->id, character.id, 1,
                                                    before, after, trade);
  ASSERT_TRUE(committed);
  ASSERT_EQ(committed->size(), 1U);
  EXPECT_EQ(reconnected.character_state->load_world_progress(owned_account->id, character.id,
                                                           1),
            after);
  const auto after_commit =
      reconnected.characters->characters(owned_account->id, 1).front().items;
  EXPECT_FALSE(std::ranges::any_of(after_commit, [&](const CharacterItem &item) {
    return item.id == sellable->id;
  }));
  EXPECT_TRUE(std::ranges::any_of(after_commit, [&](const CharacterItem &item) {
    return item.id == committed->front().id && item.durability == 2000 &&
           item.created_epoch_ticks == bought_item.created_epoch_ticks;
  }));
  EXPECT_TRUE(std::ranges::any_of(after_commit, [&](const CharacterItem &item) {
    return item.id == worn->id && item.durability == 10000;
  }));

  // Replaying the same request carries a stale revision and changes nothing.
  EXPECT_FALSE(repositories.character_state->commit_item_trade(
      owned_account->id, character.id, 1, before, after,
      ItemTrade{trade.granted_items, {}, {}}));
  EXPECT_EQ(reconnected.character_state->load_world_progress(owned_account->id, character.id,
                                                           1),
            after);
}

TEST(Storage, StoreImplementsEveryRepository) {
  // Each persistence interface has one memory and one PostgreSQL implementation.
  static_assert(std::is_base_of_v<AccountRepository, MemoryAccountRepository>);
  static_assert(std::is_base_of_v<CharacterRepository, MemoryCharacterRepository>);
  static_assert(std::is_base_of_v<CharacterStateRepository,
                                  MemoryCharacterStateRepository>);
  static_assert(std::is_base_of_v<AccountRepository, PostgresAccountRepository>);
  static_assert(std::is_base_of_v<CharacterRepository, PostgresCharacterRepository>);
  static_assert(std::is_base_of_v<CharacterStateRepository,
                                  PostgresCharacterStateRepository>);

  MemoryRepositories repositories;
  AccountRepository &accounts = *repositories.accounts;
  EXPECT_FALSE(accounts.account("nobody").has_value());
}

TEST(Storage, MemoryRepositoriesShareOneDatabase) {
  auto database = std::make_shared<MemoryDatabase>();
  MemoryAccountRepository accounts(database);
  MemoryCharacterRepository characters(database);

  Account account;
  account.name = "shared";
  ASSERT_TRUE(accounts.add_account(account));
  const auto stored_account = accounts.account("shared");
  ASSERT_TRUE(stored_account.has_value());

  Character character;
  character.account_id = stored_account->id;
  character.world_id = 1;
  character.name = "SharedHero";
  const auto created = characters.create(character);

  MemoryCharacterStateRepository state(database);
  const auto progress = state.load_world_progress(stored_account->id, created.id, 1);
  EXPECT_EQ(progress.revision, 0);
  EXPECT_EQ(characters.characters(stored_account->id, 1).size(), 1u);
}

TEST(Storage, MemoryDeletionHookCanBlockDeletion) {
  auto database = std::make_shared<MemoryDatabase>();
  MemoryAccountRepository accounts(database);
  MemoryCharacterRepository characters(database);
  Account account;
  account.name = "hooked";
  ASSERT_TRUE(accounts.add_account(account));
  const auto stored_account = accounts.account("hooked");

  Character character;
  character.account_id = stored_account->id;
  character.world_id = 1;
  character.name = "HookedHero";
  const auto created = characters.create(character);

  bool erased = false;
  database->deletion_hooks.push_back({
      .blocks_deletion = [](std::int64_t) { return true; },
      .erase_rows = [&](std::int64_t) { erased = true; },
  });
  EXPECT_FALSE(characters.delete_character(created.id));
  EXPECT_FALSE(erased);

  database->deletion_hooks.back().blocks_deletion = [](std::int64_t) { return false; };
  EXPECT_TRUE(characters.delete_character(created.id));
  EXPECT_TRUE(erased);
}

// Needs the disposable migrated cluster (never port 5433).
TEST(Storage, PostgresRepositoriesShareOneDatabase) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (connection == nullptr) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  // Same idempotent migration the death/revival test applies; here it exercises the shared
  // migrate_postgres helper the repositories rely on.
  const auto migration_path = std::filesystem::path(CHRONICLES_PROTOCOL_DIR).parent_path() /
                              "migrations" / "001_accounts_characters.sql";
  std::ifstream migration_file(migration_path);
  ASSERT_TRUE(migration_file);
  const std::string migration_sql(std::istreambuf_iterator<char>{migration_file},
                                  std::istreambuf_iterator<char>{});
  migrate_postgres(connection, migration_sql);

  PostgresAccountRepository accounts(connection);
  PostgresCharacterRepository characters(connection);
  PostgresCharacterStateRepository state(connection);

  const auto suffix =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string name = "repositories_shared_" + suffix;
  Account account;
  account.name = name;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(accounts.add_account(account));
  const auto stored_account = accounts.account(name);
  ASSERT_TRUE(stored_account.has_value());

  Character character;
  character.account_id = stored_account->id;
  character.world_id = 1;
  character.name = "RepoShared" + suffix;
  const auto created = characters.create(character);
  EXPECT_EQ(state.load_world_progress(stored_account->id, created.id, 1).revision, 0);
  EXPECT_EQ(characters.characters(stored_account->id, 1).size(), 1u);
}

} // namespace
