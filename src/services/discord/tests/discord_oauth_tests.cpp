#include "services/discord/discord_oauth.hpp"
#include "persistence/character_repositories.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr auto kAppId = "1563414029114810438";
constexpr auto kDiscordId = "123456789012345678";
constexpr auto kOtherDiscordId = "223456789012345678";

class DiscordOAuthTest : public testing::Test {
protected:
  MemoryRepositories repositories_;
  Clock::time_point now_ = Clock::now();
  AuthService auth_{repositories_.character_repositories(), {}, [&] { return now_; }};
  DiscordOAuthConfig config_{kAppId, "synthetic-oauth-client-secret",
                             "https://realm.example/emulator/v1/discord/callback"};
  unsigned exchanges_{};
  bool wrong_app_{};
  bool missing_scope_{};
  bool provider_failure_{};
  std::string subject_{kDiscordId};
  std::string exchanged_verifier_;
  DiscordOAuth oauth_{
      auth_, config_,
      [&](const auto &method, const auto &path, const auto &form, const auto &bearer) {
        if (provider_failure_) {
          throw std::runtime_error("untrusted-provider-body-with-synthetic-secret");
        }
        if (path == "/api/oauth2/token") {
          EXPECT_EQ(method, "POST");
          EXPECT_TRUE(bearer.empty());
          const auto fields = oauth_query(form);
          EXPECT_EQ(fields.at("client_id"), kAppId);
          EXPECT_EQ(fields.at("redirect_uri"), config_.redirect_uri);
          EXPECT_EQ(fields.at("client_secret"), config_.client_secret);
          EXPECT_EQ(fields.at("grant_type"), "authorization_code");
          exchanged_verifier_ = fields.at("code_verifier");
          ++exchanges_;
          return Json{{"access_token", "synthetic-discord-bearer"}, {"token_type", "Bearer"}};
        }
        EXPECT_EQ(method, "GET");
        EXPECT_EQ(path, "/api/v10/oauth2/@me");
        EXPECT_EQ(bearer, "synthetic-discord-bearer");
        return Json{{"application", {{"id", wrong_app_ ? "999999999999999999" : kAppId}}},
                    {"scopes", missing_scope_ ? Json::array() : Json::array({"identify"})},
                    {"user", {{"id", subject_}, {"username", "alice"}}}};
      },
      [&] { return now_; }};

  Json complete(const Json &started) {
    const auto destination =
        oauth_.callback(started.at("request_id"), "synthetic-code", "https://discord.com");
    EXPECT_TRUE(destination.starts_with("http://127.0.0.1:49152/discord-complete?"));
    const auto fields = oauth_query(destination.substr(destination.find('?') + 1));
    EXPECT_EQ(oauth_.poll(started.at("request_id"), started.at("poll_secret")).at("status"),
              "pending");
    return oauth_.poll(started.at("request_id"), started.at("poll_secret"), fields.at("proof"));
  }
};

TEST_F(DiscordOAuthTest, CreatesStableAccountAndOneUseLauncherSessionWithPkce) {
  const auto started = oauth_.begin(49152);
  const auto url = started.at("authorization_url").get<std::string>();
  EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos);
  EXPECT_EQ(url.find(config_.client_secret), std::string::npos);
  EXPECT_EQ(url.find(started.at("poll_secret").get<std::string>()), std::string::npos);
  EXPECT_EQ(oauth_.poll(started.at("request_id"), started.at("poll_secret")).at("status"),
            "pending");
  const auto result = complete(started);
  ASSERT_EQ(result.at("status"), "signed_in");
  EXPECT_EQ(exchanged_verifier_.size(), 64);
  EXPECT_TRUE(auth_.authenticated(result.at("access_token")));
  EXPECT_TRUE(auth_.issue_launch_grant(result.at("access_token")).has_value());
  EXPECT_EQ(repositories_.accounts->discord_account(kDiscordId)->role, AccountRole::player);
  const auto repeated = complete(oauth_.begin(49152));
  EXPECT_EQ(repeated.at("account"), result.at("account"));
  EXPECT_FALSE(auth_.authenticated(result.at("access_token")));
  EXPECT_THROW(oauth_.poll(started.at("request_id"), started.at("poll_secret")), Unauthorized);
}

TEST_F(DiscordOAuthTest, RejectsUnknownExpiredReplayedAndUnownedAttempts) {
  const auto started = oauth_.begin(49152);
  EXPECT_THROW(oauth_.poll(started.at("request_id"), std::string(64, '0')), Unauthorized);
  EXPECT_THROW(oauth_.callback(std::string(64, '0'), "code", "https://discord.com"),
               std::invalid_argument);
  const auto rejected =
      oauth_.callback(started.at("request_id"), "code", "https://wrong-issuer.example");
  const auto rejected_fields = oauth_query(rejected.substr(rejected.find('?') + 1));
  EXPECT_EQ(exchanges_, 0);
  EXPECT_THROW(oauth_.callback(started.at("request_id"), "code", "https://discord.com"),
               std::invalid_argument);
  EXPECT_EQ(
      oauth_.poll(started.at("request_id"), started.at("poll_secret"), rejected_fields.at("proof"))
          .at("status"),
      "failed");
  const auto expired = oauth_.begin(49152);
  now_ += std::chrono::minutes(5);
  EXPECT_THROW(oauth_.callback(expired.at("request_id"), "code", "https://discord.com"),
               std::invalid_argument);
  EXPECT_THROW(oauth_.poll(expired.at("request_id"), expired.at("poll_secret")), Unauthorized);
}

TEST_F(DiscordOAuthTest, WrongApplicationOrMissingIdentityScopeCannotCreateAccounts) {
  wrong_app_ = true;
  EXPECT_EQ(complete(oauth_.begin(49152)).at("status"), "failed");
  wrong_app_ = false;
  missing_scope_ = true;
  EXPECT_EQ(complete(oauth_.begin(49152)).at("status"), "failed");
  EXPECT_FALSE(repositories_.accounts->discord_account(kDiscordId));
  missing_scope_ = false;
  provider_failure_ = true;
  const auto failed = complete(oauth_.begin(49152)).dump();
  EXPECT_EQ(failed.find("untrusted-provider"), std::string::npos);
  EXPECT_EQ(failed.find(config_.client_secret), std::string::npos);
}

TEST_F(DiscordOAuthTest, OnlyExplicitAuthenticatedLinkPreservesExistingAccountOwnership) {
  ASSERT_TRUE(auth_.create_account("alice", "12345678"));
  const auto session = auth_.login("alice", "12345678", true).access_token();
  const auto linked = complete(oauth_.begin(49152, session));
  EXPECT_EQ(linked.at("status"), "linked");
  EXPECT_EQ(repositories_.accounts->discord_account(kDiscordId)->name, "alice");
  auto login = complete(oauth_.begin(49152));
  EXPECT_EQ(login.at("account"), "alice");

  ASSERT_TRUE(auth_.create_account("bob", "12345678"));
  const auto bob = auth_.login("bob", "12345678", true).access_token();
  EXPECT_EQ(complete(oauth_.begin(49152, bob)).at("status"), "failed");
  EXPECT_EQ(repositories_.accounts->discord_account(kDiscordId)->name, "alice");
  subject_ = kOtherDiscordId;
  const auto alice = auth_.login("alice", "12345678", true).access_token();
  EXPECT_EQ(complete(oauth_.begin(49152, alice)).at("status"), "failed");
  EXPECT_FALSE(repositories_.accounts->discord_account(kOtherDiscordId));
}

TEST_F(DiscordOAuthTest, UsernameCannotClaimAnExistingAccountAndBansStillApply) {
  ASSERT_TRUE(auth_.create_account("alice", "12345678"));
  const auto result = complete(oauth_.begin(49152));
  EXPECT_NE(result.at("account"), "alice");
  auto account = repositories_.accounts->discord_account(kDiscordId);
  ASSERT_TRUE(account);
  EXPECT_TRUE(repositories_.accounts->set_account_banned(account->id, true));
  EXPECT_EQ(complete(oauth_.begin(49152)).at("status"), "failed");
}

TEST_F(DiscordOAuthTest, SignOutDuringLinkRequiresFreshAuthentication) {
  ASSERT_TRUE(auth_.create_account("alice", "12345678"));
  const auto token = auth_.login("alice", "12345678", true).access_token();
  const auto started = oauth_.begin(49152, token);
  auth_.logout(token);
  EXPECT_EQ(complete(started).at("status"), "failed");
  EXPECT_FALSE(repositories_.accounts->discord_account(kDiscordId));
}

TEST_F(DiscordOAuthTest, BrowserProofPrecedesAccountCreationAndLinking) {
  const auto started = oauth_.begin(49152);
  const auto destination = oauth_.callback(started.at("request_id"), "code", "https://discord.com");
  const auto fields = oauth_query(destination.substr(destination.find('?') + 1));
  EXPECT_FALSE(repositories_.accounts->discord_account(kDiscordId));
  EXPECT_THROW(
      oauth_.poll(started.at("request_id"), started.at("poll_secret"), std::string(64, '0')),
      Unauthorized);
  EXPECT_FALSE(repositories_.accounts->discord_account(kDiscordId));
  EXPECT_EQ(oauth_.poll(started.at("request_id"), started.at("poll_secret"), fields.at("proof"))
                .at("status"),
            "signed_in");

  ASSERT_TRUE(auth_.create_account("alice", "12345678"));
  const auto token = auth_.login("alice", "12345678", true).access_token();
  subject_ = kOtherDiscordId;
  const auto linking = oauth_.begin(49152, token);
  const auto link_destination =
      oauth_.callback(linking.at("request_id"), "code", "https://discord.com");
  const auto link_fields = oauth_query(link_destination.substr(link_destination.find('?') + 1));
  EXPECT_FALSE(repositories_.accounts->discord_account(kOtherDiscordId));
  auth_.logout(token);
  EXPECT_EQ(
      oauth_.poll(linking.at("request_id"), linking.at("poll_secret"), link_fields.at("proof"))
          .at("status"),
      "failed");
  EXPECT_FALSE(repositories_.accounts->discord_account(kOtherDiscordId));
}

TEST(PasswordPolicy, CreationAndResetAcceptEightButRejectSevenBytes) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories());
  EXPECT_THROW(auth.create_account("short", "1234567"), std::invalid_argument);
  EXPECT_TRUE(auth.create_account("eight", "12345678"));
  const auto account = repositories.accounts->account("eight");
  ASSERT_TRUE(account);
  EXPECT_EQ(auth.login("eight", "12345678").ec(), x5::ec::Success);
  EXPECT_THROW(auth.set_account_password(account->id, "1234567"), std::invalid_argument);
  EXPECT_TRUE(auth.set_account_password(account->id, "abcdefgh"));
  EXPECT_EQ(auth.login("eight", "abcdefgh").ec(), x5::ec::Success);
  EXPECT_NE(auth.login("eight", "12345678").ec(), x5::ec::Success);
}

TEST(DiscordOAuthQuery, RejectsDuplicateControlsAndMalformedEscapes) {
  EXPECT_THROW(oauth_query("state=a&state=b"), std::invalid_argument);
  EXPECT_THROW(oauth_query("state=%0a"), std::invalid_argument);
  EXPECT_THROW(oauth_query("state=%X1"), std::invalid_argument);
  EXPECT_EQ(oauth_query("iss=https%3A%2F%2Fdiscord.com").at("iss"), "https://discord.com");
}

TEST(DiscordOAuthConfig, DisabledTemplatesStayDisabledAndParseErrorsNeverEchoSecrets) {
  EXPECT_FALSE(DiscordOAuthConfig::parse(R"({"enabled":false})").enabled());
  const auto configured = DiscordOAuthConfig::parse(
      R"({"enabled":true,"client_id":"1563414029114810438","client_secret":"synthetic-secret","redirect_uri":"https://realm.example/emulator/v1/discord/callback"})");
  EXPECT_TRUE(configured.enabled());
  try {
    DiscordOAuthConfig::parse(R"({"enabled":true,"client_secret":"synthetic-private-value)");
    FAIL() << "Malformed private configuration must fail";
  } catch (const std::exception &error) {
    EXPECT_EQ(std::string(error.what()).find("synthetic-private-value"), std::string::npos);
  }
}

TEST(DiscordPostgres, IdentitySurvivesReconnectAndConcurrentFirstLoginIsAtomic) {
  const auto *connection = std::getenv("CHRONICLES_DISCORD_TEST_DATABASE_URL");
  if (connection == nullptr) {
    GTEST_SKIP() << "Run tools/test_discord_postgres.py with its isolated PostgreSQL fixture";
  }
  const auto first_repositories = make_postgres_character_repositories(connection);
  AuthService first(first_repositories);
  ASSERT_TRUE(first.create_account("discord_existing", "12345678"));
  const auto token = first.login("discord_existing", "12345678", true).access_token();
  ASSERT_TRUE(first.link_discord(token, kDiscordId));

  const auto second_repositories = make_postgres_character_repositories(connection);
  AuthService second(second_repositories);
  const auto returned = second.login_discord(kDiscordId);
  ASSERT_EQ(returned.ec(), x5::ec::Success);
  EXPECT_EQ(second.account_name(returned.access_token()), "discord_existing");

  std::int64_t first_id{};
  std::int64_t second_id{};
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  std::thread first_login([&] {
    try {
      first_id = first.login_discord(kOtherDiscordId).account_id();
    } catch (...) {
      first_error = std::current_exception();
    }
  });
  std::thread second_login([&] {
    try {
      second_id = second.login_discord(kOtherDiscordId).account_id();
    } catch (...) {
      second_error = std::current_exception();
    }
  });
  first_login.join();
  second_login.join();
  EXPECT_FALSE(first_error);
  EXPECT_FALSE(second_error);
  EXPECT_GT(first_id, 0);
  EXPECT_EQ(first_id, second_id);
  EXPECT_EQ(first_repositories.accounts->list_accounts().size(), 2);
}

} // namespace
} // namespace chronicles
