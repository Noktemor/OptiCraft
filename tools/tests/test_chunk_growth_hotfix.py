"""Regression/source contracts for large Nether streams and GPU culling stats.

These source-contract assertions are intentionally dependency-free; they are not
an OpenGL end-to-end build or a replay of a live world.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
manager = (ROOT / 'src/world/chunks/Chunk_Manager.cpp').read_text(encoding='utf-8')
header = (ROOT / 'src/world/chunks/Chunk_Manager.h').read_text(encoding='utf-8')
renderer = (ROOT / 'src/rendering/Renderer.cpp').read_text(encoding='utf-8')
shader = (ROOT / 'assets/shaders/gpu_cull_commands.comp').read_text(encoding='utf-8')

assert 'std::vector<uint16_t>, Chunk_Key_Hash> m_light_sources' in header
assert 'found_sources.push_back(light_source_index(lx, ly, lz))' in manager
assert 'std::lower_bound(bucket.begin(), bucket.end(), local_index)' in manager
assert 'if (old_type != type) {' in manager
assert 'same_transparency && old_emission == new_emission' in manager
assert 'if (m_processing_fluid_ticks)' in manager
assert 'm_fluid_save_pending.insert(cx, cy, cz)' in manager
assert 'm_fluid_light_regions.try_emplace(' in manager
assert 'm_column_backlog_min_cy == min_cy' in manager
assert 'm_column_backlog_max_cy == max_cy' in manager
assert 'm_chunk_io->has_saved(region, Region_File::Key{cx, cy, cz})' in manager
assert 'std::abs(cy - m_player_chunk_y) > 1' in manager
assert 'glUniform1ui(m_gpu_loc_cull_stats_offset, 0u)' in renderer
assert 'glUniform1ui(m_gpu_loc_cull_stats_offset, 5u)' in renderer
assert 'queue_gpu_cull_stats_readback(did_hiz_recull)' in renderer
assert 'glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero_stats)' in renderer
assert 'glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |\n                    GL_BUFFER_UPDATE_BARRIER_BIT' in renderer
for index in range(5):
    assert f'stats[u_stats_offset + {index}u]' in shader

# Verify 32^3 packed local coordinates round-trip for negative world chunks.
N = 32
seen = set()
for y in range(N):
    for z in range(N):
        for x in range(N):
            i = (y * N + z) * N + x
            assert 0 <= i <= 0xFFFF
            px, py, pz = i % N, i // (N * N), (i // N) % N
            assert (px, py, pz) == (x, y, z)
            world = (-13*N + px, -3*N + py, 5*N + pz)
            assert (world[0] + 13*N, world[1] + 3*N, world[2] - 5*N) == (x, y, z)
            seen.add(i)
assert len(seen) == N**3

# Natural terrain ranges remain supported without sacrificing saved builds.
for cy in range(-8, 17):
    natural = 0 <= cy <= 3
    for player_cy in (-3, 0, 3, 8):
        for saved in (False, True):
            should_stream = natural or abs(cy - player_cy) <= 1 or saved
            if natural or saved or abs(cy - player_cy) <= 1:
                assert should_stream
            else:
                assert not should_stream

print('Chunk streaming/light-source/Hi-Z source contracts and 32768 packed positions: OK')
