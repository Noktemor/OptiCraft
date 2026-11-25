//
// Created by noktemor on 11.02.2026.
//

#include "utils/configs/Config_Capture.h"
#include "utils/configs/Config_Camera.h"
#include "utils/configs/Config_Effects.h"
#include "utils/configs/Config_Mobs.h"
#include "utils/configs/Config_Player.h"
#include "utils/configs/Config_Render.h"
#include "utils/configs/Config_Window.h"
#include "utils/configs/Config_World.h"
#include "utils/profiling/Profiler.h"
#include "core/game/Game.h"
#include "world/structures/Dungeon_Layout.h"
#include "world/generation/Tree_Pack.h"
#include "core/app/Application.h"
#include "hud/layouts/Panel_Layouts.h"
#include "utils/text/Localization.h"
#include "utils/text/Utf8.h"
#include "commands/Command_System.h"
#include "world/portals/Portal_Placement.h"
#include "math/Math_Helpers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "utils/logging/Logger.h"
#include "world/crafting/Smelting.h"
#include "world/enchanting/Enchanting.h"
#include "world/logic/Logic_Manager.h"
#include "rendering/atlas/Atlas_Registry.h"
#include "rendering/Frustum.h"
#include <iostream>
#include <cmath>
#include <random>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "world/storage/Region_File.h"
#include "entities/effects/Status_Effects.h"
#include <optional>
#include <string_view>
#include <iomanip>
#include <limits>
#include <chrono>
#if defined(__linux__)
#include <unistd.h>
#endif

// F3 diagnostics: reading /proc every frame is unnecessary at high FPS.
static float process_rss_mib() {
#if defined(__linux__)
    static auto last_sample = std::chrono::steady_clock::time_point{};
    static float last_mib = -1.0f;
    const auto now = std::chrono::steady_clock::now();
    if (last_sample != std::chrono::steady_clock::time_point{} &&
        now - last_sample < std::chrono::seconds(1)) return last_mib;
    last_sample = now;
    std::ifstream statm("/proc/self/statm");
    unsigned long total_pages = 0, resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0) last_mib = static_cast<float>(
            static_cast<double>(resident_pages) * static_cast<double>(page_size) / (1024.0 * 1024.0));
    }
    return last_mib;
#else
    return -1.0f;
#endif
}

static Block_Types Game_Leaf_Sapling(Block_Types leaves);

// Строка для F3: "Effects: Regeneration 1 (7s), Speed 2 (21s)". Пусто, если эффектов нет.
static std::string describe_effects(const Status_Effects& effects) {
    if (effects.empty()) return {};
    static constexpr const char* k_names[] = {"Regeneration", "Speed", "Jump Boost"};
    std::string out = "Effects:";
    bool first = true;
    for (const Effect_Slot& slot : effects.slots()) {
        out += first ? " " : ", ";
        first = false;
        out += k_names[static_cast<int>(slot.effect)];
        out += " " + std::to_string(slot.amplifier + 1);
        out += " (" + std::to_string(static_cast<int>(std::ceil(slot.remaining))) + "s)";
    }
    return out;
}

Game::Game(int width, int height, const std::string& title)
    : m_is_running(false)
    , m_delta_time(0.0f)
    , m_last_frame_time(0.0f)
    , m_generation_folder("Classic")
{
    LOG_INFO("Creating Game instance...");
}

Game::~Game() {
    shutdown();
}

bool Game::initialize() {
    LOG_INFO("Initializing Game...");

    m_window_ptr = std::make_unique<Window>(
        Config::default_window_width,
        Config::default_window_height,
        Config::default_window_title
    );

    bool try_vulkan = false;
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    const char* renderer = std::getenv("OPTICRAFT_RENDERER");
    try_vulkan = !renderer || std::string_view(renderer) != "opengl";
#endif
    if (!m_window_ptr->initialize(try_vulkan)) {
        LOG_ERROR("Failed to initialize window");
        return false;
    }

    m_input_ptr = std::make_unique<Input_Manager>(m_window_ptr->get_glfw_ptr());
    m_window_ptr->set_cursor_mode(GLFW_CURSOR_NORMAL);

    m_window_ptr->set_mouse_callback([this](double x, double y) {
        m_input_ptr->update_mouse(x, y);
    });

    // Текстовый ввод: чат и поля экранов (символы приходят уже с учётом раскладки и Shift).
    m_window_ptr->set_char_callback([this](unsigned int codepoint) {
        on_text_char(codepoint);
    });
    m_window_ptr->set_scroll_callback([this](double, double y) {
        m_input_ptr->add_scroll(y);
    });

    m_camera_ptr = std::make_unique<Camera>(
        glm::vec3(8.0f, 20.0f, 8.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        -90.0f,
        -30.0f
    );

    m_renderer_ptr = std::make_unique<Renderer>();

    // Обязан отработать ДО первого обращения к Block_Registry ниже — она резолвит
    // текстуры блоков (atlas+имя спрайта из blocks.json) через уже загруженные атласы.
    // Требует живого GL-контекста (грузит реальные PNG в текстуры), поэтому строго
    // после m_window_ptr->initialize() выше.
    if (Atlas_Registry::get_instance().load_all("assets/atlases") == 0) {
        LOG_ERROR("No atlases loaded from assets/atlases — nothing will render correctly");
        return false;
    }

    // Триггерим загрузку blocks.json прямо здесь (Block_Registry — ленивый синглтон,
    // конструируется при первом обращении) — важно сделать это ПОСЛЕ атласов выше.
    Block_Registry::get_instance();



    // Preload the immutable tree template archive before any chunk workers begin.
    // Generation/plant growth read it without I/O or locking; missing/corrupt archive
    // automatically uses the existing procedural Tree_Generator implementation.
    const Tree_Pack& tree_pack = Tree_Pack::instance();
    if (!tree_pack.enabled()) {
        LOG_INFO("Tree pack disabled (OPTICRAFT_TREE_PACK=on to try it)");
    } else if (tree_pack.valid()) {
        LOG_INFO("Tree pack ready: " + std::to_string(tree_pack.template_count()) + " variants");
    } else {
        LOG_WARN("Tree pack unavailable: using procedural tree fallback");
    }

    if (!m_renderer_ptr->initialize("blocks_main")) {
        LOG_ERROR("Failed to initialize renderer");
        return false;
    }

    m_hud_ptr = std::make_unique<Hud>();
    if (!m_hud_ptr->initialize()) {
        LOG_ERROR("Failed to initialize HUD");
        return false;
    }

    load_options();
    bool selected_vulkan = false;
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if (m_window_ptr->vulkan_candidate()) {
        try {
            m_vulkan_game_mirror = std::make_unique<OptiCraft::Vulkan::VulkanGameMirror>();
            m_vulkan_game_mirror->initialize(m_window_ptr->vulkan_candidate(), Block_Registry::get_instance());
            m_vulkan_chunk_feed.setTrackedBudget(m_vulkan_game_mirror->residentCapacity());
            m_vulkan_chunk_feed.setMeshKinds(m_vulkan_game_mirror->meshKinds());
            set_vulkan_chunk_sink(m_vulkan_game_mirror->uploadSink(),m_vulkan_game_mirror->evictSink());
            m_hud_ptr->set_ui_sink([this](std::span<const Hud::Ui_Vertex> v,bool blocks){m_vulkan_game_mirror->submitUi(v,blocks);});
            selected_vulkan = true;
            LOG_INFO(std::string("Primary renderer: Vulkan / ")+m_vulkan_game_mirror->gpuName()+
                (m_vulkan_game_mirror->compactDraws()?" / compact indirect":" / fixed indirect"));
        } catch (const std::exception& e) {
            LOG_WARN(std::string("Vulkan initialization failed, using OpenGL: ")+e.what());
            m_hud_ptr->set_ui_sink({});set_vulkan_chunk_sink({},{});m_vulkan_game_mirror.reset();
        }
    }
#endif
    m_window_ptr->select_vulkan(selected_vulkan);
    m_input_ptr->rebind_window(m_window_ptr->get_glfw_ptr());
    if (!selected_vulkan) LOG_INFO("Primary renderer: OpenGL");

    // Мир не создаётся при старте: игрок сначала выбирает/создаёт его в меню.
    m_application_ptr = std::make_unique<Application>(*this);
    m_application_ptr->initialize();

    m_is_running = true;
    register_tick_systems();
    LOG_INFO("Game initialized successfully");
    return true;
}

void Game::run() {
    LOG_INFO("Starting game loop...");
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    uint64_t lastVulkanPresented=0;
#endif

    while (m_is_running && !m_window_ptr->should_close()) {
        OC_ZONE("Frame");
        const float current_time = static_cast<float>(glfwGetTime());
        const float raw_frame_delta = std::max(0.0f, current_time - m_last_frame_time);
        const float raw_frame_ms = raw_frame_delta * 1000.0f;
        m_last_raw_frame_ms = raw_frame_ms;
        m_delta_time = raw_frame_delta;
        m_last_frame_time = current_time;
        // Only simulation/physics delta is clamped. FPS/TPS accounting below uses real elapsed time.
        m_delta_time = std::clamp(m_delta_time, 0.001f, 0.1f);

        // Настоящее время кадра (без обрезки clamp выше), чтобы редкие просадки были видны в F3+F4.
        if (m_frame_window_count == 0) {
            m_frame_window_min_ms = raw_frame_ms;
            m_frame_window_max_ms = raw_frame_ms;
        } else {
            m_frame_window_min_ms = std::min(m_frame_window_min_ms, raw_frame_ms);
            m_frame_window_max_ms = std::max(m_frame_window_max_ms, raw_frame_ms);
        }
        m_frame_window_sum_ms += raw_frame_ms;
        m_frame_window_count++;

        m_fps_frame_count++;
        // Do not use the clamped simulation delta here: that capped the FPS display at 10
        // whenever a real frame took 100 ms or longer.
        m_fps_update_timer += raw_frame_delta;
        if (m_fps_update_timer >= 1.0f) {
            m_current_fps = static_cast<float>(m_fps_frame_count) / m_fps_update_timer;
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
            if(m_vulkan_game_mirror){
                const auto n=m_vulkan_game_mirror->framesPresented();
                m_current_fps=float(n-lastVulkanPresented)/m_fps_update_timer;
                lastVulkanPresented=n;
                const auto& mesh=m_vulkan_game_mirror->meshStats();
                std::cout << "\r[FPSCOUNTER] FPS: " << static_cast<int>(m_current_fps)
                    << " | VK resident " << m_vulkan_game_mirror->residentCount() << '/' << m_vulkan_game_mirror->residentCapacity()
                    << " | selected " << m_vulkan_chunk_feed.selectedCount() << " air skip " << m_vulkan_chunk_feed.remoteAirSkipped()
                    << " | upload Q " << m_vulkan_game_mirror->pendingUploads()
                    << " | voxel copy/fill/reuse/inline KiB " << (m_vulkan_game_mirror->uploadStats().copiedBytes/1024)
                    << '/' << (m_vulkan_game_mirror->uploadStats().filledBytes/1024)
                    << '/' << (m_vulkan_game_mirror->uploadStats().reusedBytes/1024)
                    << '/' << (m_vulkan_game_mirror->uploadStats().inlineBytes/1024)
                    << " | voxel dense KiB " << (m_vulkan_game_mirror->denseVoxelBytes()/1024)
                    << " allocated KiB " << (m_vulkan_game_mirror->voxelAllocatedBytes()/1024)
                    << " grows " << m_vulkan_game_mirror->voxelGrowthCount()
                    << " shrinks " << m_vulkan_game_mirror->voxelShrinkCount()
                    << " migrated KiB " << (m_vulkan_game_mirror->uploadStats().migratedBytes/1024)
                    << " admission misses " << m_vulkan_game_mirror->voxelAdmissionMisses()
                    << " | mesh Q " << m_vulkan_game_mirror->pendingMeshes()
                    << " flight " << m_vulkan_game_mirror->inFlightMeshes()
                    << " | opaque cache " << mesh.opaqueCacheHits << " / overflow " << mesh.opaqueCacheOverflow
                    << " | fast mesh " << mesh.fastJobs << " fallback " << mesh.fastFallbacks
                    << " reserve miss " << mesh.fastReservationMisses << " mixed " << mesh.fastMixedJobs << " misc overflow " << mesh.fastMiscOverflow
                    << " | published " << mesh.published << " stale " << mesh.stale
                    << " | alloc fail " << mesh.allocationFailures << " retry " << mesh.allocationRetries << " | overflow " << mesh.overflow
                    << " | mesh GPU " << m_vulkan_game_mirror->lastMeshGpuMs() << " ms"
                    << " | GPU total/cull/alpha/hiz " << m_vulkan_game_mirror->gpuTimes()[0] << '/' << m_vulkan_game_mirror->gpuTimes()[1] << '/' << m_vulkan_game_mirror->gpuTimes()[2] << '/' << m_vulkan_game_mirror->gpuTimes()[3] << " ms"
                    << " | misc reuse " << m_vulkan_game_mirror->reusedMiscCommands()
                    << " | opaque reuse " << m_vulkan_game_mirror->reusedOpaqueCommands()
                    << " | CPU update/render " << m_last_update_cpu_ms << '/' << m_last_render_cpu_ms << " ms"
                    << " | budget " << m_vulkan_game_mirror->workBudget() << "      " << std::flush;
            }else
#endif
            std::cout << "\r[FPSCOUNTER] FPS: " << static_cast<int>(m_current_fps) << "      " << std::flush;

            m_last_frame_min_ms = m_frame_window_min_ms;
            m_last_frame_max_ms = m_frame_window_max_ms;
            m_last_frame_avg_ms = m_frame_window_sum_ms / static_cast<float>(m_frame_window_count);
            m_frame_window_sum_ms = 0.0f;
            m_frame_window_count = 0;

            const std::uint64_t ticks_now = m_tick.total_ticks();
            m_current_tps = static_cast<float>(ticks_now - m_tps_tick_mark) / m_fps_update_timer;
            m_tps_tick_mark = ticks_now;

            m_fps_update_timer = 0.0f;
            m_fps_frame_count = 0;
        }

        { OC_ZONE("Frame: input polling"); m_input_ptr->update(); }
        const double update_begin = glfwGetTime();
        {
            OC_ZONE("Frame: input and simulation");
            m_application_ptr->handle_input();
            m_application_ptr->update(m_delta_time);
        }
        m_last_update_cpu_ms = static_cast<float>((glfwGetTime() - update_begin) * 1000.0);

        const double render_begin = glfwGetTime();
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
        if (m_vulkan_game_mirror) m_vulkan_game_mirror->beginGameFrame();
        try {
#endif
            { OC_ZONE("Frame: render submission"); m_application_ptr->render(); }
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
            if (m_vulkan_game_mirror) m_vulkan_game_mirror->present();
        } catch (const std::exception& e) {
            if (!m_vulkan_game_mirror) throw;
            LOG_WARN(std::string("Vulkan frame failed, switching to OpenGL: ")+e.what());
            switch_to_opengl();
            // Rebuild the frame through the old renderer before showing the fallback frame.
            m_application_ptr->render();
        }
#endif
        m_last_render_cpu_ms = static_cast<float>((glfwGetTime() - render_begin) * 1000.0);
        OC_PLOT("Frame CPU ms", raw_frame_ms);
        OC_PLOT("Update CPU ms", m_last_update_cpu_ms);
        OC_PLOT("Render CPU ms", m_last_render_cpu_ms);

        // Отладочный хук для автоматических скриншотов: если задана OPTICRAFT_SHOT_REQUEST и
        // такой файл существует, в нём лежит путь, куда сохранить кадр (PPM).
        if (const char* request = std::getenv("OPTICRAFT_SHOT_REQUEST"); request && !m_window_ptr->is_vulkan()) {
            std::ifstream in(request);
            std::string target;
            if (in && std::getline(in, target) && !target.empty()) {
                in.close();
                const int w = m_window_ptr->get_width(), h = m_window_ptr->get_height();
                std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                std::ofstream out(target, std::ios::binary);
                out << "P6\n" << w << " " << h << "\n255\n";
                for (int y = h - 1; y >= 0; --y) {
                    out.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(y) * w * 3),
                              static_cast<std::streamsize>(w) * 3);
                }
                out.close();
                std::filesystem::remove(request);
            }
        }

        // Отладочный хук: OPTICRAFT_CMDFILE — файл с командами (по строке, без '/'); выполняются
        // один раз, когда мир загружен, после чего файл удаляется.
        if (const char* cmd_file = std::getenv("OPTICRAFT_CMDFILE"); cmd_file && is_world_loaded() &&
            m_player_ptr && get_world_load_progress() >= 1.0f) {
            std::ifstream in(cmd_file);
            if (in) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(in, line)) if (!line.empty()) lines.push_back(line);
                in.close();
                std::filesystem::remove(cmd_file);
                for (const std::string& command : lines) run_command(command);
            }
        }

        {
            OC_ZONE("Frame tail: GPU present (swap)");
            m_window_ptr->swap_buffers();
        }
        {
            OC_ZONE("Frame tail: window events (poll)");
            m_window_ptr->poll_events();
        }
        OC_FRAME_MARK;
    }

    shutdown();
}

void Game::handle_gameplay_input() {
    if (m_input_ptr->is_key_pressed(GLFW_KEY_F6)) {
        toggle_generation_folder();
        return;
    }

    if (m_input_ptr->is_key_pressed(GLFW_KEY_F1) && !m_mobs.is_riding()) {
        cycle_game_mode();
    }

    if (m_input_ptr->is_key_pressed(GLFW_KEY_F3)) {
        m_show_debug_hud = !m_show_debug_hud;
    }

    // F4 — расширенный отладочный блок, работает только при открытом F3.
    if (m_show_debug_hud && m_input_ptr->is_key_pressed(GLFW_KEY_F4)) {
        m_show_debug_extended = !m_show_debug_extended;
    }

    // F10 — только в debug-режиме (F3), по требованию: не хотим, чтобы обычный игрок
    // случайно перематывал время суток нажатием функциональной клавиши. 1 нажатие = 1 минута
    // игрового времени; при цикле в 20 минут 10 нажатий переводят день в ночь и наоборот.
    if (m_show_debug_hud && m_input_ptr->is_key_pressed(GLFW_KEY_F10)) {
        m_day_night_cycle.advance(60.0f);
    }

    if (m_input_ptr->is_key_pressed(GLFW_KEY_LEFT_BRACKET)) {
        m_inventory_ptr->select_previous();
    }
    if (m_input_ptr->is_key_pressed(GLFW_KEY_RIGHT_BRACKET)) {
        m_inventory_ptr->select_next();
    }
    const float scroll = m_input_ptr->get_scroll_delta();
    if (scroll > 0.5f) m_inventory_ptr->select_previous();
    else if (scroll < -0.5f) m_inventory_ptr->select_next();
    for (int key = GLFW_KEY_1; key <= GLFW_KEY_9; ++key) {
        if (m_input_ptr->is_key_pressed(key)) {
            m_inventory_ptr->select_index(static_cast<size_t>(key - GLFW_KEY_1));
            break;
        }
    }

    auto mouse_delta = m_input_ptr->get_mouse_delta();
    m_camera_ptr->rotate(mouse_delta.x, mouse_delta.y);
    m_input_ptr->reset_mouse_delta();

    auto move_input = m_input_ptr->get_movement_input();

    if (m_mobs.is_riding()) {
        // Верхом WASD/пробел/Ctrl управляют лошадью, а не игроком: руль — к направлению взгляда
        // (S не рулит, только тормозит/сдаёт назад), ПРОБЕЛ — прыжок, CTRL — ручник (дрифт), SHIFT — слезть.
        glm::vec3 front = m_camera_ptr->get_front();
        glm::vec3 right = m_camera_ptr->get_right();
        front.y = 0.0f;
        right.y = 0.0f;
        if (glm::length(front) > 0.001f) front = glm::normalize(front);
        if (glm::length(right) > 0.001f) right = glm::normalize(right);

        glm::vec3 wish = front * std::max(move_input.forward, 0.0f) + right * move_input.strafe;
        if (glm::length(wish) > 0.001f) wish = glm::normalize(wish);

        Horse_Ride_Input ride;
        ride.wish_dir = wish;
        ride.throttle = move_input.forward;
        ride.jump = m_input_ptr->is_key_pressed(GLFW_KEY_SPACE);
        // Ручник (дрифт) — левый Ctrl, пробел — прыжок.
        // Ctrl: короткий клик — переключает бег, удержание дольше порога — ручник (дрифт).
        const bool ctrl_held = m_input_ptr->is_key_held(GLFW_KEY_LEFT_CONTROL);
        if (ctrl_held) m_ctrl_hold_time += m_delta_time;
        if (!ctrl_held && m_ctrl_was_held && m_ctrl_hold_time < Config::horse_sprint_tap_max_seconds) {
            ride.sprint_toggle = true;
        }
        if (!ctrl_held) m_ctrl_hold_time = 0.0f;
        m_ctrl_was_held = ctrl_held;
        ride.handbrake = ctrl_held && m_ctrl_hold_time >= Config::horse_drift_hold_seconds;
        m_mobs.set_ride_input(ride);
        if (m_input_ptr->is_key_pressed(GLFW_KEY_LEFT_SHIFT)) m_mobs.request_dismount();

        m_player_ptr->set_movement_input({0.0f, 0.0f, false, false}, m_camera_ptr->get_front(), m_camera_ptr->get_right());
        return;
    }

    m_ctrl_was_held = false;
    m_ctrl_hold_time = 0.0f;

    m_mobs.set_ride_input(Horse_Ride_Input{});
    m_player_ptr->set_movement_input(move_input, m_camera_ptr->get_front(), m_camera_ptr->get_right());
    m_player_ptr->set_movement_input(move_input, m_camera_ptr->get_front(), m_camera_ptr->get_right());
}

void Game::set_gameplay_input_active(bool active) {
    m_window_ptr->set_cursor_mode(active ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    m_input_ptr->reset_mouse_tracking();
}

namespace {
// infinite_stack — Creative: там стак хранится как {блок, count = 0} («запас бесконечен», см.
// Inventory::get_hotbar_slot / get_panel_slot), и Item_Stack::is_empty() (count <= 0) принимал
// ВСЕ такие слоты за пустые — иконки блоков в инвентаре и хотбаре Creative просто не рисовались.
Hud_Inventory_Slot make_hud_slot(const Item_Stack& stack, bool infinite_stack = false) {
    const bool empty = stack.type == Block_Types::Air || (!infinite_stack && stack.count <= 0);
    if (empty) {
        return {glm::vec4(0.0f), "", 0, true};
    }
    const Block_Properties& props = get_block_props(stack.type);
    // Иконка — верхняя грань блока (для большинства блоков она же самая узнаваемая).
    // UV уже предрасчитан Block_Registry при загрузке blocks.json — атлас тут
    // спрашивать по индексу больше не нужно, просто читаем готовый прямоугольник.
    float durability = -1.0f;
    const int max_durability = effective_max_durability(stack);
    if (max_durability > 0 && stack.damage > 0) {
        durability = 1.0f - static_cast<float>(stack.damage) / static_cast<float>(max_durability);
    }
    std::string display_name=props.name;
    const int enchant_count=stack.enchant_durability+stack.enchant_efficiency+stack.enchant_fortune+
        stack.enchant_sharpness+stack.enchant_protection+stack.enchant_looting+stack.enchant_feather_falling;
    if (enchant_count>0) {
        display_name += " [";
        if (stack.enchant_durability) display_name += " U"+std::to_string(stack.enchant_durability);
        if (stack.enchant_efficiency) display_name += " E"+std::to_string(stack.enchant_efficiency);
        if (stack.enchant_fortune) display_name += " F"+std::to_string(stack.enchant_fortune);
        if (stack.enchant_sharpness) display_name += " S"+std::to_string(stack.enchant_sharpness);
        if (stack.enchant_protection) display_name += " P"+std::to_string(stack.enchant_protection);
        if (stack.enchant_looting) display_name += " L"+std::to_string(stack.enchant_looting);
        if (stack.enchant_feather_falling) display_name += " FF"+std::to_string(stack.enchant_feather_falling);
        display_name += " ]";
    }
    Hud_Inventory_Slot slot{props.uv_icon, display_name, stack.count, false, durability};
    slot.enchanted = enchant_count > 0;
    // В превью повторяем не только материал, но и базовую форму блока.
    // Иначе shaped-блоки (например, ступеньки) ошибочно выглядели обычными кубами.
    if (!props.is_item && props.mesh_style == Block_Mesh_Style::Block) {
        slot.as_stairs = props.shape == Block_Shape::Stairs;
        slot.as_slab = props.shape == Block_Shape::Slab;
        slot.as_cube = props.shape == Block_Shape::None;
        // Instead of displaying their upper texture as a flat sprite, draw their real silhouette.
        switch (props.shape) {
            case Block_Shape::Door:
            case Block_Shape::Trapdoor:
            case Block_Shape::PressurePlate:
            case Block_Shape::Button:
            case Block_Shape::Bed:
            case Block_Shape::TorchPost:
            case Block_Shape::LeverPost:
                slot.icon_shape = props.shape;
                break;
            default: break;
        }
        slot.uv_cube_top = props.textures.top;
        slot.uv_cube_left = props.textures.has_front ? props.textures.front : props.textures.side;
        slot.uv_cube_right = props.textures.side;
        slot.uv_cube_bottom = props.textures.bottom;
    }
    return slot;
}
} // namespace

// ============================================================================
//  Настройки (options.txt) и миры (worlds/<папка>/world.json)
// ============================================================================
namespace {
    namespace fs = std::filesystem;
    constexpr const char* k_worlds_root = "worlds";
    constexpr const char* k_options_path = "options.txt";

    std::string now_string() {
        const std::time_t t = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm_buf);
        return buffer;
    }

    std::string sanitize_dir_name(const std::string& name) {
        std::string out;
        for (const char c : name) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || c == '_' || c == '-') out += static_cast<char>(std::tolower(u));
            else if (c == ' ' && !out.empty() && out.back() != '_') out += '_';
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out.empty() ? "world" : out;
    }

    std::string unique_dir_name(const std::string& base) {
        std::error_code ec;
        std::string candidate = base;
        for (int i = 2; fs::exists(fs::path(k_worlds_root) / candidate, ec); ++i) {
            candidate = base + "_" + std::to_string(i);
        }
        return candidate;
    }

    std::uint32_t seed_from_text(const std::string& text) {
        if (text.empty()) {
            std::random_device rd;
            return rd() & 0x7fffffffu;
        }
        bool numeric = text.size() <= 9;
        for (const char c : text) if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
        if (numeric) return static_cast<std::uint32_t>(std::stoul(text));
        std::uint32_t h = 0; // как String.hashCode в Java: одинаковый текст — одинаковый мир
        for (const char c : text) h = h * 31u + static_cast<unsigned char>(c);
        return h & 0x7fffffffu;
    }

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }
}

void Game::load_options() {
    std::ifstream file(k_options_path);
    std::string line;
    while (std::getline(file, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "gui_scale") m_gui_scale_setting = std::clamp(std::stoi(value), 0, 4);
            else if (key == "mouse_sensitivity") m_input_ptr->set_mouse_sensitivity(std::clamp(std::stof(value), 0.05f, 1.0f));
            else if (key == "difficulty") m_difficulty = std::clamp(std::stoi(value), 0, 2);
            else if (key == "render_distance") m_render_distance = std::clamp(std::stoi(value), Config::render_distance_min, Config::render_distance_max);
            else if (key == "flora_distance") m_flora_distance = std::clamp(std::stoi(value), 2, Config::flora_render_distance_max);
            else if (key == "vertical_distance") {
                // 0 — авто; иначе не меньше минимума (значение 1 из старых файлов даёт минимум).
                const int v = std::stoi(value);
                m_vertical_distance_setting = v <= 0 ? 0 : std::clamp(v, Config::vertical_render_min, Config::vertical_render_max);
            }
            else if (key == "chunk_fade") m_chunk_fade_seconds = std::clamp(std::stof(value), Config::chunk_fade_seconds_min, Config::chunk_fade_seconds_max);
            else if (key == "antialiasing") m_aa_mode = std::clamp(std::stoi(value), 0, Config::antialiasing_mode_count - 1);
            else if (key == "film_grain") m_film_grain = std::clamp(std::stof(value), 0.0f, 1.0f);
            else if (key == "vignette") m_vignette_level = std::clamp(std::stoi(value), 0, Config::vignette_level_count - 1);
            else if (key == "meshlet_renderer") m_meshlet_renderer_enabled = std::stoi(value) != 0;
            else if (key == "gpu_backend_mode") m_gpu_backend_mode = std::clamp(std::stoi(value), -1, 1);
            else if (key == "language") Localization::get().set_language(value);
        } catch (...) {}
    }
    Hud::set_gui_scale_setting(m_gui_scale_setting);
    m_renderer_ptr->set_gpu_compatibility_mode(m_gpu_backend_mode < 0
        ? m_renderer_ptr->is_gpu_compatibility_recommended() : m_gpu_backend_mode == 1);
    m_renderer_ptr->set_flora_render_distance(m_flora_distance);
    m_renderer_ptr->set_render_distance(m_render_distance);
    m_renderer_ptr->set_meshlet_renderer_enabled(m_meshlet_renderer_enabled);
    m_meshlet_renderer_enabled = m_renderer_ptr->is_meshlet_renderer_enabled();
    OC_PLOT("Meshlet renderer enabled", m_meshlet_renderer_enabled ? 1 : 0);
    apply_graphics_settings();
}

void Game::save_options() const {
    std::ofstream file(k_options_path, std::ios::trunc);
    if (!file) return;
    file << "gui_scale=" << m_gui_scale_setting << "\n"
         << "mouse_sensitivity=" << m_input_ptr->get_mouse_sensitivity() << "\n"
         << "difficulty=" << m_difficulty << "\n"
         << "render_distance=" << m_render_distance << "\n"
         << "flora_distance=" << m_flora_distance << "\n"
         << "vertical_distance=" << m_vertical_distance_setting << "\n"
         << "chunk_fade=" << m_chunk_fade_seconds << "\n"
         << "antialiasing=" << m_aa_mode << "\n"
         << "film_grain=" << m_film_grain << "\n"
         << "vignette=" << m_vignette_level << "\n"
         << "meshlet_renderer=" << (get_meshlet_renderer_enabled() ? 1 : 0) << "\n"
         << "gpu_backend_mode=" << m_gpu_backend_mode << "\n"
         << "language=" << Localization::get().get_language() << "\n";
}

int Game::get_effective_vertical_distance() const {
    if (m_vertical_distance_setting > 0) return m_vertical_distance_setting;
    return Config::auto_vertical_render_distance(m_render_distance);
}

void Game::apply_vertical_distance() {
    if (m_chunk_manager_ptr) m_chunk_manager_ptr->set_vertical_radius(get_effective_vertical_distance());
}

void Game::apply_graphics_settings() {
    if (m_renderer_ptr) {
        m_renderer_ptr->set_chunk_fade_seconds(m_chunk_fade_seconds);
        m_renderer_ptr->set_post_processing(m_aa_mode, m_film_grain);
    }
    if (m_hud_ptr) {
        const float strengths[Config::vignette_level_count] = {0.0f, 0.15f, 0.3f, 0.5f};
        m_hud_ptr->set_vignette_strength(strengths[std::clamp(m_vignette_level, 0, Config::vignette_level_count - 1)]);
    }
    apply_vertical_distance();
}

void Game::set_vertical_distance_setting(int value) {
    m_vertical_distance_setting = value <= 0 ? 0 : std::clamp(value, Config::vertical_render_min, Config::vertical_render_max);
    apply_vertical_distance();
}

void Game::set_chunk_fade_seconds(float seconds) {
    m_chunk_fade_seconds = std::clamp(seconds, Config::chunk_fade_seconds_min, Config::chunk_fade_seconds_max);
    if (m_renderer_ptr) m_renderer_ptr->set_chunk_fade_seconds(m_chunk_fade_seconds);
}

void Game::set_aa_mode(int mode) {
    m_aa_mode = std::clamp(mode, 0, Config::antialiasing_mode_count - 1);
    if (m_renderer_ptr) m_renderer_ptr->set_post_processing(m_aa_mode, m_film_grain);
}

void Game::set_film_grain(float value) {
    m_film_grain = std::clamp(value, 0.0f, 1.0f);
    if (m_renderer_ptr) m_renderer_ptr->set_post_processing(m_aa_mode, m_film_grain);
}

void Game::set_vignette_level(int level) {
    m_vignette_level = std::clamp(level, 0, Config::vignette_level_count - 1);
    apply_graphics_settings();
}

bool Game::is_meshlet_renderer_available() const {
    return m_renderer_ptr && m_renderer_ptr->is_meshlet_renderer_available();
}

void Game::set_gpu_backend_mode(int mode) {
    m_gpu_backend_mode = std::clamp(mode, -1, 1);
    if (m_renderer_ptr) {
        const bool compatibility = m_gpu_backend_mode < 0
            ? m_renderer_ptr->is_gpu_compatibility_recommended() : m_gpu_backend_mode == 1;
        m_renderer_ptr->set_gpu_compatibility_mode(compatibility);
        m_meshlet_renderer_enabled = m_renderer_ptr->is_meshlet_renderer_enabled();
    }
    save_options();
}

void Game::set_meshlet_renderer_enabled(bool enabled) {
    if (!m_renderer_ptr || !m_renderer_ptr->is_meshlet_renderer_available()) {
        m_meshlet_renderer_enabled = false;
    } else {
        m_renderer_ptr->set_meshlet_renderer_enabled(enabled);
        m_meshlet_renderer_enabled = m_renderer_ptr->is_meshlet_renderer_enabled();
    }
    OC_PLOT("Meshlet renderer enabled", m_meshlet_renderer_enabled ? 1 : 0);
    save_options();
}

void Game::set_render_distance(int chunks) {
    m_render_distance = std::clamp(chunks, Config::render_distance_min, Config::render_distance_max);
    if (m_chunk_manager_ptr) m_chunk_manager_ptr->set_load_radius(m_render_distance);
    if (m_renderer_ptr) m_renderer_ptr->set_render_distance(m_render_distance);
    // В режиме «Авто» высота зависит от горизонтальной дальности.
    apply_vertical_distance();
}

void Game::set_flora_distance(int chunks) {
    m_flora_distance = std::clamp(chunks, 2, Config::flora_render_distance_max);
    if (m_renderer_ptr) m_renderer_ptr->set_flora_render_distance(m_flora_distance);
}

void Game::set_mouse_sensitivity(float value) {
    m_input_ptr->set_mouse_sensitivity(std::clamp(value, 0.05f, 1.0f));
}

void Game::cycle_language() {
    Localization& loc = Localization::get();
    const std::vector<std::string>& languages = loc.get_available_languages();
    if (languages.empty()) return;
    const auto it = std::find(languages.begin(), languages.end(), loc.get_language());
    const size_t next = it == languages.end() ? 0 : (static_cast<size_t>(it - languages.begin()) + 1) % languages.size();
    loc.set_language(languages[next]);
}

void Game::set_gui_scale_setting(int value) {
    m_gui_scale_setting = std::clamp(value, 0, 4);
    Hud::set_gui_scale_setting(m_gui_scale_setting);
}

std::vector<Game::World_Info> Game::list_worlds() const {
    struct Item { World_Info info; std::string played; };
    std::vector<Item> items;
    std::error_code ec;
    if (!fs::exists(k_worlds_root, ec)) return {};
    for (const auto& entry : fs::directory_iterator(k_worlds_root, ec)) {
        if (!entry.is_directory()) continue;
        const std::string dir = entry.path().filename().string();
        World_File file(entry.path() / "world.json");
        World_Settings st;
        if (!file.load(st)) continue;
        Item item;
        item.info.dir = dir;
        item.info.name = st.name;
        item.info.detail = st.name + " (" + dir + (st.last_played.empty() ? "" : ", " + st.last_played) + ")";
        item.info.detail2 = tr("worlds.detail", {tr(st.game_mode == "survival" ? "mode.survival" : "mode.creative"),
                                                 st.generation_folder, std::to_string(st.seed)});
        item.played = st.last_played;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.played != b.played) return a.played > b.played;   // свежие сверху
        return a.info.dir < b.info.dir;
    });
    std::vector<World_Info> out;
    out.reserve(items.size());
    for (Item& item : items) out.push_back(std::move(item.info));
    return out;
}

bool Game::create_world(const std::string& name, const std::string& seed_text, Game_Mode mode,
                        const std::string& generation_folder, const std::string& dimension,
                        const std::string& biome, std::string& error) {
    std::string clean = trim(name);
    if (clean.empty()) clean = tr("create.world_name_default");

    World_Settings st;
    st.name = clean;
    st.generation_folder = generation_folder;
    if (!find_dimension_info(dimension)) { error = "Unknown dimension: " + dimension; return false; }
    st.dimension = dimension;
    st.biome = biome;
    st.seed = seed_from_text(trim(seed_text));
    st.game_mode = mode == Game_Mode::Survival ? "survival" : "creative";
    st.last_played = now_string();

    const std::string dir = unique_dir_name(sanitize_dir_name(clean));
    World_File file(fs::path(k_worlds_root) / dir / "world.json");
    if (!file.save(st)) {
        error = "Cannot write world files";
        return false;
    }
    return load_world(dir);
}

void Game::teardown_world() {
    m_vulkan_chunk_feed.clear();
    if (m_mobs.is_riding()) apply_mob_result(m_mobs.dismount(false, mob_view(), Mob_Blocked_Fn()));
    if (is_container_open()) close_container();
    m_chat_open = false;
    m_chat_input.clear();
    // Pending portals belong to the world being closed, never to the next save.
    m_pending_portal_type.clear();
    m_pending_command_portal_type.clear();
    m_dimension_landing = false;
    m_portal_contact_time = 0.f;
    m_portal_cooldown = 0.f;
    m_portal_exit_lock = false;
    m_destination_site_to_validate.reset();
    m_pending_portal_delay = 0.f;
    m_fire.clear();
    m_portal_links.clear();
    m_mobs.clear();
    m_villages.clear();
    m_crops_ptr.reset();
    m_saplings_ptr.reset();
    m_iso_capture_ptr.reset(); // до Chunk_Manager: снимок держит на него ссылку
    if (m_chunk_manager_ptr) {
        m_chunk_manager_ptr->clear(); // сохраняет изменённые чанки
        for (const glm::ivec3& position : m_chunk_manager_ptr->take_unloaded_chunk_positions()) {
            m_renderer_ptr->invalidate_chunk_cache(position);
        m_vulkan_chunk_feed.chunkUnloaded({0, position.x, position.y, position.z});
        }
        m_chunk_manager_ptr.reset();
    }
    m_block_interaction_ptr.reset();
    m_world_generator.reset();
    m_dropped_items_ptr.reset();
    m_inventory_ptr.reset();
    m_player_ptr.reset();
}

void Game::save_world(bool flush_chunks) {
    if (!m_chunk_manager_ptr || !m_player_ptr) return;
    m_world_settings.generation_folder = m_generation_folder;
    m_world_settings.game_mode = m_game_mode == Game_Mode::Survival ? "survival" : (m_spectator ? "spectator" : "creative");
    m_world_settings.last_played = now_string();
    const glm::vec3 p = m_player_ptr->get_position();
    m_world_settings.has_player_position = true;
    m_world_settings.player_x = p.x;
    m_world_settings.player_y = p.y;
    m_world_settings.player_z = p.z;
    m_dimension_positions[m_world_settings.dimension] = p;
    save_dimension_positions();
    {
        std::ofstream portals(m_world_file.get_path().parent_path() / "portal_sites.txt", std::ios::trunc);
        if (portals) m_portal_links.save(portals);
        std::ofstream fires(dimension_data_file("fires"), std::ios::trunc);
        if (fires) m_fire.save(fires);
    }
    // Замороженное (Fixed) время не пишем: оно не принадлежит миру и не должно затирать сохранённое.
    if (!m_day_night_cycle.is_frozen()) {
        m_world_settings.day_time = m_day_night_cycle.get_normalized_time();
        m_world_settings.day_count = m_day_night_cycle.get_day_count();
    }
    if (m_world_settings.dimension == "overworld") {
        std::ofstream citizens(dimension_data_file("villagers"),std::ios::trunc);
        if(citizens) citizens << m_villages.serialize();
    }
    m_world_file.save(m_world_settings);
    if (flush_chunks) m_chunk_manager_ptr->save_all();
    if (m_inventory_ptr) {
        m_inventory_ptr->drop_carried_into_inventory(); // курсор и сетка крафта возвращаются в сумку
        std::ofstream inv(m_world_file.get_path().parent_path() / "inventory.txt", std::ios::trunc);
        if (inv) inv << m_inventory_ptr->serialize();
    }
    if (m_crops_ptr) {
        std::ofstream crops(dimension_data_file("crops"), std::ios::trunc);
        if (crops) crops << m_crops_ptr->serialize();
    }
    if (m_saplings_ptr) {
        std::ofstream saplings(dimension_data_file("saplings"), std::ios::trunc);
        if (saplings) saplings << m_saplings_ptr->serialize();
    }
}

void Game::close_world() {
    if (!is_world_loaded()) return;
    save_world();
    teardown_world();
    m_world_dir.clear();
}

bool Game::delete_world(const std::string& dir) {
    if (dir.empty() || dir == m_world_dir) return false;
    std::error_code ec;
    const fs::path path = fs::path(k_worlds_root) / dir;
    if (!fs::exists(path, ec)) return false;
    Region_File::forget_under(path);
    fs::remove_all(path, ec);
    return !ec;
}

bool Game::load_world(const std::string& dir) {
    OC_ZONE("Game::load_world");
    if (is_world_loaded()) close_world();

    World_File file(fs::path(k_worlds_root) / dir / "world.json");
    World_Settings st;
    if (!file.load(st)) {
        LOG_ERROR("Cannot load world: " + dir);
        return false;
    }
    if (!find_dimension_info(st.dimension)) {
        LOG_ERROR("Missing dimension definition: " + st.dimension);
        return false;
    }
    m_world_dir = dir;
    m_world_file = file;
    m_world_settings = st;
    load_dimension_positions();
    m_generation_folder = st.generation_folder;
    m_game_mode = st.game_mode == "survival" ? Game_Mode::Survival : Game_Mode::Creative;
    m_spectator = st.game_mode == "spectator";

    glm::vec3 saved(st.player_x, st.player_y, st.player_z);
    setup_world_objects(st.has_player_position ? &saved : nullptr);
    m_loading_world = true;
    LOG_INFO("Loaded world \"" + st.name + "\" from " + file.get_path().string());
    return true;
}

void Game::setup_world_objects(const glm::vec3* saved_position) {
    m_chunk_manager_ptr = std::make_unique<Chunk_Manager>(m_render_distance);
    m_chunk_manager_ptr->set_vulkan_primary(m_window_ptr->is_vulkan());
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if(m_vulkan_game_mirror)m_chunk_manager_ptr->set_vulkan_worldgen(&m_vulkan_game_mirror->worldgenRuntime());
#endif
    apply_vertical_distance();
    m_world_generator = std::make_unique<World_Generator>(m_generation_folder, m_world_settings.seed,
                                                        dimension_from_name(m_world_settings.dimension));
    m_world_generator->set_biome(biome_from_name(m_world_settings.biome));
    m_mobs.set_biome_lookup([this](int x, int y, int z) { return m_world_generator->get_biome_name(x, y, z); });
    m_block_interaction_ptr = std::make_unique<Block_Interaction>();
    m_crops_ptr = std::make_unique<Crop_Manager>();
    m_block_interaction_ptr->set_crop_manager(m_crops_ptr.get());
    {
        std::ifstream crops(dimension_data_file("crops"));
        if (crops) {
            const std::string text{std::istreambuf_iterator<char>(crops), std::istreambuf_iterator<char>()};
            m_crops_ptr->deserialize(text);
        }
    }
    load_dimension_fire();
    m_saplings_ptr = std::make_unique<Sapling_Manager>();
    m_block_interaction_ptr->set_sapling_manager(m_saplings_ptr.get());
    {
        std::ifstream saplings(dimension_data_file("saplings"));
        if (saplings) {
            const std::string text{std::istreambuf_iterator<char>(saplings), std::istreambuf_iterator<char>()};
            m_saplings_ptr->deserialize(text);
        }
    }

    m_player_ptr = std::make_unique<Player>();
    m_inventory_ptr = std::make_unique<Inventory>();
    m_inventory_ptr->set_mode(m_game_mode);
    m_mobs.set_hostiles_enabled(true);
    {
        std::ifstream inv(m_world_file.get_path().parent_path() / "inventory.txt");
        if (inv) {
            const std::string text{std::istreambuf_iterator<char>(inv), std::istreambuf_iterator<char>()};
            m_inventory_ptr->deserialize(text);
        }
    }
    m_player_ptr->set_fall_damage_enabled(m_game_mode == Game_Mode::Survival);
    m_player_ptr->set_hunger_enabled(m_game_mode == Game_Mode::Survival);
    m_player_ptr->set_flying(m_spectator);
    m_player_ptr->set_double_jump_flight(m_game_mode == Game_Mode::Creative && !m_spectator);
    m_player_ptr->set_noclip(m_spectator);
    m_logic = Logic_Manager{};
    m_logic_scan_timer = 0.0f;
    m_dropped_items_ptr = std::make_unique<Dropped_Item_Manager>();
    m_container_session = Container_Session{};
    m_open_container_paired = nullptr;
    m_enchant_seed = 0x5A19F37Bu;
    m_chat_log.clear();
    m_mobs.reset_timers();
    load_villagers();
    m_tick.reset();
    // Цикл принадлежит измерению: там, где время не идёт (Fixed), оно заморожено на своей отметке,
    // иначе — идёт и восстанавливается из world.json.
    {
        const Dimension_Info& dim = get_dimension_info(dimension_from_name(m_world_settings.dimension));
        const bool fixed = dim.time_mode == Time_Mode::Fixed;
        m_day_night_cycle.set_frozen(fixed);
        m_day_night_cycle.set_normalized_time(fixed ? dim.fixed_time : m_world_settings.day_time);
        if (!fixed) m_day_night_cycle.set_day_count(m_world_settings.day_count);
    }

    // Точка появления: в обычных измерениях — колонка (8, 8); в мире без земли — ближайший остров.
    int spawn_x = 8;
    int spawn_z = 8;
    const int spawn_surface = m_world_generator->find_spawn_surface(spawn_x, spawn_z);
    m_spawn_position = glm::vec3(
        static_cast<float>(spawn_x),
        static_cast<float>(spawn_surface + 2),
        static_cast<float>(spawn_z));
    const glm::vec3 start = saved_position ? *saved_position : m_spawn_position;
    // Prepare the landing region before the chunk stream starts. The first visible column
    // must not be the one that pays for a cold cave + hydrology cache; subsequent regions
    // are prepared by Chunk_Manager's movement lookahead on background workers.
    m_world_generator->prewarm_generation_region_for_chunk(
        static_cast<int>(std::floor(start.x / static_cast<float>(Config::chunk_size))),
        static_cast<int>(std::floor(start.z / static_cast<float>(Config::chunk_size))));
    m_player_ptr->set_position(start);

    m_camera_ptr = std::make_unique<Camera>(
        start + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, -30.0f);

    m_chunk_manager_ptr->set_world_generator(*m_world_generator);
    m_chunk_manager_ptr->set_texture_atlas(m_renderer_ptr->get_texture_atlas());
    m_chunk_manager_ptr->set_world_storage(m_world_file, m_world_generator->get_storage_key());
    m_chunk_manager_ptr->update_player_position(start.x, start.y, start.z);
}

float Game::get_world_load_progress() const {
    if (!m_chunk_manager_ptr || !m_player_ptr) return 0.0f;
    const glm::vec3 p = m_player_ptr->get_position();
    const glm::ivec3 c = m_chunk_manager_ptr->get_chunk_coords_for_world(
        static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));
    int have = 0;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                if (m_chunk_manager_ptr->get_chunk(c.x + dx, c.y + dy, c.z + dz)) ++have;
    return static_cast<float>(have) / 27.0f;
}

void Game::pump_world_loading() {
    if (!m_chunk_manager_ptr) return;
    // update_player_position() already collects completed chunk and mesh jobs. Calling the
    // collectors here as well doubled main-thread chunk insertion during the loading screen.
    const glm::vec3 p = m_player_ptr->get_position();
    m_chunk_manager_ptr->update_player_position(p.x, p.y, p.z);
}

// ============================================================================
//  Экраны и текстовый ввод
// ============================================================================
glm::vec2 Game::get_cursor_position() const {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(m_window_ptr->get_glfw_ptr(), &x, &y);
    return {static_cast<float>(x), static_cast<float>(y)};
}

glm::ivec2 Game::get_window_size() const {
    return {m_window_ptr->get_width(), m_window_ptr->get_height()};
}

bool Game::is_caret_visible() const {
    return std::fmod(static_cast<float>(glfwGetTime()), 1.0f) < 0.5f;
}

void Game::clear_menu_frame() {
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if (m_vulkan_game_mirror) {
        m_vulkan_game_mirror->setMenuClear({Config::clear_color_r, Config::clear_color_g, Config::clear_color_b});
        return;
    }
#endif
    m_renderer_ptr->clear_screen(Config::clear_color_r, Config::clear_color_g, Config::clear_color_b);
}

void Game::render_hud_screen(const Hud_Screen& screen, int hovered,
                             const std::vector<Hud_World_Entry>* worlds,
                             int selected_world, float scroll_px) {
    if (screen.dirt_background) clear_menu_frame();
    m_hud_ptr->draw_menu_screen(m_window_ptr->get_width(), m_window_ptr->get_height(), screen, hovered,
                                worlds, selected_world, scroll_px, is_caret_visible());
}

void Game::render_loading_screen(const std::string& title, const std::string& status, float progress) {
    clear_menu_frame();
    m_hud_ptr->draw_loading_screen(m_window_ptr->get_width(), m_window_ptr->get_height(), title, status, progress);
}

std::string* Game::active_text_target() {
    if (m_chat_open) return &m_chat_input;
    return m_text_target;
}

void Game::begin_text_input(std::string* target, size_t max_length) {
    m_text_target = target;
    m_text_max = max_length;
    m_text_backspace_timer = 0.0f;
}

void Game::end_text_input() { m_text_target = nullptr; }

void Game::update_text_input(float delta_time) {
    if (m_chat_open || !m_text_target) return;
    if (m_input_ptr->is_key_pressed(GLFW_KEY_BACKSPACE)) {
        Utf8::pop_back(*m_text_target);
        m_text_backspace_timer = 0.4f;
    } else if (m_input_ptr->is_key_held(GLFW_KEY_BACKSPACE)) {
        m_text_backspace_timer -= delta_time;
        if (m_text_backspace_timer <= 0.0f) {
            Utf8::pop_back(*m_text_target);
            m_text_backspace_timer = 0.04f;
        }
    }
}

void Game::on_text_char(unsigned int codepoint) {
    // Принимаем только то, что умеет рисовать шрифт HUD (ASCII + кириллица): иначе в строке
    // были бы невидимые символы. Лимиты длины — в символах, а не в байтах.
    if (!Utf8::is_supported_by_font(codepoint)) return;
    if (m_chat_open) {
        if (Utf8::length(m_chat_input) < k_chat_input_max) {
            Utf8::append(m_chat_input, codepoint);
            m_chat_history_index = -1;
        }
    } else if (m_text_target && Utf8::length(*m_text_target) < m_text_max) {
        Utf8::append(*m_text_target, codepoint);
    }
}

// ============================================================================
//  Окно инвентаря и контейнеры (одна универсальная панель слотов)
// ============================================================================
namespace {
    constexpr int k_hotbar_start = static_cast<int>(Inventory::k_panel_hotbar_start);
}

Game::Panel_View Game::build_inventory_panel() const {
    Panel_View v;
    const bool creative = m_game_mode == Game_Mode::Creative;
    if (creative) {
        static constexpr const char* category_keys[] = {
            "panel.category.building", "panel.category.decoration", "panel.category.nature",
            "panel.category.equipment", "panel.category.copper", "panel.category.mobs"
        };
        const size_t section = m_inventory_ptr->get_creative_category();
        v.layout = Panel_Layouts::inventory_creative(m_inventory_ptr->get_page_count(),
                                                     m_inventory_ptr->get_current_page(), k_hotbar_start,
                                                     static_cast<int>(section), tr(category_keys[section]));
        v.title = tr("panel.creative_inventory");
    } else {
        v.layout = Panel_Layouts::inventory_survival(k_hotbar_start);
    }

    using namespace Panel_Layouts::Panel_Slot_Ids;
    for (const Hud_Panel_Slot& slot : v.layout.slots) {
        if (slot.id >= k_inventory_craft_base && slot.id < k_inventory_craft_base + k_inventory_craft_count) {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->get_craft_slot(static_cast<size_t>(slot.id - k_inventory_craft_base))));
        } else if (slot.id == k_inventory_craft_output) {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->peek_craft_output()));
        } else if (slot.id >= k_inventory_armor_base && slot.id < k_inventory_armor_base + k_inventory_armor_count) {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->get_armor_slot(static_cast<size_t>(slot.id - k_inventory_armor_base))));
        } else if (slot.id >= k_creative_palette_base && slot.id < k_creative_palette_base + k_creative_palette_count) {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->get_palette_slot(static_cast<size_t>(slot.id - k_creative_palette_base)), true));
        } else {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->get_panel_slot(static_cast<size_t>(slot.id))));
        }
    }
    return v;
}

void Game::draw_inventory_overlay() {
    Panel_View v = build_inventory_panel();
    v.layout.preview_model = m_player_model.is_loaded();
    const glm::vec2 cursor = get_cursor_position();
    const int w = m_window_ptr->get_width(), h = m_window_ptr->get_height();
    const int hovered = Hud::get_panel_slot_at(w, h, v.layout, cursor.x, cursor.y);

    const Item_Stack carried = m_inventory_ptr->get_carried_stack();
    const Hud_Inventory_Slot carried_slot = make_hud_slot(carried, m_inventory_ptr->carried_is_infinite());
    m_hud_ptr->draw_panel_screen(w, h, m_renderer_ptr->get_texture_atlas(), v.layout, v.slots, hovered,
                                 carried.is_empty() ? nullptr : &carried_slot,
                                 cursor.x, cursor.y, v.title);
    if (v.layout.preview_model) draw_inventory_model_preview(v.layout);
}

// Модель игрока в окне инвентаря: своя камера, поворот покачивается. Рисуется поверх плоской рамки.
void Game::draw_inventory_model_preview(const Hud_Panel_Layout& layout) {
    if (m_window_ptr->is_vulkan()) return; // GL model preview not yet ported
    Shader* mob_shader = m_renderer_ptr->get_mob_shader();
    if (!mob_shader) return;
    const int w = m_window_ptr->get_width(), h = m_window_ptr->get_height();
    const Hud_Rect panel = Hud::get_panel_rect(w, h, layout);
    const float s = Hud::get_panel_scale(w, h, layout);
    const int x = static_cast<int>(std::floor(panel.x + layout.preview_rect.x * s));
    const int y_top = static_cast<int>(std::floor(panel.y + layout.preview_rect.y * s));
    const int rw = static_cast<int>(std::ceil(layout.preview_rect.w * s));
    const int rh = static_cast<int>(std::ceil(layout.preview_rect.h * s));
    if (rw <= 0 || rh <= 0) return;

    const float yaw = 25.0f * std::sin(static_cast<float>(glfwGetTime()) * 0.8f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.9f, 3.6f), glm::vec3(0.0f, 0.9f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(glm::radians(30.0f), static_cast<float>(rw) / static_cast<float>(rh), 0.1f, 20.0f);

    m_renderer_ptr->begin_sub_viewport(x, y_top, rw, rh, h);
    m_player_model.render_preview(*mob_shader, view, projection, m_day_night_cycle.get_ambient_intensity(), yaw);
    m_renderer_ptr->end_sub_viewport(w, h);
}

void Game::inventory_click(bool right) {
    const Panel_View v = build_inventory_panel();
    const glm::vec2 cursor = get_cursor_position();
    const int w = m_window_ptr->get_width(), h = m_window_ptr->get_height();

    const int category = Hud::get_panel_category_button_at(w, h, v.layout, cursor.x, cursor.y);
    if (category >= 0) {
        m_inventory_ptr->set_creative_category(static_cast<size_t>(category));
        return;
    }
    const int page_button = Hud::get_panel_page_button_at(w, h, v.layout, cursor.x, cursor.y);
    if (page_button >= 0) {
        inventory_change_page(page_button == 0 ? -1 : 1);
        return;
    }
    const int index = Hud::get_panel_slot_at(w, h, v.layout, cursor.x, cursor.y);
    if (index < 0) return;
    const int id = v.layout.slots[static_cast<size_t>(index)].id;

    using namespace Panel_Layouts::Panel_Slot_Ids;
    auto is_craft_grid_id = [](int slot_id) {
        return slot_id >= k_inventory_craft_base && slot_id < k_inventory_craft_base + k_inventory_craft_count;
    };
    const bool shift = !right && (m_input_ptr->is_key_held(GLFW_KEY_LEFT_SHIFT) || m_input_ptr->is_key_held(GLFW_KEY_RIGHT_SHIFT));
    if (id >= k_creative_palette_base && id < k_creative_palette_base + k_creative_palette_count) {
        m_inventory_ptr->click_palette_slot(static_cast<size_t>(id - k_creative_palette_base), right);
        return;
    }
    if (shift) {
        if (id >= 0 && id < static_cast<int>(Inventory::k_panel_slot_count)) m_inventory_ptr->quick_move_panel(static_cast<size_t>(id));
        else if (is_craft_grid_id(id)) m_inventory_ptr->quick_move_craft_slot(static_cast<size_t>(id - k_inventory_craft_base));
        else if (id == k_inventory_craft_output) m_inventory_ptr->quick_craft_output();
        return;
    }

    if (id >= 0 && id < static_cast<int>(Inventory::k_panel_slot_count)) {
        if (right) m_inventory_ptr->right_click_panel_slot(static_cast<size_t>(id));
        else m_inventory_ptr->click_panel_slot(static_cast<size_t>(id));
    } else if (is_craft_grid_id(id)) {
        m_inventory_ptr->click_craft_slot(static_cast<size_t>(id - k_inventory_craft_base), right);
    } else if (id == k_inventory_craft_output) {
        m_inventory_ptr->click_craft_output();
    } else if (id >= k_inventory_armor_base && id < k_inventory_armor_base + k_inventory_armor_count) {
        m_inventory_ptr->click_armor_slot(static_cast<size_t>(id - k_inventory_armor_base), right);
    }
}

void Game::inventory_change_page(int direction) {
    if (direction > 0) m_inventory_ptr->next_page();
    else m_inventory_ptr->previous_page();
}

void Game::on_inventory_closed() {
    // Если панель закрыли, держа стак на курсоре (или оставив предметы в сетке крафта),
    // всё раскладывается обратно по инвентарю, а не исчезает.
    m_inventory_ptr->drop_carried_into_inventory();
}

Game::Panel_View Game::build_container_panel() const {
    Panel_View v;
    const size_t own = m_container_session.get_container_slot_count();
    const size_t paired = m_open_container_paired ? m_open_container_paired->slots.size() : 0;
    const size_t content_count = own + paired;
    const int offset = static_cast<int>(content_count);

    switch (m_container_session.get_type()) {
    case Block_Entity_Type::Chest:
        v.title = tr(paired > 0 ? "panel.large_chest" : "panel.chest");
        v.layout = Panel_Layouts::chest(content_count, true, offset, k_hotbar_start);
        break;
    case Block_Entity_Type::Crafting_Table:
        v.title = tr("panel.crafting");
        v.layout = Panel_Layouts::crafting_table(own, true, offset, k_hotbar_start);
        break;
    case Block_Entity_Type::Enchanting_Table: {
        v.title = "Enchanting Table";
        v.layout = Panel_Layouts::enchanting_table(offset,k_hotbar_start);
        const Block_Types table = m_chunk_manager_ptr->get_block_world(
            m_open_container_pos.x,m_open_container_pos.y,m_open_container_pos.z);
        const int tier = Enchanting::tier_of(table);
        v.layout.enchant_table_tier = tier;
        v.layout.enchant_player_level = m_inventory_ptr->xp_level();
        v.layout.enchant_xp_progress = m_inventory_ptr->xp_progress();
        const Item_Stack input = m_container_session.get_container_slot(0);
        const Item_Stack lapis = m_container_session.get_container_slot(1);
        const auto options=Enchanting::offers(input,tier,m_enchant_seed);
        for (int i=0;i<3;++i) {
            const auto& option=options[static_cast<size_t>(i)];
            const bool resources=m_game_mode==Game_Mode::Creative ||
                (v.layout.enchant_player_level >= option.required_level && lapis.type==Block_Types::Lapis_Lazuli &&
                 lapis.count >= option.lapis_cost);
            v.layout.enchant_enabled[static_cast<size_t>(i)] = option.available && resources;
            v.layout.enchant_names[static_cast<size_t>(i)] = option.available
                ? std::string(Enchanting::name(option.kind))+" "+std::to_string(option.level) : "No enchant";
            v.layout.enchant_costs[static_cast<size_t>(i)] = "LV "+std::to_string(option.required_level)+
                "  XP "+std::to_string(option.xp_cost)+"  L "+std::to_string(option.lapis_cost);
        }
        break;
    }
    case Block_Entity_Type::Furnace: {
        v.title = tr("panel.furnace");
        float flame = 0.0f, arrow = 0.0f;
        if (const Block_Entity* furnace = m_container_session.get_entity()) {
            if (furnace->is_burning() && furnace->burn_time_total > 0.0f) {
                flame = std::clamp(furnace->burn_time_left / furnace->burn_time_total, 0.0f, 1.0f);
            }
            const Item_Stack& in = furnace->slots[Block_Entity_Slots::furnace_input];
            const Smelting_Recipe* recipe = in.is_empty() ? nullptr : Smelting_Registry::get_instance().find(in.type);
            if (recipe && recipe->time_seconds > 0.0f) arrow = std::clamp(furnace->cook_progress / recipe->time_seconds, 0.0f, 1.0f);
        }
        v.layout = Panel_Layouts::furnace(own, flame, arrow, true, offset, k_hotbar_start);
        break;
    }
    case Block_Entity_Type::None:
        break;
    }

    for (const Hud_Panel_Slot& slot : v.layout.slots) {
        if (slot.id == Panel_Layouts::Panel_Slot_Ids::k_container_output) {
            v.slots.push_back(make_hud_slot(m_container_session.peek_crafting_output()));
        } else if (slot.id < offset) {
            const size_t k = static_cast<size_t>(slot.id);
            v.slots.push_back(k < own ? make_hud_slot(m_container_session.get_container_slot(k))
                                      : make_hud_slot(m_open_container_paired->slots[k - own]));
        } else {
            v.slots.push_back(make_hud_slot(m_inventory_ptr->get_panel_slot(static_cast<size_t>(slot.id - offset))));
        }
    }
    return v;
}

void Game::load_villagers() {
    m_villages.clear();
    if(m_world_settings.dimension!="overworld") return;
    std::ifstream in(dimension_data_file("villagers"));
    if(!in)return;
    const std::string saved{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    m_villages.deserialize(saved);
}

Game::Panel_View Game::build_trade_panel() const {
    Panel_View v;
    const Village_System::Resident* resident=m_villages.trader();
    if(!resident)return v;
    v.title=std::string("Trade - ")+Village::name(resident->profession);
    v.layout.size={176.0f,200.0f};
    v.layout.labels.push_back({"Offers",{8.0f,8.0f}});
    const auto offers=Village::offers(resident->profession);
    for(int n=0;n<3;++n) {
        const auto offer=offers[static_cast<size_t>(n)];
        if(offer.cost==0 || offer.result==0)continue;
        const float y=22.0f+n*25.0f;
        v.layout.slots.push_back({{22.0f,y},600+n,false});
        v.slots.push_back(make_hud_slot(Item_Stack{static_cast<Block_Types>(offer.cost),offer.cost_count}));
        v.layout.labels.push_back({"=>",{92.0f,y+4.0f}});
        v.layout.slots.push_back({{128.0f,y},700+n,true});
        v.slots.push_back(make_hud_slot(Item_Stack{static_cast<Block_Types>(offer.result),offer.result_count}));
    }
    v.layout.labels.push_back({"Inventory",{8.0f,102.0f}});
    // Trade slots don't store player items. The inventory remains the same 36 slots.
    Panel_Layouts::append_player_slots(v.layout,8.0f,114.0f,172.0f,true,1000,k_hotbar_start);
    for(size_t index=0;index<Inventory::k_panel_slot_count;++index)
        v.slots.push_back(make_hud_slot(m_inventory_ptr->get_panel_slot(index)));
    return v;
}

void Game::draw_container_overlay() {
    if(m_villages.trade_open()) {
        const Panel_View v=build_trade_panel();
        const glm::vec2 cursor=get_cursor_position();
        const int w=m_window_ptr->get_width(),h=m_window_ptr->get_height();
        const int hovered=Hud::get_panel_slot_at(w,h,v.layout,cursor.x,cursor.y);
        const Item_Stack carried=m_inventory_ptr->get_carried_stack();
        const Hud_Inventory_Slot carried_slot=make_hud_slot(carried);
        m_hud_ptr->draw_panel_screen(w,h,m_renderer_ptr->get_texture_atlas(),v.layout,v.slots,hovered,
            carried.is_empty()?nullptr:&carried_slot,cursor.x,cursor.y,v.title);
        return;
    }
    if (!m_container_session.is_open()) return;
    const Panel_View v = build_container_panel();
    const glm::vec2 cursor = get_cursor_position();
    const int w = m_window_ptr->get_width(), h = m_window_ptr->get_height();
    const int hovered = Hud::get_panel_slot_at(w, h, v.layout, cursor.x, cursor.y);

    const Item_Stack carried = m_container_session.get_carried();
    const Hud_Inventory_Slot carried_slot = make_hud_slot(carried);
    m_hud_ptr->draw_panel_screen(w, h, m_renderer_ptr->get_texture_atlas(), v.layout, v.slots, hovered,
                                 carried.is_empty() ? nullptr : &carried_slot, cursor.x, cursor.y, v.title);
}

void Game::container_click(bool right) {
    if(m_villages.trade_open()) {
        const Panel_View v=build_trade_panel();
        const glm::vec2 cursor=get_cursor_position();
        const int index=Hud::get_panel_slot_at(m_window_ptr->get_width(),m_window_ptr->get_height(),
            v.layout,cursor.x,cursor.y);
        if(index<0)return;
        const int id=v.layout.slots[static_cast<size_t>(index)].id;
        if(id>=700 && id<703) {
            std::string feedback;
            m_villages.trade(id-700,*m_inventory_ptr,feedback);
            if(!feedback.empty())push_chat_message(feedback);
        } else if(id>=1000 && id<1000+static_cast<int>(Inventory::k_panel_slot_count)) {
            const size_t slot=static_cast<size_t>(id-1000);
            const bool shift=!right && (m_input_ptr->is_key_held(GLFW_KEY_LEFT_SHIFT) ||
                                        m_input_ptr->is_key_held(GLFW_KEY_RIGHT_SHIFT));
            if(shift)m_inventory_ptr->quick_move_panel(slot);
            else if(right)m_inventory_ptr->right_click_panel_slot(slot);
            else m_inventory_ptr->click_panel_slot(slot);
        }
        return;
    }
    if (!m_container_session.is_open()) return;
    const Panel_View v = build_container_panel();
    const glm::vec2 cursor = get_cursor_position();
    if (m_container_session.get_type()==Block_Entity_Type::Enchanting_Table) {
        const int option_index = Hud::get_panel_enchant_button_at(m_window_ptr->get_width(),
            m_window_ptr->get_height(),v.layout,cursor.x,cursor.y);
        if (option_index>=0) {
            if (v.layout.enchant_enabled[static_cast<size_t>(option_index)]) {
                Block_Entity* table=m_container_session.get_entity_mutable();
                const int tier=Enchanting::tier_of(m_chunk_manager_ptr->get_block_world(
                    m_open_container_pos.x,m_open_container_pos.y,m_open_container_pos.z));
                if (table && tier>0 && table->slots.size()==2) {
                    Item_Stack& item=table->slots[0];
                    Item_Stack& lapis=table->slots[1];
                    const auto options=Enchanting::offers(item,tier,m_enchant_seed);
                    const auto& option=options[static_cast<size_t>(option_index)];
                    const bool creative=m_game_mode==Game_Mode::Creative;
                    if (option.available && (creative ||
                       (m_inventory_ptr->xp_level()>=option.required_level &&
                        lapis.type==Block_Types::Lapis_Lazuli && lapis.count>=option.lapis_cost))) {
                        if (Enchanting::apply(item,option.kind,option.level)) {
                            if (!creative) {
                                lapis.count-=option.lapis_cost;
                                if (lapis.count==0) lapis=Item_Stack{};
                                m_inventory_ptr->spend_levels(option.xp_cost);
                            }
                            m_enchant_seed=m_enchant_seed*1664525u+1013904223u;
                            push_chat_message(std::string("Enchanted: ")+Enchanting::name(option.kind)+" "+
                                std::to_string(option.level));
                        }
                    }
                }
            }
            return;
        }
    }
    const int index = Hud::get_panel_slot_at(m_window_ptr->get_width(), m_window_ptr->get_height(),
                                             v.layout, cursor.x, cursor.y);
    if (index < 0) return;
    const int id = v.layout.slots[static_cast<size_t>(index)].id;
    const size_t session_index = id == Panel_Layouts::Panel_Slot_Ids::k_container_output ? Container_Session::k_output_slot : static_cast<size_t>(id);
    const bool shift = !right && (m_input_ptr->is_key_held(GLFW_KEY_LEFT_SHIFT) || m_input_ptr->is_key_held(GLFW_KEY_RIGHT_SHIFT));
    if (shift) {
        m_container_session.quick_move(session_index, *m_inventory_ptr, m_open_container_paired);
        return;
    }
    m_container_session.click_slot(session_index, *m_inventory_ptr, m_open_container_paired, right);
}

void Game::close_container() {
    m_villages.close_trade();
    m_inventory_ptr->drop_carried_into_inventory();
    m_container_session.close(*m_inventory_ptr);
    m_open_container_paired = nullptr;
}

bool Game::is_camera_underwater() const {
    if (!m_chunk_manager_ptr) return false;

    const glm::vec3 eye = m_camera_ptr->get_position();
    const Block_Types block = m_chunk_manager_ptr->get_block_world(
        static_cast<int>(std::floor(eye.x)),
        static_cast<int>(std::floor(eye.y)),
        static_cast<int>(std::floor(eye.z))
    );
    return get_block_props(block).state == Block_State::Liquid;
}

void Game::reload_world(const std::string& generation_folder) {
    m_vulkan_chunk_feed.clear();
    // A reload destroys the chunk namespace. Do not let old pending edits survive it.
    m_pending_portal_type.clear();
    m_pending_command_portal_type.clear();
    const glm::vec3 player_pos = m_player_ptr->get_position();
    int spawn_x = static_cast<int>(std::floor(player_pos.x));
    int spawn_z = static_cast<int>(std::floor(player_pos.z));

    // Quiesce generation/meshing/disk-reader threads before releasing old
    // dimension data. clear() cannot safely race with workers from the old world.
    LOG_INFO("World reload: waiting for old streaming workers");
    m_chunk_manager_ptr->prepare_for_reload();
    LOG_INFO("World reload: old workers stopped; flushing changed chunks");
    m_chunk_manager_ptr->clear();
    LOG_INFO("World reload: old chunks cleared; invalidating render caches");
    m_mobs.clear();
    m_villages.clear();
    load_villagers();
    m_mobs.reset_timers();
    m_logic = Logic_Manager{};
    m_logic_scan_timer = 0.0f;
    load_dimension_fire();
    m_crops_ptr = std::make_unique<Crop_Manager>();
    m_saplings_ptr = std::make_unique<Sapling_Manager>();
    m_block_interaction_ptr->set_crop_manager(m_crops_ptr.get());
    m_block_interaction_ptr->set_sapling_manager(m_saplings_ptr.get());
    {
        std::ifstream crops(dimension_data_file("crops"));
        if (crops) {
            const std::string data{std::istreambuf_iterator<char>(crops), std::istreambuf_iterator<char>()};
            m_crops_ptr->deserialize(data);
        }
        std::ifstream saplings(dimension_data_file("saplings"));
        if (saplings) {
            const std::string data{std::istreambuf_iterator<char>(saplings), std::istreambuf_iterator<char>()};
            m_saplings_ptr->deserialize(data);
        }
    }
    // Предметы лежали в старом мире (другой пресет генерации) — в новом им делать нечего.
    if (m_dropped_items_ptr) m_dropped_items_ptr->clear();
    for (const glm::ivec3& position : m_chunk_manager_ptr->take_unloaded_chunk_positions()) {
        m_renderer_ptr->invalidate_chunk_cache(position);
        m_vulkan_chunk_feed.chunkUnloaded({0, position.x, position.y, position.z});
    }
    m_chunk_manager_ptr.reset();
    m_world_generator = std::make_unique<World_Generator>(generation_folder, m_world_settings.seed,
                                                        dimension_from_name(m_world_settings.dimension));
    m_world_generator->set_biome(biome_from_name(m_world_settings.biome));
    m_mobs.set_biome_lookup([this](int x, int y, int z) { return m_world_generator->get_biome_name(x, y, z); });
    m_chunk_manager_ptr = std::make_unique<Chunk_Manager>();
    m_chunk_manager_ptr->set_vulkan_primary(m_window_ptr->is_vulkan());
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if(m_vulkan_game_mirror)m_chunk_manager_ptr->set_vulkan_worldgen(&m_vulkan_game_mirror->worldgenRuntime());
#endif
    // Пересоздание менеджера не должно сбрасывать дальность прорисовки к значению
    // по умолчанию — иначе выбранная в настройках дальность терялась при смене
    // пресета генерации (F6).
    m_chunk_manager_ptr->set_load_radius(m_render_distance);
    apply_vertical_distance();
    m_chunk_manager_ptr->set_world_generator(*m_world_generator);
    m_chunk_manager_ptr->set_texture_atlas(m_renderer_ptr->get_texture_atlas());
    m_chunk_manager_ptr->set_world_storage(m_world_file, m_world_generator->get_storage_key());
    LOG_INFO("World reload: new generator and streaming workers created");

    // Игрок остаётся в своей колонке; в мире без земли find_spawn_surface сдвинет его к ближайшему острову.
    const int spawn_y = m_world_generator->find_spawn_surface(spawn_x, spawn_z) + 2;
    int respawn_x = 8, respawn_z = 8;
    const int respawn_y = m_world_generator->find_spawn_surface(respawn_x, respawn_z) + 2;
    m_spawn_position = glm::vec3(static_cast<float>(respawn_x),
                                 static_cast<float>(respawn_y), static_cast<float>(respawn_z));
    const Dimension_Info& dim = m_world_generator->get_dimension();
    m_day_night_cycle.set_frozen(dim.time_mode == Time_Mode::Fixed);
    m_day_night_cycle.set_normalized_time(dim.time_mode == Time_Mode::Fixed
        ? dim.fixed_time : m_world_settings.day_time);
    if (dim.time_mode == Time_Mode::Cycle) m_day_night_cycle.set_day_count(m_world_settings.day_count);
    m_player_ptr->set_position(glm::vec3(
        static_cast<float>(spawn_x),
        static_cast<float>(spawn_y),
        static_cast<float>(spawn_z)
    ));

    const int player_cx = static_cast<int>(std::floor(spawn_x / static_cast<float>(Config::chunk_size)));
    const int player_cz = static_cast<int>(std::floor(spawn_z / static_cast<float>(Config::chunk_size)));

    // A generation-preset reload is another cold-cache entry point. Warm this region before
    // asking the manager for its first chunk column; otherwise its first column waits on the
    // entire regional hydrology solve despite the column worker itself being asynchronous.
    m_world_generator->prewarm_generation_region_for_chunk(player_cx, player_cz);

    m_chunk_manager_ptr->update_player_position(
        static_cast<float>(spawn_x),
        static_cast<float>(spawn_y),
        static_cast<float>(spawn_z)
    );

    m_camera_ptr->set_position(
        m_player_ptr->get_position() + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f)
    );

    LOG_INFO("Switched world generation to " + generation_folder);
}

// --- Тики -------------------------------------------------------------------------------
// Системы, которые живут в тиках (20 в секунду). Порядок: Pre -> Simulate -> Post.
// Животные — отдельным модулем (mobs/Mobs_Update.h). Ввод, камера и отрисовка сюда не входят.
void Game::register_tick_systems() {
    using Phase = Tick_Phase;
    m_tick.add("day_night", Phase::Pre, 0, [this](float dt) {
        OC_ZONE("Tick::day_night");
        m_day_night_cycle.update(dt);
        m_weather.update(dt);
    });
    m_tick.add("hunger", Phase::Pre, 10, [this](float dt) {
        OC_ZONE("Tick::hunger");
        m_player_ptr->update_hunger(dt);
    });
    m_tick.add("block_entities", Phase::Simulate, 0, [this](float dt) {
        OC_ZONE("Tick::block_entities");
        m_chunk_manager_ptr->update_block_entities(dt);
    });
    m_tick.add("fluids", Phase::Simulate, 10, [this](float dt) {
        OC_ZONE("Tick::fluids");
        m_chunk_manager_ptr->update_fluids(dt);
    });
    m_tick.add("fire", Phase::Simulate, 15, [this](float dt) {
        OC_ZONE("Tick::fire");
        m_fire.tick(dt, *m_chunk_manager_ptr);
        m_fire_damage_timer -= dt;
        if (m_fire_damage_timer > 0.f || m_dimension_landing) return;
        m_fire_damage_timer = 1.f;
        if (m_game_mode == Game_Mode::Survival && m_player_ptr->is_alive()) {
            const glm::ivec3 feet(glm::floor(m_player_ptr->get_position()));
            if (m_chunk_manager_ptr->get_block_world(feet.x, feet.y, feet.z) == Block_Types::Fire ||
                m_chunk_manager_ptr->get_block_world(feet.x, feet.y + 1, feet.z) == Block_Types::Fire)
                m_player_ptr->take_damage(1);
        }
        // Only nearby active flames need to affect live mobs. This is deliberately
        // bounded: fire never performs a world-wide entity sweep every frame.
        m_fire.for_each_near(m_player_ptr->get_position(), 16.f, 16,
            [this](const glm::vec3& flame) {
                m_mobs.damage_in_radius(flame, 1.15f, 2.f);
            });
    });
    m_tick.add("mobs", Phase::Simulate, 20, [this](float dt) {
        OC_ZONE("Tick::mobs");
        apply_mob_result(m_mobs.update_tick(dt, mob_view(), *m_chunk_manager_ptr));
    });
    m_tick.add("villages", Phase::Simulate, 24, [this](float dt) {
        if (m_world_settings.dimension != "overworld" || m_dimension_landing) return;
        const auto hostiles=m_mobs.hostile_positions();
        const auto village_result=m_villages.tick(dt,m_player_ptr->get_position(),
            m_day_night_cycle.get_normalized_time()>=Config::night_start_normalized,
            *m_chunk_manager_ptr,m_mobs.navigation(),hostiles,
            [this](const glm::vec3& from,float radius,float damage) {
                return m_mobs.damage_hostile_near(from,radius,damage);
            });
        if(m_game_mode==Game_Mode::Survival && m_player_ptr->is_alive() && village_result.player_damage>0) {
            Mob_Result damage;
            damage.player_damage=village_result.player_damage;
            apply_mob_result(damage);
        }
    });
    m_tick.add("crops", Phase::Simulate, 30, [this](float dt) {
        OC_ZONE("Tick::crops");
        if (!m_crops_ptr) return;
        std::vector<glm::ivec3> changed_blocks;
        m_crops_ptr->update(dt, *m_chunk_manager_ptr, changed_blocks);
        for (const glm::ivec3& pos : changed_blocks) {
            invalidate_block_mesh(pos.x, pos.y, pos.z, *m_chunk_manager_ptr, *m_renderer_ptr);
        }
        });
    m_tick.add("saplings", Phase::Simulate, 35, [this](float dt) {
        OC_ZONE("Tick::saplings");
        if (!m_saplings_ptr) return;
        std::vector<glm::ivec3> changed_blocks;
        m_saplings_ptr->update(dt, *m_chunk_manager_ptr, changed_blocks);
        for (const glm::ivec3& pos : changed_blocks) {
            invalidate_block_mesh(pos.x, pos.y, pos.z, *m_chunk_manager_ptr, *m_renderer_ptr);
        }
    });
    // Красная логика: редстоун-тики, поиск узлов вокруг игрока (раз в секунду), обновление мешей.
    m_tick.add("redstone", Phase::Simulate, 37, [this](float dt) {
        OC_ZONE("Tick::redstone");
        auto logic_world = make_chunk_world(*m_chunk_manager_ptr);
        m_logic.update(logic_world, dt);
        m_logic_scan_timer -= dt;
        if (m_logic_scan_timer <= 0.0f) {
            m_logic_scan_timer = 1.0f;
            { OC_ZONE("Tick::redstone discovery");
              m_logic.discover(logic_world, glm::ivec3(glm::floor(m_player_ptr->get_position())), 12); }
        }
        for (const glm::ivec3& pos : m_logic.take_changed()) {
            invalidate_block_mesh(pos.x, pos.y, pos.z, *m_chunk_manager_ptr, *m_renderer_ptr);
        }
        // Динамит, на который пришёл сигнал красной логики, взрывается.
        for (const glm::ivec3& pos : m_logic.take_triggered()) {
            if (m_chunk_manager_ptr->get_block_world(pos.x, pos.y, pos.z) == Block_Types::Dynamite) {
                detonate(pos, 4.0f);
            }
        }
    });
    m_tick.add("dropped_items", Phase::Simulate, 40, [this](float dt) {
        OC_ZONE("Tick::dropped_items");
        // Подбирать предметы можно только в Survival и пока жив.
        const glm::vec3 player_pos = m_player_ptr->get_position();
        m_dropped_items_ptr->update(
            dt,
            *m_chunk_manager_ptr,
            player_pos,
            m_player_ptr->get_height(),
            (m_game_mode == Game_Mode::Survival && m_player_ptr->is_alive()) ? m_inventory_ptr.get() : nullptr,
            [this](const glm::vec3& p) { return m_mobs.mob_near(p, Config::potion_mob_hit_radius); });

        // Разбившиеся шарики: эффект получают мобы и игрок в радиусе разлёта.
        for (const Thrown_Hit& hit : m_dropped_items_ptr->take_thrown_hits()) {
            const std::optional<Potion_Info> potion = potion_of(hit.type);
            if (!potion) continue;
            m_mobs.splash_potion(hit.position, Config::potion_splash_radius, *potion);
            const glm::vec3 player_body = m_player_ptr->get_position() + glm::vec3(0.0f, m_player_ptr->get_height() * 0.5f, 0.0f);
            if (m_player_ptr->is_alive() && glm::distance(player_body, hit.position) <= Config::potion_splash_radius) {
                m_player_ptr->effects().apply(potion->effect, potion->duration, potion->amplifier);
            }
        }
    });
}

Mob_Player_View Game::mob_view() const {
    Mob_Player_View view;
    view.spawn_radius = static_cast<float>(m_render_distance * Config::chunk_size) * 2.0f / 3.0f;
    view.difficulty = m_difficulty;
    view.position = m_player_ptr->get_position();
    view.eye_height = m_player_ptr->get_eye_height();
    view.alive = m_player_ptr->is_alive();
    view.sneaking = m_player_ptr->is_crouching();
    view.forward = m_camera_ptr->get_front();
    view.velocity = m_player_ptr->get_velocity();
    view.horizontal_speed = m_player_ptr->get_horizontal_speed();
    view.night = m_day_night_cycle.get_normalized_time() >= Config::night_start_normalized;
    view.sky_brightness = m_day_night_cycle.get_sky_brightness();
    view.night_light_floor = m_day_night_cycle.get_night_light_floor();
    return view;
}

Mob_Blocked_Fn Game::mob_blocked_fn() const {
    return [this](const glm::vec3& position) {
        return m_player_ptr->check_collision(position, *m_chunk_manager_ptr);
    };
}

// Применяет результат Mobs_Update к игроку, камере, блок-взаимодействию и инвентарю.
void Game::apply_mob_result(const Mob_Result& result) {
    // Перемещение игрока — только одна из возможных частей результата мобов.
    // Урон/истощение/износ предметов не должны зависеть от set_player_position:
    // обычная атака врага возвращает только player_damage.
    if (result.set_player_position) {
        m_player_ptr->set_position(result.player_position);
        m_camera_ptr->set_position(result.player_position + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f));
    }
    if (result.set_riding) m_player_ptr->set_riding(result.riding);
    if (result.cancel_block_actions) m_block_interaction_ptr->cancel_actions();
    if (result.consume_held_item) m_inventory_ptr->consume_selected(1);
    if (result.damage_held_tool) m_inventory_ptr->damage_selected_tool(1);
    if (result.exhaustion > 0.0f) m_player_ptr->add_exhaustion(result.exhaustion);
    if (result.player_damage > 0 && m_player_ptr->is_alive()) {
        const int points = std::min(m_inventory_ptr->armor_points(), Config::armor_points_cap);
        const float reduction = static_cast<float>(points) / Config::armor_points_divisor;
        const float extra=std::min(0.80f,0.04f*m_inventory_ptr->total_enchant_protection());
        const int damage = static_cast<int>(std::ceil(static_cast<float>(result.player_damage) *
            (1.0f - reduction)*(1.0f-extra)));
        m_player_ptr->take_damage(std::max(damage, 1));
        m_inventory_ptr->damage_armor(1);
    }
}

void Game::update_gameplay(float delta_time, bool simulation_active, bool player_actions) {
    OC_ZONE("Game::update_gameplay");
    { OC_ZONE("Gameplay::poll_iso_capture"); poll_iso_capture_result(); }

    if (!simulation_active) {
        // On paused frames update_player_position() is not reached, so collect worker results
        // here. Active frames collect them exactly once inside update_player_position().
        m_chunk_manager_ptr->collect_finished_chunks();
        m_chunk_manager_ptr->collect_finished_mesh_jobs();
        // Мир на паузе, пока открыт экран контейнера, но печь должна продолжать жариться —
        // иначе игрок не увидит, как пожарилось мясо, пока держит окно печи открытым.
        if (is_container_open()) {
            m_chunk_manager_ptr->update_block_entities(delta_time);
        }
        return;
    }

    if (!player_actions) {
        // Окно открыто: управление отключено (иначе зажатая клавиша «залипла» бы), но физика идёт.
        m_player_ptr->set_movement_input({0.0f, 0.0f, false, false}, m_camera_ptr->get_front(), m_camera_ptr->get_right());
        m_mobs.set_ride_input(Horse_Ride_Input{});
    }

    // Boot enchantment must be sampled before physics processes landing damage.
    m_player_ptr->set_feather_falling_level(m_inventory_ptr->get_armor_slot(3).enchant_feather_falling);
    if (m_dimension_landing) {
        const glm::vec3 landing = m_player_ptr->get_position();
        const int cx = static_cast<int>(std::floor(landing.x / Config::chunk_size));
        const int cy = static_cast<int>(std::floor((landing.y - 2.0f) / Config::chunk_height));
        const int cz = static_cast<int>(std::floor(landing.z / Config::chunk_size));
        // Do not simulate gravity against absent chunks on the first arrival frames.
        if (m_chunk_manager_ptr->get_chunk(cx, cy, cz)) m_dimension_landing = false;
        else m_player_ptr->reset_velocity();
    }
    if (!m_dimension_landing) {
        OC_ZONE("Gameplay::player update");
        m_player_ptr->update(delta_time, *m_chunk_manager_ptr);
    }

    // Модель игрока: поворот по взгляду, ходьба/стойка по скорости. Без файла модели ничего не делает.
    m_player_model.ensure_loaded();
    {
        const glm::vec3 front = m_camera_ptr->get_front();
        m_player_model.update(delta_time, m_player_ptr->get_position(),
                              glm::degrees(std::atan2(front.x, front.z)),
                              m_player_ptr->get_horizontal_speed() > 0.3f);
    }

    // Survival: смерть -> телепорт обратно на изначальную точку спавна с полным здоровьем.
    if (m_game_mode == Game_Mode::Survival && !m_player_ptr->is_alive()) {
        m_player_ptr->respawn(m_spawn_position);
        m_block_interaction_ptr->cancel_actions();
    }
            // КОМПРОМИСС: животные-всадники (лошади) обновляются по кадрам, а не в тиках. Верхом позицию
            // игрока выставляет седло, и камера должна взять её в этом же кадре, иначе картинка дёргается.
            apply_mob_result(m_mobs.update_frame(delta_time, mob_view(), mob_blocked_fn(), *m_chunk_manager_ptr));

    m_camera_ptr->set_position(
        m_player_ptr->get_position() + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f)
    );

    tick_portal_travel(delta_time);
    auto player_pos = m_player_ptr->get_position();
    m_chunk_manager_ptr->update_player_position(player_pos.x, player_pos.y, player_pos.z);

    // --- Тики: мир, существа, время и голод выполняются с фиксированным шагом -------------
    // Системы регистрируются в register_tick_systems(). Здесь только прокрутка часов.
    { OC_ZONE("Gameplay::tick systems"); m_tick.run_frame(delta_time); }

    for (const glm::ivec3& position : m_chunk_manager_ptr->take_unloaded_chunk_positions()) {
        m_renderer_ptr->invalidate_chunk_cache(position);
        m_vulkan_chunk_feed.chunkUnloaded({0, position.x, position.y, position.z});
    }

    if (!player_actions) return;

    const glm::vec3 eye = m_camera_ptr->get_position();
    m_block_interaction_ptr->set_place_block(m_inventory_ptr->get_selected_block());
    // Ближайшее животное на луче взгляда (логика — в Mobs_Update::aim).
    const float mob_distance = m_mobs.aim(eye,m_camera_ptr->get_front());
    const float villager_distance = m_world_settings.dimension == "overworld"
        ? m_villages.aim(eye,m_camera_ptr->get_front(),Config::player_attack_reach) : -1.0f;
    const bool village_target = villager_distance>=0 &&
        (mob_distance<0 || villager_distance<=mob_distance);
    const float entity_distance = village_target ? villager_distance : mob_distance;

    { OC_ZONE("Gameplay::block interaction"); m_block_interaction_ptr->update(
        delta_time,
        eye,
        m_camera_ptr->get_front(),
        *m_input_ptr,
        *m_player_ptr,
        *m_chunk_manager_ptr,
        *m_renderer_ptr,
        *m_inventory_ptr,
        entity_distance
    ); }

    // Survival-compatible manual portal activation (commands remain debug shortcuts).
    // The interaction code ignores items as placeable blocks, so these clicks do not
    // interfere with ordinary block placing or portal raycast physics.
    const Block_Types portal_item = m_inventory_ptr->get_selected_block();
    if (m_input_ptr->is_mouse_button_pressed(1) && m_player_ptr->is_alive()) {
        if (portal_item == Block_Types::Lighter || portal_item == Block_Types::Flint_And_Steel) {
            const Raycast_Hit hit = Raycast::cast(eye, m_camera_ptr->get_front(), 6.0f,
                                                 *m_chunk_manager_ptr);
            const bool portal_lit = hit.hit && hit.block_type == Block_Types::Obsidian &&
                                    ignite_nether_portal();
            bool fire_lit = false;
            if (!portal_lit && portal_item == Block_Types::Lighter && hit.hit) {
                fire_lit = m_fire.ignite(glm::ivec3(hit.place_x, hit.place_y, hit.place_z),
                                         *m_chunk_manager_ptr);
            }
            if ((portal_lit || fire_lit) && m_game_mode == Game_Mode::Survival)
                m_inventory_ptr->damage_selected_tool(1);
        } else if (portal_item == Block_Types::Eye_Of_Ender) {
            const Raycast_Hit hit = Raycast::cast(eye, m_camera_ptr->get_front(), 6.0f,
                                                 *m_chunk_manager_ptr);
            if (hit.hit && hit.block_type == Block_Types::End_Portal_Frame &&
                insert_ender_eye(glm::ivec3(hit.block_x, hit.block_y, hit.block_z))) {
                if (m_game_mode == Game_Mode::Survival) m_inventory_ptr->consume_selected(1);
            }
        }
    }

    // Spawn eggs work on the targeted ground/adjacent block, without placing an item block.
    const Block_Types held_egg = m_inventory_ptr->get_selected_block();
    const bool holding_egg = Mobs_Update::is_spawn_egg(held_egg);
    if (holding_egg && m_input_ptr->is_mouse_button_pressed(1) && m_player_ptr->is_alive()) {
        const glm::vec3 front = m_camera_ptr->get_front();
        const Raycast_Hit hit = Raycast::cast(eye, front, 6.0f, *m_chunk_manager_ptr);
        const glm::vec3 location = hit.hit
            ? glm::vec3(static_cast<float>(hit.place_x) + .5f, static_cast<float>(hit.place_y),
                        static_cast<float>(hit.place_z) + .5f)
            : eye + front * 3.0f;
        const glm::ivec3 cell = glm::ivec3(glm::floor(location));
        const bool space_clear = !get_block_props(m_chunk_manager_ptr->get_block_world(cell.x, cell.y, cell.z)).is_solid &&
            !get_block_props(m_chunk_manager_ptr->get_block_world(cell.x, cell.y + 1, cell.z)).is_solid;
        if (space_clear && m_mobs.spawn_from_egg(held_egg, location, *m_chunk_manager_ptr)) {
            if (m_game_mode == Game_Mode::Survival) m_inventory_ptr->consume_selected(1);
            push_chat_message("Spawned: " + get_block_props(held_egg).name);
        }
    }

    handle_tool_impulse(delta_time);

    // Взаимодействие с животными: кормление, посадка, удар. Логика — в Mobs_Update::interact. /
    // Зелье-шарик в руке: ПКМ бросает его вперёд (не ставит блок и не кормит моба). В Survival
    // шарик расходуется, в Creative — нет.
    const Item_Stack held_potion = m_inventory_ptr->get_selected_stack();
    const bool holding_potion = is_potion(held_potion.type);
    if (holding_potion && m_input_ptr->is_mouse_button_pressed(1) && m_player_ptr->is_alive()) {
        const bool has_ball = m_game_mode != Game_Mode::Survival || m_inventory_ptr->consume_selected(1);
        if (has_ball) {
            const glm::vec3 front = m_camera_ptr->get_front();
            m_dropped_items_ptr->spawn_thrown(Item_Stack{held_potion.type, 1}, eye + front * 0.6f,
                                              front * Config::potion_throw_speed + glm::vec3(0.0f, 2.0f, 0.0f));
        }
    }

    Mob_Interact_Input mob_input;
    mob_input.delta_time = delta_time;
    mob_input.eye = eye;
    mob_input.look_dir = m_camera_ptr->get_front();
    mob_input.block_targets_entity = m_block_interaction_ptr->is_entity_targeted();
    mob_input.right_click = m_input_ptr->is_mouse_button_pressed(1) && !holding_potion && !holding_egg;
    mob_input.left_click = m_input_ptr->is_mouse_button_pressed(0);
    mob_input.survival = m_game_mode == Game_Mode::Survival;
    mob_input.held_block = m_inventory_ptr->get_selected_block();
    mob_input.held_stack = m_inventory_ptr->get_selected_stack();
    mob_input.player = mob_view();
    mob_input.chat = [this](const std::string& text) { push_chat_message(text); };
    if(village_target && mob_input.block_targets_entity) {
        std::string feedback;
        const bool used=m_villages.interact(mob_input.right_click,mob_input.left_click,
            mob_input.player.position,mob_input.held_stack,mob_input.survival,feedback);
        if(used) {
            m_block_interaction_ptr->cancel_actions();
            if(!feedback.empty())push_chat_message(feedback);
        }
    } else apply_mob_result(m_mobs.interact(mob_input,*m_dropped_items_ptr));

    // Сломанные блоки -> выпавшие предметы (содержимое контейнеров снято ещё до удаления блока).
    for (const Broken_Block_Event& event : m_block_interaction_ptr->take_broken_blocks()) {
        if (Crop_Manager::is_crop_block(event.type)) {
            if (m_crops_ptr) m_crops_ptr->remove(event.position);
            if (event.allow_drops) handle_crop_harvest(event.position, event.type);
            continue;
        }
        if (Sapling_Manager::is_sapling(event.type) && m_saplings_ptr) {
            m_saplings_ptr->remove(event.position);
        }
        if (event.allow_drops && Game_Leaf_Sapling(event.type) != Block_Types::Air) {
            handle_leaf_drops(event.position, event.type);
        }
        // Creative удаляет блоки без предметов; Survival учитывает пригодность инструмента
        // и стандартное содержимое контейнеров.
        if (event.allow_drops) {
            m_dropped_items_ptr->spawn_block_drops(event.position,
                                                   event.harvested ? event.type : Block_Types::Air,
                                                   event.container_contents);
            // Fortune increases mineral yield without duplicating the base drop.
            if (event.harvested && event.fortune_level>0 &&
                (event.type==Block_Types::Lapis_Ore || event.type==Block_Types::Coal_Ore ||
                 event.type==Block_Types::Diamond_Ore)) {
                static std::mt19937 fortune_rng(0xD1A0u);
                const int extra=std::uniform_int_distribution<int>(0,event.fortune_level)(fortune_rng);
                if (extra>0) {
                    const Block_Properties& ore=get_block_props(event.type);
                    if (ore.drop_id>0) m_dropped_items_ptr->spawn_from_mob(
                        glm::vec3(event.position)+glm::vec3(0.5f,0.0f,0.5f),
                        Item_Stack{static_cast<Block_Types>(ore.drop_id),extra});
                }
            }
            if (event.harvested && (event.type==Block_Types::Lapis_Ore ||
                event.type==Block_Types::Coal_Ore || event.type==Block_Types::Diamond_Ore)) {
                m_dropped_items_ptr->spawn_experience(glm::vec3(event.position)+glm::vec3(0.5f,0.0f,0.5f),
                    event.type==Block_Types::Diamond_Ore ? 5 : 2);
            }
        }
        // Красная логика: узлы без опоры удаляются в обоих режимах, но в Creative не дропаются.
        auto logic_world = make_chunk_world(*m_chunk_manager_ptr);
        for (const glm::ivec3& node : Logic_Manager::nodes_without_support(logic_world, event.position)) {
            const Block_Types type = logic_world.type(node.x, node.y, node.z);
            set_block_at(node.x, node.y, node.z, Block_Types::Air);
            invalidate_block_mesh(node.x, node.y, node.z, *m_chunk_manager_ptr, *m_renderer_ptr);
            if (event.allow_drops) {
                m_dropped_items_ptr->spawn_block_drops(node, Logic_Manager::item_of(type), {});
            }
        }
    }

    // Красная логика: ПКМ по рычагу/кнопке/повторителю и только что поставленный узел.
    if (auto touched = m_block_interaction_ptr->take_redstone_interact()) {
        auto logic_world = make_chunk_world(*m_chunk_manager_ptr);
        m_logic.interact(logic_world, *touched);
    }
    if (auto placed = m_block_interaction_ptr->take_redstone_placed()) {
        m_logic.track(*placed);
    }

    if (m_block_interaction_ptr->has_container_open_request()) {
        const glm::ivec3 pos = m_block_interaction_ptr->consume_container_open_request();
        if (Block_Entity* entity = m_chunk_manager_ptr->get_block_entity(pos.x, pos.y, pos.z)) {
            m_block_interaction_ptr->cancel_actions();
            m_container_session.open(*entity);
            m_open_container_pos = pos;
            m_open_container_paired = m_chunk_manager_ptr->get_paired_block_entity(pos.x, pos.y, pos.z);
        }
    }
}

// Инструмент с "impulse_speed" в blocks.json: ПКМ разгоняет игрока в сторону взгляда и тратит прочность.
void Game::handle_tool_impulse(float delta_time) {
    m_tool_impulse_cooldown = std::max(0.0f, m_tool_impulse_cooldown - delta_time);
    if (!m_input_ptr->is_mouse_button_pressed(1)) return;   // edge: только момент нажатия
    if (m_game_mode != Game_Mode::Survival || !m_player_ptr->is_alive() || m_player_ptr->is_riding()) return;
    if (m_tool_impulse_cooldown > 0.0f) return;

    const Item_Stack held = m_inventory_ptr->get_selected_stack();
    if (held.is_empty()) return;
    const Block_Properties& props = get_block_props(held.type);
    // В жидкости берём impulse_speed_water, если он задан (копьё слабо тянет в воде, трезубец сильно).
    const float speed = (m_player_ptr->is_in_liquid() && props.impulse_speed_water >= 0.0f)
        ? props.impulse_speed_water : props.impulse_speed;
    if (speed <= 0.0f) return;

    m_tool_impulse_cooldown = props.impulse_cooldown >= 0.0f ? props.impulse_cooldown : Config::tool_impulse_cooldown;
    m_player_ptr->apply_impulse(m_camera_ptr->get_front(), speed);
    m_player_ptr->add_exhaustion(Config::exhaustion_attack);
    m_inventory_ptr->damage_selected_tool(1);
}

void Game::render_gameplay(bool show_gameplay_hud) {
    OC_ZONE("Game::render_gameplay");
    if (m_window_ptr->get_width() <= 0 || m_window_ptr->get_height() <= 0) {
        return;
    }
    // Если включено сглаживание или зерно, мир рисуется в промежуточный буфер; до хотбара (он — поверх
    // постобработки) буфер сворачивается в кадр.
    const bool vulkan = m_window_ptr->is_vulkan();
    const bool scene_is_post_processed = !vulkan && m_renderer_ptr->begin_scene(m_window_ptr->get_width(),
                                                                     m_window_ptr->get_height());

    glm::vec3 sky_color = m_weather.apply_to_sky(m_day_night_cycle.get_sky_color());
    const std::string& active_dimension = m_world_settings.dimension;
    if (active_dimension == "nether") sky_color = glm::vec3(0.115f, 0.033f, 0.034f);
    else if (active_dimension == "end") sky_color = glm::vec3(0.044f, 0.033f, 0.082f);
    if (!vulkan) m_renderer_ptr->clear_screen(sky_color.r, sky_color.g, sky_color.b);

    const float aspect_ratio = static_cast<float>(m_window_ptr->get_width()) /
                               static_cast<float>(m_window_ptr->get_height());

    const auto view = m_camera_ptr->get_view_matrix();
    const auto projection = m_camera_ptr->get_projection_matrix(aspect_ratio);
    const glm::vec3 camera_position = m_camera_ptr->get_position();
    std::vector<Renderable_Chunk> gl_renderable;
    if (!vulkan) {
        const Frustum frustum(projection * view);
        gl_renderable = m_chunk_manager_ptr->get_renderable_chunks(
            &frustum, &camera_position, m_renderer_ptr->get_flora_render_distance(), m_spectator);
    }
    const auto& renderable = vulkan ? m_chunk_manager_ptr->get_vulkan_resident_chunks() : gl_renderable;
    // Real Game -> Vulkan chunk stream. No duplicate frustum query or CPU meshing.
    if (m_vulkan_chunk_feed.active()) {
        m_vulkan_chunk_feed.setPriorityCenter({
            static_cast<int32_t>(std::floor(double(camera_position.x) / Config::chunk_size)),
            static_cast<int32_t>(std::floor(double(camera_position.y) / Config::chunk_height)),
            static_cast<int32_t>(std::floor(double(camera_position.z) / Config::chunk_size))});
        size_t packets=4;
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
        if(m_vulkan_game_mirror && m_vulkan_game_mirror->pendingUploads()>=32) packets=0;
#endif
        m_vulkan_chunk_feed.acceptFrame(renderable, *m_chunk_manager_ptr,0,packets);
    }
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if (m_vulkan_game_mirror) {
        const auto heldBlock=m_inventory_ptr->get_selected_block();
        const auto emission=Block_Registry::get_instance().get_properties(heldBlock).light_emission;
        std::array<float,4> heldLight{1.f,0.74f,0.43f,float(std::clamp(emission,0,15))};
        if(heldBlock==Block_Types::Sun_Stone)heldLight={1.f,0.91f,0.70f,heldLight[3]};
        m_vulkan_game_mirror->setWorld(projection * view,sky_color,camera_position,scene_ambient(),
            std::min(std::max(96.0f,float(m_render_distance * Config::chunk_size)),Config::camera_far*0.90f),
            active_dimension=="overworld"?m_day_night_cycle.get_sun_direction():glm::vec3(0.0f),heldLight);
    }
#endif
    if (!vulkan) {

    if (active_dimension != "nether" && active_dimension != "end") m_renderer_ptr->draw_sky(
        camera_position,
        m_camera_ptr->get_right(),
        m_camera_ptr->get_up(),
        view,
        projection,
        m_day_night_cycle.get_sun_direction(),
        m_day_night_cycle.get_moon_direction(),
        m_day_night_cycle.get_moon_illumination(),
        m_day_night_cycle.get_sun_darkness(),
        m_day_night_cycle.get_moon_darkness()
    );

    // Мобы рисуются между непрозрачным миром и водой (см. before_liquid в Renderer::render_chunks),
    // чтобы вода закрывала их, когда они под поверхностью.
    const auto draw_mobs = [this, &view, &projection]() {
        if (Shader* mob_shader = m_renderer_ptr->get_mob_shader()) {
            m_mobs.render(*mob_shader, view, projection, scene_ambient());
            if(m_world_settings.dimension=="overworld")
                m_villages.render(*mob_shader,view,projection,scene_ambient());
            // Тело игрока: видно при взгляде вниз (голова скрыта, см. Player_Model).
            if (m_player_ptr->is_alive()) {
                m_player_model.render_world(*mob_shader, view, projection, scene_ambient());
            }
        }
    };
    m_renderer_ptr->render_chunks(renderable, view, projection,
                                  scene_ambient(), m_spectator, draw_mobs,
                                  camera_position, sky_color,
                                  m_day_night_cycle.get_sun_direction(), true);

    // Выпавшие предметы: одна динамическая геометрия на все, тем же атласом, что и блоки.
    m_dropped_items_mesh.clear();
    m_dropped_items_ptr->build_mesh(m_dropped_items_mesh, *m_chunk_manager_ptr);
    m_renderer_ptr->render_dropped_items(m_dropped_items_mesh, view, projection,
                                         scene_ambient());

    } // legacy GL world, entities and dropped-item passes
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if (m_vulkan_game_mirror) {
        m_dropped_items_mesh.clear();
        m_dropped_items_ptr->build_mesh(m_dropped_items_mesh, *m_chunk_manager_ptr);
        m_vulkan_game_mirror->submitItems(m_dropped_items_mesh);
        if (Shader* mob_shader = m_renderer_ptr->get_mob_shader()) {
            const Mob::EntitySink sink=[this](OptiCraft::Vulkan::EntityDraw&& draw){m_vulkan_game_mirror->submitEntity(std::move(draw));};
            const Mob::ScopedEntitySink capture(sink);
            m_mobs.render(*mob_shader,view,projection,scene_ambient());
            if(active_dimension=="overworld")m_villages.render(*mob_shader,view,projection,scene_ambient());
            if(m_player_ptr->is_alive())m_player_model.render_world(*mob_shader,view,projection,scene_ambient());
        }
    }
#endif

    // Подводный фильтр рисуется ДО виньетки, чтобы затемнение краёв ложилось
    // поверх синевы, а не наоборот.
    if (is_camera_underwater()) {
        m_hud_ptr->draw_underwater_overlay(m_window_ptr->get_width(), m_window_ptr->get_height());
    }

    m_hud_ptr->draw_vignette(m_window_ptr->get_width(), m_window_ptr->get_height());

    if (show_gameplay_hud && m_block_interaction_ptr->has_target()) {
        const auto& hit = m_block_interaction_ptr->get_current_hit();
        if (hit.has_box) {
            m_hud_ptr->draw_box_highlight(hit.box_lo, hit.box_hi, view, projection);
        } else {
            m_hud_ptr->draw_block_highlight(
                glm::ivec3(hit.block_x, hit.block_y, hit.block_z),
                view,
                projection
            );
        }
    }

    if (scene_is_post_processed) m_renderer_ptr->end_scene_and_post();

    // Хотбар всегда на экране во время игры (как в Minecraft — виден и на паузе/инвентаре),
    // а не только когда show_gameplay_hud (это относится к прицелу/подсветке блока).
    {
        std::vector<Hud_Inventory_Slot> hotbar;
        for (size_t i = 0; i < m_inventory_ptr->get_hotbar_slot_count(); ++i) {
            hotbar.push_back(make_hud_slot(m_inventory_ptr->get_hotbar_slot(i)));
        }
        m_hud_ptr->draw_hotbar(
            m_window_ptr->get_width(),
            m_window_ptr->get_height(),
            m_renderer_ptr->get_texture_atlas(),
            hotbar,
            m_inventory_ptr->get_selected_index(),
            m_game_mode == Game_Mode::Survival
        );
    }

    if (show_gameplay_hud) {
        const auto boss_bars = m_mobs.boss_bars(m_player_ptr->get_position());
        for (size_t i=0; i<boss_bars.size(); ++i) {
            const auto& bar=boss_bars[i];
            m_hud_ptr->draw_boss_bar(m_window_ptr->get_width(),m_window_ptr->get_height(),
                                     bar.name,bar.health,bar.max_health,bar.state,static_cast<int>(i));
        }
        m_hud_ptr->draw_crosshair(m_window_ptr->get_width(), m_window_ptr->get_height(),
                                  m_block_interaction_ptr->has_target());
        // Сердца и ножки — только в Survival (в Creative игрок неуязвим и не голодает).
        if (m_game_mode == Game_Mode::Survival) {
            m_hud_ptr->draw_experience(m_window_ptr->get_width(),m_window_ptr->get_height(),
                m_inventory_ptr->xp_level(),m_inventory_ptr->xp_progress());
            m_hud_ptr->draw_player_status(
                m_window_ptr->get_width(),
                m_window_ptr->get_height(),
                m_player_ptr->get_health(),
                m_player_ptr->get_max_health(),
                m_player_ptr->get_food_level(),
                m_player_ptr->get_max_food(),
                m_player_ptr->get_air(),
                m_player_ptr->get_max_air()
            );
            // Полоска под прицелом: прогресс ломания блока (жёлтая) или поедания (зелёная).
            const float eat_progress = m_block_interaction_ptr->get_eat_progress();
            const float break_progress = m_block_interaction_ptr->get_break_progress();
            if (eat_progress > 0.0f) {
                m_hud_ptr->draw_action_progress(m_window_ptr->get_width(), m_window_ptr->get_height(),
                                                eat_progress, glm::vec4(0.45f, 0.85f, 0.3f, 0.95f));
            } else if (break_progress > 0.0f) {
                m_hud_ptr->draw_action_progress(m_window_ptr->get_width(), m_window_ptr->get_height(),
                                                break_progress, glm::vec4(1.0f, 0.85f, 0.2f, 0.95f));
            }
        }
    }

    // Чат рисуется поверх хотбара/полосок; лог виден и на паузе (сообщения не пропадают
    // молча), а строка ввода — только пока чат открыт.
    m_hud_ptr->draw_chat_overlay(
        m_window_ptr->get_width(),
        m_window_ptr->get_height(),
        m_chat_log,
        m_chat_input,
        m_chat_open,
        std::fmod(static_cast<float>(glfwGetTime()), 1.0f) < 0.5f
    );

    if (m_show_debug_hud) {
        Hud_Debug_Stats stats{};
        stats.fps = m_current_fps;
        stats.frame_time_ms = m_last_raw_frame_ms;
        stats.player_position = m_player_ptr->get_position();
        stats.seed = m_world_settings.seed;
        stats.generation_folder = m_generation_folder;
        stats.chunks_loaded = m_chunk_manager_ptr->get_loaded_count();
        stats.process_rss_mib = process_rss_mib();
        stats.chunks_rendered = m_renderer_ptr->get_last_rendered_chunk_count();
        stats.triangles_rendered = m_renderer_ptr->get_last_rendered_triangle_count();
        stats.sections_rendered = m_renderer_ptr->get_last_rendered_section_count();
        stats.sections_culled = m_renderer_ptr->get_last_culled_section_count();
        stats.sections_occluded = m_renderer_ptr->get_last_occluded_section_count();
        stats.is_flying = m_player_ptr->is_flying();
        stats.day_night_normalized_time = m_day_night_cycle.get_normalized_time();
        stats.biome_name = m_world_generator->get_biome_name(
            static_cast<int>(std::floor(stats.player_position.x)),
            static_cast<int>(std::floor(stats.player_position.y)),
            static_cast<int>(std::floor(stats.player_position.z))
        );
        stats.effects_line = describe_effects(m_player_ptr->effects());

        if (m_show_debug_extended) {
            stats.extended = true;
            stats.frame_min_ms = m_last_frame_min_ms;
            stats.frame_avg_ms = m_last_frame_avg_ms;
            stats.frame_max_ms = m_last_frame_max_ms;
            stats.update_cpu_ms = m_last_update_cpu_ms;
            stats.render_cpu_ms = m_last_render_cpu_ms;
            stats.render_gpu_ms = m_renderer_ptr->get_last_gpu_frame_ms();
            stats.tps = m_current_tps;
            const Mobs_Update::Mob_Debug_Counts counts = m_mobs.debug_counts();
            stats.hostile_count = counts.hostile;
            stats.passive_count = counts.passive;
            const Chunk_Manager::Chunk_Work_Stats work = m_chunk_manager_ptr->get_work_stats();
            stats.chunk_mesh_pending = work.mesh_pending;
            stats.chunk_mesh_requeued = work.mesh_requeued;
            stats.chunk_loads_requested = work.loads_requested;
            stats.chunk_generation_jobs_queued = work.generation_jobs_queued;
            stats.chunk_mesh_jobs_queued = work.mesh_jobs_queued;
            stats.chunk_columns_backlogged = work.columns_backlogged;
            stats.target_lines = m_mobs.debug_target_lines();
        }

        m_hud_ptr->draw_debug_overlay(m_window_ptr->get_width(), m_window_ptr->get_height(), stats);
    }

    static int frame_counter = 0;
    if (++frame_counter % 60 == 0) {
        log_stats();
    }
}

// ============================================================================
//  Изометрический снимок мира (F7)
// ============================================================================
bool Game::start_iso_capture() {
    if (m_window_ptr && m_window_ptr->is_vulkan()) return false; // capture pipeline is GL-only
    if (!m_chunk_manager_ptr || !m_player_ptr || !m_renderer_ptr) return false;
    if (m_iso_capture_ptr) {
        // Предыдущий PNG ещё пишется в фоне — второй снимок параллельно не начинаем.
        if (!m_iso_capture_ptr->is_finished()) {
            push_chat_message(tr("iso.busy"), true);
            return false;
        }
        poll_iso_capture_result();
    }

    int radius = Config::iso_capture_radius_chunks;
    // Отладочный хук: OPTICRAFT_ISO_RADIUS=N — меньший радиус для быстрых проверок.
    if (const char* override_radius = std::getenv("OPTICRAFT_ISO_RADIUS")) {
        const int value = std::atoi(override_radius);
        if (value > 0) radius = value;
    }

    m_iso_capture_ptr = std::make_unique<Isometric_Capture>(*m_chunk_manager_ptr, *m_renderer_ptr);
    if (!m_iso_capture_ptr->start(m_player_ptr->get_position(), radius,
                                  m_day_night_cycle.get_sky_color(),
                                  m_day_night_cycle.get_ambient_intensity())) {
        m_iso_capture_ptr.reset();
        return false;
    }
    return true;
}

void Game::update_iso_capture() {
    if (m_iso_capture_ptr) m_iso_capture_ptr->update();
}

void Game::cancel_iso_capture() {
    if (m_iso_capture_ptr) m_iso_capture_ptr->cancel();
}

void Game::render_iso_capture_overlay() {
    if (!m_iso_capture_ptr) {
        clear_menu_frame();
        return;
    }
    const Isometric_Capture& capture = *m_iso_capture_ptr;
    std::string status;
    switch (capture.get_stage()) {
        case Isometric_Capture::Stage::Planning:   status = tr("iso.stage.planning"); break;
        case Isometric_Capture::Stage::Generating:
            status = tr("iso.stage.generating",
                        {std::to_string(capture.get_chunks_loaded()), std::to_string(capture.get_chunks_total())});
            break;
        case Isometric_Capture::Stage::Lighting:   status = tr("iso.stage.lighting"); break;
        case Isometric_Capture::Stage::Meshing:    status = tr("iso.stage.meshing"); break;
        case Isometric_Capture::Stage::Rendering:
            status = tr("iso.stage.rendering",
                        {std::to_string(capture.get_tiles_done()), std::to_string(capture.get_tiles_total())});
            break;
        case Isometric_Capture::Stage::Saving:
        case Isometric_Capture::Stage::Finished:   status = tr("iso.stage.saving"); break;
    }
    render_loading_screen(tr("iso.title"), status, capture.get_progress());
}

void Game::poll_iso_capture_result() {
    if (!m_iso_capture_ptr || !m_iso_capture_ptr->is_finished()) return;
    if (m_iso_capture_ptr->has_succeeded()) {
        push_chat_message(tr("iso.saved", {m_iso_capture_ptr->get_output_path()}));
    } else if (!m_iso_capture_ptr->was_cancelled()) {
        const std::string& error = m_iso_capture_ptr->get_error();
        push_chat_message(tr("iso.failed", {error.empty() ? m_iso_capture_ptr->get_output_path() : error}), true);
    }
    m_iso_capture_ptr.reset();
    // Отладочный хук (см. State_Playing::update): после автотеста снимка выходим из игры.
    if (std::getenv("OPTICRAFT_ISO_AUTOTEST")) request_exit();
}

void Game::log_stats() {
    std::cout << "\n=== Stats ===\n";
    std::cout << "Chunks loaded: " << m_chunk_manager_ptr->get_loaded_count() << "\n";
    std::cout << "Chunks rendered: " << m_renderer_ptr->get_last_rendered_chunk_count() << "\n";
    std::cout << "Player pos: "
              << static_cast<int>(m_player_ptr->get_position().x) << ", "
              << static_cast<int>(m_player_ptr->get_position().y) << ", "
              << static_cast<int>(m_player_ptr->get_position().z) << "\n";
    std::cout << "=============\n\n";
}

void Game::switch_to_opengl() {
#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    if (!m_vulkan_game_mirror) return;
    if(m_hud_ptr)m_hud_ptr->set_ui_sink({});
    set_vulkan_chunk_sink({},{});
    if(m_chunk_manager_ptr)m_chunk_manager_ptr->set_vulkan_worldgen(nullptr);
    m_vulkan_game_mirror.reset();
    m_window_ptr->select_vulkan(false);
    if (m_chunk_manager_ptr) m_chunk_manager_ptr->set_vulkan_primary(false);
    m_input_ptr->rebind_window(m_window_ptr->get_glfw_ptr());
#endif
}

void Game::shutdown() {
    if (m_is_shutdown) {
        return;
    }
    m_is_shutdown = true;

    LOG_INFO("Shutting down...");
    m_is_running = false;
    save_world();
    if (m_input_ptr) save_options();

#if defined(OPTICRAFT_VULKAN_GAME_MIRROR)
    // Detach callbacks BEFORE releasing GPU buffers or the window.
    if (m_hud_ptr) m_hud_ptr->set_ui_sink({});
    set_vulkan_chunk_sink({}, {});
    if(m_chunk_manager_ptr)m_chunk_manager_ptr->set_vulkan_worldgen(nullptr);
    m_vulkan_game_mirror.reset();
#endif
    m_application_ptr.reset();
        m_villages.clear();
        m_mobs.shutdown();  // мобы держат GL-ресурсы: очищаются до GL-контекста (см. Mobs_Update.h)
    m_hud_ptr.reset();
    m_block_interaction_ptr.reset();
    m_chunk_manager_ptr.reset();
    m_renderer_ptr.reset();
    m_world_generator.reset();
    Atlas_Registry::get_instance().clear();
    m_player_ptr.reset();
    m_camera_ptr.reset();
    m_input_ptr.reset();
    m_window_ptr.reset();
    glfwTerminate();

    LOG_INFO("OptiCraft exited");
}

bool Game::is_key_pressed(int key) const {
    return m_input_ptr->is_key_pressed(key);
}

bool Game::is_mouse_button_pressed(int button) const {
    return m_input_ptr->is_mouse_button_pressed(button);
}

bool Game::is_mouse_button_held(int button) const {
    return m_input_ptr->is_mouse_button_held(button);
}

void Game::request_exit() {
    m_is_running = false;
}

void Game::toggle_generation_folder() {
    m_generation_folder = (m_generation_folder == "Classic") ? "Islands" : "Classic";
    m_input_ptr->reset_mouse_delta();
    reload_world(m_generation_folder);
    m_world_settings.generation_folder = m_generation_folder;
    m_world_file.save(m_world_settings);
}

std::string Game::get_game_mode_name() const {
    if (m_spectator) return "SPECTATOR";
    return m_game_mode == Game_Mode::Survival ? "SURVIVAL" : "CREATIVE";
}

void Game::apply_game_mode(Game_Mode mode, bool spectator) {
    const bool survival = mode == Game_Mode::Survival;
    m_game_mode = mode;
    m_spectator = !survival && spectator;   // наблюдатель — это Creative с полётом сквозь блоки
    m_inventory_ptr->set_mode(m_game_mode);
    m_mobs.set_hostiles_enabled(true);
    m_player_ptr->set_fall_damage_enabled(survival);
    m_player_ptr->set_hunger_enabled(survival);
    // Творческий режим: ходим по земле, полёт — двойным прыжком (или /fly). Наблюдатель летает всегда, сквозь блоки.
    m_player_ptr->set_flying(m_spectator);
    m_player_ptr->set_double_jump_flight(!survival && !m_spectator);
    m_player_ptr->set_noclip(m_spectator);
    m_block_interaction_ptr->cancel_actions();
    if (survival) {
        // Чтобы не словить "долг" урона за падение, случившееся ещё в Creative/полёте.
        m_player_ptr->reset_fall_tracking();
    }
}

void Game::toggle_game_mode() {
    apply_game_mode(m_game_mode == Game_Mode::Creative ? Game_Mode::Survival : Game_Mode::Creative, false);
}

void Game::cycle_game_mode() {
    // F1: Survival → Creative (полёт с коллизиями) → наблюдатель (полёт без коллизий) → Survival.
    if (m_game_mode == Game_Mode::Survival) {
        apply_game_mode(Game_Mode::Creative, false);
    } else if (!m_spectator) {
        apply_game_mode(Game_Mode::Creative, true);
    } else {
        apply_game_mode(Game_Mode::Survival, false);
    }
}

// ============================================================================
//  Чат
// ============================================================================

void Game::open_chat(bool with_slash) {
    m_chat_open = true;
    m_chat_input = with_slash ? "/" : "";
    m_chat_history_index = -1;
    m_chat_history_draft.clear();
    m_backspace_timer = 0.0f;
    // Игрок стоит на паузе вместе со всем миром (см. State_Playing::update), но последний
    // ввод движения мог остаться "зажатым" — гасим, чтобы после закрытия чата он не побежал.
    m_player_ptr->set_movement_input({0.0f, 0.0f, false, false},
                                     m_camera_ptr->get_front(), m_camera_ptr->get_right());
    set_gameplay_input_active(false);
}

void Game::close_chat() {
    if (!m_chat_open) return;
    m_chat_open = false;
    m_chat_input.clear();
    m_chat_history_index = -1;
    set_gameplay_input_active(true);
}

void Game::push_chat_message(const std::string& text, bool is_error) {
    // Многострочные ответы (например /help) режем на отдельные строки лога.
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) {
            m_chat_log.push_back({text.substr(start, end - start), is_error, k_chat_line_seconds});
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    if (m_chat_log.size() > k_chat_log_capacity) {
        m_chat_log.erase(m_chat_log.begin(),
                         m_chat_log.begin() + static_cast<std::ptrdiff_t>(m_chat_log.size() - k_chat_log_capacity));
    }
}

void Game::update_chat(float delta_time) {
    for (Hud_Chat_Line& line : m_chat_log) {
        line.time_left = std::max(0.0f, line.time_left - delta_time);
    }
    if (!m_chat_open) return;

    // Backspace: сразу по нажатию, потом автоповтор при удержании.
    if (m_input_ptr->is_key_pressed(GLFW_KEY_BACKSPACE)) {
        Utf8::pop_back(m_chat_input);
        m_backspace_timer = 0.4f;
    } else if (m_input_ptr->is_key_held(GLFW_KEY_BACKSPACE)) {
        m_backspace_timer -= delta_time;
        if (m_backspace_timer <= 0.0f) {
            Utf8::pop_back(m_chat_input);
            m_backspace_timer = 0.04f;
        }
    }
}

void Game::submit_chat_line() {
    const std::string line = m_chat_input;
    close_chat();
    if (line.empty()) return;

    if (m_chat_history.empty() || m_chat_history.back() != line) {
        m_chat_history.push_back(line);
        if (m_chat_history.size() > 50) m_chat_history.erase(m_chat_history.begin());
    }

    if (line[0] == '/') {
        run_command(line.substr(1));
    } else {
        push_chat_message("<You> " + line);
    }
}

void Game::chat_history_prev() {
    if (m_chat_history.empty()) return;
    if (m_chat_history_index < 0) {
        m_chat_history_draft = m_chat_input;
        m_chat_history_index = static_cast<int>(m_chat_history.size()) - 1;
    } else if (m_chat_history_index > 0) {
        --m_chat_history_index;
    }
    m_chat_input = m_chat_history[static_cast<size_t>(m_chat_history_index)];
}

void Game::chat_history_next() {
    if (m_chat_history_index < 0) return;
    if (++m_chat_history_index >= static_cast<int>(m_chat_history.size())) {
        m_chat_history_index = -1;
        m_chat_input = m_chat_history_draft;
    } else {
        m_chat_input = m_chat_history[static_cast<size_t>(m_chat_history_index)];
    }
}

void Game::chat_autocomplete() {
    const std::vector<std::string> options = get_command_completions(m_chat_input);
    if (options.empty()) return;

    if (options.size() == 1) {
        m_chat_input = options[0] + " ";
        return;
    }

    // Несколько вариантов: дописываем общий префикс, а сами варианты показываем в чате.
    std::string common = options[0];
    for (const std::string& option : options) {
        size_t n = 0;
        while (n < common.size() && n < option.size() && common[n] == option[n]) ++n;
        common.resize(n);
    }
    if (common.size() > m_chat_input.size()) m_chat_input = common;

    const size_t last_space = options[0].find_last_of(' ');
    std::string list;
    for (const std::string& option : options) {
        if (!list.empty()) list += "  ";
        list += option.substr(last_space == std::string::npos ? 0 : last_space + 1);
    }
    push_chat_message(list);
}

void Game::run_command(const std::string& line) {
    std::string reply;
    const bool ok = execute_command(*this, line, reply);
    if (!reply.empty()) push_chat_message(reply, !ok);
    LOG_INFO("[command] /" + line + " -> " + (ok ? "ok: " : "FAILED: ") + reply);
}

// ============================================================================
//  API команд
// ============================================================================

void Game::teleport_player(const glm::vec3& pos) {
    m_player_ptr->set_position(pos);
    m_player_ptr->reset_velocity();
    m_player_ptr->reset_fall_tracking(); // иначе телепорт вниз засчитался бы как падение
    m_camera_ptr->set_position(pos + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f));
    // Сразу двигаем окно подгрузки чанков — иначе после дальнего телепорта мир пришлось бы ждать.
    m_chunk_manager_ptr->update_player_position(pos.x, pos.y, pos.z);
    m_block_interaction_ptr->cancel_actions();
}

bool Game::locate_biome(const std::string& name, glm::vec3& out_pos, std::string& out_biome_name) {
    std::string target = name;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(target.begin(), target.end(), '_', ' ');
    if (target.empty()) return false;

    const glm::vec3 from = m_player_ptr->get_position();
    const bool underground = target.find("underground") != std::string::npos;

    auto lowered = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    // Проверка одной точки. Для подземных биомов ищем пещеру по глубине, для остальных —
    // берём поверхность (над морем — уровень воды, чтобы не телепортироваться на дно).
    auto probe = [&](int x, int z, bool exact, glm::vec3& pos, std::string& biome_out) -> bool {
        const int surface = m_world_generator->get_height(x, z);
        auto matches = [&](const std::string& biome) {
            const std::string l = lowered(biome);
            return exact ? (l == target) : (l.find(target) != std::string::npos);
        };
        if (!underground) {
            const int y = std::max(surface, World_Generator::sea_level());
            std::string biome = m_world_generator->get_biome_name(x, surface, z);
            if (!matches(biome)) return false;
            pos = glm::vec3(static_cast<float>(x) + 0.5f, static_cast<float>(y + 2), static_cast<float>(z) + 0.5f);
            biome_out = biome;
            return true;
        }
        for (int y = surface - 8; y > -120; y -= 6) {
            if (!m_world_generator->cave_at(x, y, z)) continue;
            std::string biome = m_world_generator->get_biome_name(x, y, z);
            if (!matches(biome)) continue;
            pos = glm::vec3(static_cast<float>(x) + 0.5f, static_cast<float>(y), static_cast<float>(z) + 0.5f);
            biome_out = biome;
            return true;
        }
        return false;
    };

    const float step = underground ? 64.0f : 48.0f;
    const int max_radius = underground ? 1200 : k_biome_search_radius;

    // Сначала точное совпадение ("mountains" не должно находить "High Mountains", если
    // обычные горы где-то есть), потом — частичное.
    for (int pass = 0; pass < 2; ++pass) {
        const bool exact = (pass == 0);

        // Свой чанк-центр тоже проверяем: игрок уже может стоять в нужном биоме.
        if (probe(static_cast<int>(std::floor(from.x)), static_cast<int>(std::floor(from.z)),
                  exact, out_pos, out_biome_name)) {
            return true;
        }
        for (float radius = step; radius <= static_cast<float>(max_radius); radius += step) {
            // Число точек на кольце пропорционально длине окружности — иначе на больших
            // радиусах между точками появлялись бы дыры шире самих биомов.
            const int points = std::max(8, static_cast<int>(6.2831853f * radius / step));
            for (int i = 0; i < points; ++i) {
                const float angle = static_cast<float>(i) / static_cast<float>(points) * 6.2831853f;
                const int x = static_cast<int>(std::floor(from.x + std::cos(angle) * radius));
                const int z = static_cast<int>(std::floor(from.z + std::sin(angle) * radius));
                if (probe(x, z, exact, out_pos, out_biome_name)) return true;
            }
        }
    }
    return false;
}

int Game::get_day_time_ticks() const {
    return static_cast<int>(std::floor(m_day_night_cycle.get_normalized_time() * 24000.0f)) % 24000;
}

void Game::set_day_time_ticks(int ticks) {
    const int wrapped = ((ticks % 24000) + 24000) % 24000;
    m_day_night_cycle.set_normalized_time(static_cast<float>(wrapped) / 24000.0f);
}

void Game::add_day_time_ticks(int ticks) {
    set_day_time_ticks(get_day_time_ticks() + ticks);
}


void Game::kill_player() {
    // Респавн в обоих режимах: в Survival так же случилось бы само после смерти, в Creative
    // игрок неуязвим и иначе команда ничего бы не делала.
    m_player_ptr->respawn(m_spawn_position);
    m_camera_ptr->set_position(m_spawn_position + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f));
    m_chunk_manager_ptr->update_player_position(m_spawn_position.x, m_spawn_position.y, m_spawn_position.z);
    m_block_interaction_ptr->cancel_actions();
}

void Game::set_game_mode(Game_Mode mode) {
    if (m_game_mode != mode || m_spectator) apply_game_mode(mode, false);
}

int Game::give_item(Block_Types type, int count) {
    return m_inventory_ptr->try_add(type, count);
}

bool Game::enchant_held_item(int level) {
    return m_inventory_ptr->enchant_selected_durability(level);
}

bool Game::build_dungeon(const glm::ivec3& origin, std::uint32_t seed) {
    const Dungeon_Layout layout = make_dungeon_layout(seed);
    bool all_placed = true;
    for (const Dungeon_Block& block : layout.blocks) {
        all_placed &= set_block_at(origin.x + block.x, origin.y + block.y, origin.z + block.z, block.type);
    }
    return all_placed;
}

void Game::detonate(const glm::ivec3& center, float radius) {
    m_block_interaction_ptr->explode(center, radius, *m_chunk_manager_ptr, *m_renderer_ptr);
    const glm::vec3 c = glm::vec3(center) + glm::vec3(0.5f);
    const float reach = radius * 2.0f;
    constexpr float k_max_damage = 14.0f;
    m_mobs.damage_in_radius(c, reach, k_max_damage);
    const float d = glm::distance(m_player_ptr->get_position(), c);
    if (d < reach) {
        m_player_ptr->take_damage(static_cast<int>(std::ceil(k_max_damage * (1.0f - d / reach))));
    }
}

std::filesystem::path Game::dimension_data_file(const char* basename) const {
    const std::string suffix = m_world_settings.dimension == "overworld" ? "" :
                                ("_" + m_world_settings.dimension);
    return m_world_file.get_path().parent_path() / (std::string(basename) + suffix + ".txt");
}

void Game::load_dimension_fire() {
    m_fire.clear();
    std::ifstream fires(dimension_data_file("fires"));
    if (fires) m_fire.load(fires);
    m_fire_damage_timer = 0.f;
}

void Game::remember_touched_nether_portal(const glm::ivec3& cell) {
    if (!m_chunk_manager_ptr) return;
    auto is_portal = [this](int x, int y, int z) {
        return m_chunk_manager_ptr->get_block_world(x, y, z) == Block_Types::Nether_Portal;
    };
    int bottom_y = cell.y;
    while (bottom_y > Config::world_min_y && is_portal(cell.x, bottom_y - 1, cell.z)) --bottom_y;
    bool along_z = is_portal(cell.x, bottom_y, cell.z + 1) ||
                   is_portal(cell.x, bottom_y, cell.z - 1);
    int inner_x = cell.x, inner_z = cell.z;
    if (along_z) {
        if (is_portal(cell.x, bottom_y, cell.z - 1)) --inner_z;
    } else if (is_portal(cell.x - 1, bottom_y, cell.z)) --inner_x;
    m_portal_links.remember({m_world_settings.dimension,
                             glm::ivec3(inner_x, bottom_y - 1, inner_z), along_z, false});
}

void Game::load_dimension_positions() {
    m_dimension_positions.clear();
    m_portal_links.clear();
    m_portal_exit_lock = false;
    m_destination_site_to_validate.reset();
    {
        std::ifstream portals(m_world_file.get_path().parent_path() / "portal_sites.txt");
        if (portals) m_portal_links.load(portals);
    }
    std::ifstream file(m_world_file.get_path().parent_path() / "dimension_positions.txt");
    std::string name;
    glm::vec3 pos{};
    while (file >> std::quoted(name) >> pos.x >> pos.y >> pos.z) {
        if (find_dimension_info(name) && std::isfinite(pos.x) && std::isfinite(pos.y) &&
            std::isfinite(pos.z)) m_dimension_positions[name] = pos;
    }
}

void Game::save_dimension_positions() const {
    const auto final = m_world_file.get_path().parent_path() / "dimension_positions.txt";
    const auto temp = final.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!file) return;
        file << std::setprecision(std::numeric_limits<float>::max_digits10);
        for (const auto& [name, pos] : m_dimension_positions)
            file << std::quoted(name) << ' ' << pos.x << ' ' << pos.y << ' ' << pos.z << '\n';
        if (!file) return;
    }
    std::error_code error;
    std::filesystem::rename(temp, final, error);
    if (error) { // Windows rename may require removing an existing destination.
        std::filesystem::remove(final, error);
        std::filesystem::rename(temp, final, error);
    }
}

bool Game::switch_dimension(const std::string& name, bool via_portal) {
    const Dimension_Info* destination = find_dimension_info(name);
    if (!destination || !m_player_ptr || !m_chunk_manager_ptr) return false;
    if (m_world_settings.dimension == name) return true;
    const glm::vec3 departure = m_player_ptr->get_position();
    const Dimension_Info& source = get_dimension_info(dimension_from_name(m_world_settings.dimension));
    const bool end_exit = via_portal && source.terrain == Terrain_Mode::End &&
                          destination->terrain != Terrain_Mode::End;
    const glm::vec3 mapped(departure.x * source.coordinate_scale / destination->coordinate_scale,
                           static_cast<float>(destination->spawn_height),
                           departure.z * source.coordinate_scale / destination->coordinate_scale);
    const Portal_Site* candidate = nullptr;
    if (via_portal && !end_exit) {
        const bool end_portal = destination->terrain == Terrain_Mode::End;
        const float seek_x = end_portal ? 8.f : mapped.x;
        const float seek_z = end_portal ? 8.f : mapped.z;
        candidate = m_portal_links.nearest(name, seek_x, seek_z, end_portal,
                                            destination->terrain == Terrain_Mode::Nether ? 32.f : 128.f);
    }
    const std::optional<Portal_Site> linked = candidate ? std::optional<Portal_Site>(*candidate) : std::nullopt;
    const auto saved = m_dimension_positions.find(name);
    const bool restore_saved = !via_portal && saved != m_dimension_positions.end();
    const glm::vec3 intended = restore_saved ? saved->second :
        linked ? Portal_Link_Registry::entry_position(*linked) :
        destination->terrain == Terrain_Mode::End ? glm::vec3(8.f, 70.f, 8.f) : mapped;

    LOG_INFO("Dimension switch " + m_world_settings.dimension + " -> " + name + ": saving source");
    save_world();
    m_world_settings.dimension = name;
    m_pending_portal_type.clear();
    m_pending_command_portal_type.clear();
    m_player_ptr->set_position(intended);
    reload_world(m_generation_folder);
    // reload_world chooses a safe generated floor for new portals. Existing
    // anchors and admin jumps instead restore their exact recorded position.
    const glm::vec3 landing = end_exit ? m_spawn_position :
        (linked || restore_saved) ? intended : m_player_ptr->get_position();
    teleport_player(landing);
    m_portal_contact_time = 0.f;
    m_portal_cooldown = 2.f;
    m_portal_exit_lock = via_portal;
    m_destination_site_to_validate = linked;
    m_dimension_landing = true;

    // Never place a second portal next to an already linked entrance.
    // Nether -> Overworld, End -> Overworld and repeat visits all reuse it.
    if (via_portal && !end_exit && !linked) {
        m_pending_portal_type = destination->terrain == Terrain_Mode::End ? "end" : "nether";
        glm::vec3 front = m_camera_ptr->get_front();
        front.y = 0.f;
        const float len = glm::length(front);
        front = len > .001f ? front / len : glm::vec3(0.f, 0.f, -1.f);
        const glm::vec3 anchor = landing + front * 6.f;
        m_pending_portal_origin = glm::ivec3(static_cast<int>(std::floor(anchor.x)),
            static_cast<int>(std::floor(landing.y)) - 1,
            static_cast<int>(std::floor(anchor.z)));
        m_pending_portal_along_z = m_pending_portal_type == "nether" &&
                                   std::abs(front.x) > std::abs(front.z);
        m_pending_portal_delay = 1.0f;
    }
    save_world(false);
    LOG_INFO("Dimension switch: completed " + name +
             (linked ? " (existing portal)" : end_exit ? " (world spawn)" : ""));
    return true;
}

bool Game::construct_portal_at(const std::string& type, const glm::ivec3& origin, bool along_z) {
    if (!m_chunk_manager_ptr || (type != "nether" && type != "end")) return false;
    const auto kind = type == "nether" ? Portal_Placement::Kind::Nether : Portal_Placement::Kind::End;
    const auto plane = along_z ? Portal_Placement::Plane::Along_Z : Portal_Placement::Plane::Along_X;
    const Portal_Placement::Position pos{origin.x, origin.y, origin.z};

    // A chunk may still be streaming. Never write the first blocks until ALL cells
    // of the portal, including the End support pad, are available and within bounds.
    bool ready = true;
    Portal_Placement::for_each_cell(kind, pos, plane, [&](const Portal_Placement::Cell& cell) {
        if (!ready) return;
        const int x = cell.pos.x, y = cell.pos.y, z = cell.pos.z;
        if (y < Config::world_min_y || y > Config::world_max_y) { ready = false; return; }
        const int cx = Math_Helpers::floor_div(x, Config::chunk_size);
        const int cy = Math_Helpers::floor_div(y, Config::chunk_height);
        const int cz = Math_Helpers::floor_div(z, Config::chunk_size);
        ready = m_chunk_manager_ptr->get_chunk(cx, cy, cz) != nullptr;
    });
    if (!ready) return false;

    // Apply the entire frame as one batch. The old per-cell path performed a
    // complete block-light BFS and queued disk writes for EACH of the 20/34
    // blocks, potentially stalling the game on the first End arrival.
    std::vector<std::pair<glm::ivec3, Block_Types>> writes;
    writes.reserve(kind == Portal_Placement::Kind::Nether ? 20 : 34);
    Portal_Placement::for_each_cell(kind, pos, plane, [&](const Portal_Placement::Cell& cell) {
        Block_Types block = Block_Types::Air;
        if (kind == Portal_Placement::Kind::Nether)
            block = cell.rim ? Block_Types::Obsidian : Block_Types::Nether_Portal;
        else if (cell.support)
            block = Block_Types::End_Stone;
        else
            block = cell.rim ? Block_Types::End_Portal_Frame_Filled : Block_Types::End_Portal;
        writes.emplace_back(glm::ivec3(cell.pos.x, cell.pos.y, cell.pos.z), block);
    });
    std::vector<glm::ivec3> changed;
    changed.reserve(writes.size());
    m_chunk_manager_ptr->set_blocks_world_batch(writes, changed);
    const bool built = changed.size() == writes.size();
    if (built) m_portal_links.remember({m_world_settings.dimension, origin, along_z,
                                       kind == Portal_Placement::Kind::End});
    return built;
}

bool Game::build_portal(const std::string& type) {
    if (!m_player_ptr || !m_camera_ptr || !m_chunk_manager_ptr ||
        (type != "nether" && type != "end")) return false;

    const glm::vec3 p = m_player_ptr->get_position();
    glm::vec3 forward = m_camera_ptr->get_front();
    forward.y = 0.f;
    const float len = glm::length(forward);
    if (len > 0.001f) forward /= len;
    else forward = glm::vec3(0.f, 0.f, -1.f);
    // Place the structure in front of the player, not invariably on +X where
    // it might be behind the camera or viewed edge-on.
    const glm::vec3 target = p + forward * 6.f;
    const glm::ivec3 at(static_cast<int>(std::floor(target.x)),
                         static_cast<int>(std::floor(p.y)) - 1,
                         static_cast<int>(std::floor(target.z)));
    const bool along_z = (type == "nether" && std::abs(forward.x) > std::abs(forward.z));
    const int bottom = at.y - (type == "end" ? 1 : 0);
    const int top = at.y + (type == "nether" ? 4 : 0);
    if (bottom < Config::world_min_y || top > Config::world_max_y) return false;

    if (construct_portal_at(type, at, along_z)) {
        m_pending_command_portal_type.clear();
        return true;
    }
    // The command should not fail simply because one neighbor is still streaming.
    // Reattempt on game ticks, after Chunk_Manager collects completed chunks.
    m_pending_command_portal_type = type;
    m_pending_command_portal_origin = at;
    m_pending_command_portal_along_z = along_z;
    return true;
}

bool Game::ignite_nether_portal() {
    if (!m_player_ptr || !m_chunk_manager_ptr) return false;
    const glm::ivec3 center = glm::ivec3(glm::floor(m_player_ptr->get_position()));
    // Search X-oriented and Z-oriented obsidian frames, only filled if completely valid.
    for (int axis = 0; axis < 2; ++axis) {
        for (int y = center.y - 4; y <= center.y + 2; ++y)
        for (int a = -5; a <= 5; ++a)
        for (int b = -5; b <= 5; ++b) {
            const glm::ivec3 start = center + glm::ivec3(axis ? b : a, y, axis ? a : b);
            auto cell = [&](int along, int up) {
                return start + glm::ivec3(axis ? 0 : along, up, axis ? along : 0);
            };
            bool valid = true;
            for (int yy = 0; yy < 5 && valid; ++yy) for (int xx = 0; xx < 4; ++xx) {
                const glm::ivec3 c = cell(xx, yy);
                const Block_Types block = m_chunk_manager_ptr->get_block_world(c.x, c.y, c.z);
                const bool frame = yy == 0 || yy == 4 || xx == 0 || xx == 3;
                if (frame ? block != Block_Types::Obsidian :
                    (block != Block_Types::Air && block != Block_Types::Nether_Portal)) {
                    valid = false; break;
                }
            }
            if (!valid) continue;
            // A missing chunk may be sampled through World_Generator, but cannot be edited.
            // Reject incomplete frames instead of announcing a successful ignition.
            for (int yy = 0; yy < 5 && valid; ++yy) for (int xx = 0; xx < 4; ++xx) {
                const glm::ivec3 c = cell(xx, yy);
                if (!m_chunk_manager_ptr->get_chunk(
                        Math_Helpers::floor_div(c.x, Config::chunk_size),
                        Math_Helpers::floor_div(c.y, Config::chunk_height),
                        Math_Helpers::floor_div(c.z, Config::chunk_size))) { valid = false; break; }
            }
            if (!valid) continue;
            bool activated = true;
            for (int yy = 1; yy <= 3; ++yy) for (int xx = 1; xx <= 2; ++xx) {
                const glm::ivec3 c = cell(xx, yy);
                activated = set_block_at(c.x, c.y, c.z, Block_Types::Nether_Portal) && activated;
            }
            if (activated) {
                m_portal_links.remember({m_world_settings.dimension, cell(1, 0), axis == 1, false});
            }
            return activated;
        }
    }
    return false;
}

// Place an Eye into an existing End frame. Activate only when all 16 ring cells
// of a horizontal 5x5 frame are filled and the center is clear (or already active).
bool Game::insert_ender_eye(const glm::ivec3& clicked) {
    if (!m_chunk_manager_ptr || m_chunk_manager_ptr->get_block_world(clicked.x, clicked.y, clicked.z)
        != Block_Types::End_Portal_Frame) return false;
    if (!set_block_at(clicked.x, clicked.y, clicked.z, Block_Types::End_Portal_Frame_Filled)) return false;
    // Search candidate centers; clicking any rim cell may be the last Eye.
    for (int cx = clicked.x - 2; cx <= clicked.x + 2; ++cx)
        for (int cz = clicked.z - 2; cz <= clicked.z + 2; ++cz) {
            if (std::max(std::abs(cx - clicked.x), std::abs(cz - clicked.z)) != 2) continue;
            bool complete = true;
            for (int dz = -2; dz <= 2 && complete; ++dz)
                for (int dx = -2; dx <= 2; ++dx) {
                    const Block_Types b = m_chunk_manager_ptr->get_block_world(
                        cx + dx, clicked.y, cz + dz);
                    if (std::max(std::abs(dx), std::abs(dz)) == 2) {
                        if (b != Block_Types::End_Portal_Frame_Filled) {complete = false; break;}
                    } else if (b != Block_Types::Air && b != Block_Types::End_Portal) {
                        complete = false; break;
                    }
                }
            if (!complete) continue;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    set_block_at(cx + dx, clicked.y, cz + dz, Block_Types::End_Portal);
            push_chat_message("End portal activated");
            return true;
        }
    return true;
}

// Legacy saves can have active portals created before portal_sites.txt existed.
// Search ONLY loaded chunks near the arrival position before constructing a new frame.
bool Game::reuse_loaded_portal() {
    if (!m_chunk_manager_ptr || m_pending_portal_type.empty()) return false;
    const glm::ivec3 landing(glm::floor(m_player_ptr->get_position()));
    const Block_Types target = m_pending_portal_type == "end"
        ? Block_Types::End_Portal : Block_Types::Nether_Portal;
    constexpr int radius = 24;
    for (int dy = -36; dy <= 36; ++dy) {
        const int y = landing.y + dy;
        if (y < Config::world_min_y || y > Config::world_max_y) continue;
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dx = -radius; dx <= radius; ++dx) {
                const glm::ivec3 cell = landing + glm::ivec3(dx, dy, dz);
                if (!m_chunk_manager_ptr->get_chunk(
                    Math_Helpers::floor_div(cell.x, Config::chunk_size),
                    Math_Helpers::floor_div(cell.y, Config::chunk_height),
                    Math_Helpers::floor_div(cell.z, Config::chunk_size))) continue;
                if (m_chunk_manager_ptr->get_block_world(cell.x, cell.y, cell.z) != target) continue;
                if (target == Block_Types::Nether_Portal) remember_touched_nether_portal(cell);
                else m_portal_links.remember({m_world_settings.dimension, cell, false, true});
                const Portal_Site* site = m_portal_links.nearest(m_world_settings.dimension,
                    static_cast<float>(cell.x), static_cast<float>(cell.z),
                    target == Block_Types::End_Portal, 4.f);
                if (!site) return false;
                const glm::vec3 entry = Portal_Link_Registry::entry_position(*site);
                m_pending_portal_type.clear();
                teleport_player(entry);
                m_portal_exit_lock = true;
                m_portal_cooldown = 2.f;
                m_dimension_landing = true;
                return true;
            }
    }
    return false;
}

void Game::tick_portal_travel(float dt) {
    if (!m_player_ptr || !m_chunk_manager_ptr) return;
    m_portal_cooldown = std::max(0.f, m_portal_cooldown - dt);
    if (!m_pending_command_portal_type.empty() &&
        construct_portal_at(m_pending_command_portal_type, m_pending_command_portal_origin,
                            m_pending_command_portal_along_z)) {
        push_chat_message("Portal built: " + m_pending_command_portal_type);
        m_pending_command_portal_type.clear();
    }
    if (m_dimension_landing) { m_portal_cooldown = 2.f; return; }
    if (m_destination_site_to_validate) {
        const Portal_Site site = *m_destination_site_to_validate;
        const glm::ivec3 cell = site.origin + glm::ivec3(0, site.end_portal ? 0 : 1, 0);
        if (!m_chunk_manager_ptr->get_chunk(
                Math_Helpers::floor_div(cell.x, Config::chunk_size),
                Math_Helpers::floor_div(cell.y, Config::chunk_height),
                Math_Helpers::floor_div(cell.z, Config::chunk_size))) return;
        m_destination_site_to_validate.reset();
        const Block_Types expected = site.end_portal ? Block_Types::End_Portal : Block_Types::Nether_Portal;
        if (m_chunk_manager_ptr->get_block_world(cell.x, cell.y, cell.z) != expected) {
            // The original portal was broken. Forget the stale anchor and fall
            // back to the local safe surface before scheduling a replacement.
            m_portal_links.forget(site);
            int x = site.origin.x, z = site.origin.z;
            const int safe_y = m_world_generator->find_spawn_surface(x, z) + 2;
            const glm::vec3 safe(static_cast<float>(x), static_cast<float>(safe_y), static_cast<float>(z));
            teleport_player(safe);
            m_pending_portal_type = site.end_portal ? "end" : "nether";
            m_pending_portal_origin = glm::ivec3(x + 6, safe_y - 1, z);
            m_pending_portal_along_z = false;
            m_pending_portal_delay = 1.f;
            m_dimension_landing = true;
            return;
        }
    }
    if (!m_pending_portal_type.empty()) {
        m_pending_portal_delay -= dt;
        if (m_pending_portal_delay <= 0.f) {
            if (!reuse_loaded_portal() &&
                construct_portal_at(m_pending_portal_type, m_pending_portal_origin,
                                    m_pending_portal_along_z)) {
                m_pending_portal_type.clear();
            }
            m_pending_portal_delay = 0.5f;
        }
    }

    const glm::ivec3 p(glm::floor(m_player_ptr->get_position()));
    Block_Types portal = Block_Types::Air;
    glm::ivec3 portal_cell = p;
    for (int dy = -1; dy <= 1; ++dy) {
        const Block_Types b = m_chunk_manager_ptr->get_block_world(p.x, p.y + dy, p.z);
        if (b == Block_Types::Nether_Portal || b == Block_Types::End_Portal) {
            portal = b;
            portal_cell.y += dy;
            break;
        }
    }
    if (portal == Block_Types::Air) {
        m_portal_exit_lock = false;
        m_portal_contact_time = 0.f;
        return;
    }
    if (m_portal_exit_lock || m_portal_cooldown > 0.f) {
        m_portal_contact_time = 0.f;
        return;
    }
    m_portal_contact_time += dt;
    if (m_portal_contact_time < 0.8f) return;
    if (portal == Block_Types::Nether_Portal) remember_touched_nether_portal(portal_cell);
    const std::string source = m_world_settings.dimension;
    const std::string destination = portal == Block_Types::Nether_Portal
        ? (source == "nether" ? "overworld" : "nether")
        : (source == "end" ? "overworld" : "end");
    if (switch_dimension(destination, true)) push_chat_message("Dimension: " + destination);
}

bool Game::set_block_at(int x, int y, int z, Block_Types type) {
    const bool ok = m_chunk_manager_ptr->set_block_world(x, y, z, type);
    // Росток, поставленный командой, тоже должен расти (как посаженный игроком).
    if (ok && m_crops_ptr && Crop_Manager::is_crop_block(type) && Crop_Manager::stage_of(type) == 0) {
        m_crops_ptr->plant(glm::ivec3(x, y, z), Crop_Manager::kind_of(type));
    }
    if (ok && m_saplings_ptr && Sapling_Manager::is_sapling(type)) {
        m_saplings_ptr->plant(glm::ivec3(x, y, z), type);
    }
    return ok;
}


// Саженец того же вида, что и листва; Air — если блок не листва.
static Block_Types Game_Leaf_Sapling(Block_Types leaves) {
    switch (leaves) {
        case Block_Types::Oak_Leaf:       return Block_Types::Oak_Sapling;
        case Block_Types::Birch_Leaves:   return Block_Types::Birch_Sapling;
        case Block_Types::Spruce_Leaves:  return Block_Types::Spruce_Sapling;
        case Block_Types::Jungle_Leaves:  return Block_Types::Jungle_Sapling;
        case Block_Types::Acacia_Leaves:  return Block_Types::Acacia_Sapling;
        case Block_Types::Slimewood_Leaves: return Block_Types::Slimewood_Sapling;
        default:                          return Block_Types::Air;
    }
}

// Листва дерева: 5% дают саженец своего вида, 2% — палку; листва дуба ещё 1% даёт яблоко.
// Сама листва ничего не даёт.
void Game::handle_leaf_drops(const glm::ivec3& pos, Block_Types leaves) {
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    std::uniform_real_distribution<float> jitter(-1.2f, 1.2f);
    const glm::vec3 center(pos.x + 0.5f, pos.y + 0.3f, pos.z + 0.5f);
    auto drop = [&](Block_Types type) {
        m_dropped_items_ptr->spawn(Item_Stack{type, 1}, center,
                                   glm::vec3(jitter(s_rng), 3.0f, jitter(s_rng)));
    };
    if (chance(s_rng) < 0.05f) drop(Game_Leaf_Sapling(leaves));
    if (chance(s_rng) < 0.02f) drop(Block_Types::Stick);
    if (leaves == Block_Types::Oak_Leaf && chance(s_rng) < 0.01f) drop(Block_Types::Apple);
}

void Game::handle_crop_harvest(const glm::ivec3& pos, Block_Types crop_block) {
    static std::mt19937 s_rng(std::random_device{}());
    const bool mature = Crop_Manager::is_mature(crop_block);
    const bool carrot = Crop_Manager::kind_of(crop_block) == Crop_Kind::Carrot;
    auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(s_rng); };
    const glm::vec3 center(pos.x + 0.5f, pos.y + 0.3f, pos.z + 0.5f);
    std::uniform_real_distribution<float> jitter(-1.2f, 1.2f);
    auto drop = [&](Block_Types type, int count) {
        if (count <= 0) return;
        m_dropped_items_ptr->spawn(Item_Stack{type, count}, center,
                                   glm::vec3(jitter(s_rng), 3.0f, jitter(s_rng)));
    };
    if (carrot) {
        drop(Block_Types::Carrot, mature ? roll(1, 3) : 1);
    } else if (mature) {
        drop(Block_Types::Wheat, 1);
        drop(Block_Types::Wheat_Seeds, roll(1, 3));
    } else {
        drop(Block_Types::Wheat_Seeds, 1);
    }

    // Дикая грядка исчезает вместе с собранным урожаем: на её месте снова трава.
    const Block_Types below = m_chunk_manager_ptr->get_block_world(pos.x, pos.y - 1, pos.z);
    if (below == Block_Types::Wild_Farmland) {
        if (m_chunk_manager_ptr->set_block_world(pos.x, pos.y - 1, pos.z, Block_Types::Grass)) {
            invalidate_block_mesh(pos.x, pos.y - 1, pos.z, *m_chunk_manager_ptr, *m_renderer_ptr);
        }
    }
}

int Game::clear_dropped_items() {
    const int removed = static_cast<int>(m_dropped_items_ptr->size());
    m_dropped_items_ptr->clear();
    return removed;
};
