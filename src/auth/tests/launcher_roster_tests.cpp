#include "auth/auth.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(LauncherRosterTest, IsAccountScopedAndDoesNotEnterOrChangeWorldSessions) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "test-password-123"));
  ASSERT_TRUE(service.create_account("bob", "test-password-456"));
  for (const auto *account_name : {"alice", "bob"}) {
    Character character;
    character.account_id = repositories.accounts->account(account_name)->id;
    character.name = std::string(account_name) + "Hero";
    character.class_id = 1;
    repositories.characters->create(character);
  }
  const auto token = service.login("alice", "test-password-123", true).access_token();
  ASSERT_FALSE(token.empty());
  const auto roster = service.launcher_characters(token);
  ASSERT_EQ(roster.at("characters").size(), 1);
  EXPECT_EQ(roster.at("characters").front().at("name"), "aliceHero");
  EXPECT_EQ(roster.at("characters").front().at("class_name"), "Fight");
  EXPECT_FALSE(roster.at("characters").front().contains("account_id"));
  EXPECT_THROW(service.list(token), Unauthorized);
  const auto grant = service.issue_launch_grant(token);
  ASSERT_TRUE(grant.has_value());
  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game.has_value());
  const auto game_token = game->access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  service.enter(game_token, enter);
  const auto before = service.list(game_token).SerializeAsString();
  service.launcher_characters(token);
  EXPECT_EQ(service.list(game_token).SerializeAsString(), before);
  service.logout(token);
  EXPECT_THROW(service.launcher_characters(token), Unauthorized);
}

} // namespace
} // namespace chronicles
