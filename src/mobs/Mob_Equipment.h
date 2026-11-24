// Equipment data storage for anthropomorphic mobs only.
// Item_Stack is the same as the player's inventory; no parallel item IDs.
#ifndef OPTICRAFT_MOB_EQUIPMENT_H
#define OPTICRAFT_MOB_EQUIPMENT_H
#include <array>
#include <cstddef>
#include <cstdint>
#include "entities/player/Inventory.h"

enum class Mob_Equipment_Slot : uint8_t {
    MainHand = 0, OffHand, Helmet, Chestplate, Leggings, Boots, Count
};

struct Mob_Equipment {
    std::array<Item_Stack, static_cast<std::size_t>(Mob_Equipment_Slot::Count)> slots{};
    const Item_Stack& get(Mob_Equipment_Slot slot) const { return slots[static_cast<std::size_t>(slot)]; }
    void set(Mob_Equipment_Slot slot, const Item_Stack& stack) {
        slots[static_cast<std::size_t>(slot)] = stack;
    }
};
#endif
