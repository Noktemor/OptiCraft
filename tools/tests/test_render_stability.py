#!/usr/bin/env python3
"""Static safety guard for the renderer COW / GPU compatibility integration.

Run alongside the existing game tests. This is a *source-contract check*, NOT a
substitute for an OpenGL render test on Intel and NVIDIA hardware.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
interaction = (root / 'src/world/blocks/Block_Interaction.cpp').read_text()
manager = (root / 'src/world/chunks/Chunk_Manager.cpp').read_text()
renderer_h = (root / 'src/rendering/Renderer.h').read_text()
renderer_cpp = (root / 'src/rendering/Renderer.cpp').read_text()
app = (root / 'src/core/app/Application.cpp').read_text()
game = (root / 'src/core/game/Game.cpp').read_text()

# No gameplay block modification is allowed to delete resident GPU geometry.
assert 'invalidate_chunk_cache(' not in interaction
assert 'release_gpu_chunk(' not in interaction
# The authoritative mutation path schedules mesh rebuilds; the renderer has an
# existing copy-on-write pending generation and only swaps on completion.
assert 'request_urgent_mesh_rebuild(cx, cy, cz)' in manager
assert 'res.gpu_handle = res.gpu_pending_handle;' in renderer_cpp
assert 'res.gpu_pending_handle = -1;' in renderer_cpp
assert 'if (res.mesh_version == rb.versions[i])' in renderer_cpp
assert 'glDeleteSync' in renderer_cpp

# In compatibility mode the *same* vertex-pulling path is used for program
# binding, GPU culling command layout, and indirect drawing. Both cull passes
# must see the same branch; Hi-Z should not hide sections in safe mode.
assert 'm_gpu_mesh_shader_ready && !m_gpu_compatibility_mode && !m_meshlet_renderer_enabled' in renderer_h
assert len(re.findall(r'glUniform1i\(m_gpu_loc_cull_mesh_mode,\s*use_task_mesh_shaders\(\)', renderer_cpp)) == 2
assert 'if (use_task_mesh_shaders() && m_gl_multi_draw_mesh_tasks_indirect)' in renderer_cpp
assert 'm_occlusion_enabled && !m_gpu_compatibility_mode' in renderer_cpp
assert 'std::strstr(vendor_name, "NVIDIA")' in renderer_cpp
assert 'set_gpu_compatibility_mode(m_gpu_compatibility_recommended)' in renderer_cpp
assert renderer_h.count('m_gpu_compact_opaque_commands = !m_gpu_compatibility_mode &&') == 2

# The mode must be changeable in the game UI, not through environment variables.
assert 'gpu_backend_mode=' in game
assert 'game.set_gpu_backend_mode(' in app
assert 'OPTICRAFT_GPU' not in renderer_cpp + game

print('Renderer COW + NVIDIA compatibility source contracts: OK')
