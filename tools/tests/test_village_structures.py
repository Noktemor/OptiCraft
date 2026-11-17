#!/usr/bin/env python3
"""Validate OSTR v1 and run C++ production-source structure/layout smoke tests.

Uses a tiny GLM declaration only for compiling isolated game-independent modules
when GLM is not installed. This does NOT replace a complete OptiCraft game build.
"""
from pathlib import Path
from struct import unpack_from
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
asset = ROOT / 'assets/structures/structures.struct'
data = asset.read_bytes()
magic, version, count, index_off, data_off = unpack_from('<4sHHII', data)
assert magic == b'OSTR' and version == 1 and count == 5 and index_off == 16
assert data_off == 16 + count * 44
names = ['Cottage_Oak', 'Cottage_Birch', 'Farm', 'Well', 'Fortress']
next_offset = data_off
for i, expected in enumerate(names):
    name, w, h, d, ex, ez, offset, blocks, flags = unpack_from(
        '<24sHHHhhIIH', data, index_off + i * 44)
    assert name.split(b'\0', 1)[0].decode('ascii') == expected
    assert w and h and d and 0 <= ex < w and 0 <= ez < d
    assert flags == 0 and offset == next_offset and 0 < blocks <= 250_000
    for j in range(blocks):
        x, y, z, block, meta, block_flags = unpack_from('<hhhHBB', data, offset + j*10)
        assert 0 <= x < w and -8 <= y < h and 0 <= z < d
        assert block_flags & ~1 == 0
    next_offset += blocks * 10
assert next_offset == len(data)
print(f'OSTR v1 valid: {count} templates, {len(data)} bytes, including 65x65 fortress')

with tempfile.TemporaryDirectory(prefix='opticraft_structures_') as td:
    root = Path(td)
    fake_glm = root/'fake_include/glm'
    fake_glm.mkdir(parents=True)
    (fake_glm/'glm.hpp').write_text('''#pragma once
namespace glm {
struct vec4 { float x,y,z,w; constexpr vec4(float v=0):x(v),y(v),z(v),w(v){} };
}
''')
    shared = [str(ROOT/'src/world/generation/Structure_Pack.cpp'),
              str(ROOT/'src/world/generation/Village_Layout.cpp')]
    tests = [
        ('village_structure_smoke', ['src/world/generation/Maze.cpp',
                                      'src/world/structures/Dungeon_Layout.cpp']),
        ('structure_placement_smoke', ['src/world/generation/Structure_Placement.cpp'])
    ]
    for name, extra in tests:
        exe = root/name
        source = ROOT/'tools/tests'/f'{name}.cpp'
        cmd = ['g++', '-std=c++20', '-O2', '-pthread', '-I'+str(ROOT/'src'),
               '-I'+str(root/'fake_include'), str(source), *shared,
               *(str(ROOT/e) for e in extra), '-o', str(exe)]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], cwd=ROOT, check=True)
