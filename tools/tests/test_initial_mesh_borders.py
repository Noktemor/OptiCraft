#!/usr/bin/env python3
"""Initial-neighbour mesh invalidation regression checks.

The geometric checks model the exact shared-face/edge/corner index mapping used
by Chunk_Manager::collect_finished_chunks. The source checks ensure the live
implementation still uses the rule. This is NOT an RTX/OpenGL test.
"""
from pathlib import Path
from itertools import product
import random

ROOT = Path(__file__).resolve().parents[2]
manager = (ROOT / 'src/world/chunks/Chunk_Manager.cpp').read_text()

# First-mesh snapshots use resident neighbours only; missing neighbours are air.
start = manager.index('Chunk_Neighbor_Lookup lookup = [&snapshot]')
end = manager.index('Chunk_Light_Lookup light_lookup', start)
lookup = manager[start:end]
assert 'generator->get_block' not in lookup
assert 'return Block_Types::Air;' in lookup
assert 'snapshot.ring_valid[ring]' in lookup
assert 'snapshot.neighbor_blocks[neighbor_index]' in lookup

# Both contacting sides, including when the arriving neighbour is all air.
start = manager.index('OC_ZONE("apply: mark affected neighbours dirty")')
end = manager.index('// Небесный свет приходит вместе с чанком', start)
arrival = manager[start:end]
assert 'if (!(new_blocks.is_uniform()' not in arrival
assert 'new_is_air && old_blocks.is_uniform()' in arrival
assert 'old_blocks.get(nx, ny, nz)' in arrival
assert 'neighbor->mark_mesh_dirty()' in arrival
assert 'Config::chunk_size - 1 - x' in arrival
assert 'Config::chunk_height - 1 - y' in arrival

N = 32

def face_coords(delta):
    return range(N) if delta == 0 else (0,) if delta < 0 else (N-1,)

def other(local, delta):
    return local if delta == 0 else N-1-local

def contact_non_air(arriving, resident, delta):
    if not arriving and not resident:
        return False
    for x,y,z in product(*(face_coords(v) for v in delta)):
        nx,ny,nz = (other(coord,d) for coord,d in zip((x,y,z),delta))
        if (x,y,z) in arriving or (nx,ny,nz) in resident:
            return True
    return False

rng = random.Random(1856)
count = 0
for d in product((-1,0,1),repeat=3):
    if d == (0,0,0):
        continue
    # Both genuinely empty on the touching face / edge / corner -> no remesh.
    assert not contact_non_air(set(),set(),d)
    cells = list(product(*(face_coords(v) for v in d)))
    for _ in range(60):
        p = rng.choice(cells)
        facing = tuple(other(c,v) for c,v in zip(p,d))
        # An arriving air chunk MUST rebuild the resident chunk when its border has blocks.
        assert contact_non_air(set(),{facing},d)
        # And vice versa.
        assert contact_non_air({p},set(),d)
        # Empty interior (not contacting the other chunk) MUST NOT trigger an extra rebuild.
        unrelated = tuple(15 if v != 0 else (c + 7) % N for c,v in zip(p,d))
        assert not contact_non_air(set(),{unrelated},d)
        # Symmetry: when the neighbour arrives in the opposite direction, the result matches.
        assert contact_non_air({facing},set(),tuple(-v for v in d))
        count += 1
print(f'Initial mesh border contracts and {count} 26-neighbour mapping scenarios: OK')
