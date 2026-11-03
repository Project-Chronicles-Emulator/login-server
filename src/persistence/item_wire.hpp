#pragma once

#include "persistence/character_state_types.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace chronicles {

// Wire form of an owned item instance (x5.pb.Item) as the server sends it in inventory
// snapshots and ItemsUpdateNoti. Fields at their protobuf default are omitted on the wire, and
// created_epochtime is omitted for rows that predate creation-time tracking.
inline nlohmann::json item_instance_json(const CharacterItem &item) {
  nlohmann::json entry{{"item_id", std::to_string(item.id)},
                       {"item_cms_id", item.cms_id},
                       {"durability", item.durability},
                       {"grade", item.grade},
                       {"polish_step", item.polish_step}};
  if (item.equip_slot) {
    entry["equip_slot"] = *item.equip_slot;
  }
  if (item.created_epoch_ticks > 0) {
    entry["created_epochtime"] = std::to_string(item.created_epoch_ticks);
  }
  return entry;
}

} // namespace chronicles
