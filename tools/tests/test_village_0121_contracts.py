#!/usr/bin/env python3
"""Non-GL validation of villager assets, recipes, templates and integration points."""
from pathlib import Path
import json
from struct import unpack_from
import trimesh
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
blocks=json.loads((ROOT/'assets/blocks.json').read_text())['blocks']
ids={int(b['id']):b for b in blocks}
names={b['name']:b for b in blocks}
assert len(ids)==len(blocks)
assert {530,531,532,533,534,535}.issubset(ids)
professions={530:'Fletching Table',531:'Hunter Bench',532:'Smoker',533:'Composter',534:'Smithing Table',427:'Lectern'}
for k,name in professions.items():
    assert ids[k]['name']==name
    assert ids[k]['solid']
assert ids[535]['name']=='Emerald' and ids[535]['item']
recipes=json.loads((ROOT/'assets/recipe.json').read_text())['recipes']
for recipe in recipes:
    output=recipe.get('output',{}).get('item','')
    if not output:continue
    assert output in names, (recipe.get('name'),output)
    for ing in recipe.get('ingredients',[]):
        assert ing['item'] in names or ing['item'].startswith('#'),(recipe.get('name'),ing['item'])
for name in professions.values():
    if name=='Lectern':continue
    assert any(r.get('output',{}).get('item')==name for r in recipes),name
atlas=json.loads((ROOT/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
w,h=Image.open(ROOT/'assets/texture/texture_atlas.png').size
for bid in range(530,536):
    sprite=ids[bid]['texture']['top']
    r=atlas[sprite]
    assert r['x']>=0 and r['y']>=0 and r['x']+r['w']<=w and r['y']+r['h']<=h
for role in ('unemployed','farmer','archer','librarian','hunter','cook','smith'):
    p=ROOT/f'assets/mobs/villager_{role}.glb'
    scene=trimesh.load(p,force='scene')
    assert scene.geometry and len(scene.geometry)>=5,(role,len(scene.geometry))
scene=trimesh.load(ROOT/'assets/mobs/iron_golem.glb',force='scene')
assert scene.geometry and len(scene.geometry)>=5
raw=(ROOT/'assets/structures/structures.struct').read_bytes()
magic,version,count,index_offset,data_offset=unpack_from('<4sHHII',raw)
assert (magic,version,count)==(b'OSTR',1,5)
for idx in (0,1):
    name,_,_,_,_,_,offset,n,flags=unpack_from('<24sHHHhhIIH',raw,index_offset+44*idx)
    props=[unpack_from('<hhhHBB',raw,offset+k*10) for k in range(n)]
    block_ids={v[3] for v in props}
    assert names['White Bed']['id'] in block_ids
    assert names['Torch']['id'] in block_ids
    assert any('Door' in ids[x]['name'] for x in block_ids)
    assert (530 if idx==0 else 427) in block_ids
name,_,_,_,_,_,offset,n,_=unpack_from('<24sHHHhhIIH',raw,index_offset+44*2)
farm_ids=[unpack_from('<hhhHBB',raw,offset+k*10)[3] for k in range(n)]
assert 533 in farm_ids
cmake=(ROOT/'CMakeLists.txt').read_text()
assert '${SRC_DIR}/mobs/village/Village_System.cpp' in cmake
code=(ROOT/'src/core/game/Game.cpp').read_text()
assert 'build_trade_panel() const' in code and 'm_villages.trade(' in code
assert 'm_villages.tick(' in code and 'm_villages.serialize()' in code
assert (ROOT/'VERSION').read_text().strip() in {'0.12.1','0.12.2', '0.13'}
print('Village 0.12.1 contracts PASS: profession blocks, offers, GLBs, templates, Game integration')
