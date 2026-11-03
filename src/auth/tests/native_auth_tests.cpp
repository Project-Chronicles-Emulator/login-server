#include "auth/native_auth.hpp"
#include "auth/auth_admission.hpp"

#include "p_c2a.pb.h"
#include "persistence/character_repositories.hpp"
#include "persistence/memory_repositories.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace chronicles {
namespace {

constexpr const char *kFixtureAccount = "native_fixture";
constexpr const char *kFixturePassword = "synthetic-native-password-only";

NativeAuthConfig local_config() {
  NativeAuthConfig config;
  config.enabled = true;
  config.public_address = "http://127.0.0.1:8080/";
  config.local_account = kFixtureAccount;
  config.local_password = kFixturePassword;
  config.client_version = "fixture-build";
  config.client_protocol = "fixture-protocol";

  return config;
}

std::string synthetic_steam_request() {
  x5::pc2a::AuthSteamReq request;
  request.set_auth_token("synthetic-ticket-with-no-platform-validity");
  request.set_platform("Windows");

  return request.SerializeAsString();
}

std::string synthetic_steam_request_for_identity(const std::string &identity) {
  x5::pc2a::AuthSteamReq request;
  request.set_auth_token("synthetic-ticket-with-no-platform-validity");
  request.set_identity(identity);
  request.set_platform("Windows");

  return request.SerializeAsString();
}

std::string test_credential_digest(std::string_view credential) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size{};
  if (EVP_Digest(credential.data(), credential.size(), digest.data(), &digest_size,
                 EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("Cannot hash test credential");
  }

  return hex_encode(std::string(reinterpret_cast<const char *>(digest.data()), digest_size));
}

class FailingReconnectStore : public MemoryAccountRepository {
public:
  explicit FailingReconnectStore(std::shared_ptr<MemoryDatabase> database)
      : MemoryAccountRepository(std::move(database)) {}

  bool fail_save{};
  bool throw_save{};
  bool throw_revoke{};
  bool throw_read{};
  bool throw_password_after_commit{};
  bool throw_ban_after_commit{};
  bool throw_password_before_commit{};
  bool throw_ban_before_commit{};

  bool set_account_password(std::int64_t account_id, const std::string &salt,
                            const std::string &hash, int iterations) override {
    if (throw_password_before_commit) {
      throw std::runtime_error("synthetic password mutation failed before commit");
    }
    const bool updated =
        MemoryAccountRepository::set_account_password(account_id, salt, hash, iterations);
    if (updated && throw_password_after_commit) {
      throw std::runtime_error("synthetic lost password commit response");
    }
    return updated;
  }

  bool set_account_banned(std::int64_t account_id, bool banned) override {
    if (throw_ban_before_commit) {
      throw std::runtime_error("synthetic ban mutation failed before commit");
    }
    const bool updated = MemoryAccountRepository::set_account_banned(account_id, banned);
    if (updated && throw_ban_after_commit) {
      throw std::runtime_error("synthetic lost ban commit response");
    }
    return updated;
  }

  std::optional<Account> account_by_id(std::int64_t account_id) override {
    if (throw_read) {
      throw std::runtime_error("synthetic account storage failure");
    }
    return MemoryAccountRepository::account_by_id(account_id);
  }

  bool save_reconnect_credential(std::int64_t account_id, const std::string &digest,
                                 std::int64_t expires_at) override {
    if (throw_save) {
      throw std::runtime_error("synthetic reconnect storage failure");
    }
    if (fail_save) {
      return false;
    }
    return MemoryAccountRepository::save_reconnect_credential(account_id, digest, expires_at);
  }

  void revoke_reconnect_credentials(std::int64_t account_id) override {
    if (throw_revoke) {
      throw std::runtime_error("synthetic reconnect revocation failure");
    }
    MemoryAccountRepository::revoke_reconnect_credentials(account_id);
  }
};

TEST(AuthAdmission, SaturationReleasesSlotsAndKeepsPeerBudgetsSeparate) {
  AuthAdmission limiter(4, 2, 2, 10);
  auto first = limiter.try_acquire("peer-a");
  auto second = limiter.try_acquire("peer-a");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_FALSE(limiter.try_acquire("peer-a"));
  auto third = limiter.try_acquire("peer-b");
  auto fourth = limiter.try_acquire("peer-c");
  ASSERT_TRUE(third);
  ASSERT_TRUE(fourth);
  EXPECT_FALSE(limiter.try_acquire("peer-d"));
  first.reset();
  auto replacement = limiter.try_acquire("peer-d");
  EXPECT_TRUE(replacement);
  // Refused in-flight work does not create password failures.
  EXPECT_TRUE(limiter.password_allowed("peer-a", "alice"));
}

TEST(AuthAdmission, PermitIsReleasedDuringExceptionUnwind) {
  AuthAdmission limiter(1, 1, 2, 10);
  EXPECT_THROW(
      [&] {
        auto permit = limiter.try_acquire("peer-a");
        ASSERT_TRUE(permit);
        throw std::runtime_error("synthetic worker failure");
      }(),
      std::runtime_error);
  EXPECT_TRUE(limiter.try_acquire("peer-b"));
}

TEST(AuthAdmission, OnlyPeerAndAccountFailuresThrottleAndExpire) {
  AuthAdmission limiter(4, 2, 2, 10);
  const auto start = AuthAdmission::Clock::now();
  limiter.bad_password("peer-a", "alice", start);
  limiter.bad_password("peer-a", "alice", start);
  EXPECT_FALSE(limiter.password_allowed("peer-a", "alice", start));
  EXPECT_TRUE(limiter.password_allowed("peer-b", "alice", start));
  EXPECT_TRUE(limiter.password_allowed("peer-a", "bob", start));
  EXPECT_TRUE(limiter.password_allowed("peer-a", "alice", start + std::chrono::seconds(61)));
}

TEST(AuthAdmission, PeerPasswordSprayingIsBoundedWithoutGlobalAccountLockout) {
  AuthAdmission limiter(4, 2, 2, 100, 3, 4);
  const auto start = AuthAdmission::Clock::now();
  limiter.bad_password("peer-a", "alice", start);
  limiter.bad_password("peer-a", "bob", start);
  limiter.bad_password("peer-a", "carol", start);
  EXPECT_FALSE(limiter.password_allowed("peer-a", "other", start));
  EXPECT_TRUE(limiter.password_allowed("peer-b", "alice", start));
  EXPECT_TRUE(limiter.password_allowed("peer-a", "other", start + std::chrono::seconds(61)));
}

TEST(AuthAdmission, RegistrationQuotaIsPerPeerAndExpires) {
  AuthAdmission limiter(4, 2, 2, 100, 3, 4);
  const auto start = AuthAdmission::Clock::now();
  for (int index = 0; index < 4; ++index) {
    EXPECT_TRUE(limiter.allow_registration("peer-a", start));
  }
  EXPECT_FALSE(limiter.allow_registration("peer-a", start));
  EXPECT_TRUE(limiter.allow_registration("peer-b", start));
  EXPECT_TRUE(limiter.allow_registration("peer-a", start + std::chrono::seconds(61)));
}

TEST(AuthAdmission, ConcurrentPermitsAreBoundedAndCleanedUp) {
  AuthAdmission limiter(8, 4, 12, 8192);
  std::atomic<unsigned> active{};
  std::atomic<unsigned> peak{};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 16; ++worker) {
    workers.emplace_back([&, worker] {
      for (int index = 0; index < 100; ++index) {
        auto permit = limiter.try_acquire("peer-" + std::to_string(worker));
        if (!permit) {
          continue;
        }
        const auto now_active = ++active;
        auto previous = peak.load();
        while (previous < now_active && !peak.compare_exchange_weak(previous, now_active)) {
        }
        --active;
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  EXPECT_LE(peak.load(), 8);
  EXPECT_EQ(active.load(), 0);
  EXPECT_TRUE(limiter.try_acquire("after-completion"));
}

TEST(AuthAdmission, FullFailureTableEvictsInsteadOfLockingOutNewPeers) {
  AuthAdmission limiter(4, 2, 1, 2);
  const auto start = AuthAdmission::Clock::now();
  limiter.bad_password("peer-a", "alice", start);
  limiter.bad_password("peer-b", "bob", start + std::chrono::seconds(1));
  limiter.bad_password("peer-c", "carol", start + std::chrono::seconds(2));
  EXPECT_TRUE(limiter.password_allowed("peer-a", "alice", start + std::chrono::seconds(2)));
  EXPECT_FALSE(limiter.password_allowed("peer-c", "carol", start + std::chrono::seconds(2)));
  EXPECT_TRUE(limiter.password_allowed("new-peer", "new-account", start + std::chrono::seconds(2)));
}

TEST(AuthAdmission, FullTablesKeepPeerLimitsAndExpireLookupsIndependently) {
  constexpr int maximum_keys = 8192;
  AuthAdmission limiter(4, 2, 1, maximum_keys, 1, 1);
  const auto start = AuthAdmission::Clock::now();
  for (int index = 0; index < maximum_keys; ++index) {
    const auto peer = "peer-" + std::to_string(index);
    limiter.bad_password(peer, "alice", start);
    ASSERT_TRUE(limiter.allow_registration(peer, start));
  }
  EXPECT_FALSE(limiter.password_allowed("peer-8191", "alice", start));
  EXPECT_FALSE(limiter.allow_registration("peer-8191", start));
  const auto expired = start + std::chrono::seconds(61);
  // The target is near the back of a full table: it must expire even though
  // only a bounded number of old entries are pruned on each operation.
  EXPECT_TRUE(limiter.password_allowed("peer-8191", "alice", expired));
  EXPECT_TRUE(limiter.allow_registration("peer-8191", expired));
  EXPECT_TRUE(limiter.password_allowed("unrelated-peer", "alice", expired));
}

TEST(NativeAuth, OnlyCredentialMismatchIsReportedAsBadPassword) {
  MemoryRepositories repositories;
  auto store = std::make_shared<FailingReconnectStore>(repositories.database);
  repositories.accounts = store;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  bool bad_password = false;
  service.login("alice", "wrong-password", true, &bad_password);
  EXPECT_TRUE(bad_password);
  bad_password = true;
  service.login("alice", "alice-password-123", true, &bad_password);
  EXPECT_FALSE(bad_password);
  store->throw_read = true;
  bad_password = true;
  EXPECT_THROW(service.login("alice", "alice-password-123", true, &bad_password),
               std::runtime_error);
  EXPECT_FALSE(bad_password);
}

TEST(NativeAuth, FailedMutationsDenyDurableReconnectUntilSuccessfulReconciliation) {
  for (const bool ban : {false, true}) {
    SCOPED_TRACE(ban ? "ban" : "password reset");
    MemoryRepositories repositories;
    auto store = std::make_shared<FailingReconnectStore>(repositories.database);
    repositories.accounts = store;
    AuthService service(repositories.character_repositories());
    ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
    ASSERT_TRUE(service.create_account("bobby", "bobby-password-123"));
    const auto alice = service.login("alice", "alice-password-123", true);
    const auto bob = service.login("bobby", "bobby-password-123", true);
    auto old = service.issue_launch_grant(alice.access_token());
    auto other = service.issue_launch_grant(bob.access_token());
    ASSERT_TRUE(old);
    ASSERT_TRUE(other);
    ASSERT_TRUE(service.consume_launch_grant(*old));
    ASSERT_TRUE(service.consume_launch_grant(*other));

    if (ban) {
      store->throw_ban_before_commit = true;
      EXPECT_THROW(service.set_account_banned(alice.account_id(), true), std::runtime_error);
      store->throw_ban_before_commit = false;
    } else {
      store->throw_password_before_commit = true;
      EXPECT_THROW(service.set_account_password(alice.account_id(), "new-alice-password"),
                   std::runtime_error);
      store->throw_password_before_commit = false;
    }
    EXPECT_FALSE(service.authenticated(alice.access_token()));
    EXPECT_FALSE(service.valid_launch_grant(*old));
    EXPECT_FALSE(service.reconnect_launch_grant(*old));
    EXPECT_TRUE(service.reconnect_launch_grant(*other));

    store->throw_revoke = true;
    EXPECT_THROW(service.login("alice", "alice-password-123", true), std::runtime_error);
    EXPECT_FALSE(service.reconnect_launch_grant(*old));
    store->throw_revoke = false;
    EXPECT_EQ(service.login("alice", "alice-password-123", true).ec(), x5::ec::Success);
    EXPECT_FALSE(service.reconnect_launch_grant(*old));
  }
}

TEST(NativeAuth, FailedExplicitRevocationDeniesOldReconnectUntilReconciled) {
  MemoryRepositories repositories;
  auto store = std::make_shared<FailingReconnectStore>(repositories.database);
  repositories.accounts = store;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  const auto login = service.login("alice", "alice-password-123", true);
  auto grant = service.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  store->throw_revoke = true;
  EXPECT_THROW(service.revoke_account_sessions(login.account_id()), std::runtime_error);
  EXPECT_FALSE(service.authenticated(login.access_token()));
  EXPECT_FALSE(service.authenticated(game->access_token()));
  EXPECT_FALSE(service.valid_launch_grant(*grant));
  EXPECT_FALSE(service.reconnect_launch_grant(*grant));
  EXPECT_THROW(service.login("alice", "alice-password-123", true), std::runtime_error);
  store->throw_revoke = false;
  EXPECT_EQ(service.login("alice", "alice-password-123", true).ec(), x5::ec::Success);
  EXPECT_FALSE(service.reconnect_launch_grant(*grant));
}

TEST(NativeAuth, LostPasswordOrBanCommitInvalidatesSessionsTicketsAndPendingGrant) {
  for (const bool ban : {false, true}) {
    SCOPED_TRACE(ban ? "ban" : "password reset");
    MemoryRepositories repositories;
    auto store = std::make_shared<FailingReconnectStore>(repositories.database);
    repositories.accounts = store;
    AuthService service(repositories.character_repositories());
    ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
    const auto launcher = service.login("alice", "alice-password-123", true);
    auto grant = service.issue_launch_grant(launcher.access_token());
    ASSERT_TRUE(grant);
    const auto game = service.consume_launch_grant(*grant);
    ASSERT_TRUE(game);

    x5::pb::authwebsocket::WorldEnterReq world;
    world.set_world_id(1);
    ASSERT_EQ(service.enter(game->access_token(), world).error_code(), x5::ec::Success);
    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name("Alicetest");
    create.set_char_class(static_cast<x5::cms::EX5CharacterClass>(1));
    const auto character = service.create(game->access_token(), create);
    ASSERT_EQ(character.ec(), x5::ec::Success);
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.char_().char_id());
    const auto selected = service.select(game->access_token(), select);
    ASSERT_EQ(selected.ec(), x5::ec::Success);
    ASSERT_FALSE(selected.char_token().empty());
    const auto pending = service.issue_launch_grant(launcher.access_token());
    ASSERT_TRUE(pending);

    if (ban) {
      store->throw_ban_after_commit = true;
      EXPECT_THROW(service.set_account_banned(launcher.account_id(), true), std::runtime_error);
    } else {
      store->throw_password_after_commit = true;
      EXPECT_THROW(service.set_account_password(launcher.account_id(), "new-alice-password"),
                   std::runtime_error);
    }
    EXPECT_FALSE(service.authenticated(launcher.access_token()));
    EXPECT_FALSE(service.authenticated(game->access_token()));
    EXPECT_FALSE(service.consume_ticket(selected.char_token()));
    EXPECT_FALSE(service.valid_launch_grant(*pending));
    EXPECT_FALSE(service.consume_launch_grant(*pending));
    EXPECT_FALSE(service.reconnect_launch_grant(*grant));
  }
}

TEST(NativeAuth, CommittedPasswordAndBanInvalidateSessionsWithoutSecondStorageDelete) {
  MemoryRepositories repositories;
  auto store = std::make_shared<FailingReconnectStore>(repositories.database);
  repositories.accounts = store;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  auto launcher = service.login("alice", "alice-password-123", true);
  auto grant = service.issue_launch_grant(launcher.access_token());
  ASSERT_TRUE(grant);
  auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);

  store->throw_revoke = true;
  EXPECT_TRUE(service.set_account_password(launcher.account_id(), "new-alice-password"));
  EXPECT_FALSE(service.authenticated(launcher.access_token()));
  EXPECT_FALSE(service.authenticated(game->access_token()));
  EXPECT_FALSE(service.reconnect_launch_grant(*grant));

  store->throw_revoke = false;
  launcher = service.login("alice", "new-alice-password", true);
  grant = service.issue_launch_grant(launcher.access_token());
  ASSERT_TRUE(grant);
  game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  store->throw_revoke = true;
  EXPECT_TRUE(service.set_account_banned(launcher.account_id(), true));
  EXPECT_FALSE(service.authenticated(launcher.access_token()));
  EXPECT_FALSE(service.authenticated(game->access_token()));
  EXPECT_FALSE(service.reconnect_launch_grant(*grant));
  EXPECT_TRUE(service.set_account_banned(launcher.account_id(), false));
  EXPECT_FALSE(service.reconnect_launch_grant(*grant));
}

TEST(NativeAuth, StorageFailurePreservesUnconsumedGrantAndPreviousSession) {
  MemoryRepositories repositories;
  auto store = std::make_shared<FailingReconnectStore>(repositories.database);
  repositories.accounts = store;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  const auto launcher = service.login("alice", "alice-password-123", true);
  const auto original_grant = service.issue_launch_grant(launcher.access_token());
  ASSERT_TRUE(original_grant);
  const auto original_game = service.consume_launch_grant(*original_grant);
  ASSERT_TRUE(original_game);
  const auto grant = service.issue_launch_grant(launcher.access_token());
  ASSERT_TRUE(grant);

  store->throw_read = true;
  EXPECT_THROW(service.consume_launch_grant(*grant), std::runtime_error);
  store->throw_read = false;
  EXPECT_TRUE(service.authenticated(original_game->access_token()));
  store->fail_save = true;
  EXPECT_FALSE(service.consume_launch_grant(*grant));
  EXPECT_TRUE(service.authenticated(original_game->access_token()));
  store->fail_save = false;
  store->throw_save = true;
  EXPECT_THROW(service.consume_launch_grant(*grant), std::runtime_error);
  store->throw_save = false;
  EXPECT_TRUE(service.authenticated(original_game->access_token()));
  const auto game = service.consume_launch_grant(*grant);
  ASSERT_TRUE(game);
  EXPECT_FALSE(service.authenticated(original_game->access_token()));
  EXPECT_TRUE(service.authenticated(game->access_token()));
  EXPECT_FALSE(service.consume_launch_grant(*grant));

  store->throw_revoke = true;
  EXPECT_THROW(service.issue_launch_grant(launcher.access_token()), std::runtime_error);
  store->throw_revoke = false;
  EXPECT_TRUE(service.reconnect_launch_grant(*grant));
}

TEST(NativeAuth, PerIdentityLocalAccountsKeepTwoClientsIndependent) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  auto config = local_config();
  config.separate_accounts_by_identity = true;
  NativeAuth adapter(service, config);

  const auto first_reply = adapter.handle(
      "PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf",
      synthetic_steam_request_for_identity("synthetic-player-one"));
  const auto second_reply = adapter.handle(
      "PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf",
      synthetic_steam_request_for_identity("synthetic-player-two"));
  ASSERT_TRUE(first_reply);
  ASSERT_TRUE(second_reply);

  x5::pa2c::AuthChallengeRes first_login;
  x5::pa2c::AuthChallengeRes second_login;
  ASSERT_TRUE(first_login.ParseFromString(first_reply->body));
  ASSERT_TRUE(second_login.ParseFromString(second_reply->body));
  ASSERT_EQ(first_login.ec(), x5::ec::Success);
  ASSERT_EQ(second_login.ec(), x5::ec::Success);
  EXPECT_NE(first_login.account_id(), second_login.account_id());
  EXPECT_TRUE(service.authenticated(first_login.access_token()));
  EXPECT_TRUE(service.authenticated(second_login.access_token()));

  x5::pc2a::LoginUpgradeReq upgrade_request;
  upgrade_request.set_platform("Windows");
  const auto upgrade_reply = adapter.handle(
      "PUT", "/api/auth/LoginUpgradeReq", "Bearer " + first_login.access_token(),
      "application/protobuf", upgrade_request.SerializeAsString());
  ASSERT_TRUE(upgrade_reply);

  x5::pa2c::AuthChallengeRes upgraded;
  ASSERT_TRUE(upgraded.ParseFromString(upgrade_reply->body));
  EXPECT_EQ(upgraded.ec(), x5::ec::Success);
  EXPECT_EQ(upgraded.account_id(), first_login.account_id());
  EXPECT_FALSE(service.authenticated(first_login.access_token()));
  EXPECT_TRUE(service.authenticated(upgraded.access_token()));
  EXPECT_TRUE(service.authenticated(second_login.access_token()));

  const auto missing_identity = adapter.handle(
      "PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf",
      synthetic_steam_request());
  ASSERT_TRUE(missing_identity);
  EXPECT_EQ(missing_identity->status, 400);
  EXPECT_FALSE(adapter.authenticate_session("unmapped-platform-token"));
}

TEST(NativeAuth, ClaimedSteamIdentityCannotAuthenticateNamedPasswordAccount) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("named_player", "password-known-only-to-player"));

  const auto named_login = service.login("named_player", "password-known-only-to-player");
  ASSERT_EQ(named_login.ec(), x5::ec::Success);
  service.logout(named_login.access_token());

  auto config = local_config();
  config.separate_accounts_by_identity = true;
  NativeAuth adapter(service, config);

  const auto spoofed_request = adapter.handle(
      "PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf",
      synthetic_steam_request_for_identity("named_player"));
  ASSERT_TRUE(spoofed_request);

  x5::pa2c::AuthChallengeRes identity_login;
  ASSERT_TRUE(identity_login.ParseFromString(spoofed_request->body));
  ASSERT_EQ(identity_login.ec(), x5::ec::Success);
  EXPECT_NE(identity_login.account_id(), named_login.account_id());

  x5::pc2a::AuthReq password_request;
  password_request.set_account("named_player");
  password_request.set_pass_shadow("unverified-shadow");
  const auto password_route = adapter.handle(
      "PUT", "/api/auth/AuthReq", {}, "application/protobuf",
      password_request.SerializeAsString());
  ASSERT_TRUE(password_route);
  EXPECT_EQ(password_route->status, 401);
  EXPECT_FALSE(service.authenticated(named_login.access_token()));
}

TEST(NativeAuth, LaunchPrefixAuthenticatesOnlyItsNamedAccount) {
  MemoryRepositories repositories;
  auto current_time = Clock::now();
  AuthService service(repositories.character_repositories(), {},
                      [&current_time] { return current_time; });
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  ASSERT_TRUE(service.create_account("bob", "bob-password-123"));

  const auto alice_login = service.login("alice", "alice-password-123");
  const auto bob_login = service.login("bob", "bob-password-123");
  ASSERT_EQ(alice_login.ec(), x5::ec::Success);
  ASSERT_EQ(bob_login.ec(), x5::ec::Success);

  const auto alice_grant = service.issue_launch_grant(alice_login.access_token());
  const auto bob_grant = service.issue_launch_grant(bob_login.access_token());
  ASSERT_TRUE(alice_grant);
  ASSERT_TRUE(bob_grant);

  NativeAuth adapter(service, local_config());
  const auto alice_prefix = "/launch/" + *alice_grant;
  const auto bootstrap = adapter.handle(
      "GET", alice_prefix + "/api/auth/BootstrapReq", {}, {}, {});
  ASSERT_TRUE(bootstrap);
  ASSERT_EQ(bootstrap->status, 200);

  const auto bootstrap_json = nlohmann::json::parse(bootstrap->body);
  ASSERT_EQ(bootstrap_json.at("regions").size(), 1);
  const auto auth_address = bootstrap_json.at("regions").at(0).at("auth_address")
                                .get<std::string>();
  EXPECT_NE(auth_address.find(alice_prefix), std::string::npos);

  auto wrong_grant = *alice_grant;
  wrong_grant.back() = wrong_grant.back() == '0' ? '1' : '0';
  const auto wrong_prefix = "/launch/" + wrong_grant;
  const auto wrong_bootstrap = adapter.handle(
      "GET", wrong_prefix + "/api/auth/BootstrapReq", {}, {}, {});
  const auto wrong_steam = adapter.handle(
      "PUT", wrong_prefix + "/api/auth/steam/AuthReq", {},
      "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(wrong_bootstrap);
  ASSERT_TRUE(wrong_steam);
  EXPECT_EQ(wrong_bootstrap->status, 401);
  EXPECT_EQ(wrong_steam->status, 401);
  EXPECT_TRUE(service.authenticated(alice_login.access_token()));
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  const auto alice_steam = adapter.handle(
      "PUT", alice_prefix + "/api/auth/steam/AuthReq", {},
      "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(alice_steam);
  ASSERT_EQ(alice_steam->status, 200);

  x5::pa2c::AuthChallengeRes alice_launch;
  ASSERT_TRUE(alice_launch.ParseFromString(alice_steam->body));
  ASSERT_EQ(alice_launch.ec(), x5::ec::Success);
  EXPECT_EQ(alice_launch.account_id(), alice_login.account_id());
  EXPECT_NE(alice_launch.account_id(), bob_login.account_id());
  EXPECT_FALSE(service.authenticated(alice_login.access_token()));
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  x5::pc2a::LoginUpgradeReq upgrade_request;
  upgrade_request.set_platform("Windows");
  const auto upgrade = adapter.handle(
      "PUT", alice_prefix + "/api/auth/LoginUpgradeReq",
      "Bearer " + alice_launch.access_token(), "application/protobuf",
      upgrade_request.SerializeAsString());
  ASSERT_TRUE(upgrade);
  ASSERT_EQ(upgrade->status, 200);

  x5::pa2c::AuthChallengeRes upgraded_alice;
  ASSERT_TRUE(upgraded_alice.ParseFromString(upgrade->body));
  EXPECT_EQ(upgraded_alice.ec(), x5::ec::Success);
  EXPECT_EQ(upgraded_alice.account_id(), alice_login.account_id());
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));

  current_time += kLaunchGrantTtl + std::chrono::seconds(1);
  const auto expired_prefix = "/launch/" + *bob_grant;
  const auto expired_steam = adapter.handle(
      "PUT", expired_prefix + "/api/auth/steam/AuthReq", {},
      "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(expired_steam);
  EXPECT_EQ(expired_steam->status, 401);
  EXPECT_TRUE(service.authenticated(bob_login.access_token()));
}

TEST(NativeAuth, ActivatedLaunchReconnectsAfterServiceRestart) {
  MemoryRepositories repositories;
  std::string prefix;
  std::string previous_token;
  std::int64_t account_id{};
  {
    AuthService service(repositories.character_repositories());
    ASSERT_TRUE(service.create_account("reconnect", "reconnect-password"));
    const auto launcher = service.login("reconnect", "reconnect-password", true);
    ASSERT_EQ(launcher.ec(), x5::ec::Success);
    account_id = launcher.account_id();
    const auto grant = service.issue_launch_grant(launcher.access_token());
    ASSERT_TRUE(grant);
    prefix = "/launch/" + *grant;

    auto config = local_config();
    config.require_launch_grant = true;
    NativeAuth adapter(service, config);
    const auto initial = adapter.handle("PUT", prefix + "/api/auth/steam/AuthReq", {},
                                        "application/protobuf", synthetic_steam_request());
    ASSERT_TRUE(initial);
    ASSERT_EQ(initial->status, 200);
    x5::pa2c::AuthChallengeRes login;
    ASSERT_TRUE(login.ParseFromString(initial->body));
    ASSERT_EQ(login.account_id(), account_id);
    previous_token = login.access_token();
  }

  AuthService restarted(repositories.character_repositories());
  auto config = local_config();
  config.require_launch_grant = true;
  NativeAuth adapter(restarted, config);
  EXPECT_FALSE(restarted.authenticated(previous_token));

  const auto bootstrap = adapter.handle("GET", prefix + "/api/auth/BootstrapReq", {}, {}, {});
  ASSERT_TRUE(bootstrap);
  EXPECT_EQ(bootstrap->status, 200);
  const auto reconnected = adapter.handle("PUT", prefix + "/api/auth/steam/AuthReq", {},
                                          "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(reconnected);
  ASSERT_EQ(reconnected->status, 200);
  x5::pa2c::AuthChallengeRes login;
  ASSERT_TRUE(login.ParseFromString(reconnected->body));
  ASSERT_EQ(login.ec(), x5::ec::Success);
  EXPECT_EQ(login.account_id(), account_id);
  EXPECT_NE(login.access_token(), previous_token);

  const auto worlds = adapter.handle("GET", prefix + "/api/auth/WorldListReq",
                                     "Bearer " + login.access_token(), {}, {});
  ASSERT_TRUE(worlds);
  x5::pa2c::WorldListRes world_list;
  ASSERT_TRUE(world_list.ParseFromString(worlds->body));
  ASSERT_EQ(world_list.worlds_size(), 1);

  x5::pc2a::LoginUpgradeReq request;
  const auto upgrade = adapter.handle("PUT", prefix + "/api/auth/LoginUpgradeReq",
                                      "Bearer " + login.access_token(),
                                      "application/protobuf", request.SerializeAsString());
  ASSERT_TRUE(upgrade);
  ASSERT_EQ(upgrade->status, 200);
  x5::pa2c::AuthChallengeRes upgraded;
  ASSERT_TRUE(upgraded.ParseFromString(upgrade->body));
  ASSERT_EQ(upgraded.account_id(), account_id);
  const auto entry = adapter.handle("PUT", prefix + "/api/auth/WorldEnterReq/1",
                                    "Bearer " + upgraded.access_token(), {}, {});
  ASSERT_TRUE(entry);
  x5::pa2c::WorldEnterRes entered;
  ASSERT_TRUE(entered.ParseFromString(entry->body));
  EXPECT_EQ(entered.error_code(), x5::ec::Success);

  restarted.revoke_account_sessions(account_id);
  const auto revoked = adapter.handle("PUT", prefix + "/api/auth/steam/AuthReq", {},
                                     "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(revoked);
  EXPECT_EQ(revoked->status, 401);
}

TEST(NativeAuth, ReconnectCredentialExpiryAndAccountRevocation) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("alice", "alice-password-123"));
  ASSERT_TRUE(service.create_account("bob", "bob-password-123"));
  const auto alice = service.login("alice", "alice-password-123", true);
  const auto bob = service.login("bob", "bob-password-123", true);
  ASSERT_EQ(alice.ec(), x5::ec::Success);
  ASSERT_EQ(bob.ec(), x5::ec::Success);

  const auto alice_grant = service.issue_launch_grant(alice.access_token());
  const auto bob_grant = service.issue_launch_grant(bob.access_token());
  ASSERT_TRUE(alice_grant);
  ASSERT_TRUE(bob_grant);
  const auto alice_game = service.consume_launch_grant(*alice_grant);
  const auto bob_game = service.consume_launch_grant(*bob_grant);
  ASSERT_TRUE(alice_game);
  ASSERT_TRUE(bob_game);

  auto altered_grant = *alice_grant;
  altered_grant.back() = altered_grant.back() == '0' ? '1' : '0';
  EXPECT_FALSE(service.reconnect_launch_grant(altered_grant));
  service.logout(alice_game->access_token());
  const auto first_reconnect = service.reconnect_launch_grant(*alice_grant);
  ASSERT_TRUE(first_reconnect);
  const auto second_reconnect = service.reconnect_launch_grant(*alice_grant);
  ASSERT_TRUE(second_reconnect);
  EXPECT_FALSE(service.authenticated(first_reconnect->access_token()));
  EXPECT_TRUE(service.authenticated(second_reconnect->access_token()));
  EXPECT_TRUE(service.reconnect_launch_grant(*bob_grant));

  const auto replacement = service.issue_launch_grant(alice.access_token());
  ASSERT_TRUE(replacement);
  EXPECT_FALSE(service.reconnect_launch_grant(*alice_grant));
  EXPECT_TRUE(service.reconnect_launch_grant(*bob_grant));
  ASSERT_TRUE(service.consume_launch_grant(*replacement));
  repositories.accounts->save_reconnect_credential(alice.account_id(),
                                                   test_credential_digest(*replacement), 1);
  EXPECT_FALSE(service.reconnect_launch_grant(*replacement));

  const auto fresh = service.issue_launch_grant(alice.access_token());
  ASSERT_TRUE(fresh);
  ASSERT_TRUE(service.consume_launch_grant(*fresh));
  ASSERT_TRUE(service.set_account_password(alice.account_id(), "replacement-password"));
  EXPECT_FALSE(service.reconnect_launch_grant(*fresh));
  EXPECT_TRUE(service.reconnect_launch_grant(*bob_grant));

  ASSERT_TRUE(service.set_account_banned(bob.account_id(), true));
  EXPECT_FALSE(service.reconnect_launch_grant(*bob_grant));
  ASSERT_TRUE(service.set_account_banned(bob.account_id(), false));
  EXPECT_FALSE(service.reconnect_launch_grant(*bob_grant));
}

TEST(NativeAuthPostgres, CredentialRevocationAndExpiryAreDurable) {
  const auto *database_url = std::getenv("CHRONICLES_RECONNECT_TEST_DATABASE_URL");
  if (database_url == nullptr || *database_url == '\0') {
    GTEST_SKIP() << "Requires an isolated migrated PostgreSQL database";
  }

  const auto repositories = make_postgres_character_repositories(database_url);
  AuthService service(repositories);
  const auto account_name = "reconnect_" + random_hex(6);
  ASSERT_TRUE(service.create_account(account_name, "postgres-test-password"));
  auto login = service.login(account_name, "postgres-test-password", true);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  auto grant = service.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  ASSERT_TRUE(service.consume_launch_grant(*grant));

  AuthService restarted(repositories);
  EXPECT_TRUE(restarted.reconnect_launch_grant(*grant));
  repositories.accounts->save_reconnect_credential(login.account_id(),
                                                   test_credential_digest(*grant), 1);
  EXPECT_FALSE(restarted.reconnect_launch_grant(*grant));

  login = restarted.login(account_name, "postgres-test-password", true);
  grant = restarted.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  ASSERT_TRUE(restarted.consume_launch_grant(*grant));
  ASSERT_TRUE(restarted.set_account_password(login.account_id(), "new-postgres-password"));
  AuthService after_password_reset(repositories);
  EXPECT_FALSE(after_password_reset.reconnect_launch_grant(*grant));

  login = after_password_reset.login(account_name, "new-postgres-password", true);
  grant = after_password_reset.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  ASSERT_TRUE(after_password_reset.consume_launch_grant(*grant));
  ASSERT_TRUE(repositories.accounts->set_account_banned(login.account_id(), true));
  ASSERT_TRUE(repositories.accounts->set_account_banned(login.account_id(), false));
  AuthService after_unban(repositories);
  EXPECT_FALSE(after_unban.reconnect_launch_grant(*grant));
}

TEST(NativeAuth, StrictLaunchGrantModeRejectsUnprefixedInitialAuthentication) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account("named_player", "named-password-123"));

  const auto password_login = service.login("named_player", "named-password-123");
  ASSERT_EQ(password_login.ec(), x5::ec::Success);
  const auto launch_grant = service.issue_launch_grant(password_login.access_token());
  ASSERT_TRUE(launch_grant);

  auto config = local_config();
  config.require_launch_grant = true;
  NativeAuth adapter(service, config);

  const auto unprefixed_steam = adapter.handle(
      "PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf",
      synthetic_steam_request());
  ASSERT_TRUE(unprefixed_steam);
  EXPECT_EQ(unprefixed_steam->status, 401);
  EXPECT_FALSE(adapter.authenticate_session("unmapped-platform-token"));
  EXPECT_TRUE(service.authenticated(password_login.access_token()));

  const auto prefixed_steam = adapter.handle(
      "PUT", "/launch/" + *launch_grant + "/api/auth/steam/AuthReq", {},
      "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(prefixed_steam);
  ASSERT_EQ(prefixed_steam->status, 200);

  x5::pa2c::AuthChallengeRes launched;
  ASSERT_TRUE(launched.ParseFromString(prefixed_steam->body));
  EXPECT_EQ(launched.ec(), x5::ec::Success);
  EXPECT_EQ(launched.account_id(), password_login.account_id());
  EXPECT_FALSE(service.authenticated(password_login.access_token()));
  EXPECT_TRUE(service.authenticated(launched.access_token()));
}

TEST(NativeAuth, DisabledByDefaultAndRequiresLocalConfiguration) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  NativeAuth disabled(service, {});

  EXPECT_FALSE(disabled.handle("GET", "/native/bootstrap", {}, {}, {}));
  EXPECT_FALSE(disabled.handle("PUT", "/api/auth/LoginUpgradeReq", {}, "application/protobuf", {}));

  auto config = local_config();
  config.public_address = "http://192.0.2.1:8080";
  EXPECT_THROW(NativeAuth(service, config), std::invalid_argument);

  config = local_config();
  config.local_password.clear();
  EXPECT_THROW(NativeAuth(service, config), std::invalid_argument);
}

TEST(NativeAuth, BothNativeBootstrapPathsReturnRegionJson) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  NativeAuth adapter(service, local_config());

  for (const auto *target : {"/native/bootstrap", "/api/auth/BootstrapReq"}) {
    const auto response = adapter.handle("GET", target, {}, {}, {});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->content_type, "application/json");

    const auto document = nlohmann::json::parse(response->body);
    ASSERT_EQ(document.at("regions").size(), 1);
    EXPECT_EQ(document.at("regions").at(0).at("name"), "local");
    EXPECT_EQ(document.at("regions").at(0).at("auth_address"), "http://127.0.0.1:8080");
  }
}

TEST(NativeAuth, RejectsInvalidSteamBodiesWithoutCreatingSession) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  NativeAuth adapter(service, local_config());

  const auto wrong_media_type =
      adapter.handle("PUT", "/api/auth/steam/AuthReq", {}, "application/json", "{}");
  ASSERT_TRUE(wrong_media_type);
  EXPECT_EQ(wrong_media_type->status, 415);

  const auto empty_request =
      adapter.handle("PUT", "/api/auth/steam/AuthReq", {}, "application/protobuf", {});
  ASSERT_TRUE(empty_request);
  EXPECT_EQ(empty_request->status, 400);

  const auto malformed_request = adapter.handle("PUT", "/api/auth/steam/AuthReq", {},
                                                "application/protobuf", std::string(1, '\x80'));
  ASSERT_TRUE(malformed_request);
  EXPECT_EQ(malformed_request->status, 400);

  const auto oversized_request = adapter.handle("PUT", "/api/auth/steam/AuthReq", {},
                                                "application/protobuf", std::string(65537, 'x'));
  ASSERT_TRUE(oversized_request);
  EXPECT_EQ(oversized_request->status, 413);
}

TEST(NativeAuth, LocalSteamMappingEntersWorldWithNativeProtobufReplies) {
  MemoryRepositories repositories;
  Config service_config;
  service_config.lobby_address = "http://127.0.0.1:8080";
  AuthService service(repositories.character_repositories(), service_config);
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth adapter(service, local_config());

  const auto login_response = adapter.handle("PUT", "/api/auth/steam/AuthReq", {},
                                             "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(login_response);
  ASSERT_EQ(login_response->status, 200);
  EXPECT_EQ(login_response->content_type, "application/protobuf");

  x5::pa2c::AuthChallengeRes login;
  ASSERT_TRUE(login.ParseFromString(login_response->body));
  ASSERT_EQ(login.ec(), x5::ec::Success);
  ASSERT_GT(login.account_id(), 0);
  ASSERT_FALSE(login.access_token().empty());
  const auto authorization = "Bearer " + login.access_token();

  const auto list_response = adapter.handle("GET", "/api/auth/WorldListReq", authorization, {}, {});
  ASSERT_TRUE(list_response);
  ASSERT_EQ(list_response->status, 200);

  x5::pa2c::WorldListRes worlds;
  ASSERT_TRUE(worlds.ParseFromString(list_response->body));
  ASSERT_EQ(worlds.worlds_size(), 1);
  EXPECT_EQ(worlds.region(), "local");
  EXPECT_EQ(worlds.worlds(0).version(), "fixture-build");
  EXPECT_EQ(worlds.worlds(0).protocol(), "fixture-protocol");

  const auto enter_response =
      adapter.handle("PUT", "/api/auth/WorldEnterReq/1", authorization, {}, {});
  ASSERT_TRUE(enter_response);
  ASSERT_EQ(enter_response->status, 200);

  x5::pa2c::WorldEnterRes entered;
  ASSERT_TRUE(entered.ParseFromString(enter_response->body));
  EXPECT_EQ(entered.error_code(), x5::ec::Success);
  EXPECT_EQ(entered.world_id(), 1);
  ASSERT_TRUE(entered.has_granted());
  EXPECT_EQ(entered.granted().front_rest_address(), service_config.lobby_address);
  EXPECT_TRUE(entered.granted().allow_char_create());
  EXPECT_EQ(service.list(login.access_token()).ec(), x5::ec::Success);

  const auto invalid_world = adapter.handle("GET", "/api/auth/WorldGet/9", authorization, {}, {});
  ASSERT_TRUE(invalid_world);
  ASSERT_TRUE(worlds.ParseFromString(invalid_world->body));
  EXPECT_EQ(worlds.error_code(), x5::ec::InvalidWorld);
  EXPECT_EQ(worlds.worlds_size(), 0);

  for (const auto *target : {"/api/auth/WorldEnterReq/-1", "/api/auth/WorldEnterReq/1x",
                             "/api/auth/WorldEnterReq/9999999999999999999999999999"}) {
    const auto invalid_path = adapter.handle("PUT", target, authorization, {}, {});
    ASSERT_TRUE(invalid_path);
    EXPECT_EQ(invalid_path->status, 400);
  }
}

TEST(NativeAuth, PublicWorldListingDoesNotAuthenticateInvalidCredentials) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));

  auto config = local_config();
  config.local_password = "incorrect-synthetic-password";
  NativeAuth adapter(service, config);

  const auto login_response = adapter.handle("PUT", "/api/auth/steam/AuthReq", {},
                                             "application/protobuf", synthetic_steam_request());
  ASSERT_TRUE(login_response);

  x5::pa2c::AuthChallengeRes login;
  ASSERT_TRUE(login.ParseFromString(login_response->body));
  EXPECT_EQ(login.ec(), x5::ec::LoginDenyAccountMissingOrPasswordNotMatched);
  EXPECT_TRUE(login.access_token().empty());

  const auto worlds = adapter.handle("GET", "/api/auth/WorldListReq", "Bearer invalid", {}, {});
  ASSERT_TRUE(worlds);
  EXPECT_EQ(worlds->status, 200);
  EXPECT_FALSE(service.authenticated("invalid"));

  EXPECT_FALSE(adapter.authenticate_session("invalid"));
  const auto entry = adapter.handle("PUT", "/api/auth/WorldEnterReq/1", "Bearer invalid", {}, {});
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->status, 401);
}

TEST(NativeAuth, LoginUpgradeReturnsNativeChallengeAndRotatesAnExistingLocalAlias) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth adapter(service, local_config());

  const auto login = service.login(kFixtureAccount, kFixturePassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  const std::string alias = "synthetic-upgrade-alias";
  ASSERT_TRUE(service.bind_local_session(login.access_token(), alias));

  // Observed schema: platform field 1 plus explicitly present false withdrawal field 6.
  const auto fixture = hex_decode("0a0757696e646f77733000");
  x5::pc2a::LoginUpgradeReq request;
  ASSERT_TRUE(request.ParseFromString(fixture));
  ASSERT_EQ(request.platform(), "Windows");
  ASSERT_TRUE(request.has_cancel_withdrawal());
  ASSERT_FALSE(request.cancel_withdrawal());
  ASSERT_EQ(request.SerializeAsString(), fixture);

  const auto reply = adapter.handle("PUT", "/api/auth/LoginUpgradeReq", "Bearer " + alias,
                                    "application/protobuf", fixture);
  ASSERT_TRUE(reply);
  ASSERT_EQ(reply->status, 200);
  EXPECT_EQ(reply->content_type, "application/protobuf");

  x5::pa2c::AuthChallengeRes upgraded;
  ASSERT_TRUE(upgraded.ParseFromString(reply->body));
  EXPECT_EQ(upgraded.ec(), x5::ec::Success);
  EXPECT_EQ(upgraded.account_id(), login.account_id());
  ASSERT_FALSE(upgraded.access_token().empty());
  EXPECT_NE(upgraded.access_token(), login.access_token());
  EXPECT_FALSE(service.authenticated(login.access_token()));
  EXPECT_FALSE(service.authenticated(alias));
  EXPECT_TRUE(service.authenticated(upgraded.access_token()));

  const auto entered = adapter.handle("PUT", "/api/auth/WorldEnterReq/1",
                                      "Bearer " + upgraded.access_token(), {}, {});
  ASSERT_TRUE(entered);
  ASSERT_EQ(entered->status, 200);
  x5::pa2c::WorldEnterRes world;
  ASSERT_TRUE(world.ParseFromString(entered->body));
  EXPECT_EQ(world.error_code(), x5::ec::Success);
  EXPECT_EQ(service.list(upgraded.access_token()).ec(), x5::ec::Success);
}

TEST(NativeAuth, LoginUpgradeRejectsInvalidMalformedExpiredAndRevokedSessions) {
  MemoryRepositories repositories;
  auto current_time = Clock::now();
  Config config;
  config.session_ttl = std::chrono::seconds(1);
  AuthService service(repositories.character_repositories(), config,
                      [&current_time] { return current_time; });
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth adapter(service, local_config());
  const auto login = service.login(kFixtureAccount, kFixturePassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  const auto authorization = "Bearer " + login.access_token();
  const auto fixture = hex_decode("0a0757696e646f7773");

  for (const std::string invalid_authorization :
       {std::string{}, std::string("Bearer "), std::string("Bearer unknown-upgrade-token"),
        std::string("Basic unsupported"), "Bearer " + std::string(4097, 'p')}) {
    const auto reply = adapter.handle("PUT", "/api/auth/LoginUpgradeReq", invalid_authorization,
                                      "application/protobuf", fixture);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, 401);
  }
  EXPECT_FALSE(service.authenticated("unknown-upgrade-token"));
  EXPECT_TRUE(service.authenticated(login.access_token()));

  const auto wrong_type =
      adapter.handle("PUT", "/api/auth/LoginUpgradeReq", authorization, "application/json", "{}");
  ASSERT_TRUE(wrong_type);
  EXPECT_EQ(wrong_type->status, 415);
  const auto malformed = adapter.handle("PUT", "/api/auth/LoginUpgradeReq", authorization,
                                        "application/protobuf", std::string(1, '\x80'));
  ASSERT_TRUE(malformed);
  EXPECT_EQ(malformed->status, 400);
  EXPECT_TRUE(service.authenticated(login.access_token()));

  auto wrong_credentials = local_config();
  wrong_credentials.local_password = "incorrect-upgrade-fixture-password";
  NativeAuth misconfigured(service, wrong_credentials);
  const auto denied = misconfigured.handle("PUT", "/api/auth/LoginUpgradeReq", authorization,
                                           "application/protobuf", fixture);
  ASSERT_TRUE(denied);
  ASSERT_EQ(denied->status, 200);
  x5::pa2c::AuthChallengeRes denied_upgrade;
  ASSERT_TRUE(denied_upgrade.ParseFromString(denied->body));
  EXPECT_EQ(denied_upgrade.ec(), x5::ec::LoginDenyAccountMissingOrPasswordNotMatched);
  EXPECT_TRUE(denied_upgrade.access_token().empty());
  EXPECT_TRUE(service.authenticated(login.access_token()));

  current_time += std::chrono::seconds(2);
  const auto expired = adapter.handle("PUT", "/api/auth/LoginUpgradeReq", authorization,
                                      "application/protobuf", fixture);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->status, 401);
  EXPECT_FALSE(service.authenticated(login.access_token()));

  const auto replacement = service.login(kFixtureAccount, kFixturePassword);
  ASSERT_EQ(replacement.ec(), x5::ec::Success);
  service.logout(replacement.access_token());
  const auto revoked =
      adapter.handle("PUT", "/api/auth/LoginUpgradeReq", "Bearer " + replacement.access_token(),
                     "application/protobuf", fixture);
  ASSERT_TRUE(revoked);
  EXPECT_EQ(revoked->status, 401);
  EXPECT_FALSE(service.authenticated(replacement.access_token()));
}

TEST(NativeAuth, PublicMetadataPrecedesLocalMappingOfRetainedClientToken) {
  MemoryRepositories repositories;
  Config service_config;
  service_config.world_name = "Metadata fixture world";
  service_config.site_name = "fixture-site";
  AuthService service(repositories.character_repositories(), service_config);
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth adapter(service, local_config());
  const std::string retained_token(847, 'p');
  const auto authorization = "Bearer " + retained_token;

  for (const auto *target : {"/api/auth/WorldListReq", "/api/auth/WorldGet/1"}) {
    const auto reply = adapter.handle("GET", target, authorization, {}, {});
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->status, 200);

    x5::pa2c::WorldListRes worlds;
    ASSERT_TRUE(worlds.ParseFromString(reply->body));
    ASSERT_EQ(worlds.worlds_size(), 1);
    EXPECT_EQ(worlds.worlds(0).world_name(), service_config.world_name);
    EXPECT_EQ(worlds.worlds(0).site_name(), service_config.site_name);
    EXPECT_TRUE(worlds.world_character_counts().empty());
    EXPECT_FALSE(service.authenticated(retained_token));
  }

  const auto entry = adapter.handle("PUT", "/api/auth/WorldEnterReq/1", authorization, {}, {});
  ASSERT_TRUE(entry);
  ASSERT_EQ(entry->status, 200);
  EXPECT_TRUE(service.authenticated(retained_token));
  EXPECT_EQ(service.list(retained_token).ec(), x5::ec::Success);
  EXPECT_TRUE(adapter.authenticate_session(retained_token));
}

TEST(NativeAuth, SessionMappingRequiresEnabledAdapterValidCredentialsAndBoundedToken) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth disabled(service, {});
  EXPECT_FALSE(disabled.authenticate_session("synthetic-retained-token"));

  NativeAuth adapter(service, local_config());
  EXPECT_FALSE(adapter.authenticate_session({}));
  EXPECT_FALSE(adapter.authenticate_session(std::string(4097, 'p')));
  EXPECT_FALSE(service.bind_local_session("missing-local-session", "synthetic-retained-token"));

  const auto login = service.login(kFixtureAccount, kFixturePassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  EXPECT_FALSE(service.bind_local_session(login.access_token(), {}));
  EXPECT_FALSE(service.bind_local_session(login.access_token(), std::string(4097, 'p')));
}

TEST(NativeAuth, LogoutAcceptsUnknownTokensAndRevokesLocalSessionAndHandoff) {
  MemoryRepositories repositories;
  AuthService service(repositories.character_repositories());
  ASSERT_TRUE(service.create_account(kFixtureAccount, kFixturePassword));
  NativeAuth adapter(service, local_config());

  const auto unknown_logout = adapter.handle("PUT", "/api/auth/AuthLogoutReq",
                                             "Bearer synthetic-old-platform-token", {}, {});
  ASSERT_TRUE(unknown_logout);
  EXPECT_EQ(unknown_logout->status, 200);
  EXPECT_EQ(unknown_logout->content_type, "application/protobuf");
  EXPECT_TRUE(unknown_logout->body.empty());

  const auto login = service.login(kFixtureAccount, kFixturePassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  const std::string alias = "synthetic-retained-client-token";
  ASSERT_TRUE(service.bind_local_session(login.access_token(), alias));
  const auto authorization = "Bearer " + alias;

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(service.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("LogoutFixture");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = service.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = service.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  ASSERT_FALSE(selected.char_token().empty());

  const auto logout = adapter.handle("PUT", "/api/auth/AuthLogoutReq", authorization, {}, {});
  ASSERT_TRUE(logout);
  EXPECT_EQ(logout->status, 200);
  EXPECT_TRUE(logout->body.empty());
  EXPECT_FALSE(service.authenticated(login.access_token()));
  EXPECT_FALSE(service.authenticated(alias));
  EXPECT_FALSE(service.consume_ticket(selected.char_token()));

  const auto repeated_logout =
      adapter.handle("PUT", "/api/auth/AuthLogoutReq", authorization, {}, {});
  ASSERT_TRUE(repeated_logout);
  EXPECT_EQ(repeated_logout->status, 200);
}

} // namespace
} // namespace chronicles
