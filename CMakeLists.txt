cmake_minimum_required(VERSION 3.16)
project(OptiCraft VERSION 0.13.1)

# =============== Настройки ===============
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Без этого блока `cmake ..` без явного -DCMAKE_BUILD_TYPE собирает с -O0, то есть
# вообще без оптимизаций. Для генератора мира, который состоит из вызовов шума,
# это само по себе замедление в несколько раз.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE не задан — используется Release")
endif()

# Нативные инструкции ускоряют шум/битовые очереди. Для локальной игры включены,
# так как сборка запускается на той же машине. Для переносимого релиза отключить:
# -DOPTICRAFT_NATIVE_ARCH=OFF.
option(OPTICRAFT_NATIVE_ARCH "Оптимизировать под процессор сборочной машины" ON)

# Межмодульная оптимизация (LTO) в Release включена по умолчанию: это позволяет
# оптимизатору видеть мелкие функции шума/сеток между .cpp-файлами. При необходимости
# её можно отключить через -DOPTICRAFT_LTO=OFF; время линковки может увеличиться.
option(OPTICRAFT_LTO "Включить LTO в Release" ON)

# Удобно для IDE (VS Code, CLion и т.д.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============== Пути ===============
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# =============== Все исходные файлы ===============
set(SOURCES
        main.cpp

        # commands
        ${SRC_DIR}/commands/Command_System.cpp

        # core
        ${SRC_DIR}/core/app/Application.cpp
        ${SRC_DIR}/core/game/Game.cpp
        ${SRC_DIR}/core/game/Day_Night_Cycle.cpp
        ${SRC_DIR}/core/game/Weather_Cycle.cpp
        ${SRC_DIR}/core/game/Tick.cpp
        ${SRC_DIR}/mobs/Boss.cpp
        ${SRC_DIR}/mobs/Knight_Ai.cpp
        ${SRC_DIR}/mobs/Mobs_Update.cpp
        ${SRC_DIR}/mobs/navigation/Navigation_System.cpp

        # entities
        ${SRC_DIR}/entities/player/Camera.cpp
        ${SRC_DIR}/entities/player/Player.cpp
        ${SRC_DIR}/entities/player/Player_Model.cpp
        ${SRC_DIR}/entities/player/Inventory.cpp
        ${SRC_DIR}/mobs/village/Village_System.cpp
        ${SRC_DIR}/entities/items/Dropped_Item.cpp

        # input
        ${SRC_DIR}/input/Input_Manager.cpp

        # rendering
        ${SRC_DIR}/rendering/capture/Isometric_Capture.cpp
        ${SRC_DIR}/rendering/Renderer.cpp
        ${SRC_DIR}/rendering/vulkan/VulkanGameChunkFeed.cpp
        ${SRC_DIR}/rendering/Shader.cpp
        ${SRC_DIR}/rendering/atlas/Texture_Atlas.cpp
        ${SRC_DIR}/rendering/atlas/Atlas_Registry.cpp

        # utils
        ${SRC_DIR}/utils/logging/Logger.cpp
        ${SRC_DIR}/utils/text/Json.cpp
        ${SRC_DIR}/utils/binary/Binary_Json.cpp
        ${SRC_DIR}/utils/text/Localization.cpp

        # world
        ${SRC_DIR}/world/blocks/Block.cpp
        ${SRC_DIR}/world/blocks/Block_Entity.cpp
        ${SRC_DIR}/world/blocks/Block_Entity_Store.cpp
        ${SRC_DIR}/world/crafting/Recipe.cpp
        ${SRC_DIR}/world/crafting/Crop_Manager.cpp
        ${SRC_DIR}/world/crafting/Sapling_Manager.cpp
        ${SRC_DIR}/world/logic/Logic_Manager.cpp
        ${SRC_DIR}/world/crafting/Smelting.cpp
        ${SRC_DIR}/world/storage/Region_File.cpp
        ${SRC_DIR}/world/storage/Chunk_IO.cpp
        ${SRC_DIR}/world/generation/Biome.cpp
        ${SRC_DIR}/world/generation/Cave_Network.cpp
        ${SRC_DIR}/world/generation/River_Network.cpp
        ${SRC_DIR}/world/generation/Structure_Placement.cpp
        ${SRC_DIR}/world/generation/Structure_Pack.cpp
        ${SRC_DIR}/world/generation/Village_Layout.cpp
        ${SRC_DIR}/world/blocks/Block_Interaction.cpp
        ${SRC_DIR}/world/chunks/Chunk.cpp
        ${SRC_DIR}/world/chunks/Chunk_Manager.cpp
        ${SRC_DIR}/world/generation/Generation_Type.cpp
        ${SRC_DIR}/world/generation/Structures.cpp
        ${SRC_DIR}/world/generation/Tree_Generator.cpp
        ${SRC_DIR}/world/generation/Tree_Pack.cpp
        ${SRC_DIR}/world/generation/World_Generator.cpp
        ${SRC_DIR}/world/generation/Gpu_Worldgen.cpp
        ${SRC_DIR}/world/storage/World_File.cpp
        ${SRC_DIR}/world/fluids/Fluid_Simulator.cpp
        ${SRC_DIR}/world/dimensions/Dimension.cpp
        ${SRC_DIR}/world/structures/Dungeon_Layout.cpp

        # physics
        ${SRC_DIR}/physics/Raycast.cpp
        ${SRC_DIR}/physics/Liquid_Physics.cpp

        # hud
        ${SRC_DIR}/hud/Hud.cpp
        ${SRC_DIR}/hud/Hud_Screens.cpp
        ${SRC_DIR}/hud/layouts/Panel_Layouts.cpp

        # mobs
        ${SRC_DIR}/mobs/Mob_Model.cpp
        ${SRC_DIR}/mobs/Mob.cpp
        ${SRC_DIR}/mobs/Mob_Equipment.h
        ${SRC_DIR}/mobs/Mob_Hit_Zone.h

        # entities/AI
        ${SRC_DIR}/entities/animals/ai/Pig_AI.cpp
        ${SRC_DIR}/entities/animals/Pig.cpp
        ${SRC_DIR}/entities/animals/Passive_Animal.cpp
        ${SRC_DIR}/entities/hostile/Hostile_Mob.cpp
        ${SRC_DIR}/entities/animals/ai/Horse_AI.cpp
        ${SRC_DIR}/entities/animals/Horse.cpp
        src/world/blocks/Block_Shapes.cpp
        src/entities/effects/Status_Effects.cpp
)

set(PLATFORM_SOURCES
        ${SRC_DIR}/core/app/Window.cpp
        ${SRC_DIR}/world/blocks/Block_Types.cpp
        ${SRC_DIR}/rendering/atlas/Texture.cpp
)

set(THIRD_PARTY_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/src/gl.c
        # шум для генерации мира (OpenSimplex2S, C)
        ${CMAKE_CURRENT_SOURCE_DIR}/external/opensimplex/OpenSimplex2S.c
)

# =============== Инструмент json -> OCJB (tools/json2bin) ===============
# Переводит ассеты (blocks.json, рецепты, атласы, ...) в бинарный формат *.ocb и читает их обратно.
add_executable(json2bin
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/json2bin/json2bin.cpp
        ${SRC_DIR}/utils/binary/Binary_Json.cpp
        ${SRC_DIR}/utils/text/Json.cpp
        ${SRC_DIR}/utils/logging/Logger.cpp
)
target_include_directories(json2bin PRIVATE ${SRC_DIR})

# =============== Офлайновый сборщик assets/trees/trees.tree ===============
# Не входит в обычную сборку/игровой исполняемый файл. Запускается вручную при
# изменении исходных вариантов деревьев; не участвует в генерации чанков.
add_executable(treepack EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/treepack/treepack.cpp
        ${SRC_DIR}/world/generation/Tree_Generator.cpp
        ${SRC_DIR}/world/generation/Tree_Pack.cpp)
target_include_directories(treepack PRIVATE ${SRC_DIR})

# =============== Создаём исполняемый файл ===============
add_executable(${PROJECT_NAME}
        ${SOURCES}
        ${PLATFORM_SOURCES}
        ${THIRD_PARTY_SOURCES}
)

# Добавляем заголовочные файлы (чтобы они отображались в IDE)
target_sources(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}/commands/Command_System.h
        ${SRC_DIR}/core/app/Application.h
        ${SRC_DIR}/core/game/Game.h
        ${SRC_DIR}/core/game/Tick.h
        ${SRC_DIR}/core/app/Window.h

        ${SRC_DIR}/entities/player/Camera.h
        ${SRC_DIR}/entities/player/Player.h
        ${SRC_DIR}/entities/items/Dropped_Item.h

        ${SRC_DIR}/input/Input_Call_Backs.h
        ${SRC_DIR}/input/Input_Manager.h

        ${SRC_DIR}/rendering/Frustum.h
        ${SRC_DIR}/rendering/capture/Isometric_Capture.h
        ${SRC_DIR}/rendering/Renderer.h
        ${SRC_DIR}/rendering/Shader.h
        ${SRC_DIR}/utils/concurrency/Render_Worker_Pool.h
        ${SRC_DIR}/rendering/atlas/Texture_Atlas.h
        ${SRC_DIR}/rendering/atlas/Atlas_Registry.h

        ${SRC_DIR}/utils/configs/Config_Camera.h
        ${SRC_DIR}/utils/configs/Config_Capture.h
        ${SRC_DIR}/utils/configs/Config_Effects.h
        ${SRC_DIR}/utils/configs/Config_Farming.h
        ${SRC_DIR}/utils/configs/Config_Items.h
        ${SRC_DIR}/utils/configs/Config_Mobs.h
        ${SRC_DIR}/utils/configs/Config_Physics.h
        ${SRC_DIR}/utils/configs/Config_Player.h
        ${SRC_DIR}/utils/configs/Config_Render.h
        ${SRC_DIR}/utils/configs/Config_Window.h
        ${SRC_DIR}/utils/configs/Config_World.h
        ${SRC_DIR}/math/Hash_Utils.h
        ${SRC_DIR}/utils/logging/Logger.h
        ${SRC_DIR}/math/Math_Helpers.h
        ${SRC_DIR}/math/PerlinNoise.h
        ${SRC_DIR}/rendering/atlas/Texture.h
        ${SRC_DIR}/utils/text/Json.h
        ${SRC_DIR}/utils/text/Localization.h

        ${SRC_DIR}/world/blocks/Block.h
        ${SRC_DIR}/world/blocks/Block_Entity.h
        ${SRC_DIR}/world/blocks/Block_Entity_Store.h
        ${SRC_DIR}/world/crafting/Recipe.h
        ${SRC_DIR}/world/crafting/Smelting.h
        ${SRC_DIR}/world/blocks/Container_Session.h
        ${SRC_DIR}/world/blocks/Block_Types.h
        ${SRC_DIR}/world/generation/Biome.h
        ${SRC_DIR}/world/generation/Cave_Network.h
        ${SRC_DIR}/world/generation/River_Network.h
        ${SRC_DIR}/world/generation/Structure_Placement.h
        ${SRC_DIR}/world/generation/Structure_Pack.h
        ${SRC_DIR}/world/generation/Village_Layout.h
        ${SRC_DIR}/world/generation/Structure_Adapters.h
        ${SRC_DIR}/world/blocks/Block_Interaction.h
        ${SRC_DIR}/world/chunks/Chunk.h
        ${SRC_DIR}/world/chunks/Chunk_Manager.h
        ${SRC_DIR}/world/generation/Generation_Type.h
        ${SRC_DIR}/world/generation/Structures.h
        ${SRC_DIR}/world/generation/Tree_Generator.h
        ${SRC_DIR}/world/generation/Tree_Pack.h
        ${SRC_DIR}/world/generation/World_Generator.h
        ${SRC_DIR}/world/storage/World_File.h
        ${SRC_DIR}/world/storage/Region_File.h
        ${SRC_DIR}/world/storage/Chunk_IO.h

        ${SRC_DIR}/physics/Raycast.h

        ${SRC_DIR}/hud/Hud.h
        ${SRC_DIR}/hud/layouts/Panel_Layouts.h

        ${SRC_DIR}/mobs/Mob_Model.h
        ${SRC_DIR}/mobs/Mob.h
        ${SRC_DIR}/mobs/village/Village_Rules.h
        ${SRC_DIR}/mobs/village/Village_System.h
        ${SRC_DIR}/mobs/Knight_Ai.h
        ${SRC_DIR}/mobs/Mobs_Update.h

        ${SRC_DIR}/entities/animals/ai/Pig_AI.h
        ${SRC_DIR}/entities/animals/Pig.h
        ${SRC_DIR}/entities/animals/ai/Horse_AI.h
        ${SRC_DIR}/entities/animals/Horse.h
        ${SRC_DIR}/entities/animals/Passive_Animal.h
        ${SRC_DIR}/entities/hostile/Hostile_Mob.h
)

# =============== Профайлер Tracy (клиент) ===============
# Исходники Tracy лежат в external/tracy и НЕ хранятся в git (см. .gitignore).
# Без этой папки проект собирается как обычно, макросы OC_ZONE/OC_FRAME_MARK становятся пустыми.
# Отключить вручную: -DOPTICRAFT_TRACY=OFF.
option(OPTICRAFT_TRACY "Собрать клиент Tracy (нужна папка external/tracy)" ON)
set(OPTICRAFT_TRACY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tracy" CACHE PATH
        "Каталог исходников Tracy; можно указать установленную локальную копию")
if(OPTICRAFT_TRACY AND EXISTS ${OPTICRAFT_TRACY_DIR}/public/TracyClient.cpp)
    target_sources(${PROJECT_NAME} PRIVATE ${OPTICRAFT_TRACY_DIR}/public/TracyClient.cpp)
    target_include_directories(${PROJECT_NAME} PRIVATE ${OPTICRAFT_TRACY_DIR}/public)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE)
    message(STATUS "Tracy: клиент включён (${OPTICRAFT_TRACY_DIR})")
elseif(OPTICRAFT_TRACY)
    message(WARNING "Tracy: external/tracy не найден — сборка без профайлера")
endif()

# =============== Профайлер Tracy (GUI, цель tracy-profiler) ===============
# Отдельная цель, которая НЕ входит в обычную сборку (EXCLUDE_FROM_ALL): собирается,
# только когда её запускают (CLion: run-конфигурация tracy-profiler или cmake --build . --target tracy-profiler).
# Linux: X11-бэкенд (LEGACY), без диалога выбора файлов — меньше системных зависимостей.
# Нужны пакеты: libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libzstd-dev libcapstone-dev (Debian/Ubuntu).
# Выключить при необходимости: -DOPTICRAFT_TRACY_PROFILER=OFF.
option(OPTICRAFT_TRACY_PROFILER "Добавить цель tracy-profiler (GUI), собирается по запросу" OFF)
if(OPTICRAFT_TRACY_PROFILER AND EXISTS ${OPTICRAFT_TRACY_DIR}/profiler/CMakeLists.txt)
    if(UNIX AND NOT APPLE)
        # Системные зависимости профайлера на Linux: glfw3 (pkg-config) и X11.
        # Если их нет — не валим конфигурацию всего проекта, а только предупреждаем.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(OC_TRACY_GLFW QUIET glfw3)
        endif()
        find_package(X11 QUIET)
        if(OC_TRACY_GLFW_FOUND AND X11_FOUND)
            set(LEGACY ON)
            set(NO_FILESELECTOR ON)
            add_subdirectory(${OPTICRAFT_TRACY_DIR}/profiler ${CMAKE_BINARY_DIR}/tracy-profiler EXCLUDE_FROM_ALL)
        else()
            message(WARNING "tracy-profiler пропущен: нужны libglfw3-dev и libx11-dev (sudo apt install libglfw3-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libcapstone-dev)")
        endif()
    else()
        add_subdirectory(${OPTICRAFT_TRACY_DIR}/profiler ${CMAKE_BINARY_DIR}/tracy-profiler EXCLUDE_FROM_ALL)
    endif()
endif()

# =============== Флаги оптимизации ===============
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/O2 /fp:fast>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O2>)
    if(OPTICRAFT_NATIVE_ARCH)
        target_compile_options(${PROJECT_NAME} PRIVATE -march=native)
    endif()
endif()

# =============== Include directories ===============
target_include_directories(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}                          # для #include "core/..."
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/opensimplex
        ${CMAKE_CURRENT_SOURCE_DIR}/external/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/external/cgltf
)

# =============== Библиотеки ===============
find_package(OpenGL REQUIRED)

# GLFW (ищем системную)
find_package(glfw3 REQUIRED QUIET)

find_package(Threads REQUIRED)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

if(OPTICRAFT_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT OPTICRAFT_IPO_OK OUTPUT OPTICRAFT_IPO_LOG)
    if(OPTICRAFT_IPO_OK)
        set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "LTO не поддерживается компилятором: ${OPTICRAFT_IPO_LOG}")
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenGL::GL
        glfw
        ${X11_LIBRARIES}
        Threads::Threads
        ${CMAKE_DL_LIBS}        # для dlopen (нужен glad на Linux)
)

# =============== Копирование ассетов после сборки ===============
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
        COMMENT "Копирование папки assets в директорию сборки"
)

# =============== Финальное сообщение ===============
message(STATUS "✅ OptiCraft configured successfully with modern structure!")

# ======================================================================
# Vulkan backend — incremental replacement for OpenGL. Game voxel data
# feed is wired, production graphics/HUD migration remains unfinished. Keep the existing OptiCraft executable fully unchanged.
# The SDK / compiler are optional, so ordinary OpenGL builds still work.
# ======================================================================
option(OPTICRAFT_VULKAN "Build the Vulkan GPU-world backend library" ON)
add_library(OptiCraft_VulkanCore STATIC
        ${SRC_DIR}/rendering/vulkan/GpuWorldCore.cpp
        ${SRC_DIR}/rendering/vulkan/GpuFrustum.cpp
        ${SRC_DIR}/rendering/vulkan/VulkanChunkStreamPlan.cpp
        ${SRC_DIR}/rendering/vulkan/GpuMeshTransactions.cpp)
target_compile_features(OptiCraft_VulkanCore PUBLIC cxx_std_20)
target_include_directories(OptiCraft_VulkanCore PUBLIC ${SRC_DIR})

option(OPTICRAFT_VULKAN_CORE_TESTS "Build Vulkan residency/allocator tests" ON)
if(OPTICRAFT_VULKAN_CORE_TESTS)
    add_executable(OptiCraft_Vulkan_Core_Tests
            tools/tests/test_vulkan_world_core.cpp)
    target_link_libraries(OptiCraft_Vulkan_Core_Tests PRIVATE
            OptiCraft_VulkanCore Threads::Threads)
    enable_testing()
    add_test(NAME VulkanWorldCore COMMAND OptiCraft_Vulkan_Core_Tests)
    add_executable(OptiCraft_Vulkan_Stream_Tests
            tools/tests/test_vulkan_chunk_stream.cpp)
    target_link_libraries(OptiCraft_Vulkan_Stream_Tests PRIVATE
            OptiCraft_VulkanCore Threads::Threads)
    add_test(NAME VulkanChunkStream COMMAND OptiCraft_Vulkan_Stream_Tests)
    add_executable(OptiCraft_Vulkan_Mesh_Transactions_Tests tools/tests/test_vulkan_mesh_transactions.cpp)
    target_link_libraries(OptiCraft_Vulkan_Mesh_Transactions_Tests PRIVATE OptiCraft_VulkanCore)
    add_test(NAME VulkanMeshTransactions COMMAND OptiCraft_Vulkan_Mesh_Transactions_Tests)
    add_executable(OptiCraft_Vulkan_World_Policies_Tests tools/tests/test_vulkan_stage75_77.cpp)
    target_compile_features(OptiCraft_Vulkan_World_Policies_Tests PRIVATE cxx_std_20)
    target_include_directories(OptiCraft_Vulkan_World_Policies_Tests PRIVATE ${SRC_DIR})
    add_test(NAME VulkanWorldPolicies COMMAND OptiCraft_Vulkan_World_Policies_Tests)
    add_executable(OptiCraft_Vulkan_Entity_Batch_Tests tools/tests/test_vulkan_stage78_79.cpp ${SRC_DIR}/rendering/vulkan/GpuFrustum.cpp)
    target_compile_features(OptiCraft_Vulkan_Entity_Batch_Tests PRIVATE cxx_std_20)
    target_include_directories(OptiCraft_Vulkan_Entity_Batch_Tests PRIVATE ${SRC_DIR})
    add_test(NAME VulkanEntityBatch COMMAND OptiCraft_Vulkan_Entity_Batch_Tests)
    add_executable(OptiCraft_Vulkan_Column_Batch_Tests tools/tests/test_vulkan_stage83_85.cpp)
    target_compile_features(OptiCraft_Vulkan_Column_Batch_Tests PRIVATE cxx_std_20)
    target_include_directories(OptiCraft_Vulkan_Column_Batch_Tests PRIVATE ${SRC_DIR})
    add_test(NAME VulkanColumnBatch COMMAND OptiCraft_Vulkan_Column_Batch_Tests)
endif()

if(OPTICRAFT_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        # Window/WSI is an independent library: the runnable smoke test does not
        # require compiling any compute shader or linking incomplete chunk pipelines.
        add_library(OptiCraft_VulkanWindow STATIC
                ${SRC_DIR}/rendering/vulkan/VulkanFrameRuntime.cpp)
        target_include_directories(OptiCraft_VulkanWindow PUBLIC ${SRC_DIR})
        target_link_libraries(OptiCraft_VulkanWindow PUBLIC Vulkan::Vulkan glfw Threads::Threads)

        add_library(OptiCraft_VulkanBackend STATIC
                ${SRC_DIR}/rendering/vulkan/VulkanVoxelPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanGpuCuller.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanGpuMesher.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanResidentLight.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanChunkMeshing.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanOpaqueMaterialPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanLiveWorldRenderer.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanHiZ.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanAlphaSort.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanGameMirror.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanUiPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanItemPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanEntityPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanDeviceBuffer.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanColumnMaterialRuntime.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanMaterialTables.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanBlockAtlas.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanChunkStreamGpu.cpp
                ${SRC_DIR}/rendering/vulkan/ChunkGpuExport.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanBlockCatalog.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanMiscMesher.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanMiscPipeline.cpp
                ${SRC_DIR}/rendering/vulkan/VulkanParallelDraw.cpp)
        target_include_directories(OptiCraft_VulkanBackend PUBLIC ${SRC_DIR}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/stb ${CMAKE_CURRENT_SOURCE_DIR}/external/opensimplex)
        target_link_libraries(OptiCraft_VulkanBackend PUBLIC OptiCraft_VulkanWindow
                OptiCraft_VulkanCore)

        # Runnable Vulkan surface/device/swapchain/frame submission test.
        # The production OpenGL Game is left unchanged until all block shapes render.
        add_executable(OptiCraft_Vulkan
                ${SRC_DIR}/rendering/vulkan/VulkanProbe.cpp)
        target_link_libraries(OptiCraft_Vulkan PRIVATE OptiCraft_VulkanWindow)

        # Shader compilation is done once at build time, NEVER at runtime.
        find_program(OPTICRAFT_GLSLANG_VALIDATOR glslangValidator)
        if(OPTICRAFT_GLSLANG_VALIDATOR)
            set(OC_VK_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/vulkan_shaders")
            set(OC_VK_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets/vulkan_shaders")
            set(OC_VK_SPVS)
            foreach(shader
                    worldgen_climate.comp worldgen_terrain.comp worldgen_layers.comp worldgen_layer_merge.comp worldgen_cave_carve.comp worldgen_cave_resolve.comp worldgen_cave_pack.comp worldgen_cave_batch_carve.comp worldgen_cave_batch_resolve.comp worldgen_cave_batch_pack.comp worldgen_river_height.comp worldgen_river_smooth.comp worldgen_river_resolve.comp worldgen_river_profile.comp worldgen_river_pack.comp worldgen_river_compact_resolve.comp worldgen_river_compact_profile.comp worldgen_river_column_resolve.comp worldgen_river_column_merge.comp worldgen_material_input.comp worldgen_cave_batch_column_pack.comp worldgen_cave_column_merge.comp worldgen_material.comp worldgen_template_overlay.comp worldgen_tree_select.comp worldgen_tree_compact.comp worldgen_overlay.comp worldgen_light_propagate.comp worldgen_light_resident.comp voxel_misc_count.comp voxel_misc_emit.comp
                    voxel_misc.vert voxel_misc.frag
                    voxel_textured.vert voxel_textured.frag
                    voxel_count.comp voxel_emit.comp voxel_finalize.comp
                    voxel_publish.comp voxel_cull.comp voxel_misc_cull.comp voxel_hiz.comp voxel_alpha_sort.comp
                    voxel_debug.vert voxel_debug.frag ui.vert ui.frag item.vert item.frag entity.vert entity.frag)
                set(input "${OC_VK_SHADER_DIR}/${shader}")
                set(output "${OC_VK_SPV_DIR}/${shader}.spv")
                add_custom_command(OUTPUT "${output}"
                        COMMAND ${CMAKE_COMMAND} -E make_directory "${OC_VK_SPV_DIR}"
                        COMMAND ${OPTICRAFT_GLSLANG_VALIDATOR} -V --target-env vulkan1.2
                            "-I${OC_VK_SHADER_DIR}" -o "${output}" "${input}"
                        DEPENDS "${input}" "${OC_VK_SHADER_DIR}/voxel_row_runs.glsl"
                            "${OC_VK_SHADER_DIR}/mesher_common.glsl"
                            "${OC_VK_SHADER_DIR}/voxel_misc_common.glsl"
                            "${OC_VK_SHADER_DIR}/dynamic_light.glsl"
                            "${OC_VK_SHADER_DIR}/voxel_address.glsl"
                        VERBATIM)
                list(APPEND OC_VK_SPVS "${output}")
            endforeach()
            add_custom_target(OptiCraft_Vulkan_Shaders DEPENDS ${OC_VK_SPVS})
            add_dependencies(OptiCraft_VulkanBackend OptiCraft_Vulkan_Shaders)
            # Link the real Game to the Vulkan world-frame consumer only when
            # the full SDK and shader compiler are available. The existing
            # Primary Vulkan world/UI path; legacy GL resource context and fallback remain.
            target_link_libraries(${PROJECT_NAME} PRIVATE OptiCraft_VulkanBackend)
            target_sources(${PROJECT_NAME} PRIVATE ${SRC_DIR}/rendering/vulkan/VulkanMaterialWorldgen.cpp)
            target_compile_definitions(${PROJECT_NAME} PRIVATE OPTICRAFT_VULKAN_GAME_MIRROR=1 OPTICRAFT_VULKAN_SHADER_DIR=\"${OC_VK_SPV_DIR}\")
            target_compile_definitions(OptiCraft_VulkanBackend PRIVATE OPTICRAFT_VULKAN_SHADER_DIR=\"${OC_VK_SPV_DIR}\")
            add_dependencies(${PROJECT_NAME} OptiCraft_Vulkan_Shaders)
            message(STATUS "Vulkan backend: core/compute library and shaders enabled")
        else()
            message(WARNING "Vulkan SDK present, but glslangValidator absent. Install glslang-tools to compile Vulkan shaders")
        endif()
    else()
        message(STATUS "Vulkan SDK not found: core built, Vulkan compute backend skipped")
    endif()
endif()

# The game stream adapter consumes pure core serialization, not Vulkan API
# calls. Keep its code available even when no Vulkan SDK is installed.
target_link_libraries(${PROJECT_NAME} PRIVATE OptiCraft_VulkanCore)
