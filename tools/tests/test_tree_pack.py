#!/usr/bin/env python3
"""Validate TREE v1 archive index, block ranges and variation uniqueness."""
from collections import defaultdict
from hashlib import sha256
from pathlib import Path
from struct import unpack_from

ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "assets/trees/trees.tree"
raw = path.read_bytes()
assert len(raw) <= 64 * 1024 * 1024
magic, version, count, index_offset, payload_offset = unpack_from("<4sHHII", raw)
assert (magic, version, count, index_offset) == (b"TREE", 1, 576, 16)
assert payload_offset == index_offset + count * 32
assert payload_offset <= len(raw)

names = set()
species = defaultdict(set)
entries = defaultdict(int)
ranges = []
for i in range(count):
    name, kind, variant, offset, blocks, reserved = unpack_from("<16sHHIII", raw, index_offset + i * 32)
    name = name.split(b"\0")[0].decode("ascii")
    assert name and name not in names
    names.add(name)
    assert kind < 16 and variant < 36 and 0 < blocks <= 20000
    assert offset >= payload_offset and offset + blocks * 8 <= len(raw)
    ranges.append((offset, offset + blocks * 8))
    chunk = raw[offset:offset + blocks * 8]
    species[kind].add(sha256(chunk).digest())
    entries[kind] += 1

ranges.sort()
assert all(a[1] == b[0] for a, b in zip(ranges, ranges[1:]))
assert ranges[0][0] == payload_offset and ranges[-1][1] == len(raw)
assert set(entries) == set(range(16))
assert all(entries[k] == 36 and len(species[k]) == 36 for k in range(16))
print(f"TREE v1 passed: {count} valid, unique variants for 16 species ({len(raw)} bytes)")
