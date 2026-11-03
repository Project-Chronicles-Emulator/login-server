#include "auth/auth.hpp"

namespace chronicles {

nlohmann::json AuthService::launcher_characters(const std::string &token) {
  std::int64_t account_id{};
  {
    std::lock_guard lock(mutex_);
    account_id = require_session(token).account;
  }

  // Reading a launcher roster must not enter a world or replace the game's selected character.
  auto characters = nlohmann::json::array();
  for (const auto &character : characters_->characters(account_id, config_.world_id)) {
    auto class_name = x5::cms::EX5CharacterClass_Name(
        static_cast<x5::cms::EX5CharacterClass>(character.class_id));
    constexpr std::string_view prefix = "EX5CharacterClass_";
    if (class_name.starts_with(prefix)) {
      class_name.erase(0, prefix.size());
    }
    auto equipment = nlohmann::json::array();
    for (const auto &item : character.items) {
      if (item.equip_slot) {
        equipment.push_back(
            {{"slot", *item.equip_slot}, {"grade", item.grade}, {"durability", item.durability}});
      }
    }
    characters.push_back({{"id", std::to_string(character.id)},
                          {"name", character.name},
                          {"slot", character.slot},
                          {"class_id", character.class_id},
                          {"class_name", class_name},
                          {"level", character.level},
                          {"experience", std::to_string(character.experience)},
                          {"world_id", character.world_id},
                          {"world_name", config_.world_name},
                          {"equipment", equipment}});
  }
  return {{"characters", characters}};
}

} // namespace chronicles
