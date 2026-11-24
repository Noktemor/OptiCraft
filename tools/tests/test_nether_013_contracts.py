"""Nether 0.12.1 source/asset contracts. No GPU dependencies."""
from pathlib import Path
import json
root = Path(__file__).resolve().parents[2]
gen = (root / 'src/world/generation/World_Generator.cpp').read_text()
game = (root / 'src/core/game/Game.cpp').read_text()
inter = (root / 'src/world/blocks/Block_Interaction.cpp').read_text()
registry = (root / 'src/world/portals/Portal_Link_Registry.h').read_text()
fire = (root / 'src/world/logic/Fire_Simulation.h').read_text()
assert 'if (y < 0 || y > 512)' in gen
assert 'if (y == 0 || y == 512)' in gen
assert 'return start_y > 512' in gen
assert 'm_portal_links.nearest(' in game
assert 'remember_touched_nether_portal(' in game
assert 'reuse_loaded_portal()' in game
assert 'end_exit ? m_spawn_position' in game
assert 'm_portal_exit_lock' in game
assert 'portal_sites.txt' in game
assert 'm_fire.ignite(' in game
assert 'm_fire.tick(' in game
assert 'm_fire.save(' in game
assert 'm_fire.load(' in game
assert 'kPerSecondBudget = 128' in fire
assert 'm_portal_links.forget(site)' in game
assert 'Block_Types::Nether_Portal ||' in inter and 'Block_Types::End_Portal ||' in inter
assert 'std::quoted(e.dimension)' in registry
blocks = {x['id']:x for x in json.loads((root/'assets/blocks.json').read_text())['blocks']}
assert blocks[521]['item'] is False and not blocks[521]['show_in_creative']
assert blocks[523]['item'] is False and not blocks[523]['show_in_creative']
assert blocks[528]['item'] and blocks[528]['name']=='Lighter'
assert not blocks[529]['item'] and not blocks[529]['show_in_creative']
assert blocks[529]['light_emission'] >= 8
atlas = json.loads((root/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
assert 'oc_lighter' in atlas and 'oc_fire' in atlas
recipes = json.loads((root/'assets/recipe.json').read_text())['recipes']
assert any(r.get('output',{}).get('item')=='Lighter' for r in recipes)
print('Nether 0.12.1 source/asset contracts: PASS')
