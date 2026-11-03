#pragma once

#include "persistence/quest_service_effects.hpp"

#include <nlohmann/json_fwd.hpp>

namespace chronicles {

// One canonical representation is shared by the mutation, recovery and settlement paths.
nlohmann::json encode_quest_service_effects(const QuestServiceEffects &effects);
QuestServiceEffects decode_quest_service_effects(const nlohmann::json &document);
nlohmann::json encode_quest_granted_items(const std::vector<CharacterItem> &items);
std::vector<CharacterItem> decode_quest_granted_items(const nlohmann::json &document);

} // namespace chronicles
