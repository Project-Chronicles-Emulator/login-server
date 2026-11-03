#pragma once

#include <array>
#include <span>

namespace chronicles {

struct StarterGearItem {
  int cms_id;
  int slot_index;
  const char *slot_name;
};

// Slot indexes follow EX5EquipSlotType. Character base clothes packs and Item
// model references identify each class outfit. The Magic armor set is also
// present in an official WorldCharItemsNoti; other armor sets lack a proven
// class-to-starter mapping and are not assigned here.
inline constexpr std::array<StarterGearItem, 5> kFightStarterGear{{
    {11101102, 0, "EX5EquipSlotType_Weapon_1"},
    {10130102, 7, "EX5EquipSlotType_CostumeSuit"},
    {10130103, 8, "EX5EquipSlotType_CostumeGloves"},
    {10130104, 9, "EX5EquipSlotType_CostumeShoes"},
    {11700201, 24, "EX5EquipSlotType_Bag"},
}};

inline constexpr std::array<StarterGearItem, 5> kWildStarterGear{{
    {11102302, 0, "EX5EquipSlotType_Weapon_1"},
    {10130202, 7, "EX5EquipSlotType_CostumeSuit"},
    {10130203, 8, "EX5EquipSlotType_CostumeGloves"},
    {10130204, 9, "EX5EquipSlotType_CostumeShoes"},
    {11700201, 24, "EX5EquipSlotType_Bag"},
}};

inline constexpr std::array<StarterGearItem, 5> kAdamantStarterGear{{
    {11103102, 0, "EX5EquipSlotType_Weapon_1"},
    {10130302, 7, "EX5EquipSlotType_CostumeSuit"},
    {10130303, 8, "EX5EquipSlotType_CostumeGloves"},
    {10130304, 9, "EX5EquipSlotType_CostumeShoes"},
    {11700201, 24, "EX5EquipSlotType_Bag"},
}};

inline constexpr std::array<StarterGearItem, 9> kMagicStarterGear{{
    {11104102, 0, "EX5EquipSlotType_Weapon_1"},
    {11403101, 19, "EX5EquipSlotType_ArmorHelmet"},
    {11403201, 20, "EX5EquipSlotType_ArmorSuit"},
    {11403301, 21, "EX5EquipSlotType_ArmorGloves"},
    {11403401, 22, "EX5EquipSlotType_ArmorShoes"},
    {10130402, 7, "EX5EquipSlotType_CostumeSuit"},
    {10130403, 8, "EX5EquipSlotType_CostumeGloves"},
    {10130404, 9, "EX5EquipSlotType_CostumeShoes"},
    {11700201, 24, "EX5EquipSlotType_Bag"},
}};

inline constexpr int kEquipSlotCount = 25;
inline constexpr int kWildCharacterClass = 2;

// Only classes with a complete catalog-backed starter set are mapped. Character
// rows exist for Death (4) and Love (6), but the Item table contains no weapon
// either class can equip, so they cannot be given a playable loadout and are
// rejected at creation. Classes 0 and 7 through 11 have no Character row at
// all. Rejecting an unmappable class is preferable to spawning it gearless,
// which the client displays as a generic default weapon.
inline std::span<const StarterGearItem> starter_gear_for_class(int class_id) {
  switch (class_id) {
  case 1:
    return kFightStarterGear;
  case 2:
    return kWildStarterGear;
  case 3:
    return kAdamantStarterGear;
  case 5:
    return kMagicStarterGear;
  default:
    return {};
  }
}

} // namespace chronicles
