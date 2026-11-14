//
// Created by noktemor on 26.08.2026.
//

#include "entities/player/Inventory.h"
#include "world/crafting/Recipe.h"
#include "world/enchanting/Enchanting.h"
#include <algorithm>
#include <array>
#include <sstream>
#include <cctype>

namespace {
    bool includes_any(const std::string& s, std::initializer_list<const char*> tokens) {
        for (const char* token : tokens) if (s.find(token) != std::string::npos) return true;
        return false;
    }

    size_t creative_category(const Block_Properties& props) {
        std::string n = props.name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});
        if (includes_any(n, {"spawn egg"})) return 5; // Mobs
        if (includes_any(n, {"copper", "lever", "repeater", "signal", "button",
                             "pressure plate", "piston", "wire", "hopper", "rail", "dynamite"})) return 4;
        if (includes_any(n, {"flower", "rose", "tulip", "orchid", "daisy", "bluet",
                             "lilac", "grass", "dirt", "log", "leaves", "sapling",
                             "cactus", "vine", "seed", "crop", "mushroom", "fern",
                             "lily", "berry", "moss", "mud", "root"})) return 2;
        if (includes_any(n, {"enchanting table", "wool", "bed", "glass", "fence", "gate", "stairs", "slab",
                             "wall", "lantern", "carpet", "sign", "door", "chain",
                             "candle", "bell", "grindstone", "lectern", "pot", "torch",
                             "pane", "ladder", "cauldron", "snow layer"})) return 1;
        if (props.is_item) return 3;
        return 0;
    }
}

Inventory::Inventory() {
    // Верхняя граница — вся ёмкость реестра (k_max_block_types), а не фиксированный
    // Block_Types::Count: блок, добавленный только в blocks.json (без нового имени в enum),
    // тоже появится в полном инвентаре без пересборки движка. Проверка на пустое имя отсекает
    // незанятые слоты, так что лишних итераций по факту не видно нигде, кроме старта игры.
    for (size_t raw_id = 1; raw_id < Block_Registry::get_instance().id_capacity(); ++raw_id) {
        const Block_ID id = static_cast<Block_ID>(raw_id);
        if (Block_Registry::get_instance().is_reserved(id)) continue;
        if (get_block_props(id).name.empty()) continue; // пропуск незанятых/зарезервированных id
        if (!get_block_props(id).show_in_creative) continue;
        m_all_blocks.push_back(static_cast<Block_Types>(id));
        m_creative_sections[creative_category(get_block_props(id))].push_back(static_cast<Block_Types>(id));
    }

    // Инвентарь начинается пустым в обоих режимах: хотбар и хранилище наполняются
    // добычей либо предметами, взятыми из палитры Creative.
    m_hotbar.assign(k_hotbar_size, Item_Stack{});
    m_storage.assign(k_storage_size, Item_Stack{});
}

void Inventory::select_next() {
    m_selected_index = (m_selected_index + 1) % static_cast<int>(k_hotbar_size);
}

void Inventory::select_previous() {
    m_selected_index = (m_selected_index - 1 + static_cast<int>(k_hotbar_size)) % static_cast<int>(k_hotbar_size);
}

void Inventory::select_index(size_t index) {
    if (index >= k_hotbar_size) return;
    m_selected_index = static_cast<int>(index);
}

Item_Stack Inventory::get_hotbar_slot(size_t index) const {
    if (index >= m_hotbar.size()) return {};
    return m_hotbar[index];
}

const char* Inventory::creative_category_name(size_t index) {
    static constexpr const char* labels[] = {"Building", "Decoration", "Nature", "Equipment", "Copper logic", "Mobs"};
    return index < k_creative_category_count ? labels[index] : "Unknown";
}

void Inventory::set_creative_category(size_t category) {
    if (category >= k_creative_category_count || m_creative_category == category) return;
    m_creative_category = category;
    m_creative_page = 0;
}

// --- Панель 9 x 4 ----------------------------------------------------------
Block_Types Inventory::creative_block_at(size_t panel_index) const {
    if (panel_index >= k_panel_storage_slots) return Block_Types::Air;
    const size_t absolute = m_creative_page * k_panel_storage_slots + panel_index;
    const auto& section = m_creative_sections[m_creative_category];
    if (absolute >= section.size()) return Block_Types::Air;
    return section[absolute];
}

size_t Inventory::get_page_count() const {
    if (m_mode != Game_Mode::Creative) return 1;
    const size_t size = m_creative_sections[m_creative_category].size();
    if (size == 0) return 1;
    return (size + k_panel_storage_slots - 1) / k_panel_storage_slots;
}

void Inventory::next_page() {
    const size_t pages = get_page_count();
    if (pages <= 1) return;
    m_creative_page = (m_creative_page + 1) % pages;
}

void Inventory::previous_page() {
    const size_t pages = get_page_count();
    if (pages <= 1) return;
    m_creative_page = (m_creative_page + pages - 1) % pages;
}

Item_Stack Inventory::get_panel_slot(size_t index) const {
    if (index >= k_panel_slot_count) return {};

    // Нижняя строка панели — это тот же хотбар, что виден на экране.
    if (index >= k_panel_hotbar_start) {
        return get_hotbar_slot(index - k_panel_hotbar_start);
    }
    if (index >= m_storage.size()) return {};
    return m_storage[index];
}

Item_Stack Inventory::get_palette_slot(size_t index) const {
    if (m_mode != Game_Mode::Creative || index >= k_palette_slots) return {};
    const Block_Types block = creative_block_at(index);
    // count = 0 — HUD не рисует число: палитра бесконечна.
    return block == Block_Types::Air ? Item_Stack{} : Item_Stack{ block, 0 };
}

void Inventory::place_infinite(Item_Stack& slot, bool right) {
    const Block_Types type = m_carried.type;
    if (slot.is_empty()) {
        slot = Item_Stack{ type, 0, 0 };
    } else if (slot.type != type || slot.damage != 0) {
        // Занятый слот другого предмета: забираем его на курсор, ничего не теряя.
        m_carried = slot;
        slot = Item_Stack{};
        m_carried_infinite = false;
        return;
    }
    const int max_stack = max_stack_for(type);
    if (right) {
        if (slot.count < max_stack) slot.count += 1;
    } else {
        slot.count = max_stack;
    }
}

void Inventory::click_palette_slot(size_t index, bool right) {
    if (m_mode != Game_Mode::Creative || index >= k_palette_slots) return;
    // Реальный стак с курсора сначала пытаемся вернуть в инвентарь, иначе он пропадёт.
    if (!m_carried.is_empty() && !m_carried_infinite) {
        const int added = try_add_stack(m_carried);
        m_carried.count -= added;
        if (m_carried.count > 0) return;
        m_carried = Item_Stack{};
    }
    const Block_Types block = creative_block_at(index);
    if (block == Block_Types::Air) {
        m_carried = Item_Stack{};
        m_carried_infinite = false;
        return;
    }
    m_carried = Item_Stack{ block, right ? 1 : max_stack_for(block) };
    m_carried_infinite = true;
}

void Inventory::click_panel_slot(size_t index) {
    if (index >= k_panel_slot_count) return;

    const bool is_hotbar_slot = index >= k_panel_hotbar_start;
    Item_Stack& slot = is_hotbar_slot ? m_hotbar[index - k_panel_hotbar_start]
                                      : m_storage[index];
    if (m_carried_infinite) {
        place_infinite(slot, false);
        return;
    }
    // Обычный обмен "слот <-> курсор" — одинаков для хранилища и хотбара в обоих режимах.
    left_click_stack(slot, m_carried);
}

void Inventory::left_click_stack(Item_Stack& slot, Item_Stack& carried) {
    // Клик по слоту с тем же блоком — досыпаем в стак, а не меняем местами.
    if (!carried.is_empty() && !slot.is_empty() && slot.type == carried.type &&
        slot.damage == carried.damage && slot.count < max_stack_for(slot.type)) {
        const int space = max_stack_for(slot.type) - slot.count;
        const int moved = std::min(space, carried.count);
        slot.count += moved;
        carried.count -= moved;
        if (carried.count <= 0) carried = Item_Stack{};
        return;
    }
    std::swap(slot, carried);
}

void Inventory::right_click_stack(Item_Stack& slot, Item_Stack& carried) {
    if (carried.is_empty()) {
        // Пустая рука: берём половину (с округлением вверх).
        if (slot.is_empty()) return;
        const int take = (slot.count + 1) / 2;
        carried = slot;
        carried.count = take;
        slot.count -= take;
        if (slot.count <= 0) slot = Item_Stack{};
        return;
    }
    // В руке стак: кладём ровно один предмет в пустой слот или в стак того же типа.
    if (slot.is_empty()) {
        slot = carried;
        slot.count = 1;
    } else if (slot.type == carried.type && slot.damage == carried.damage &&
               slot.count < max_stack_for(slot.type)) {
        slot.count += 1;
    } else {
        std::swap(slot, carried);
        return;
    }
    carried.count -= 1;
    if (carried.count <= 0) carried = Item_Stack{};
}

void Inventory::right_click_panel_slot(size_t index) {
    if (index >= k_panel_slot_count) return;
    const bool is_hotbar_slot = index >= k_panel_hotbar_start;
    Item_Stack& slot = is_hotbar_slot ? m_hotbar[index - k_panel_hotbar_start]
                                      : m_storage[index];
    if (m_carried_infinite) {
        place_infinite(slot, true);
        return;
    }
    right_click_stack(slot, m_carried);
}

Item_Stack Inventory::get_craft_slot(size_t index) const {
    return index < k_craft_size ? m_craft_grid[index] : Item_Stack{};
}

Item_Stack Inventory::peek_craft_output() const {
    if (m_mode != Game_Mode::Survival) return {};
    std::array<Item_Stack, 9> grid{};
    grid[0] = m_craft_grid[0];
    grid[1] = m_craft_grid[1];
    grid[3] = m_craft_grid[2];
    grid[4] = m_craft_grid[3];
    const Recipe* recipe = Recipe_Registry::get_instance().match(grid);
    if (!recipe) return {};
    return Item_Stack{recipe->output_type, recipe->output_count};
}

void Inventory::click_craft_slot(size_t index, bool right) {
    if (index >= k_craft_size || m_mode != Game_Mode::Survival) return;
    if (right) right_click_stack(m_craft_grid[index], m_carried);
    else left_click_stack(m_craft_grid[index], m_carried);
}

void Inventory::click_craft_output() {
    const Item_Stack result = peek_craft_output();
    if (result.is_empty()) return;
    if (!m_carried.is_empty() &&
        !(m_carried.type == result.type && m_carried.damage == 0 &&
          m_carried.count + result.count <= max_stack_for(result.type))) {
        return;
    }
    for (Item_Stack& cell : m_craft_grid) {
        if (cell.is_empty()) continue;
        cell.count -= 1;
        if (cell.count <= 0) cell = Item_Stack{};
    }
    if (m_carried.is_empty()) m_carried = result;
    else m_carried.count += result.count;
}

Item_Stack Inventory::get_armor_slot(size_t index) const {
    return index < k_armor_count ? m_armor[index] : Item_Stack{};
}

namespace {
    // Подходит ли предмет в слот брони с таким индексом.
    bool fits_armor_slot(const Item_Stack& stack, size_t index) {
        if (stack.is_empty()) return true;
        return get_block_props(stack.type).armor_slot == static_cast<Armor_Slot>(index + 1);
    }
}

void Inventory::click_armor_slot(size_t index, bool right) {
    if (index >= k_armor_count || m_mode != Game_Mode::Survival) return;
    if (!fits_armor_slot(m_carried, index)) return;
    if (right) right_click_stack(m_armor[index], m_carried);
    else left_click_stack(m_armor[index], m_carried);
}

int Inventory::total_enchant_protection() const {
    int n = 0;
    for (const Item_Stack& piece : m_armor) if (!piece.is_empty()) n += piece.enchant_protection;
    return n;
}

int Inventory::armor_points() const {
    if (m_mode != Game_Mode::Survival) return 0;
    int points = 0;
    for (const Item_Stack& piece : m_armor) {
        if (!piece.is_empty()) points += get_block_props(piece.type).armor_points;
    }
    return points;
}

void Inventory::damage_armor(int amount) {
    if (m_mode != Game_Mode::Survival || amount <= 0) return;
    for (Item_Stack& piece : m_armor) {
        if (piece.is_empty()) continue;
        piece.damage += amount;
        if (piece.damage >= effective_max_durability(piece)) piece = Item_Stack{};
    }
}

void Inventory::return_craft_to_inventory() {
    for (Item_Stack& cell : m_craft_grid) {
        if (!cell.is_empty()) try_add_stack(cell);
        cell = Item_Stack{};
    }
}

void Inventory::drop_carried_into_inventory() {
    return_craft_to_inventory();
    if (m_carried.is_empty()) {
        m_carried = Item_Stack{};
        return;
    }
    // Копия из бесконечной палитры возвращать некуда: просто отпускаем.
    if (m_carried_infinite) {
        m_carried = Item_Stack{};
        m_carried_infinite = false;
        return;
    }
    // Реальный стак нельзя терять при закрытии панели: раскладываем обратно.
    try_add_stack(m_carried);
    m_carried = Item_Stack{};
}

int Inventory::get_selected_grid_index() const {
    // Активный слот хотбара всегда подсвечивается в нижней строке панели.
    return static_cast<int>(k_panel_hotbar_start) + m_selected_index;
}

Block_Types Inventory::get_selected_block() const {
    return get_hotbar_slot(static_cast<size_t>(m_selected_index)).type;
}

const std::string& Inventory::get_selected_block_name() const {
    static const std::string empty_name = "-";
    const Block_Types block = get_selected_block();
    if (block == Block_Types::Air) return empty_name;
    return get_block_props(block).name;
}

void Inventory::merge_into(std::vector<Item_Stack>& slots, Block_Types type, int& remaining) {
    const int max_stack = max_stack_for(type);
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.type == type && slot.count > 0 && slot.count < max_stack && slot.damage == 0) {
            const int can_take = max_stack - slot.count;
            const int take = std::min(can_take, remaining);
            slot.count += take;
            remaining -= take;
        }
    }
}

void Inventory::fill_empty(std::vector<Item_Stack>& slots, Block_Types type, int& remaining, int damage, const Item_Stack* full_stack) {
    const int max_stack = max_stack_for(type);
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.is_empty()) {
            const int take = std::min(max_stack, remaining);
            if (full_stack) slot = *full_stack;
            slot.type = type;
            slot.count = take;
            slot.damage = damage;
            remaining -= take;
        }
    }
}

void Inventory::consume_from(std::vector<Item_Stack>& slots, Block_Types type, int& remaining) {
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.type == type && slot.count > 0) {
            const int take = std::min(slot.count, remaining);
            slot.count -= take;
            remaining -= take;
            if (slot.count <= 0) {
                slot = Item_Stack{};
            }
        }
    }
}

int Inventory::try_add(Block_Types type, int amount) {
    return try_add_stack(Item_Stack{type, amount});
}

Item_Stack Inventory::get_selected_stack() const {
    if (m_mode != Game_Mode::Survival) return {};
    return get_hotbar_slot(static_cast<size_t>(m_selected_index));
}

bool Inventory::damage_selected_tool(int amount) {
    if (m_mode != Game_Mode::Survival) return false;
    Item_Stack& slot = m_hotbar[static_cast<size_t>(m_selected_index)];
    if (slot.is_empty()) return false;
    const int max_durability = effective_max_durability(slot);
    if (max_durability <= 0) return false;
    // Unbreaking: each durability point is consumed with probability 1/(level+1).
    for (int i = 0; i < amount; ++i) {
        static uint32_t luck = 0xBAA77EEDu;
        luck ^= luck << 13; luck ^= luck >> 17; luck ^= luck << 5;
        if (slot.enchant_durability <= 0 || luck % static_cast<uint32_t>(slot.enchant_durability+1) == 0) ++slot.damage;
    }
    if (slot.damage >= max_durability) {
        slot = Item_Stack{};
        return true;
    }
    return false;
}

bool Inventory::enchant_selected_durability(int level) {
    Item_Stack& slot = m_hotbar[static_cast<size_t>(m_selected_index)];
    if (slot.is_empty() || get_block_props(slot.type).max_durability <= 0) return false;
    slot.enchant_durability = std::clamp(level, 0, 3);
    return true;
}

bool Inventory::consume_selected(int count) {
    if (m_mode != Game_Mode::Survival) return false;
    Item_Stack& slot = m_hotbar[static_cast<size_t>(m_selected_index)];
    if (slot.is_empty() || slot.count < count) return false;
    slot.count -= count;
    if (slot.count <= 0) slot = Item_Stack{};
    return true;
}

int Inventory::try_add_stack(const Item_Stack& stack) {
    const Block_Types type = stack.type;
    const int amount = stack.count;
    if (amount <= 0 || type == Block_Types::Air) return 0;

    int remaining = amount;
    // Сначала пытаемся доложить в уже существующие стаки (хотбар приоритетнее, как в
    // Minecraft), и только потом — в пустые слоты.
    merge_into(m_hotbar, type, remaining);
    merge_into(m_storage, type, remaining);
    fill_empty(m_hotbar, type, remaining, stack.damage, &stack);
    fill_empty(m_storage, type, remaining, stack.damage, &stack);

    return amount - remaining;
}

bool Inventory::add_resource(Block_Types type, int amount) {
    // Если инвентарь переполнен, то, что не поместилось, теряется — упрощение, которое
    // осталось для путей, где предмет некуда выбросить (возврат стака с курсора при закрытии
    // экрана). Подбор с земли использует try_add и оставляет остаток лежать в мире.
    return try_add(type, amount) > 0;
}

bool Inventory::has_resource(Block_Types type, int amount) const {
    if (amount <= 0) return true;
    int total = 0;
    for (const Item_Stack& s : m_hotbar) if (s.type == type) total += s.count;
    for (const Item_Stack& s : m_storage) if (s.type == type) total += s.count;
    return total >= amount;
}

bool Inventory::remove_resource(Block_Types type, int amount) {
    if (amount <= 0) return true;
    if (!has_resource(type, amount)) return false;

    int remaining = amount;
    consume_from(m_hotbar, type, remaining);
    consume_from(m_storage, type, remaining);
    return true;
}

int Inventory::xp_required_for_level(int lv) {
    lv = std::clamp(lv, 0, 10000);
    if (lv <= 16) return lv*lv + 6*lv;
    if (lv <= 31) return (5*lv*lv-81*lv+720)/2;
    return static_cast<int>(std::min<int64_t>((9LL*lv*lv-325LL*lv+4440)/2, 2000000000LL));
}

int Inventory::xp_level() const {
    int lo=0, hi=10000;
    while (lo<hi) {
        const int mid=(lo+hi+1)/2;
        if (xp_required_for_level(mid)<=m_experience) lo=mid; else hi=mid-1;
    }
    return lo;
}

float Inventory::xp_progress() const {
    const int lv=xp_level();
    const int start=xp_required_for_level(lv);
    const int end=xp_required_for_level(lv+1);
    return end>start ? static_cast<float>(m_experience-start)/static_cast<float>(end-start) : 0.0f;
}

void Inventory::add_xp(int amount) {
    if (amount<=0) return;
    m_experience=static_cast<int>(std::min<int64_t>(2000000000LL,static_cast<int64_t>(m_experience)+amount));
}

bool Inventory::spend_levels(int levels) {
    if (levels<=0) return true;
    const int current=xp_level();
    if (current<levels) return false;
    m_experience -= xp_required_for_level(current)-xp_required_for_level(current-levels);
    return true;
}

std::string Inventory::serialize() const {
    std::ostringstream out;
    out << "sel " << m_selected_index << "\n";
    out << "xp " << m_experience << "\n";
    auto write_extra = [&](char group, size_t i, const Item_Stack& stack) {
        if (stack.enchant_efficiency || stack.enchant_fortune || stack.enchant_sharpness ||
            stack.enchant_protection || stack.enchant_looting || stack.enchant_feather_falling)
            out << "ench " << group << " " << i << " " << stack.enchant_efficiency << " "
                << stack.enchant_fortune << " " << stack.enchant_sharpness << " "
                << stack.enchant_protection << " " << stack.enchant_looting << " "
                << stack.enchant_feather_falling << "\n";
    };
    for (size_t i = 0; i < m_hotbar.size(); ++i) {
        if (m_hotbar[i].is_empty()) continue;
        out << "h " << i << " " << static_cast<int>(m_hotbar[i].type) << " " << m_hotbar[i].count << "\n";
        if (m_hotbar[i].damage > 0) out << "hd " << i << " " << m_hotbar[i].damage << "\n";
        if (m_hotbar[i].enchant_durability > 0) out << "he " << i << " " << m_hotbar[i].enchant_durability << "\n";
        write_extra('h',i,m_hotbar[i]);
    }
    for (size_t i = 0; i < m_storage.size(); ++i) {
        if (m_storage[i].is_empty()) continue;
        out << "s " << i << " " << static_cast<int>(m_storage[i].type) << " " << m_storage[i].count << "\n";
        if (m_storage[i].damage > 0) out << "sd " << i << " " << m_storage[i].damage << "\n";
        if (m_storage[i].enchant_durability > 0) out << "se " << i << " " << m_storage[i].enchant_durability << "\n";
        write_extra('s',i,m_storage[i]);
    }
    for (size_t i = 0; i < m_armor.size(); ++i) {
        if (m_armor[i].is_empty()) continue;
        out << "a " << i << " " << static_cast<int>(m_armor[i].type) << "\n";
        if (m_armor[i].damage > 0) out << "ad " << i << " " << m_armor[i].damage << "\n";
        if (m_armor[i].enchant_durability > 0) out << "ae " << i << " " << m_armor[i].enchant_durability << "\n";
        write_extra('a',i,m_armor[i]);
    }
    return out.str();
}

void Inventory::deserialize(const std::string& text) {
    std::istringstream in(text);
    std::string tag;
    while (in >> tag) {
        if (tag == "xp") {
            int v=0; in >> v; m_experience=std::clamp(v,0,2000000000);
        } else if (tag == "ench") {
            char group='h'; size_t i=0;int e=0,f=0,sh=0,p=0,l=0,ff=0;
            in >> group >> i >> e >> f >> sh >> p >> l >> ff;
            Item_Stack* dest = nullptr;
            if (group=='h' && i<m_hotbar.size()) dest=&m_hotbar[i];
            if (group=='s' && i<m_storage.size()) dest=&m_storage[i];
            if (group=='a' && i<m_armor.size()) dest=&m_armor[i];
            if (dest && !dest->is_empty()) {
                dest->enchant_efficiency=std::clamp(e,0,5);dest->enchant_fortune=std::clamp(f,0,3);
                dest->enchant_sharpness=std::clamp(sh,0,5);dest->enchant_protection=std::clamp(p,0,5);
                dest->enchant_looting=std::clamp(l,0,3);dest->enchant_feather_falling=std::clamp(ff,0,4);
            }
        } else if (tag == "sel") {
            int v = 0; in >> v;
            if (v >= 0 && v < static_cast<int>(k_hotbar_size)) m_selected_index = v;
        } else if (tag == "h" || tag == "s") {
            size_t i = 0; int id = 0, count = 0; in >> i >> id >> count;
            std::vector<Item_Stack>& list = tag == "h" ? m_hotbar : m_storage;
            if (i < list.size() && id > 0 && id < static_cast<int>(k_max_block_types) && count > 0) {
                list[i] = Item_Stack{static_cast<Block_Types>(id), std::min(count, max_stack_for(static_cast<Block_Types>(id)))};
            }
        } else if (tag == "a") {
            size_t i = 0; int id = 0; in >> i >> id;
            if (i < m_armor.size() && id > 0 && id < static_cast<int>(k_max_block_types) &&
                fits_armor_slot(Item_Stack{static_cast<Block_Types>(id), 1}, i)) {
                m_armor[i] = Item_Stack{static_cast<Block_Types>(id), 1};
            }
        } else if (tag == "ad") {
            size_t i = 0; int dmg = 0; in >> i >> dmg;
            if (i < m_armor.size() && !m_armor[i].is_empty()) m_armor[i].damage = std::max(0, dmg);
        } else if (tag == "he" || tag == "se") {
            size_t i = 0; int level = 0; in >> i >> level;
            std::vector<Item_Stack>& list = tag == "he" ? m_hotbar : m_storage;
            if (i < list.size() && !list[i].is_empty() && level > 0) {
                list[i].enchant_durability = std::clamp(level, 0, 3);
            }
        } else if (tag == "ae") {
            size_t i = 0; int level = 0; in >> i >> level;
            if (i < m_armor.size() && !m_armor[i].is_empty() && level > 0) {
                m_armor[i].enchant_durability = std::clamp(level, 0, 3);
            }
        } else if (tag == "hd" || tag == "sd") {
            size_t i = 0; int dmg = 0; in >> i >> dmg;
            std::vector<Item_Stack>& list = tag == "hd" ? m_hotbar : m_storage;
            if (i < list.size() && !list[i].is_empty()) list[i].damage = std::max(0, dmg);
        } else {
            std::string rest; std::getline(in, rest);
        }
    }
}

Item_Stack* Inventory::get_panel_slot_mutable(size_t index) {
    if (index >= k_panel_slot_count) return nullptr;
    return index >= k_panel_hotbar_start ? &m_hotbar[index - k_panel_hotbar_start]
                                         : &m_storage[index];
}

void Inventory::quick_move_panel(size_t index) {
    Item_Stack* slot = get_panel_slot_mutable(index);
    if (!slot || slot->is_empty()) return;
    // Из хранилища — в хотбар, из хотбара — в хранилище.
    std::vector<Item_Stack>& target = index >= k_panel_hotbar_start ? m_storage : m_hotbar;
    int remaining = slot->count;
    merge_into(target, slot->type, remaining);
    fill_empty(target, slot->type, remaining, slot->damage, slot);
    if (remaining <= 0) *slot = Item_Stack{};
    else slot->count = remaining;
}

void Inventory::quick_move_craft_slot(size_t index) {
    if (index >= k_craft_size || m_mode != Game_Mode::Survival) return;
    Item_Stack& cell = m_craft_grid[index];
    if (cell.is_empty()) return;
    const int added = try_add_stack(cell);
    cell.count -= added;
    if (cell.count <= 0) cell = Item_Stack{};
}

void Inventory::quick_craft_output() {
    for (int guard = 0; guard < 64; ++guard) {
        const Item_Stack result = peek_craft_output();
        if (result.is_empty()) return;
        const int added = try_add(result.type, result.count);
        if (added < result.count) {          // не влезло целиком — откатываем и выходим
            if (added > 0) remove_resource(result.type, added);
            return;
        }
        for (Item_Stack& cell : m_craft_grid) {
            if (cell.is_empty()) continue;
            if (--cell.count <= 0) cell = Item_Stack{};
        }
    }
}
