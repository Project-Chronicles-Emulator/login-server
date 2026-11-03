#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>
#include <thread>

namespace chronicles {
namespace {

class TestAuthorityBridge final : public AuthAuthority {
public:
  explicit TestAuthorityBridge(AuthService &login) : login_(&login) {}

  void replace_login(AuthService &login) {
    login_ = &login;
  }

  nlohmann::json call(std::string_view operation, const nlohmann::json &arguments) override {
    ++calls;
    if (unavailable) {
      throw std::runtime_error("Synthetic authority outage");
    }
    const auto reply = login_->handle_authority_operation(operation, arguments);
    if (operation == lose_reply_for) {
      lose_reply_for.clear();
      throw std::runtime_error("Synthetic lost authority reply after commit");
    }
    return reply;
  }

  bool unavailable{};
  std::atomic<unsigned> calls{};
  std::string lose_reply_for;

private:
  AuthService *login_;
};

class SplitAuthTest : public testing::Test {
protected:
  MemoryRepositories repositories_;
  Clock::time_point now_ = Clock::now();
  Config config_ = [] {
    Config config;
    config.game_address = "127.0.0.1:8081";
    return config;
  }();
  AuthService login_{repositories_.character_repositories(), config_, [this] {
                       return now_;
                     }};
  AuthService game_{repositories_.character_repositories(), config_, [this] {
                      return now_;
                    }};
  std::shared_ptr<TestAuthorityBridge> bridge_ = std::make_shared<TestAuthorityBridge>(login_);
  std::string token_;
  std::int64_t account_id_{};
  std::int64_t character_id_{};

  void SetUp() override {
    game_.set_auth_authority(bridge_);
    ASSERT_TRUE(login_.create_account("splitaccount", "split-test-password-123"));
    const auto login = login_.login("splitaccount", "split-test-password-123");
    ASSERT_EQ(login.ec(), x5::ec::Success);
    token_ = login.access_token();
    account_id_ = login.account_id();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    ASSERT_EQ(game_.enter(token_, enter).error_code(), x5::ec::Success);

    Character character;
    character.account_id = account_id_;
    character.world_id = 1;
    character.slot = 0;
    character.name = "SplitCharacter";
    character.class_id = 1;
    character_id_ = repositories_.characters->create(character).id;
    ASSERT_GT(character_id_, 0);
  }

  x5::pb::frontwebsocket::CharSelectRes select_character() {
    x5::pb::ws::front::CharSelectReq request;
    request.set_char_id(character_id_);
    return game_.select(token_, request);
  }
};

TEST_F(SplitAuthTest, LoginAuthorityControlsSessionValidityAcrossServiceInstances) {
  ASSERT_TRUE(game_.authenticated(token_));
  const auto owner = game_.character_owner(token_, character_id_, false);
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->account_id, account_id_);
  EXPECT_FALSE(game_.authenticated("forged-token"));

  now_ += std::chrono::seconds(3601);
  EXPECT_FALSE(game_.authenticated(token_));
}

TEST_F(SplitAuthTest, OneTicketCoversBothCredentialFormatsAndFinalClaimIsLocalOnly) {
  const auto selected = select_character();
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  const auto identity = game_.chat_identity(token_);
  ASSERT_TRUE(identity);
  EXPECT_EQ(identity->character_id, character_id_);

  bool busy{};
  EXPECT_FALSE(game_.consume_packet_key(
      selected.packet_key(),
      [](const Handoff &) {
        return false;
      },
      &busy));
  EXPECT_TRUE(busy);
  const auto handoff = game_.consume_world_token(selected.char_token());
  ASSERT_TRUE(handoff);
  EXPECT_FALSE(game_.consume_packet_key(selected.packet_key()));
  EXPECT_FALSE(game_.claim_world_admission(*handoff));
  ASSERT_TRUE(game_.prepare_world_admission(*handoff));
  const auto calls_before_final_claim = bridge_->calls.load();
  EXPECT_TRUE(game_.claim_world_admission(*handoff));
  EXPECT_FALSE(game_.claim_world_admission(*handoff));
  EXPECT_EQ(bridge_->calls.load(), calls_before_final_claim);
}

TEST_F(SplitAuthTest, ReselectionCancelsAConsumedAdmissionBeforeItsFinalClaim) {
  const auto first = select_character();
  const auto handoff = game_.consume_world_token(first.char_token());
  ASSERT_TRUE(handoff);
  ASSERT_EQ(select_character().ec(), x5::ec::Success);
  EXPECT_FALSE(game_.prepare_world_admission(*handoff));
  EXPECT_FALSE(game_.claim_world_admission(*handoff));
}

TEST_F(SplitAuthTest, RevocationCancelsConsumedAdmissionsAndCachedSessionReads) {
  const auto selected = select_character();
  const auto handoff = game_.consume_world_token(selected.char_token());
  ASSERT_TRUE(handoff);
  login_.revoke_account_sessions(account_id_);
  EXPECT_FALSE(game_.authenticated(token_));
  EXPECT_FALSE(game_.prepare_world_admission(*handoff));
  EXPECT_FALSE(game_.claim_world_admission(*handoff));
}

TEST_F(SplitAuthTest, ExpiredTicketsAndAuthorityOutagesNeverUseCachedAuthorization) {
  const auto selected = select_character();
  now_ += std::chrono::seconds(61);
  EXPECT_FALSE(game_.consume_world_token(selected.char_token()));
  ASSERT_TRUE(game_.authenticated(token_));
  bridge_->unavailable = true;
  EXPECT_THROW(game_.authenticated(token_), std::runtime_error);
  EXPECT_THROW(game_.consume_world_token(selected.char_token()), std::runtime_error);
}

TEST_F(SplitAuthTest, AuthorityRejectsAnUnownedCharacterAndAnUnconsumedClaim) {
  EXPECT_THROW(login_.handle_authority_operation("register_handoff",
                                                 {{"token", token_},
                                                  {"character_id", character_id_ + 1},
                                                  {"char_token", std::string(64, 'a')},
                                                  {"packet_key_hex", std::string(64, 'b')}}),
               Unauthorized);
  const auto selected = select_character();
  const auto pending = login_.handle_authority_operation(
      "peek_handoff", {{"kind", "token"}, {"credential_hex", hex_encode(selected.char_token())}});
  EXPECT_FALSE(login_
                   .handle_authority_operation("claim_admission",
                                               {{"account_id", account_id_},
                                                {"admission_id", pending.at("admission_id")},
                                                {"authority_id", pending.at("authority_id")}})
                   .at("accepted")
                   .get<bool>());
}

TEST_F(SplitAuthTest, LoginRestartCannotReuseAnEarlierIncarnationsConsumedHandoff) {
  const auto first = select_character();
  const auto old_handoff = game_.consume_world_token(first.char_token());
  ASSERT_TRUE(old_handoff);

  AuthService restarted(repositories_.character_repositories(), config_, [this] {
    return now_;
  });
  bridge_->replace_login(restarted);
  const auto session = restarted.login("splitaccount", "split-test-password-123");
  ASSERT_EQ(session.ec(), x5::ec::Success);
  token_ = session.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(game_.enter(token_, enter).error_code(), x5::ec::Success);

  const auto next = select_character();
  const auto new_handoff = game_.consume_world_token(next.char_token());
  ASSERT_TRUE(new_handoff);
  EXPECT_NE(old_handoff->authority_id, new_handoff->authority_id);
  EXPECT_FALSE(game_.prepare_world_admission(*old_handoff));
  ASSERT_TRUE(game_.prepare_world_admission(*new_handoff));
  EXPECT_TRUE(game_.claim_world_admission(*new_handoff));
}

TEST_F(SplitAuthTest, ConcurrentConsumersCannotReceiveTheSameHandoff) {
  const auto selected = select_character();
  AuthService second_game(repositories_.character_repositories(), config_, [this] {
    return now_;
  });
  second_game.set_auth_authority(bridge_);
  std::optional<Handoff> first;
  std::optional<Handoff> second;
  std::thread first_consumer([&] {
    first = game_.consume_world_token(selected.char_token());
  });
  std::thread second_consumer([&] {
    second = second_game.consume_packet_key(selected.packet_key());
  });
  first_consumer.join();
  second_consumer.join();
  EXPECT_NE(first.has_value(), second.has_value());
}

TEST_F(SplitAuthTest, LostPasswordResetReplyKeepsTheGameClosedUntilAnOperatorRetry) {
  bridge_->lose_reply_for = "set_password";
  EXPECT_THROW(game_.set_account_password(account_id_, "new-split-password-123"),
               std::runtime_error);
  const auto fresh = login_.login("splitaccount", "new-split-password-123");
  ASSERT_EQ(fresh.ec(), x5::ec::Success);
  EXPECT_FALSE(game_.authenticated(fresh.access_token()));

  ASSERT_TRUE(game_.set_account_password(account_id_, "reviewed-split-password-123"));
  const auto reconciled = login_.login("splitaccount", "reviewed-split-password-123");
  ASSERT_EQ(reconciled.ec(), x5::ec::Success);
  EXPECT_TRUE(game_.authenticated(reconciled.access_token()));
}

TEST_F(SplitAuthTest, MissingOrStaleWorldHeartbeatClosesRealmAdmission) {
  login_.require_world_heartbeat();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  EXPECT_EQ(game_.enter(token_, enter).error_code(), x5::ec::WorldIsUnderMaintenance);
  game_.publish_world_status();
  EXPECT_EQ(game_.enter(token_, enter).error_code(), x5::ec::Success);
  now_ += std::chrono::seconds(6);
  EXPECT_EQ(game_.enter(token_, enter).error_code(), x5::ec::WorldIsUnderMaintenance);
}

} // namespace
} // namespace chronicles
