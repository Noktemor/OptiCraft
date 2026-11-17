#!/usr/bin/env python3
"""Draw copper-logic tiles into unused slots of the existing OptiCraft atlas; deterministic.

Run from the project root: python3 tools/copper_logic/generate_textures.py
Only edits the target texture rectangles and named sprites. Other sprites are unchanged.
"""
import json
from pathlib import Path
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parents[2]
atlas=ROOT/'assets/texture/texture_atlas.png'
manifest=ROOT/'assets/atlases/blocks_main.atlas.json'
image=Image.open(atlas).convert('RGBA')
data=json.loads(manifest.read_text())
# Chosen from unused row 6 (y=96). Do not collide with catalog tiles.
names=['opt_observer_face','opt_observer_side','opt_observer_back',
       'opt_piston_face','opt_piston_side','opt_sticky_face',
       'opt_lamp_off','opt_lamp_on']
slots={name:(i*16,96) for i,name in enumerate(names)}
for i,name in enumerate(names):
    x,y=slots[name]
    data['sprites'][name]={'x':x,'y':y,'w':16,'h':16}
    tile=Image.new('RGBA',(16,16),(69,80,76,255))
    d=ImageDraw.Draw(tile)
    d.rectangle((0,0,15,15),fill=(64,76,72),outline=(24,33,32))
    d.rectangle((2,2,13,13),outline=(110,130,118),width=1)
    if name=='opt_observer_face':
        d.rectangle((3,4,12,11),fill=(28,45,43),outline=(118,164,146))
        d.rectangle((5,6,7,8),fill=(125,206,174))
        d.rectangle((9,6,11,8),fill=(125,206,174))
    elif name=='opt_observer_back':
        d.rectangle((3,3,12,12),fill=(41,58,54),outline=(84,116,105))
        d.rectangle((6,6,9,9),fill=(196,61,41))
    elif name=='opt_observer_side':
        d.line((3,8,12,8),fill=(166,110,56),width=2)
        d.polygon([(10,5),(13,8),(10,11)],fill=(187,151,87))
    elif name in ('opt_piston_face','opt_sticky_face'):
        d.rectangle((3,3,12,12),fill=(98,77,58) if name=='opt_piston_face' else (69,131,73),outline=(209,176,122))
        for k in (5,10):
            d.line((k,4,k,11),fill=(45,54,38),width=1)
    elif name=='opt_piston_side':
        d.rectangle((2,5,13,10),fill=(132,94,54),outline=(206,157,98))
        d.line((7,2,7,13),fill=(38,47,46),width=2)
    else:
        d.rectangle((3,3,12,12),fill=(29,43,40),outline=(103,135,105))
        light=name=='opt_lamp_on'
        d.rectangle((5,5,10,10),fill=(230,198,98) if light else (65,95,78))
        if light:
            d.point((7,6),fill=(255,248,172))
    image.paste(tile,(x,y))
# Wire texture now makes sense on 2px-wide shaped geometry, not a large square plate.
w=data['sprites']['copper_wire']; x,y=w['x'],w['y']
tile=Image.new('RGBA',(16,16),(147,87,42,255));d=ImageDraw.Draw(tile)
d.rectangle((0,0,15,15),outline=(83,49,34),width=1)
for i in range(3,14,4):d.line((2,i,13,i),fill=(188,128,63),width=1)
image.paste(tile,(x,y))
image.save(atlas)
manifest.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
