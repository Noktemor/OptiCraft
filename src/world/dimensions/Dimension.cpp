#include "utils/configs/Config_World.h"
#include "world/dimensions/Dimension.h"
#include <array>
#include <deque>
#include <mutex>
#include <limits>
#include <cctype>
#include "utils/text/Json.h"

int Dimension_Info::flat_top_y() const {
    int y = flat_base_y - 1;
    for (const Flat_Layer& layer : flat_layers) y += layer.thickness;
    return y;
}

Block_Types Dimension_Info::flat_block_at(int y) const {
    int bottom = flat_base_y;
    for (const Flat_Layer& layer : flat_layers) {
        if (y >= bottom && y < bottom + layer.thickness) return layer.block;
        bottom += layer.thickness;
    }
    return Block_Types::Air;
}

std::string Dimension_Info::storage_key(const std::string& generation_folder) const {
    if (id == Dimension_Id::Overworld) return generation_folder;
    return std::string(name) + "_" + generation_folder;
}

namespace {
    std::array<Dimension_Info, static_cast<size_t>(Dimension_Id::Count)> register_dimensions() {
        std::array<Dimension_Info, static_cast<size_t>(Dimension_Id::Count)> table;

        auto& overworld = table[static_cast<size_t>(Dimension_Id::Overworld)];
        overworld.id = Dimension_Id::Overworld;
        overworld.name = "overworld";
        overworld.display_name = "Overworld";
        overworld.terrain = Terrain_Mode::Noise;

        auto& flat = table[static_cast<size_t>(Dimension_Id::Flat_Test)];
        flat.id = Dimension_Id::Flat_Test;
        flat.name = "flat_test";
        flat.display_name = "Flat test";
        flat.terrain = Terrain_Mode::Flat;
        flat.decorations = false;
        flat.sky_islands = false;
        flat.flat_base_y = Config::world_min_y;
        flat.flat_layers = {
            {Block_Types::Deep_Stone, 1 - Config::world_min_y - 8},   // до Y = -8
            {Block_Types::Stone, 5},
            {Block_Types::Dirt, 2},
            {Block_Types::Grass, 1},
        };
        flat.spawn_height = flat.flat_top_y() + 1;

        auto& empty = table[static_cast<size_t>(Dimension_Id::Void_Test)];
        empty.id = Dimension_Id::Void_Test;
        empty.name = "void_test";
        empty.display_name = "Void test";
        empty.terrain = Terrain_Mode::Void;
        empty.time_mode = Time_Mode::Fixed;   // в пустоте времени суток нет: вечный полдень
        empty.fixed_time = 0.25f;
        empty.decorations = false;
        empty.sky_islands = false;
        empty.spawn_height = 64;

        // Супер-плоский: как в Minecraft — низ уходит под мир, сверху: камень, земля, трава.
        auto& superflat = table[static_cast<size_t>(Dimension_Id::Superflat)];
        superflat.id = Dimension_Id::Superflat;
        superflat.name = "superflat";
        superflat.display_name = "Superflat";
        superflat.terrain = Terrain_Mode::Flat;
        superflat.decorations = false;
        superflat.sky_islands = false;
        superflat.flat_base_y = Config::world_min_y;
        superflat.flat_layers = {
            {Block_Types::Deep_Stone, 0 - Config::world_min_y},       // до Y = -1
            {Block_Types::Stone, 3},                                  // Y 0..2
            {Block_Types::Dirt, 2},                                   // Y 3..4
            {Block_Types::Grass, 1},                                  // Y 5
        };
        superflat.spawn_height = superflat.flat_top_y() + 1;

        // Один биом: обычный рельеф и декорации, но без пещер и без островов.
        auto& single = table[static_cast<size_t>(Dimension_Id::Single_Biome)];
        single.id = Dimension_Id::Single_Biome;
        single.name = "single_biome";
        single.display_name = "Single biome";
        single.terrain = Terrain_Mode::Noise;
        single.caves = false;
        single.sky_islands = false;
        single.single_biome = true;

        // То же, но без земли: только парящие острова над пустотой.
        auto& islands = table[static_cast<size_t>(Dimension_Id::Single_Islands)];
        islands.id = Dimension_Id::Single_Islands;
        islands.name = "single_islands";
        islands.display_name = "Floating islands";
        islands.terrain = Terrain_Mode::Noise;
        islands.caves = false;
        islands.ground = false;
        // На небесных островах растут skywood, цветы и свисающие лианы.
        islands.decorations = true;
        islands.sky_islands = true;
        islands.single_biome = true;

        // Верх/низ миров пока используют общий диапазон Config::world_min/max_y;
        // внутри этих границ Nether ограничен бедроковым потолком, End — пустотой.
        auto& nether = table[static_cast<size_t>(Dimension_Id::Nether)];
        nether.id = Dimension_Id::Nether;
        nether.name = "nether";
        nether.display_name = "Nether";
        nether.terrain = Terrain_Mode::Nether;
        nether.decorations = false;
        nether.has_sky_light = false;
        nether.sky_islands = false;
        nether.rivers = false;
        nether.villages = false;
        nether.caves = false;
        nether.ground = true;
        nether.time_mode = Time_Mode::Fixed;
        nether.fixed_time = 0.75f;
        nether.spawn_height = 66;
        nether.coordinate_scale = 8.0f;

        auto& end = table[static_cast<size_t>(Dimension_Id::End)];
        end.id = Dimension_Id::End;
        end.name = "end";
        end.display_name = "The End";
        end.terrain = Terrain_Mode::End;
        end.decorations = false;
        end.has_sky_light = false;
        end.sky_islands = false;
        end.rivers = false;
        end.villages = false;
        end.caves = false;
        end.ground = false;
        end.time_mode = Time_Mode::Fixed;
        end.fixed_time = 0.50f;
        end.spawn_height = 66;

        return table;
    }

    const auto& dimension_table() {
        static const auto table = register_dimensions();
        return table;
    }
}

namespace {
    struct Dynamic_Dimension {
        Dimension_Info info;
        std::string name;
        std::string title;
    };
    std::deque<Dynamic_Dimension>& custom_dimensions() {
        static std::deque<Dynamic_Dimension> items;
        return items;
    }
    std::mutex& dimension_mutex() {
        static std::mutex m;
        return m;
    }
    void load_dimensions_json() {
        if (!json_asset_exists("assets/dimensions.json")) return;
        try {
            const Json_Value root = parse_json_file("assets/dimensions.json");
            const Json_Value* definitions = root.find("dimensions");
            if (!definitions || !definitions->is_array()) return;
            for (const Json_Value& data : definitions->array_value) {
                if (!data.is_object()) continue;
                const auto field = [&](const char* key) -> const Json_Value& {
                    static const Json_Value empty{};
                    if (const Json_Value* v = data.find(key)) return *v;
                    return empty;
                };
                const std::string name = json_read_string(field("name"));
                const std::string title = json_read_string(field("display_name"), name);
                const std::string mode = json_read_string(field("terrain"), "flat");
                if (name.empty() || title.empty()) continue;
                Dimension_Info info;
                // The registration copies these strings before the temporaries expire.
                info.name = name.c_str();
                info.display_name = title.c_str();
                if (mode == "flat") info.terrain = Terrain_Mode::Flat;
                else if (mode == "void") info.terrain = Terrain_Mode::Void;
                else if (mode == "noise") info.terrain = Terrain_Mode::Noise;
                else if (mode == "nether") info.terrain = Terrain_Mode::Nether;
                else if (mode == "end") info.terrain = Terrain_Mode::End;
                else continue;
                info.spawn_height = json_read_int(field("spawn_height"), 70);
                info.coordinate_scale = json_read_float(field("coordinate_scale"), 1.f);
                if (info.coordinate_scale <= 0.f) info.coordinate_scale = 1.f;
                info.decorations = json_read_bool(field("decorations"), false);
                info.has_sky_light = json_read_bool(field("has_sky_light"), true);
                info.sky_islands = json_read_bool(field("sky_islands"), false);
                info.rivers = json_read_bool(field("rivers"), false);
                info.villages = json_read_bool(field("villages"), false);
                info.caves = json_read_bool(field("caves"), false);
                info.ground = json_read_bool(field("ground"), true);
                info.time_mode = json_read_bool(field("fixed_time"), false) ? Time_Mode::Fixed : Time_Mode::Cycle;
                info.flat_base_y = json_read_int(field("flat_base_y"), Config::world_min_y);
                if (const Json_Value* layers = data.find("layers"); layers && layers->is_array()) {
                    for (const Json_Value& layer : layers->array_value) {
                        if (!layer.is_object()) continue;
                        const Json_Value* id = layer.find("block_id");
                        const Json_Value* thick = layer.find("thickness");
                        const int block = id ? json_read_int(*id, 5) : 5;
                        const int depth = thick ? json_read_int(*thick, 1) : 1;
                        if (block > 0 && block < 65536 && depth > 0 && depth < 2048)
                            info.flat_layers.push_back({static_cast<Block_Types>(block), depth});
                    }
                }
                register_dimension(info);
            }
        } catch (...) {
            // Invalid optional configuration cannot prevent built-in worlds from loading.
        }
    }
    void ensure_dynamic_loaded() {
        static std::once_flag once;
        std::call_once(once, load_dimensions_json);
    }
}

bool register_dimension(const Dimension_Info& info) {
    if (!info.name || !*info.name || !info.display_name || !*info.display_name) return false;
    std::lock_guard lock(dimension_mutex());
    const std::string name(info.name);
    // Names are also used in on-disk paths: reject path separators and traversal.
    if (name.size() > 64 || name.empty()) return false;
    for (const unsigned char ch : name)
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') &&
            ch != '_' && ch != '-') return false;
    // Make IDs deterministic based on assets/dimensions.json entry order.
    for (const Dimension_Info& builtin : dimension_table())
        if (name == builtin.name) return false;
    for (const auto& entry : custom_dimensions())
        if (name == entry.name) return false;
    if (custom_dimensions().size() >= std::numeric_limits<uint16_t>::max() -
                                     static_cast<size_t>(Dimension_Id::Count)) return false;
    auto& entry = custom_dimensions().emplace_back();
    entry.name = name;
    entry.title = info.display_name;
    entry.info = info;
    entry.info.id = static_cast<Dimension_Id>(static_cast<size_t>(Dimension_Id::Count) +
                                              custom_dimensions().size() - 1);
    entry.info.name = entry.name.c_str();
    entry.info.display_name = entry.title.c_str();
    return true;
}

std::vector<std::string> dimension_names() {
    ensure_dynamic_loaded();
    std::lock_guard lock(dimension_mutex());
    std::vector<std::string> names;
    names.reserve(dimension_table().size() + custom_dimensions().size());
    for (const Dimension_Info& info : dimension_table()) names.emplace_back(info.name);
    for (const auto& entry : custom_dimensions()) names.push_back(entry.name);
    return names;
}

const Dimension_Info& get_dimension_info(Dimension_Id id) {
    ensure_dynamic_loaded();
    const size_t index = static_cast<size_t>(id);
    if (index < dimension_table().size()) return dimension_table()[index];
    const size_t custom = index - dimension_table().size();
    std::lock_guard lock(dimension_mutex());
    return custom < custom_dimensions().size() ? custom_dimensions()[custom].info
                                                : dimension_table()[0];
}

const Dimension_Info* find_dimension_info(const std::string& name) {
    ensure_dynamic_loaded();
    for (const Dimension_Info& info : dimension_table())
        if (name == info.name) return &info;
    std::lock_guard lock(dimension_mutex());
    for (const auto& entry : custom_dimensions())
        if (name == entry.name) return &entry.info;
    return nullptr;
}

Dimension_Id dimension_from_name(const std::string& name) {
    if (const auto* info = find_dimension_info(name)) return info->id;
    return Dimension_Id::Overworld;
}
