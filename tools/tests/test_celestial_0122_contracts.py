#!/usr/bin/env python3
"""Static+asset integration validation for 0.12.2 celestial bosses. No GL required."""
import json
import struct
from pathlib import Path

import trimesh
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
assert (ROOT/'VERSION').read_text().strip() in {'0.12.2', '0.13'}
assert any('VERSION '+v in (ROOT/'CMakeLists.txt').read_text() for v in ('0.12.2', '0.13'))
blocks = json.loads((ROOT/'assets/blocks.json').read_text())['blocks']
ids = {b['id']: b for b in blocks}
assert len(ids) == len(blocks)
atlas = json.loads((ROOT/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
atlas_image = Image.open(ROOT/'assets/texture/texture_atlas.png')
for num, boss in ((610, 'moon'), (611, 'sun')):
    entry = ids[num]
    assert entry['item'] and entry['name'] == f'{boss.title()} Princess Spawn Egg'
    sprite = entry['texture']['top']
    assert sprite == f'egg_{boss}_princess'
    x, y, w, h = (atlas[sprite][k] for k in ('x', 'y', 'w', 'h'))
    assert x >= 0 and y >= 0 and w == 16 and h == 16
    assert x + w <= atlas_image.width and y + h <= atlas_image.height
    assert atlas_image.crop((x, y, x+w, y+h)).convert('RGBA').getbbox() is not None
    path = ROOT/f'assets/mobs/{boss}_princess.glb'
    model = trimesh.load(path, force='scene')
    assert model.geometry, path
    height = model.bounds[1][1] - model.bounds[0][1]
    assert 2.9 <= height <= 3.5, (boss, height)

for name in ('moon_shield', 'sun_hand', 'moon_wind_blade'):
    path = ROOT/f'assets/mobs/{name}.glb'
    model = trimesh.load(path, force='scene')
    assert model.geometry, path

boss_cpp = (ROOT/'src/mobs/Boss.cpp').read_text()
boss_h = (ROOT/'src/mobs/Boss.h').read_text()
mobs = (ROOT/'src/mobs/Mobs_Update.cpp').read_text()
cmd = (ROOT/'src/commands/Command_System.cpp').read_text()
game = (ROOT/'src/core/game/Game.cpp').read_text()
hud = (ROOT/'src/hud/Hud.cpp').read_text()
assert 'Moon_Princess' in boss_h and 'Sun_Princess' in boss_h
assert 'update_moon(' in boss_cpp and 'update_sun(' in boss_cpp
assert 'Celestial_Boss_Rules::moon_vulnerable' in boss_cpp
assert 'Celestial_Boss_Rules::sun_speed' in boss_cpp
assert 'Boss_Choreography::' not in boss_cpp or 'choose_sun_move' in boss_cpp
assert 'in_strike_window' in boss_cpp
assert 'm_sun_active_hand' in boss_cpp
assert 'moon_swooping' in boss_cpp
assert 'navigate_flying' in boss_cpp
assert 'blocked_segment' in boss_cpp
assert 'm_target_boss_part = d.subpart' in mobs
assert 'm_target_boss_part))' in mobs
assert 'Moon_Princess_Spawn_Egg' in mobs and 'Sun_Princess_Spawn_Egg' in mobs
assert 'boss_bars(' in mobs and 'm_mobs.boss_bars(' in game
assert 'draw_boss_bar(' in hud and 'draw_boss_bar(' in game
assert 'moon_princess' in cmd and 'sun_princess' in cmd
assert 'hotbar_y - 22.0f * s' in hud and 'window_height-29.0f*s' in hud
print('Celestial 0.12.2: contracts PASS (models, eggs, texture atlas, HUD, commands, hits, flight)')
