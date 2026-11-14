#ifndef OPTICRAFT_ENCHANTING_H
#define OPTICRAFT_ENCHANTING_H

// Data-driven tier limits; tiers II-IV can be registered as blocks later.
// No bookshelves are required: the table tier alone determines maximum strength.
#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "entities/player/Inventory.h"

namespace Enchanting {
    enum class Kind : uint8_t { Unbreaking, Efficiency, Fortune, Sharpness, Protection, Looting, Feather_Falling };
    struct Tier {
        int max_power;
        int max_enchantment_level;
        std::array<int,3> required_xp;
    };
    inline constexpr std::array<Tier,4> tiers{{
        {12, 2, {1,5,10}}, {22, 3, {5,12,20}}, {32, 4, {10,20,30}}, {45, 5, {15,30,40}}
    }};
    inline int tier_of(Block_Types block) {
        switch (block) {
            case Block_Types::Enchanting_Table_I: return 1;
            // Reserved IDs for future tables (not catalog entries yet).
            case Block_Types::Enchanting_Table_II: return 2;
            case Block_Types::Enchanting_Table_III: return 3;
            case Block_Types::Enchanting_Table_IV: return 4;
            default: return 0;
        }
    }
    inline bool supports(Kind kind, const Item_Stack& item) {
        if (item.is_empty()) return false;
        const auto& p = get_block_props(item.type);
        if (p.max_durability <= 0) return false;
        switch (kind) {
            case Kind::Unbreaking: return true;
            case Kind::Efficiency: case Kind::Fortune:
                return p.tool_type == Tool_Type::Pickaxe || p.tool_type == Tool_Type::Axe ||
                       p.tool_type == Tool_Type::Shovel || p.tool_type == Tool_Type::Hoe;
            case Kind::Sharpness: case Kind::Looting:
                return p.tool_type == Tool_Type::Sword || p.tool_type == Tool_Type::Weapon;
            case Kind::Protection: return p.armor_slot != Armor_Slot::None;
            case Kind::Feather_Falling: return p.armor_slot == Armor_Slot::Feet;
        }
        return false;
    }
    inline int level(const Item_Stack& item, Kind kind) {
        switch (kind) {
            case Kind::Unbreaking: return item.enchant_durability;
            case Kind::Efficiency: return item.enchant_efficiency;
            case Kind::Fortune: return item.enchant_fortune;
            case Kind::Sharpness: return item.enchant_sharpness;
            case Kind::Protection: return item.enchant_protection;
            case Kind::Looting: return item.enchant_looting;
            case Kind::Feather_Falling: return item.enchant_feather_falling;
        }
        return 0;
    }
    inline int maximum(Kind kind) {
        switch (kind) {
            case Kind::Unbreaking: case Kind::Fortune: case Kind::Looting: return 3;
            case Kind::Feather_Falling: return 4;
            default: return 5;
        }
    }
    inline const char* name(Kind kind) {
        switch (kind) {
            case Kind::Unbreaking: return "Unbreaking";
            case Kind::Efficiency: return "Efficiency";
            case Kind::Fortune: return "Fortune";
            case Kind::Sharpness: return "Sharpness";
            case Kind::Protection: return "Protection";
            case Kind::Looting: return "Looting";
            case Kind::Feather_Falling: return "Feather Falling";
        }
        return "?";
    }
    inline bool apply(Item_Stack& item, Kind kind, int desired) {
        if (!supports(kind,item)) return false;
        const int lvl = std::clamp(desired, 1, maximum(kind));
        if (level(item,kind) >= lvl) return false;
        switch (kind) {
            case Kind::Unbreaking: item.enchant_durability = lvl; break;
            case Kind::Efficiency: item.enchant_efficiency = lvl; break;
            case Kind::Fortune: item.enchant_fortune = lvl; break;
            case Kind::Sharpness: item.enchant_sharpness = lvl; break;
            case Kind::Protection: item.enchant_protection = lvl; break;
            case Kind::Looting: item.enchant_looting = lvl; break;
            case Kind::Feather_Falling: item.enchant_feather_falling = lvl; break;
        }
        return true;
    }
    struct Offer {
        Kind kind = Kind::Unbreaking;
        int level = 0;
        int required_level = 0;
        int xp_cost = 0;
        int lapis_cost = 0;
        bool available = false;
    };
    // Stable offers within the same session and item. Purchases reroll via session seed.
    inline std::array<Offer,3> offers(const Item_Stack& item, int table_tier, uint32_t seed) {
        std::array<Offer,3> out{};
        if (table_tier < 1 || table_tier > 4) return out;
        const Tier& t = tiers[static_cast<size_t>(table_tier-1)];
        std::vector<Kind> choices;
        for (int i=0;i<7;++i) {
            const Kind kind = static_cast<Kind>(i);
            if (supports(kind,item) && level(item,kind)<maximum(kind)) choices.push_back(kind);
        }
        if (choices.empty()) return out;
        uint32_t state = seed ^ (static_cast<uint32_t>(item.type)*2654435761u) ^
                         (static_cast<uint32_t>(item.damage)*40503u);
        auto next = [&]() {state ^= state << 13;state ^= state >> 17;state ^= state << 5;return state;};
        for (int i=0;i<3;++i) {
            Offer& o=out[static_cast<size_t>(i)];
            o.kind=choices[next()%choices.size()];
            o.required_level=t.required_xp[static_cast<size_t>(i)];
            o.xp_cost=i+1; o.lapis_cost=i+1;
            const int power=std::min(t.max_enchantment_level, 1 + i + static_cast<int>(next()%2));
            o.level=std::min(maximum(o.kind),power);
            if (level(item,o.kind)>=o.level) o.level=std::min(maximum(o.kind),level(item,o.kind)+1);
            o.available=level(item,o.kind)<o.level;
            // Prefer distinct enchantments when the item has several candidates.
            for (int tries=0;tries<static_cast<int>(choices.size()) && choices.size()>static_cast<size_t>(i);++tries) {
                bool duplicate=false;
                for (int j=0;j<i;++j) if(out[static_cast<size_t>(j)].kind==o.kind) duplicate=true;
                if (!duplicate) break;
                o.kind=choices[next()%choices.size()];
                o.level=std::min(maximum(o.kind),power);
                if (level(item,o.kind)>=o.level) o.level=std::min(maximum(o.kind),level(item,o.kind)+1);
                o.available=level(item,o.kind)<o.level;
            }
        }
        return out;
    }
}
#endif
