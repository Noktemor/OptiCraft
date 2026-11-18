#!/usr/bin/env python3
"""Regression contracts for the optional NVIDIA baked-light path.

This test checks code wiring, not the image produced by an RTX driver.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
config = (root / 'src/utils/configs/Config_Render.h').read_text()
renderer = (root / 'src/rendering/Renderer.cpp').read_text()
manager = (root / 'src/world/chunks/Chunk_Manager.cpp').read_text()
chunk = (root / 'src/world/chunks/Chunk.h').read_text()
fragment = (root / 'assets/shaders/terrain_gpu_fragment.glsl').read_text()

assert 'inline std::atomic<bool> gpu_light_volume_active{gpu_light_volume}' in config
assert 'std::memory_order_relaxed' in config
assert 'Config::gpu_light_volume_active.store(!m_gpu_compatibility_recommended' in renderer
assert 'Config::use_gpu_light_volume() ? 0 : 1' in renderer
assert 'Config::use_gpu_light_volume() && !capture_active()' in manager
assert 'Config::use_gpu_light_volume() && m_meshes.lod_stride == 1' in chunk
assert 'if (Config::use_gpu_light_volume()) {' in renderer
assert 'if (m_gpu_volume_ssbo != 0 && volumes != 0)' in renderer
assert 'glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, m_gpu_volume_ssbo)' in renderer
assert 'c = clamp(c, ivec3(-1), ivec3(32));' in fragment
assert 'GL_MAX_SHADER_STORAGE_BLOCK_SIZE' in renderer
print('NVIDIA baked-light first-mesh compatibility contracts: OK')
