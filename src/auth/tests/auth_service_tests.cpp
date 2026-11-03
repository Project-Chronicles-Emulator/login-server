// AuthService tests (AuthTest, Auth, RealmPolicyTest suites). Moved verbatim from the
// former aggregated src/testing/tests.cpp (phase 1b-2 Task 6).
#include "auth/auth.hpp"
#include "data/starter_gear.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace chronicles;

namespace {

class AuthTest : public testing::Test {
protected:
  MemoryRepositories repositories;
  Clock::time_point now = Clock::now();
  Config config;
  AuthService service{repositories.character_repositories(), config,
                      [this] { return now; }};
  std::string access_token;

  void SetUp() override {
    ASSERT_TRUE(service.create_account("alice", "test-password-123"));
    access_token = service.login("Alice", "test-password-123").access_token();
    ASSERT_FALSE(access_token.empty());
  }

  void enter_world() {
    x5::pb::authwebsocket::WorldEnterReq request;
    request.set_world_id(1);
    ASSERT_EQ(service.enter(access_token, request).error_code(), x5::ec::Success);
  }

  x5::pb::ws::front::CharCreateReq make_character_request(std::string name = "Archetester",
                                                          int slot = 0) {
    x5::pb::ws::front::CharCreateReq request;
    request.set_slot(slot);
    request.set_char_name(name);
    request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(1));
    return request;
  }
};

TEST_F(AuthTest, PasswordAndStateBoundaries) {
  EXPECT_TRUE(service.login("alice", "wrong").access_token().empty());
  EXPECT_FALSE(service.authenticated("bad"));
  EXPECT_THROW(service.list(access_token), Unauthorized);

  x5::pb::authwebsocket::WorldEnterReq invalid_world_request;
  invalid_world_request.set_world_id(9);
  EXPECT_EQ(service.enter(access_token, invalid_world_request).error_code(), x5::ec::InvalidWorld);
  enter_world();
  EXPECT_EQ(service.list(access_token).char_list_size(), 0);

  now += std::chrono::seconds(3601);
  EXPECT_FALSE(service.authenticated(access_token));
  EXPECT_THROW(service.worlds(access_token), Unauthorized);
}

TEST_F(AuthTest, CharacterOwnerFollowsSelection) {
  enter_world();
  const auto account = repositories.accounts->account("alice");
  ASSERT_TRUE(account.has_value());
  const auto created = service.create(access_token, make_character_request());
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();

  // Before CharSelectReq the owned character may be read (the client's hot-key load arrives
  // from the lobby) but only the selected character may be written.
  const auto unselected = service.character_owner(access_token, character_id, false);
  ASSERT_TRUE(unselected.has_value());
  EXPECT_EQ(unselected->account_id, account->id);
  EXPECT_EQ(unselected->world_id, 1);
  EXPECT_FALSE(service.character_owner(access_token, character_id, true).has_value());

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character_id);
  ASSERT_EQ(service.select(access_token, select).ec(), x5::ec::Success);
  const auto selected = service.character_owner(access_token, character_id, true);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->account_id, account->id);
  EXPECT_EQ(selected->world_id, 1);

  EXPECT_FALSE(service.character_owner(access_token, character_id + 1000, false).has_value());
  EXPECT_FALSE(service.character_owner(access_token, character_id + 1000, true).has_value());
}

TEST_F(AuthTest, RealmListAdvertisesNameStateAndLastWorld) {
  auto listed = service.worlds(access_token);
  ASSERT_EQ(listed.worlds_size(), 1);
  ASSERT_EQ(listed.sites_size(), 1);
  EXPECT_EQ(listed.worlds(0).world_name(), "Project Chronicles Private Server");
  EXPECT_EQ(listed.sites(0).display_name(), "Project Chronicles");
  EXPECT_EQ(listed.worlds(0).state(), x5::pb::ServerState_Normal);
  EXPECT_EQ(listed.worlds(0).label(), x5::pb::ServerLabel_None);
  EXPECT_FALSE(listed.worlds(0).char_creation_restricted());
  EXPECT_EQ(listed.last_world_id(), 0);

  enter_world();
  ASSERT_EQ(service.create(access_token, make_character_request()).ec(), x5::ec::Success);

  listed = service.worlds(access_token);
  EXPECT_EQ(listed.world_character_counts().at(1), 1);
  EXPECT_EQ(listed.last_world_id(), 1);
}

class RealmPolicyTest : public testing::Test {
protected:
  MemoryRepositories repositories;

  // Each test needs its own Config, which AuthService copies at construction.
  std::string sign_in(AuthService &service) {
    EXPECT_TRUE(service.create_account("alice", "test-password-123"));
    return service.login("alice", "test-password-123").access_token();
  }

  static x5::pa2c::WorldEnterRes enter(AuthService &service, const std::string &token) {
    x5::pb::authwebsocket::WorldEnterReq request;
    request.set_world_id(1);
    return service.enter(token, request);
  }
};

TEST_F(RealmPolicyTest, PopulationDrivesListedStateAndRefusesEntryWhenFull) {
  Config config;
  config.world_capacity = 5;
  AuthService service(repositories.character_repositories(), config);
  const auto token = sign_in(service);

  std::vector<WorldPopulation::Lease> leases;
  for (int index = 0; index < 3; ++index) {
    leases.push_back(service.admit_world_character());
  }
  EXPECT_EQ(service.world_status().online, 3u);
  EXPECT_EQ(service.world_status().capacity, 5u);
  EXPECT_EQ(service.public_worlds().worlds(0).state(), x5::pb::ServerState_Normal);

  // Four of five is the 80% congestion threshold.
  leases.push_back(service.admit_world_character());
  EXPECT_EQ(service.public_worlds().worlds(0).state(), x5::pb::ServerState_Congested);
  EXPECT_EQ(enter(service, token).error_code(), x5::ec::Success);

  leases.push_back(service.admit_world_character());
  EXPECT_EQ(service.public_worlds().worlds(0).state(), x5::pb::ServerState_Overloaded);
  const auto refused = enter(service, token);
  EXPECT_EQ(refused.error_code(), x5::ec::ServerOverload);
  EXPECT_FALSE(refused.has_granted());

  leases.pop_back();
  EXPECT_EQ(enter(service, token).error_code(), x5::ec::Success);

  leases.clear();
  EXPECT_EQ(service.world_status().online, 0u);
}

TEST_F(RealmPolicyTest, LeaseCountsOnceAcrossMovesAndExplicitRelease) {
  AuthService service(repositories.character_repositories());

  auto first = service.admit_world_character();
  auto moved = std::move(first);
  EXPECT_EQ(service.world_status().online, 1u);

  // Releasing the moved-from lease must not decrement the count a second time.
  first.release();
  EXPECT_EQ(service.world_status().online, 1u);

  moved.release();
  moved.release();
  EXPECT_EQ(service.world_status().online, 0u);

  auto assigned = service.admit_world_character();
  assigned = service.admit_world_character();
  EXPECT_EQ(service.world_status().online, 1u);
}

TEST_F(RealmPolicyTest, MaintenanceListsGmOnlyAndRefusesEntry) {
  Config config;
  config.world_maintenance = true;
  AuthService service(repositories.character_repositories(), config);
  const auto token = sign_in(service);

  EXPECT_EQ(service.public_worlds().worlds(0).state(), x5::pb::ServerState_GmOnly);
  EXPECT_EQ(enter(service, token).error_code(), x5::ec::WorldIsUnderMaintenance);
  EXPECT_THROW(service.list(token), Unauthorized);
}

TEST_F(RealmPolicyTest, RestrictedCreationIsAdvertisedAndEnforced) {
  Config config;
  config.char_creation_restricted = true;
  config.world_label = x5::pb::ServerLabel_Recommended;
  config.world_name = "Fixture realm";
  AuthService service(repositories.character_repositories(), config);
  const auto token = sign_in(service);

  const auto listed = service.public_worlds();
  EXPECT_TRUE(listed.worlds(0).char_creation_restricted());
  EXPECT_EQ(listed.worlds(0).label(), x5::pb::ServerLabel_Recommended);
  EXPECT_EQ(listed.worlds(0).world_name(), "Fixture realm");

  const auto entered = enter(service, token);
  ASSERT_EQ(entered.error_code(), x5::ec::Success);
  EXPECT_FALSE(entered.granted().allow_char_create());

  x5::pb::ws::front::CharCreateReq request;
  request.set_slot(0);
  request.set_char_name("Archetester");
  request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(1));
  EXPECT_EQ(service.create(token, request).ec(), x5::ec::CharCreationRestricted);
  EXPECT_EQ(service.list(token).char_list_size(), 0);
}

TEST_F(AuthTest, ConsumedHandoffIsAdmittedOnceWhileCurrent) {
  enter_world();
  const auto created = service.create(access_token, make_character_request());
  ASSERT_EQ(created.ec(), x5::ec::Success);
  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(created.char_().char_id());
  const auto selected = service.select(access_token, request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  const auto handoff = service.consume_world_token(selected.char_token());
  ASSERT_TRUE(handoff);

  EXPECT_TRUE(service.claim_world_admission(*handoff));
  EXPECT_FALSE(service.claim_world_admission(*handoff));

  // A handoff that select() did not issue carries no admission identifier.
  Handoff unissued = *handoff;
  unissued.admission_id = 0;
  EXPECT_FALSE(service.claim_world_admission(unissued));
}

TEST_F(AuthTest, RevocationCancelsAlreadyConsumedWorldHandoff) {
  enum class Revocation { kick, ban, password_reset, quarantine };
  for (const auto revocation : {Revocation::kick, Revocation::ban, Revocation::password_reset,
                                Revocation::quarantine}) {
    SCOPED_TRACE(static_cast<int>(revocation));
    access_token = service.login("alice", "test-password-123").access_token();
    ASSERT_FALSE(access_token.empty());
    enter_world();
    if (service.list(access_token).char_list_size() == 0) {
      ASSERT_EQ(service.create(access_token, make_character_request()).ec(), x5::ec::Success);
    }
    x5::pb::ws::front::CharSelectReq request;
    request.set_char_id(service.list(access_token).char_list(0).char_id());
    const auto selected = service.select(access_token, request);
    ASSERT_EQ(selected.ec(), x5::ec::Success);
    const auto handoff = service.consume_world_token(selected.char_token());
    ASSERT_TRUE(handoff);

    if (revocation == Revocation::kick) {
      service.revoke_account_sessions(handoff->account_id);
    } else if (revocation == Revocation::ban) {
      ASSERT_TRUE(service.set_account_banned(handoff->account_id, true));
      ASSERT_TRUE(service.set_account_banned(handoff->account_id, false));
    } else if (revocation == Revocation::password_reset) {
      ASSERT_TRUE(service.set_account_password(handoff->account_id, "test-password-123"));
    } else {
      service.quarantine_account_sessions(handoff->account_id);
    }

    EXPECT_FALSE(service.claim_world_admission(*handoff));
  }
}

TEST_F(AuthTest, ReselectionSupersedesAlreadyConsumedWorldHandoff) {
  enter_world();
  const auto first = service.create(access_token, make_character_request("Firstplayer", 0));
  const auto second = service.create(access_token, make_character_request("Secondplayer", 1));
  ASSERT_EQ(first.ec(), x5::ec::Success);
  ASSERT_EQ(second.ec(), x5::ec::Success);

  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(first.char_().char_id());
  const auto selected_first = service.select(access_token, request);
  ASSERT_EQ(selected_first.ec(), x5::ec::Success);
  const auto first_handoff = service.consume_world_token(selected_first.char_token());
  ASSERT_TRUE(first_handoff);

  request.set_char_id(second.char_().char_id());
  const auto selected_second = service.select(access_token, request);
  ASSERT_EQ(selected_second.ec(), x5::ec::Success);
  EXPECT_FALSE(service.claim_world_admission(*first_handoff));

  const auto second_handoff = service.consume_world_token(selected_second.char_token());
  ASSERT_TRUE(second_handoff);
  EXPECT_TRUE(service.claim_world_admission(*second_handoff));
}

TEST_F(AuthTest, DelayedWorldAdmissionRejectsExpiredHandoff) {
  enter_world();
  const auto created = service.create(access_token, make_character_request());
  ASSERT_EQ(created.ec(), x5::ec::Success);
  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(created.char_().char_id());
  const auto selected = service.select(access_token, request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  const auto handoff = service.consume_world_token(selected.char_token());
  ASSERT_TRUE(handoff);

  now += config.ticket_ttl;
  EXPECT_FALSE(service.claim_world_admission(*handoff));
}

TEST_F(AuthTest, CreateListSelectOwnershipAndSingleUseTicket) {
  enter_world();
  auto created = service.create(access_token, make_character_request());
  ASSERT_EQ(created.ec(), x5::ec::Success);
  auto character_id = created.char_().char_id();
  EXPECT_EQ(service.list(access_token).char_list(0).char_id(), character_id);
  EXPECT_EQ(service.create(access_token, make_character_request("archetester", 1)).ec(),
            x5::ec::CharCreateNameAlreadyUsed);
  EXPECT_EQ(service.create(access_token, make_character_request("Othername", 0)).ec(),
            x5::ec::CharCreateMaxLimit);
  EXPECT_EQ(service.create(access_token, make_character_request("Invalid!", 1)).ec(),
            x5::ec::CharCreateNameInvalid);
  EXPECT_EQ(service.create(access_token, make_character_request("Another", 4)).ec(),
            x5::ec::CharCreateMaxLimit);

  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(character_id);
  auto selected = service.select(access_token, request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  EXPECT_FALSE(selected.has_game_address());
  EXPECT_FALSE(selected.has_packet_key());

  auto handoff = service.consume_ticket(selected.char_token());
  ASSERT_TRUE(handoff);
  EXPECT_EQ(handoff->character_id, character_id);
  EXPECT_EQ(handoff->packet_key.size(), 32);
  const auto world_character = service.world_character(*handoff);
  ASSERT_TRUE(world_character);
  EXPECT_EQ(world_character->id, character_id);
  EXPECT_EQ(world_character->name, "Archetester");

  auto wrong_owner = *handoff;
  wrong_owner.account_id += 1;
  EXPECT_FALSE(service.world_character(wrong_owner));
  EXPECT_FALSE(service.consume_ticket(selected.char_token()));

  auto expired = service.select(access_token, request);

  now += std::chrono::seconds(61);
  EXPECT_FALSE(service.consume_ticket(expired.char_token()));

  ASSERT_TRUE(service.create_account("bob", "second-password-123"));
  auto other_access_token = service.login("bob", "second-password-123").access_token();
  x5::pb::authwebsocket::WorldEnterReq world;
  world.set_world_id(1);
  service.enter(other_access_token, world);
  EXPECT_EQ(service.select(other_access_token, request).ec(), x5::ec::InvalidChar);
  EXPECT_EQ(service.list(other_access_token).char_list_size(), 0);
}

TEST_F(AuthTest, WildStarterAppearanceIsPresentInCreateAndList) {
  enter_world();

  auto request = make_character_request("Wildstarter");
  request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(2));
  const auto created = service.create(access_token, request);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  ASSERT_EQ(created.char_().equip_cms_ids_size(), 25);
  EXPECT_EQ(created.char_().equip_cms_ids(0), 11102302);
  EXPECT_EQ(created.char_().equip_cms_ids(7), 10130202);
  EXPECT_EQ(created.char_().equip_cms_ids(19), 0);
  EXPECT_EQ(created.char_().equip_cms_ids(24), 11700201);

  const auto listed = service.list(access_token);
  ASSERT_EQ(listed.char_list_size(), 1);
  EXPECT_EQ(listed.char_list(0).equip_cms_ids_size(), 25);
  EXPECT_EQ(listed.char_list(0).equip_cms_ids(7), 10130202);

  const auto account = repositories.accounts->account("alice");
  ASSERT_TRUE(account);
  auto characters = repositories.characters->characters(account->id, 1);
  ASSERT_EQ(characters.size(), 1);
  const auto costume = std::find_if(
      characters.front().items.begin(), characters.front().items.end(),
      [](const CharacterItem &item) { return item.equip_slot == 7; });
  ASSERT_NE(costume, characters.front().items.end());

  auto unequipped = *costume;
  unequipped.equip_slot.reset();
  ASSERT_TRUE(repositories.character_state->update_item(account->id, characters.front().id,
                                                      1, unequipped));

  const auto refreshed = service.list(access_token);
  ASSERT_EQ(refreshed.char_list_size(), 1);
  EXPECT_EQ(refreshed.char_list(0).equip_cms_ids(7), 0);
}

TEST_F(AuthTest, EachSupportedClassStoresItsOwnStarterWeapon) {
  enter_world();
  // The client-visible weapon model comes from the stored CMS id. Assert the
  // request's char_class is what reaches storage for every supported class.
  const std::pair<x5::cms::EX5CharacterClass, int> expectations[] = {
      {x5::cms::EX5CharacterClass_Fight, 11101102},
      {x5::cms::EX5CharacterClass_Wild, 11102302},
      {x5::cms::EX5CharacterClass_Adamant, 11103102},
      {x5::cms::EX5CharacterClass_Magic, 11104102},
  };

  int slot = 0;
  for (const auto &[class_value, weapon_cms_id] : expectations) {
    SCOPED_TRACE(static_cast<int>(class_value));
    auto request = make_character_request("Class" + std::to_string(slot), slot++);
    request.set_char_class(class_value);
    const auto created = service.create(access_token, request);
    ASSERT_EQ(created.ec(), x5::ec::Success);
    EXPECT_EQ(static_cast<int>(created.char_().char_class()),
              static_cast<int>(class_value));
    ASSERT_EQ(created.char_().equip_cms_ids_size(), kEquipSlotCount);
    EXPECT_EQ(created.char_().equip_cms_ids(0), weapon_cms_id);
  }
  const auto account = repositories.accounts->account("alice");
  ASSERT_TRUE(account);
  const auto characters = repositories.characters->characters(account->id, 1);
  ASSERT_EQ(characters.size(), std::size(expectations));
  for (std::size_t index = 0; index < std::size(expectations); ++index) {
    const auto &[class_value, weapon_cms_id] = expectations[index];
    SCOPED_TRACE(static_cast<int>(class_value));
    EXPECT_EQ(characters[index].class_id, static_cast<int>(class_value));
    const auto weapon = std::ranges::find_if(
        characters[index].items, [](const CharacterItem &item) {
          return item.equip_slot == 0;
        });
    ASSERT_NE(weapon, characters[index].items.end());
    EXPECT_EQ(weapon->cms_id, weapon_cms_id);
  }
}

TEST_F(AuthTest, RejectsClassesWithoutCatalogBackedStarterGear) {
  enter_world();
  // Death (4) and Love (6) have Character rows but no equippable weapon in the
  // Item table; classes 7-11 have no creation data at all. Creating any of
  // them would spawn a weaponless character the client shows with a default
  // axe.
  for (const int class_id : {4, 6, 7, 8, 9, 10, 11}) {
    SCOPED_TRACE(class_id);
    auto request =
        make_character_request("Bad" + std::to_string(class_id), 0);
    request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(class_id));
    const auto created = service.create(access_token, request);
    EXPECT_EQ(created.ec(), x5::ec::BadRequest);
  }

  const auto account = repositories.accounts->account("alice");
  ASSERT_TRUE(account);
  EXPECT_TRUE(repositories.characters->characters(account->id, 1).empty());
}

TEST_F(AuthTest, LoginRevokesOldSessionAndTicket) {
  enter_world();
  auto character = service.create(access_token, make_character_request());

  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(character.char_().char_id());
  auto selected = service.select(access_token, request);
  auto new_token = service.login("alice", "test-password-123").access_token();
  EXPECT_FALSE(service.authenticated(access_token));
  EXPECT_TRUE(service.authenticated(new_token));
  EXPECT_FALSE(service.consume_ticket(selected.char_token()));
}

TEST_F(AuthTest, DistinctAccountsKeepSessionsAndCharactersSeparate) {
  ASSERT_TRUE(service.create_account("BOB", "bob-password-123"));
  EXPECT_FALSE(service.create_account("bob", "different-password-123"));

  const auto alice_account = repositories.accounts->account("alice");
  const auto bob_account = repositories.accounts->account("bob");
  ASSERT_TRUE(alice_account);
  ASSERT_TRUE(bob_account);
  EXPECT_NE(alice_account->id, bob_account->id);

  EXPECT_TRUE(service.login("bob", "test-password-123").access_token().empty());
  EXPECT_TRUE(service.login("alice", "bob-password-123").access_token().empty());

  const auto bob_login = service.login("bOb", "bob-password-123");
  ASSERT_EQ(bob_login.ec(), x5::ec::Success);
  const auto bob_token = bob_login.access_token();
  ASSERT_FALSE(bob_token.empty());
  EXPECT_NE(access_token, bob_token);
  EXPECT_NE(bob_login.account_id(), alice_account->id);
  EXPECT_TRUE(service.authenticated(access_token));

  enter_world();
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(service.enter(bob_token, world_request).error_code(), x5::ec::Success);

  const auto alice_character =
      service.create(access_token, make_character_request("AliceHero"));
  const auto bob_character =
      service.create(bob_token, make_character_request("BobHero"));
  ASSERT_EQ(alice_character.ec(), x5::ec::Success);
  ASSERT_EQ(bob_character.ec(), x5::ec::Success);
  EXPECT_NE(alice_character.char_().char_id(), bob_character.char_().char_id());

  const auto alice_list = service.list(access_token);
  const auto bob_list = service.list(bob_token);
  ASSERT_EQ(alice_list.char_list_size(), 1);
  ASSERT_EQ(bob_list.char_list_size(), 1);
  EXPECT_EQ(alice_list.char_list(0).name(), "AliceHero");
  EXPECT_EQ(bob_list.char_list(0).name(), "BobHero");

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(alice_character.char_().char_id());
  EXPECT_EQ(service.select(bob_token, select_request).ec(), x5::ec::InvalidChar);

  const auto alice_selection = service.select(access_token, select_request);
  ASSERT_EQ(alice_selection.ec(), x5::ec::Success);
  select_request.set_char_id(bob_character.char_().char_id());
  EXPECT_EQ(service.select(access_token, select_request).ec(), x5::ec::InvalidChar);

  const auto bob_relogin = service.login("bob", "bob-password-123");
  ASSERT_EQ(bob_relogin.ec(), x5::ec::Success);
  EXPECT_FALSE(service.authenticated(bob_token));
  EXPECT_TRUE(service.authenticated(access_token));
  EXPECT_TRUE(service.consume_ticket(alice_selection.char_token()));

  service.logout(access_token);
  EXPECT_FALSE(service.authenticated(access_token));
  EXPECT_TRUE(service.authenticated(bob_relogin.access_token()));
  ASSERT_EQ(service.enter(bob_relogin.access_token(), world_request).error_code(),
            x5::ec::Success);
  ASSERT_EQ(service.list(bob_relogin.access_token()).char_list_size(), 1);
}

TEST_F(AuthTest, LaunchGrantsAreOneUseShortLivedAndAccountScoped) {
  ASSERT_TRUE(service.create_account("bob", "bob-password-123"));
  const auto bob_login = service.login("bob", "bob-password-123");
  ASSERT_EQ(bob_login.ec(), x5::ec::Success);

  EXPECT_FALSE(service.issue_launch_grant("unknown-session"));
  const auto alice_grant = service.issue_launch_grant(access_token);
  const auto replacement_alice_grant = service.issue_launch_grant(access_token);
  const auto bob_grant = service.issue_launch_grant(bob_login.access_token());
  ASSERT_TRUE(alice_grant);
  ASSERT_TRUE(replacement_alice_grant);
  ASSERT_TRUE(bob_grant);
  EXPECT_NE(*alice_grant, *bob_grant);

  EXPECT_FALSE(service.valid_launch_grant(*alice_grant));
  EXPECT_TRUE(service.valid_launch_grant(*replacement_alice_grant));

  auto incorrect_grant = *replacement_alice_grant;
  incorrect_grant.back() = incorrect_grant.back() == '0' ? '1' : '0';
  EXPECT_FALSE(service.consume_launch_grant(incorrect_grant));
  EXPECT_TRUE(service.authenticated(access_token));
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  const auto alice_launch = service.consume_launch_grant(*replacement_alice_grant);
  ASSERT_TRUE(alice_launch);
  EXPECT_EQ(alice_launch->ec(), x5::ec::Success);
  EXPECT_NE(alice_launch->account_id(), bob_login.account_id());
  EXPECT_FALSE(service.consume_launch_grant(*alice_grant));
  EXPECT_FALSE(service.consume_launch_grant(*replacement_alice_grant));
  EXPECT_FALSE(service.authenticated(access_token));
  EXPECT_TRUE(service.authenticated(alice_launch->access_token()));
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  now += kLaunchGrantTtl + std::chrono::seconds(1);
  EXPECT_FALSE(service.consume_launch_grant(*bob_grant));
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  const auto refreshed_bob_grant = service.issue_launch_grant(bob_login.access_token());
  ASSERT_TRUE(refreshed_bob_grant);
  const auto bob_launch = service.consume_launch_grant(*refreshed_bob_grant);
  ASSERT_TRUE(bob_launch);
  EXPECT_EQ(bob_launch->account_id(), bob_login.account_id());
  EXPECT_NE(bob_launch->account_id(), alice_launch->account_id());
  EXPECT_TRUE(service.authenticated(alice_launch->access_token()));
}

TEST_F(AuthTest, LaunchGrantSurvivesLauncherRenewalDuringClientIntro) {
  const auto launcher = service.login("alice", "test-password-123", true).access_token();

  // Issue the grant shortly before the launcher session expires. The game may
  // still be playing its intro when password-based session renewal occurs.
  now += std::chrono::seconds(3590);
  const auto grant = service.issue_launch_grant(launcher);
  ASSERT_TRUE(grant);

  now += std::chrono::seconds(20);
  const auto replacement = service.login("alice", "test-password-123", true);
  ASSERT_EQ(replacement.ec(), x5::ec::Success);
  EXPECT_FALSE(service.authenticated(launcher));
  EXPECT_TRUE(service.valid_launch_grant(*grant));

  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  EXPECT_EQ(game->ec(), x5::ec::Success);
  EXPECT_TRUE(service.authenticated(replacement.access_token()));
  EXPECT_TRUE(service.authenticated(game->access_token()));
  EXPECT_FALSE(service.consume_launch_grant(*grant));
}

TEST_F(AuthTest, LauncherLoginSurvivesGameHandoffRotationAndLauncherLogoutLeavesGameOpen) {
  const auto launcher = service.login("alice", "test-password-123", true).access_token();
  const auto grant = service.issue_launch_grant(launcher);
  ASSERT_TRUE(grant);
  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  EXPECT_TRUE(service.authenticated(launcher));
  const auto rotated = service.rotate_session(game->access_token());
  ASSERT_TRUE(rotated);
  EXPECT_TRUE(service.authenticated(launcher));
  EXPECT_FALSE(service.authenticated(game->access_token()));

  service.logout(launcher);
  EXPECT_FALSE(service.authenticated(launcher));
  EXPECT_TRUE(service.authenticated(rotated->access_token()));
}

TEST_F(AuthTest, GameLogoutKeepsLauncherSignedInAndPasswordReloginStillRevokesOlderSessions) {
  const auto launcher = service.login("alice", "test-password-123", true).access_token();
  const auto grant = service.issue_launch_grant(launcher);
  ASSERT_TRUE(grant);
  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  service.logout(game->access_token());
  EXPECT_FALSE(service.authenticated(game->access_token()));
  EXPECT_TRUE(service.authenticated(launcher));
  EXPECT_TRUE(service.issue_launch_grant(launcher));

  const auto replacement = service.login("alice", "test-password-123", true);
  EXPECT_FALSE(service.authenticated(launcher));
  EXPECT_TRUE(service.authenticated(replacement.access_token()));
}

TEST(Auth, NativeKeyAndDevelopmentTokenShareSingleUseHandoff) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  auto now = Clock::now();
  AuthService service(repositories.character_repositories(), config,
                      [&] { return now; });
  ASSERT_TRUE(service.create_account("native", "local-test-password"));
  const auto token = service.login("native", "local-test-password").access_token();

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  service.enter(token, world_request);
  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("NativeTest");
  create_request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(1));
  const auto created = service.create(token, create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = service.select(token, select_request);
  ASSERT_EQ(selected.packet_key().size(), 32);
  const auto key_diagnostic =
      service.diagnose_world_credential(std::string("pad") + selected.packet_key());
  EXPECT_EQ(key_diagnostic.packet_key.offset, 3);
  EXPECT_EQ(key_diagnostic.packet_key.length, 32);

  const auto token_diagnostic = service.diagnose_world_credential(
      std::string("prefix") + selected.char_token().substr(0, 24));
  EXPECT_EQ(token_diagnostic.char_token.offset, 6);
  EXPECT_EQ(token_diagnostic.char_token.length, 24);

  EXPECT_FALSE(service.consume_packet_key("short"));
  EXPECT_FALSE(service.consume_packet_key(std::string(32, '\0')));
  bool admission_busy{};
  EXPECT_FALSE(service.consume_packet_key(selected.packet_key(),
                                          [](const Handoff &) { return false; },
                                          &admission_busy));
  EXPECT_TRUE(admission_busy);
  const auto handoff = service.consume_packet_key(selected.packet_key());
  ASSERT_TRUE(handoff);
  EXPECT_EQ(handoff->character_id, created.char_().char_id());
  EXPECT_FALSE(service.consume_packet_key(selected.packet_key()));
  EXPECT_FALSE(service.consume_ticket(selected.char_token()));
  const auto consumed_diagnostic = service.diagnose_world_credential(selected.packet_key());
  EXPECT_EQ(consumed_diagnostic.packet_key.length, 0);

  const auto token_selection = service.select(token, select_request);
  bool token_admission_busy{true};
  EXPECT_FALSE(service.consume_world_token("short", {}, &token_admission_busy));
  EXPECT_FALSE(token_admission_busy);
  EXPECT_FALSE(service.consume_world_token(std::string(64, 'z')));
  EXPECT_FALSE(service.consume_world_token(token_selection.char_token(),
                                           [](const Handoff &) { return false; },
                                           &token_admission_busy));
  EXPECT_TRUE(token_admission_busy);
  const auto token_handoff = service.consume_world_token(token_selection.char_token());
  ASSERT_TRUE(token_handoff);
  EXPECT_EQ(token_handoff->character_id, created.char_().char_id());
  EXPECT_FALSE(service.consume_world_token(token_selection.char_token()));
  EXPECT_FALSE(service.consume_packet_key(token_selection.packet_key()));

  const auto expired = service.select(token, select_request);
  now += std::chrono::seconds(61);
  EXPECT_FALSE(service.consume_packet_key(expired.packet_key()));
}

} // namespace
