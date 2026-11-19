// Dimension — отдельный контекст генерации, времени и хранения чанков.
// Встроенные измерения определены в Dimension.cpp; дополнительные задаются в
// assets/dimensions.json или регистрируются из C++ (register_dimension).
// Переходы игроков и портальные конструкции: Game::switch_dimension и portal helpers.
// Документация: documentation/DIMENSIONS_AND_PORTALS.md.

#ifndef OPTICRAFT_DIMENSION_H
#define OPTICRAFT_DIMENSION_H

#include <cstdint>
#include <string>
#include <vector>
#include "world/blocks/Block_Types.h"

enum class Dimension_Id : uint16_t {
    Overworld = 0,
    Flat_Test,     // плоский мир со слоями — пример режима Flat, годится для тестов построек
    Void_Test,     // пустота — пример режима Void
    Superflat,     // супер-плоский мир (как в Minecraft): слои из Dimension.cpp, без декораций
    Single_Biome,  // обычный рельеф, но весь мир одного выбранного биома, без пещер
    Single_Islands,// то же, но земли нет: только парящие острова над пустотой
    Nether,        // пещерный огненный мир
    End,           // пустотные острова
    Count
};

// Как генерируется рельеф измерения.
enum class Terrain_Mode : uint8_t {
    Noise = 0,     // обычный мир: климат, биомы, пещеры, острова (World_Generator)
    Flat,          // плоские слои из Dimension_Info::flat_layers
    Void,          // пустота (всё — воздух)
    Nether,        // процедурные лавовые пещеры
    End            // процедурные парящие острова
};

// Один слой плоского мира; слои перечисляются СНИЗУ ВВЕРХ начиная с flat_base_y.
struct Flat_Layer {
    Block_Types block = Block_Types::Stone;
    int thickness = 1;
};

// Как идёт время суток в измерении: Cycle — свои часы (день и ночь сменяются), Fixed — время
// заморожено на fixed_time. Цикл принадлежит измерению, а не миру целиком.
enum class Time_Mode : uint8_t {
    Cycle = 0,
    Fixed
};

struct Dimension_Info {
    Dimension_Id id = Dimension_Id::Overworld;
    const char* name = "overworld";        // ключ для world.json и имён файлов
    const char* display_name = "Overworld";
    Terrain_Mode terrain = Terrain_Mode::Noise;

    // Параметры, которые в будущем смогут отличаться у измерений.
    bool decorations = true;               // деревья, трава, цветы (decorate_column)
    bool has_sky_light = true;             // нет неба (пещерное измерение) — false
    bool sky_islands = true;               // парящие острова (только Noise)
    bool rivers = true;                    // реки (River_Network); только Noise с землёй
    bool villages = true;                  // деревни (Structure_Placement + Village_Layout)
    // Noise-режим: пещеры (false — их нет вовсе) и наличие земли под островами
    // (ground = false — под парящими островами только пустота, без рельефа и воды).
    bool caves = true;
    bool ground = true;
    // Весь мир одного биома. Сам биом задаётся не здесь, а в мире (World_Settings::biome) и
    // передаётся в World_Generator::set_biome — поэтому одно измерение годится для любого биома.
    bool single_biome = false;

    // Время суток измерения. Для Fixed — отметка [0,1) (0.25 — полдень), которая не меняется.
    Time_Mode time_mode = Time_Mode::Cycle;
    float fixed_time = 0.25f;

    // Flat: слои и нижняя отметка. Void: spawn_height — куда ставить игрока.
    std::vector<Flat_Layer> flat_layers;
    int flat_base_y = 0;
    int spawn_height = 64;
    // X/Z в обычном мире / coordinate_scale = X/Z в этом измерении.
    float coordinate_scale = 1.0f;

    // Верхний Y последнего слоя плоского мира (включительно); для Noise/Void — flat_base_y - 1.
    int flat_top_y() const;
    // Блок плоского мира на высоте y (Air выше слоёв и ниже flat_base_y).
    Block_Types flat_block_at(int y) const;

    // Ключ хранения чанков. Overworld сохраняет прежнее имя папки генерации (старые миры
    // читаются без изменений), остальные измерения получают отдельный префикс.
    std::string storage_key(const std::string& generation_folder) const;
};

const Dimension_Info& get_dimension_info(Dimension_Id id);
// Добавление измерений из C++ и assets/dimensions.json без изменения enum.
// Программная регистрация выполняется до запуска генераторов. Имя — уникальный ключ
// сохранения; зарегистрированные описания имеют стабильные адреса до завершения программы.
bool register_dimension(const Dimension_Info& info);
std::vector<std::string> dimension_names();
const Dimension_Info* find_dimension_info(const std::string& name);
// По имени из world.json; неизвестное имя → Overworld.
Dimension_Id dimension_from_name(const std::string& name);

#endif // OPTICRAFT_DIMENSION_H
