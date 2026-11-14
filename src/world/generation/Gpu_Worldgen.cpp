#include "world/generation/Gpu_Worldgen.h"
#include "world/generation/World_Generator.h"
#include "world/blocks/Block_Types.h"
#include "utils/logging/Logger.h"
#include <glad/gl.h>
#include <fstream>
#include <iterator>
#include <string>

namespace {
GLuint make_buffer(GLuint binding, const void* data, size_t bytes, GLenum usage) {
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, usage);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, id);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return id;
}
}

Gpu_Worldgen::~Gpu_Worldgen() { shutdown(); }

void Gpu_Worldgen::shutdown() {
    abandon();
    if (m_timer_query) glDeleteQueries(1, &m_timer_query);
    if (m_program) glDeleteProgram(m_program);
    for (GLuint& b : m_static_buffers) if (b) glDeleteBuffers(1, &b);
    if (m_column_buffer) glDeleteBuffers(1, &m_column_buffer);
    if (m_levels_buffer) glDeleteBuffers(1, &m_levels_buffer);
    if (m_output_buffer) glDeleteBuffers(1, &m_output_buffer);
    m_timer_query = m_program = m_column_buffer = m_levels_buffer = m_output_buffer = 0;
    for (auto& b : m_static_buffers) b = 0;
    m_initialized = false;
}

void Gpu_Worldgen::abandon() {
    if (m_sync) {
        glDeleteSync(reinterpret_cast<GLsync>(m_sync));
        m_sync = nullptr;
    }
    m_count = 0;
}

bool Gpu_Worldgen::initialize(const World_Generator& generator) {
    if (m_disabled) return false;
    if (m_initialized) return true;
    if (!GLAD_GL_VERSION_4_3) {
        LOG_WARN("GPU worldgen: OpenGL 4.3 is unavailable; CPU fallback");
        m_disabled = true;
        return false;
    }
    std::ifstream stream("assets/shaders/worldgen_material.comp");
    if (!stream) {
        LOG_WARN("GPU worldgen: shader is missing; CPU fallback");
        m_disabled = true;
        return false;
    }
    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    const char* text = source.c_str();
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_WARN(std::string("GPU worldgen shader failed: ") + log);
        glDeleteShader(shader);
        m_disabled = true;
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, shader);
    glLinkProgram(m_program);
    glDeleteShader(shader);
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        char log[2048]{};
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        LOG_WARN(std::string("GPU worldgen link failed: ") + log);
        shutdown();
        m_disabled = true;
        return false;
    }
    Gpu_Noise_Tables tables;
    if (!generator.export_gpu_noise_tables(tables)) {
        shutdown();
        m_disabled = true;
        return false;
    }
    m_static_buffers[0] = make_buffer(0, tables.permutation.data(),
        sizeof(tables.permutation), GL_STATIC_DRAW);
    m_static_buffers[1] = make_buffer(1, tables.gradients.data(),
        sizeof(tables.gradients), GL_STATIC_DRAW);
    m_static_buffers[2] = make_buffer(2, tables.nodes.data(),
        tables.nodes.size() * sizeof(Gpu_Lattice_Node), GL_STATIC_DRAW);
    m_static_buffers[3] = make_buffer(3, tables.starts.data(),
        sizeof(tables.starts), GL_STATIC_DRAW);
    m_column_buffer = make_buffer(4, nullptr,
        Config::chunk_size * Config::chunk_size * sizeof(Gpu_Material_Column), GL_STREAM_DRAW);
    m_levels_buffer = make_buffer(5, nullptr, 9 * sizeof(int), GL_STREAM_DRAW);
    m_output_buffer = make_buffer(6, nullptr,
        9 * Config::chunk_size * Config::chunk_size * Config::chunk_height * sizeof(uint32_t),
        GL_STREAM_READ);
    glGenQueries(1, &m_timer_query);
    if (glGetError() != GL_NO_ERROR) {
        LOG_WARN("GPU worldgen: GL buffer allocation failed; CPU fallback");
        shutdown();
        m_disabled = true;
        return false;
    }
    m_initialized = true;
    LOG_INFO("GPU worldgen: FP64 OpenSimplex2S material backend initialized");
    return true;
}

bool Gpu_Worldgen::start(const World_Generator& generator, int chunk_x, int chunk_z,
    const std::array<Gpu_Material_Column, Config::chunk_size * Config::chunk_size>& columns,
    const std::vector<int>& chunk_ys) {
    if (busy() || chunk_ys.empty() || chunk_ys.size() > 9 || !initialize(generator)) return false;
    m_count = chunk_ys.size();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_column_buffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(columns), columns.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_levels_buffer);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, chunk_ys.size() * sizeof(int), chunk_ys.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    for (int i = 0; i < 4; ++i) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_static_buffers[i]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_column_buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_levels_buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_output_buffer);
    glUseProgram(m_program);
    auto set_i = [&](const char* name, int val) {
        const GLint loc = glGetUniformLocation(m_program, name);
        if (loc >= 0) glUniform1i(loc, val);
    };
    set_i("uChunkX", chunk_x);
    set_i("uChunkZ", chunk_z);
    set_i("uChunkCount", static_cast<int>(chunk_ys.size()));
    set_i("uWorldMinY", Config::world_min_y);
    set_i("uWorldMaxY", Config::world_max_y);
    const GLuint blocks[10] = {
        static_cast<GLuint>(Block_Types::Stone), static_cast<GLuint>(Block_Types::Deep_Stone),
        static_cast<GLuint>(Block_Types::Rare_Mineral), static_cast<GLuint>(Block_Types::Diamond_Ore),
        static_cast<GLuint>(Block_Types::Gold_Ore), static_cast<GLuint>(Block_Types::Copper_Ore),
        static_cast<GLuint>(Block_Types::Lapis_Ore), static_cast<GLuint>(Block_Types::Coal_Ore),
        static_cast<GLuint>(Block_Types::Iron_Ore), static_cast<GLuint>(Block_Types::Mossy_Stone)
    };
    glUniform1uiv(glGetUniformLocation(m_program, "uBlocks"), 10, blocks);
    glBeginQuery(GL_TIME_ELAPSED, m_timer_query);
    const GLuint voxel_count = static_cast<GLuint>(Config::chunk_size * Config::chunk_size * Config::chunk_height);
    glDispatchCompute((voxel_count * static_cast<GLuint>(m_count) + 63u) / 64u, 1u, 1u);
    glEndQuery(GL_TIME_ELAPSED);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    m_sync = reinterpret_cast<void*>(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    glFlush();
    glUseProgram(0);
    if (!m_sync || glGetError() != GL_NO_ERROR) {
        LOG_WARN("GPU worldgen dispatch failed; switching to CPU");
        abandon();
        m_disabled = true;
        return false;
    }
    return true;
}

bool Gpu_Worldgen::poll(std::vector<uint32_t>& output, double& gpu_ms) {
    if (!m_sync) return false;
    const GLenum status = glClientWaitSync(reinterpret_cast<GLsync>(m_sync), 0, 0);
    if (status == GL_TIMEOUT_EXPIRED) return false;
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        m_disabled = true;
        abandon();
        return false;
    }
    output.resize(m_count * Config::chunk_size * Config::chunk_size * Config::chunk_height);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output_buffer);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       static_cast<GLsizeiptr>(output.size() * sizeof(uint32_t)), output.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    GLuint64 nanoseconds = 0;
    glGetQueryObjectui64v(m_timer_query, GL_QUERY_RESULT, &nanoseconds);
    gpu_ms = static_cast<double>(nanoseconds) / 1'000'000.0;
    const bool success = glGetError() == GL_NO_ERROR;
    abandon();
    if (!success) {
        output.clear();
        m_disabled = true;
    }
    return true;
}
