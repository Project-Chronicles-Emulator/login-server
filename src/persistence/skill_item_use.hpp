#pragma once

#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace chronicles {

// A caller retains this exact plan, nonce, and expected sequence before reservation.
struct SkillItemUsePlan {
  std::string nonce;
  int item_cms_id{};
  WorldProgress before;
  WorldProgress after;
  std::string response_json;
};

enum class SkillItemUseState { reserved, committed, completed, aborted };
enum class SkillItemUseStatus { accepted, replay, conflict, mismatch, exhausted };

struct SkillItemUseReceipt {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::int64_t sequence{};
  std::string event_id;
  std::string fingerprint;
  int item_cms_id{};
  int remaining{};
  std::string response_json;
  SkillItemUseState state{SkillItemUseState::reserved};
};

struct SkillItemUseResult {
  SkillItemUseStatus status{SkillItemUseStatus::conflict};
  std::optional<SkillItemUseReceipt> receipt;
};

// Preserve the established utility-slot consumption order for both legacy and planned use.
bool spend_skill_item(WorldProgress &progress, int item_cms_id);

// Prepare without mutation or I/O. The nonce is eight random bytes encoded as 16 lower-case
// hex digits. The event ID is the big-endian 64-bit sequence in hex followed by that nonce.
SkillItemUsePlan prepare_skill_item_use(const std::string &nonce, int item_cms_id,
                                       const WorldProgress &before);
std::string skill_item_use_fingerprint(std::int64_t account_id, std::int64_t character_id,
                                       int world_id, const SkillItemUsePlan &plan);
std::string skill_item_use_event_id(std::int64_t sequence, const std::string &nonce);

} // namespace chronicles
