#!/usr/bin/env python3
"""Compile actual OptiCraft Logic_Manager and Block_Shapes using isolated GLM test doubles.

Checks actual production method implementations without depending on GL/GLFW. Not a full game build.
"""
from pathlib import Path
import json,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
tests=root/'tools/tests/copper_logic_smoke'
with tempfile.TemporaryDirectory() as tmp:
    command=['g++','-std=c++20','-O2','-Wall','-Wextra','-Wno-sign-compare','-Wno-unused-parameter',
             f'-I{tests/"stubs"}',f'-I{root/"src"}',
             str(root/'src/world/blocks/Block_Shapes.cpp'),
             str(root/'src/world/logic/Logic_Manager.cpp'),
             str(tests/'test.cpp'),'-o',str(Path(tmp)/'copper_logic_smoke')]
    subprocess.run(command,check=True)
    subprocess.run([str(Path(tmp)/'copper_logic_smoke')],check=True)
blocks=json.loads((root/'assets/blocks.json').read_text())['blocks']
sprites=json.loads((root/'assets/atlases/blocks_main.atlas.json').read_text())['sprites']
assert len({b['id'] for b in blocks})==len(blocks)
for name in ('Observer','Piston','Sticky Piston','Piston Head','Copper Lamp','Copper Lamp On'):
    assert any(b['name']==name for b in blocks),name
for b in blocks:
    if 511<=b['id']<=516:
        assert all(tile in sprites for tile in b['texture'].values() if tile!='blocks_main'),b
assert (root/'assets/texture/texture_atlas.png').exists()
print('copper assets: OK (unique IDs, devices, atlas sprites)')
