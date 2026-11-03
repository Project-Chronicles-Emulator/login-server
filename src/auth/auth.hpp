#pragma once

#include "p_a2c.pb.h"
#include "p_a2c.ws.pb.h"
#include "p_c2a.ws.pb.h"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.pb.h"
#include "persistence/character_repositories.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <google/protobuf/message.h>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chronicles {
class AuthAuthority;
using Clock = std::chrono::steady_clock;

inline constexpr auto kLaunchGrantTtl = std::chrono::seconds(300);
inline constexpr auto kReconnectCredentialTtl = std::chrono::hours(24);

inline constexpr unsigned kDefaultWorldCapacity = 500;
inline constexpr unsigned kMaximumWorldCapacity = 100000;

// Realm names are shown in the client's realm list and the launcher; keep them short.
inline constexpr std::size_t kMaximumRealmTextBytes = 64;

// Emulator policy, not a recovered official threshold: the realm list reports Congested from
// this share of capacity and Overloaded once capacity is reached.
inline constexpr unsigned kCongestedPopulationPercent = 80;

struct Config {
  // Non-secret PostgreSQL identity used only on the private service handshake.
  std::string authority_database_identity;
  int world_id{1};
  std::string world_name{"Project Chronicles Private Server"};
  std::string site_name{"local"};
  std::string site_display_name{"Project Chronicles"};
  // Maximum characters connected to the world service at once. Realm entry is refused with
  // ServerOverload while the world is full; the official queue ticket is not modeled.
  unsigned world_capacity{kDefaultWorldCapacity};
  x5::pb::ServerLabel world_label{x5::pb::ServerLabel_None};
  // Lists the realm as GM-only and refuses every realm entry with WorldIsUnderMaintenance.
  bool world_maintenance{};
  // Lists the realm as closed to new characters and refuses CharCreateReq.
  bool char_creation_restricted{};
  std::string lobby_address{"http://127.0.0.1:8080/emulator/v1"};
  // World listener is deliberately not advertised until a real world service exists.
  std::string game_address;
  std::chrono::seconds session_ttl{3600};
  std::chrono::seconds ticket_ttl{60};
};

struct Handoff {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::string packet_key;
  Clock::time_point expiry;
  // Copied from the lobby session's account role when the character is selected. The world
  // service uses only this flag to authorize GM packets; the client never supplies it.
  bool game_master{};
  // Process-local identifier of the character selection that issued this handoff. It is not
  // a credential; zero never matches, so a handoff not issued by select() cannot be admitted.
  std::uint64_t admission_id{};
  // Private login-service incarnation; never sent in the player's native packets.
  std::string authority_id;
};

struct CredentialPrefixMatch {
  std::size_t offset{};
  std::size_t length{};
};

struct WorldCredentialDiagnostic {
  CredentialPrefixMatch packet_key;
  CredentialPrefixMatch char_token;
};

struct ChatIdentity {
  int world_id{};
  std::int64_t character_id{};
  std::string name;
};

// Counts characters whose world-service credential was accepted. Lobby sessions are not
// counted: they remain valid until their TTL even after the client has closed.
class WorldPopulation {
public:
  // Keeps one character counted until it is released or destroyed. The shared counter lets a
  // lease safely outlive the population object during shutdown.
  class Lease {
  public:
    Lease() = default;

    explicit Lease(std::shared_ptr<std::atomic<unsigned>> counter);

    Lease(Lease &&other) noexcept;

    Lease &operator=(Lease &&other) noexcept;

    Lease(const Lease &) = delete;

    Lease &operator=(const Lease &) = delete;

    ~Lease();

    void release() noexcept;

  private:
    std::shared_ptr<std::atomic<unsigned>> counter_;
  };

  Lease admit();

  unsigned online() const;

private:
  std::shared_ptr<std::atomic<unsigned>> counter_ = std::make_shared<std::atomic<unsigned>>(0);
};

// Realm presentation shared by the native realm list and the launcher health summary.
struct WorldStatus {
  int world_id{};
  std::string name;
  unsigned online{};
  unsigned capacity{};
  x5::pb::ServerState state{x5::pb::ServerState_Normal};
  bool char_creation_restricted{};
};

// Realm presentation and admission policy that the operator console can change while the
// server runs. Starts from Config; changes apply to the next realm list and admission check.
struct RealmSettings {
  std::string world_name;
  std::string site_display_name;
  unsigned capacity{kDefaultWorldCapacity};
  x5::pb::ServerLabel label{x5::pb::ServerLabel_None};
  bool maintenance{};
  bool char_creation_restricted{};
};

// Realm text is copied into protobuf strings rendered by the client: 1-64 bytes, no control
// characters. Throws std::invalid_argument naming `field` otherwise.
void validate_realm_text(std::string_view field, const std::string &value);

// Throws std::invalid_argument when a field is outside the accepted ranges.
void validate_realm_settings(const RealmSettings &settings);

// Launcher/console names for a realm state: offline, maintenance, congested, full or normal.
std::string_view world_state_label(x5::pb::ServerState state);

// The lobby character-name rule: 3-16 ASCII letters or digits.
bool is_valid_character_name(std::string_view name);

// Deletion blockers reported by realm services outside AuthService.
struct CharacterDeletionCheck {
  // CharDeleteGuildMaster, CharDeleteGuildMember or CharDeleteHavingHousing; Success when
  // nothing blocks the deletion.
  x5::ec::EErrorCode block{x5::ec::Success};
  // CharDeletePreCheckRes.block_reasons (only Housing, Market and GuildMaster exist).
  std::vector<x5::pb::frontwebsocket::CharDeleteBlockReason> block_reasons;
};

// Character state owned by other realm services (companies, guilds, housing). AuthService asks
// it while building lobby and selection responses and before deleting a character; it is called
// with AuthService's lock held, so an implementation must never call back into AuthService.
class CharacterSocialState {
public:
  virtual ~CharacterSocialState() = default;

  // LobbyCharacter.company_type and company_contribution.
  virtual void decorate_lobby(std::int64_t char_id, x5::pb::LobbyCharacter &character) const = 0;

  // WorldCharacter.company, guild and housing_site in CharSelectRes.
  virtual void decorate_world(std::int64_t char_id, x5::pb::WorldCharacter &character) const = 0;

  virtual CharacterDeletionCheck deletion_check(std::int64_t char_id) const = 0;
};

class Unauthorized : public std::runtime_error {
public:
  Unauthorized() : std::runtime_error("Authentication required") {}
};

std::string random_hex(std::size_t bytes);

std::string hex_encode(const std::string &bytes);

std::string hex_decode(const std::string &hex);

nlohmann::json message_json(const google::protobuf::Message &message);

void parse_json(const nlohmann::json &json, google::protobuf::Message &message);

class AuthService {
public:
  // Primary constructor: the three persistence repositories the service reads and commits
  // (accounts, character rows, revisioned character state).
  AuthService(
      CharacterRepositories repositories, Config config = {},
      std::function<Clock::time_point()> now = [] { return Clock::now(); });

  // Startup-only wiring for a game process whose session authority is a separate login service.
  void set_auth_authority(std::shared_ptr<AuthAuthority> authority);

  void require_world_heartbeat();

  void publish_world_status();

  // Called only after private service authentication by the login listener.
  nlohmann::json handle_authority_operation(std::string_view operation,
                                            const nlohmann::json &arguments);

  // Runs on a worker after the connection identity has been published for revocation.
  // The final I/O continuation still performs its local once-only claim without I/O.
  bool prepare_world_admission(const Handoff &handoff);

  bool create_account(std::string name, const std::string &password);

  // Startup wiring, before requests are served. Null removes the decoration and blockers.
  void set_character_social_state(std::shared_ptr<const CharacterSocialState> state);

  // Operator-only privilege change for an existing account (chronicles-account role).
  // Returns false for an invalid or unknown account name.
  bool set_account_role(std::string name, AccountRole role);

  x5::pa2c::AuthChallengeRes login(std::string name, const std::string &password,
                                  bool launcher_session = false,
                                  bool *credential_mismatch = nullptr);

  // A password-authenticated launcher session may issue one short-lived grant for a native
  // client launch. The native Steam request consumes it and receives a session for that account.
  std::optional<std::string> issue_launch_grant(const std::string &token);

  bool valid_launch_grant(std::string_view grant);

  std::optional<x5::pa2c::AuthChallengeRes> consume_launch_grant(std::string_view grant);

  // Reauthenticate an already activated game credential after a disconnect or restart.
  std::optional<x5::pa2c::AuthChallengeRes> reconnect_launch_grant(std::string_view grant);

  // Replace a live token without changing its account during native login upgrade.
  std::optional<x5::pa2c::AuthChallengeRes> rotate_session(const std::string &token);

  bool authenticated(const std::string &token);

  // Only the server's verified Discord OAuth completion may call these methods.
  x5::pa2c::AuthChallengeRes login_discord(const std::string &verified_discord_id);
  bool link_discord(const std::string &token, const std::string &verified_discord_id);
  std::string account_name(const std::string &token);

  // Identity is established by owner-checked character selection, never by a chat payload.
  std::optional<ChatIdentity> chat_identity(const std::string &token);

  // The stored row of the session's selected character (level, class and name are the
  // authority for guild membership). Throws Unauthorized for an invalid realm session;
  // nullopt when no character is selected or it no longer exists.
  std::optional<Character> selected_character(const std::string &token);

  // The account and realm of a session that may use `character_id`. `selected_only` false also
  // accepts any character the session's account owns on its realm (hot-key loads arrive before
  // CharSelectReq). Nullopt when the token, world session or ownership check fails.
  struct CharacterOwner {
    std::int64_t account_id{};
    int world_id{};
  };

  std::optional<CharacterOwner> character_owner(const std::string &token,
                                                std::int64_t character_id, bool selected_only);

  bool bind_local_session(const std::string &valid_local_token, const std::string &presented_token);

  void logout(const std::string &token);

  x5::pa2c::WorldListRes public_worlds() const;

  x5::pa2c::WorldListRes worlds(const std::string &token);

  WorldStatus world_status() const;

  RealmSettings realm_settings() const;

  // Operator console only. Validates, then replaces the live realm settings atomically.
  void update_realm_settings(const RealmSettings &settings);

  // Operator console only. Applies the normal password rules and revokes the account's
  // sessions, so the old password cannot keep a session alive. Throws std::invalid_argument
  // for an unacceptable password; returns false when the account does not exist.
  bool set_account_password(std::int64_t account_id, const std::string &password);

  // A ban and its credential revocation are serialized with native login.
  bool set_account_banned(std::int64_t account_id, bool banned);

  // Revokes every session, selection ticket and launch grant belonging to the account.
  // Characters already connected to the world service are disconnected separately.
  void revoke_account_sessions(std::int64_t account_id);

  // Block reconnects in this process when a revocation request cannot reach storage.
  void quarantine_account_sessions(std::int64_t account_id);

  // Live lobby sessions per account ID; used by the operator console's account list.
  std::map<std::int64_t, unsigned> lobby_session_counts();

  // The world service holds the returned lease while an accepted character stays connected.
  WorldPopulation::Lease admit_world_character();

  x5::pa2c::WorldEnterRes enter(const std::string &token,
                                const x5::pb::authwebsocket::WorldEnterReq &request);

  x5::pb::frontwebsocket::CharListRes list(const std::string &token);

  nlohmann::json launcher_characters(const std::string &token);

  x5::pb::frontwebsocket::FriendListRes friends(const std::string &token,
                                                const x5::pb::ws::front::FriendListReq &request);

  x5::pb::frontwebsocket::CharCreateRes create(const std::string &token,
                                               const x5::pb::ws::front::CharCreateReq &request);

  x5::pb::frontwebsocket::CharSelectRes select(const std::string &token,
                                               const x5::pb::ws::front::CharSelectReq &request);

  x5::pb::frontwebsocket::CharDeletePreCheckRes
  delete_precheck(const std::string &token,
                  const x5::pb::ws::front::CharDeletePreCheckReq &request);

  // Deletion is refused until pending-deletion state and official block
  // reasons can be persisted and verified; Cancel/Immediate only report the
  // resulting state for owned characters.
  x5::pb::frontwebsocket::CharDeleteRes
  delete_character(const std::string &token,
                   const x5::pb::ws::front::CharDeleteReq &request);

  x5::pb::frontwebsocket::CharDeleteCancelRes
  delete_cancel(const std::string &token,
                const x5::pb::ws::front::CharDeleteCancelReq &request);

  x5::pb::frontwebsocket::CharDeleteImmediateRes
  delete_immediate(const std::string &token,
                   const x5::pb::ws::front::CharDeleteImmediateReq &request);

  x5::pb::frontwebsocket::LookupCharIdByNameRes
  lookup_char_id(const std::string &token,
                 const x5::pb::ws::front::LookupCharIdByNameReq &request);

  std::optional<Handoff> consume_ticket(const std::string &token);

  // The callback takes only the short admission-registry lock; no database or network work.
  // A refused reservation leaves the one-use ticket available for a later retry.
  std::optional<Handoff> consume_packet_key(
      std::string_view packet_key, const std::function<bool(const Handoff &)> &admit = {},
      bool *admission_busy = nullptr);

  // The native world startup supplies the character token in a fixed-size field.
  // Compare against pending handoffs without copying it into a std::string.
  // `admission_busy` has the same meaning as for consume_packet_key.
  std::optional<Handoff> consume_world_token(
      std::string_view char_token, const std::function<bool(const Handoff &)> &admit = {},
      bool *admission_busy = nullptr);

  // Final world-admission point for a consumed handoff. Succeeds once, only while the handoff
  // is unexpired and is still its account's latest character selection. Reselection and
  // account revocation (kick, ban, password reset, uncertain storage outcome) cancel it.
  // Takes only a leaf mutex, so the world stream executor may call it. The caller must make
  // the connection visible to disconnect_account() before claiming: a revocation that runs
  // after a successful claim must find and close the admitted connection.
  bool claim_world_admission(const Handoff &handoff);

  // Resolve the selected character after a world handoff has been consumed.
  std::optional<Character> world_character(const Handoff &handoff);

  // Commits item mutations under the same revision as world progress.
  bool update_world_items(const Handoff &handoff, std::int64_t expected_revision,
                          const std::vector<CharacterItem> &items);

  bool commit_world_death(
      const Handoff &handoff, std::int64_t expected_revision,
      const std::vector<EquippedDurabilityChange> &durability_changes);

  bool commit_world_revival(const Handoff &handoff, std::int64_t expected_revision,
                            const WorldPosition &position);

  bool commit_world_shrine_revival(const Handoff &handoff, const WorldProgress &before,
                                   const WorldProgress &after, const WorldPosition &position);

  bool discard_world_items(const Handoff &handoff, std::int64_t expected_revision,
                           const std::vector<std::int64_t> &item_ids,
                           const std::map<int, int> &stackable_counts);

  bool apply_world_item_polish(const Handoff &handoff, std::int64_t expected_revision,
                               const ItemPolishChange &change);

  bool save_world_position(const Handoff &handoff, const WorldPosition &position);

  WorldProgress world_progress(const Handoff &handoff);

  bool save_world_progress(const Handoff &handoff, const WorldProgress &before,
                           const WorldProgress &after);

  bool fence_world_entry(const Handoff &handoff);

  std::optional<DeathReconciliationSnapshot> fence_world_death_reconciliation(
      const Handoff &handoff);

  std::vector<std::int64_t> offline_admin_quarantines();

  bool commit_world_progress_and_position(const Handoff &handoff, const WorldProgress &before,
                                          const WorldProgress &after,
                                          const WorldPosition &position);

  // Commits world progress together with item instance grants and removals atomically.
  std::optional<std::vector<CharacterItem>>
  commit_world_item_trade(const Handoff &handoff, const WorldProgress &before,
                          const WorldProgress &after,
                          const ItemTrade &trade);

  std::optional<QuestServiceMembership> read_quest_service_membership(const Handoff &handoff);

  std::optional<std::vector<CharacterItem>> commit_unaffiliated_activity(
      const Handoff &handoff, const QuestServiceMembership &membership,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade);

  QuestMutationResult commit_quest_mutation(
      const Handoff &handoff, const std::string &effect_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects);

  QuestMutationResult
  commit_combat_quest_event(const Handoff &handoff, const std::string &operation_id,
                            const std::string &source_fingerprint, const WorldProgress &before,
                            const std::optional<WorldProgress> &after, const ItemTrade &trade,
                            const QuestServiceEffects &effects);
  MonsterRewardCommitResult commit_monster_reward_with_effects(
      const Handoff &handoff, const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects);

  LootClaimResult lookup_world_loot_claim(const Handoff &handoff, const std::string &receipt_id,
                                         const std::string &corpse_fingerprint);

  LootClaimResult commit_world_loot_claim(const Handoff &handoff, const std::string &receipt_id,
                                         const std::string &corpse_fingerprint,
                                         const WorldProgress &before, const WorldProgress &after,
                                         const ItemTrade &trade);

  LootClaimResult commit_gathering_quest_claim(
      const Handoff &handoff, const std::string &receipt_id,
      const std::string &source_fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source);
  GatheringFinalizeResult finalize_gathering_quest_claim(
      const Handoff &handoff, const std::string &receipt_id,
      const std::string &source_fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmed_epoch_ticks);

  std::vector<SharedGatheringSourceRecord> shared_gathering_sources(
      int world_id, const std::optional<SharedGatheringSourceKey> &after, std::size_t limit);
  SharedGatheringSourceUpdateResult fence_shared_gathering_source(
      int world_id, const SharedGatheringSourceTransition &initial);
  SharedGatheringSourceUpdateResult update_shared_gathering_source(
      int world_id, const SharedGatheringSourceTransition &transition,
      std::int64_t epoch_ticks, bool require_due);

  bool enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts);
  bool enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &receipts,
      const std::optional<BossScriptIntent> &script);
  std::vector<BossScriptIntent> pending_boss_scripts(
      int world_id, const std::optional<std::string> &after, std::size_t limit);
  BossScriptSettlement settle_shared_boss_script(const BossScriptIntent &intent);
  bool mark_boss_script_published(const BossScriptIntent &intent);
  bool mark_private_boss_script_applied(const BossScriptIntent &intent);
  bool retire_private_boss_script(const BossScriptIntent &intent);
  std::int64_t acquire_monster_reward_owner_epoch();
  void adopt_monster_reward_owner_epoch(std::int64_t epoch);
  bool mark_monster_reward_ready(const std::string &receipt_id);
  std::size_t recover_staged_monster_rewards(std::size_t limit);
  bool has_staged_monster_rewards();
  std::vector<MonsterRewardReceipt> pending_monster_rewards(const Handoff &handoff);
  MonsterRewardCommitResult commit_monster_reward(
      const Handoff &handoff, const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade);

  // Inspect pending handoffs without consuming one or returning any credential bytes.
  WorldCredentialDiagnostic diagnose_world_credential(std::string_view received);

private:
  struct Session {
    std::int64_t account{};
    int world{};
    Clock::time_point expiry;
    bool launcher{};
    std::optional<ChatIdentity> selected_character;
    // Account role at login; carried through grants, rotation and local aliases.
    bool game_master{};
  };

  struct LaunchGrant {
    std::int64_t account{};
    Clock::time_point expiry;
    bool game_master{};
  };

  // The latest character selection of an account that has not yet entered the world.
  struct PendingWorldAdmission {
    std::uint64_t admission_id{};
    Clock::time_point expiry;
    std::string authority_id;
  };

  // Call these helpers while holding mutex_; both access session/ticket state.
  Session require_session(const std::string &token, bool in_world = false);

  Session authority_session(const std::string &token, bool in_world);

  std::optional<Handoff> consume_authority_handoff(
      std::string_view kind, std::string_view credential,
      const std::function<bool(const Handoff &)> &admit, bool *admission_busy);

  Handoff register_authority_handoff(const std::string &token, std::int64_t character_id,
                                      const std::string &char_token,
                                      const std::string &packet_key);

  bool authority_account_mutation(std::string_view operation,
                                   const nlohmann::json &arguments,
                                   std::int64_t account_id, bool revoke_local = true);

  void prune_expired();

  x5::pa2c::AuthChallengeRes create_login_session(const Account &account,
                                                 bool launcher_session);

  static Account make_password_account(std::string name, const std::string &password);

  x5::pb::ServerState current_world_state(unsigned online, const RealmSettings &realm) const;

  // Only characters belonging to the session account satisfy a request.
  static const Character *find_owned_character(const std::vector<Character> &characters,
                                               std::int64_t character_id);

  // Call while holding mutex_. Explicit revocation removes persistent credentials first.
  void revoke_account_sessions_locked(std::int64_t account_id);

  // Password changes and bans already revoke persistent credentials in their storage transaction.
  void invalidate_account_sessions_locked(std::int64_t account_id);

  // A failed mutation can have an unknown durable result. Keep reconnects denied until
  // a later successful authenticated login reconciles the persisted credential.
  void mark_pending_revocation_locked(std::int64_t account_id);
  void reconcile_pending_revocation_locked(std::int64_t account_id);

  // Call while holding mutex_. Makes the account's newest selection the only admissible one.
  void replace_world_admission_locked(std::int64_t account_id, const Handoff &handoff);

  // Call while holding mutex_. A consumed handoff of this account can no longer be admitted.
  void cancel_world_admission_locked(std::int64_t account_id);

  std::shared_ptr<AccountRepository> accounts_;
  std::shared_ptr<AuthAuthority> authority_;
  const std::string authority_id_{random_hex(16)};
  bool world_heartbeat_required_{};
  mutable std::mutex world_heartbeat_mutex_;
  std::optional<std::pair<unsigned, Clock::time_point>> world_heartbeat_;
  std::shared_ptr<CharacterRepository> characters_;
  std::shared_ptr<CharacterStateRepository> character_state_;
  // Guarded by mutex_.
  std::shared_ptr<const CharacterSocialState> social_state_;
  Config config_;
  std::function<Clock::time_point()> now_;
  WorldPopulation population_;

  // Leaf lock: never acquire mutex_ while holding realm_mutex_.
  mutable std::mutex realm_mutex_;
  RealmSettings realm_;

  std::mutex mutex_;
  std::unordered_map<std::string, Session> sessions_;
  std::unordered_map<std::string, Handoff> tickets_;
  std::unordered_map<std::string, LaunchGrant> launch_grants_;
  // Process-local only: unresolved entries need external durable recovery across restart.
  std::unordered_set<std::int64_t> pending_revocations_;
  // Guarded by mutex_. Selection identifiers start at one.
  std::uint64_t last_world_admission_id_{};

  // Leaf lock: may be acquired while holding mutex_, but never acquire another lock, call
  // storage or perform I/O while holding it. The world stream executor waits on it.
  std::mutex admission_mutex_;
  // At most one entry per account; consumed handoffs are admitted only against this entry.
  std::unordered_map<std::int64_t, PendingWorldAdmission> pending_world_admissions_;
  std::set<std::pair<std::string, std::uint64_t>> prepared_world_admissions_;
  std::set<std::pair<std::string, std::uint64_t>> consumed_authority_handoffs_;
};
} // namespace chronicles
