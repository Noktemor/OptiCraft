#!/usr/bin/env python3
"""Offline OSTR v1 builder. Run from project root; never used during worldgen.

Names are stable IDs consumed by Structure_Pack. Coordinates are relative to a
front entrance at (entrance_x, 0, entrance_z); the front faces -Z by default.
"""
import json
from pathlib import Path
from struct import pack

ROOT = Path(__file__).resolve().parents[2]
IDS = {b['name']: int(b['id']) for b in json.loads((ROOT/'assets/blocks.json').read_text())['blocks']}
AIR=0

def id(name):
    if name not in IDS: raise KeyError(name)
    return IDS[name]

class Template:
    def __init__(self, name, width, height, depth, entrance_x, entrance_z=0):
        self.name=name;self.width=width;self.height=height;self.depth=depth
        self.entrance_x=entrance_x;self.entrance_z=entrance_z
        self.blocks={}
    def set(self,x,y,z,block,meta=0,flags=0):
        if not (0<=x<self.width and -8<=y<self.height and 0<=z<self.depth):
            raise ValueError((self.name,x,y,z))
        self.blocks[(x,y,z)]=(block,meta,flags)
    def cube(self,x0,y0,z0,x1,y1,z1,b,meta=0):
        for y in range(y0,y1+1):
            for z in range(z0,z1+1):
                for x in range(x0,x1+1): self.set(x,y,z,b,meta)
    def volume_clear(self, y_top):
        self.cube(0,1,0,self.width-1,y_top,self.depth-1,AIR)


def house(name, wood, log, door, width=9,depth=9):
    t=Template(name,width,11,depth,width//2)
    # Air records clear previous trees and terrain. Built to be clipped by chunks.
    t.volume_clear(9)
    t.cube(0,-3,0,width-1,0,depth-1,id('Cobblestone'))
    t.cube(1,0,1,width-2,0,depth-2,id(wood))
    for y in range(1,5):
        for x in range(width):
            for z in (0,depth-1):t.set(x,y,z,id(wood))
        for z in range(depth):
            for x in (0,width-1):t.set(x,y,z,id(wood))
    for x,z in [(0,0),(width-1,0),(0,depth-1),(width-1,depth-1)]:
        for y in range(1,6):t.set(x,y,z,id(log))
    for x in (2,width-3):
        t.set(x,2,0,id('Glass'));t.set(x,2,depth-1,id('Glass'))
    for z in (2,depth-3):
        t.set(0,2,z,id('Glass'));t.set(width-1,2,z,id('Glass'))
    # Roof in three stepped layers. Opaque full-cube blocks, no extra mesh pipeline.
    t.cube(0,5,0,width-1,5,depth-1,id('Spruce Planks'))
    t.cube(1,6,1,width-2,6,depth-2,id('Spruce Planks'))
    t.cube(2,7,2,width-3,7,depth-3,id('Spruce Planks'))
    # Two-block game-native door (bit 3 denotes upper half).
    t.set(width//2,1,0,id(door),0,1)
    t.set(width//2,2,0,id(door),8,1)
    # Every generated cottage is a usable home: bed, torch and an entrance.
    t.set(2,1,3,id("White Bed"),0)
    t.set(1,2,2,id("Torch"),0)
    # Every profession has a physical job site. Additional jobs are craftable.
    t.set(width-3,1,3,id("Lectern" if name=="Cottage_Birch" else "Fletching Table"))
    return t

def farm():
    t=Template('Farm',11,8,11,5)
    t.volume_clear(6)
    t.cube(0,-2,0,10,0,10,id('Dirt'))
    t.cube(1,0,1,9,0,9,id('Farmland Wet'))
    for z in range(1,10):
        for x in range(1,10):
            if x == 5: t.set(x,0,z,id('Water'))
            else: t.set(x,1,z,id('Wheat Crop 3'))
    for z in range(11):
        for x in (0,10): t.set(x,1,z,id('Oak Fence'))
    for x in range(11):
        for z in (0,10): t.set(x,1,z,id('Oak Fence'))
    t.set(5,1,0,AIR)
    t.set(0,1,5,id("Composter"))
    return t

def well():
    t=Template('Well',7,9,7,3,3)
    t.volume_clear(8)
    t.cube(0,-2,0,6,0,6,id('Cobblestone'))
    t.cube(2,0,2,4,0,4,id('Water'))
    for x,z in [(1,1),(5,1),(1,5),(5,5)]:
        for y in range(1,5):t.set(x,y,z,id('Cobblestone'))
    t.cube(1,5,1,5,5,5,id('Spruce Planks'))
    return t

def fortress():
    # Demonstrates a multi-chunk monolithic template. Not auto-spawned in villages.
    size=65
    t=Template('Fortress',size,24,size,size//2)
    t.volume_clear(20)
    t.cube(0,-3,0,size-1,0,size-1,id('Cobblestone'))
    for y in range(1,12):
        for x in range(size):
            for z in (0,size-1):t.set(x,y,z,id('Stone'))
        for z in range(size):
            for x in (0,size-1):t.set(x,y,z,id('Stone'))
    # Corner towers
    for ox in (0,size-9):
        for oz in (0,size-9):
            for y in range(1,17):
                for x in range(ox,ox+9):
                    for z in range(oz,oz+9):
                        if x in (ox,ox+8) or z in (oz,oz+8) or y in (1,16):
                            t.set(x,y,z,id('Cobblestone'))
            for x in range(ox,ox+9):
                for z in range(oz,oz+9):
                    if x in (ox,ox+8) or z in (oz,oz+8):t.set(x,17,z,id('Stone'))
    # Big front gate plus bridge-like driveway
    for x in range(size//2-2,size//2+3):
        for y in range(1,6):t.set(x,y,0,AIR)
    # Inner keep 15x15 with clearable interior and no chests/block-entity dependencies.
    for y in range(1,14):
        for x in range(25,40):
            for z in range(25,40):
                if y == 1 or y == 13 or x in (25,39) or z in (25,39):
                    t.set(x,y,z,id('Stone'))
    for y in (2,3):t.set(32,y,25,AIR)
    return t


def main():
    templates=[house('Cottage_Oak','Oak Planks','Oak Log','Oak Door'),
               house('Cottage_Birch','Birch Planks','Birch Log','Birch Door'),
               farm(),well(),fortress()]
    index_offset=16
    data_offset=index_offset+44*len(templates)
    out=bytearray(pack('<4sHHII',b'OSTR',1,len(templates),index_offset,data_offset))
    records=bytearray()
    for t in templates:
        name=t.name.encode('ascii')
        if len(name)>=24:raise ValueError(t.name)
        ordered=sorted(t.blocks.items(),key=lambda v:(v[0][1],v[0][2],v[0][0]))
        offset=data_offset+len(records)
        out+=pack('<24sHHHhhIIH',name,t.width,t.height,t.depth,
                  t.entrance_x,t.entrance_z,offset,len(ordered),0)
        for (x,y,z),(block,meta,flags) in ordered:
            records+=pack('<hhhHBB',x,y,z,block,meta,flags)
        print(f'{t.name}: {len(ordered)} blocks, {t.width}x{t.height}x{t.depth}')
    out+=records
    destination=ROOT/'assets/structures/structures.struct'
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_bytes(out)
    print(f'Wrote {destination.relative_to(ROOT)}: {len(out):,} bytes')

if __name__=='__main__':main()
