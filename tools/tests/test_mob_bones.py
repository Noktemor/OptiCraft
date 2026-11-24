#!/usr/bin/env python3
"""Offline GLB skeleton-name validation for regional mob hitboxes."""
from pathlib import Path
import json
import struct

ROOT = Path(__file__).resolve().parents[2]
MODELS = ("pig", "cow", "sheep", "zombie", "skeleton", "knight", "spider")
for model in MODELS:
    data = (ROOT / "assets/mobs" / (model + ".glb")).read_bytes()
    assert data[:4] == b"glTF"
    length, tag = struct.unpack_from("<II", data, 12)
    assert tag == 0x4E4F534A
    doc = json.loads(data[20:20 + length].decode("utf8"))
    nodes = doc["nodes"]
    assert doc.get("skins"), model + ": missing skeleton"
    names = {nodes[i].get("name", "").lower() for i in doc["skins"][0]["joints"]}
    assert any("head" in n for n in names), model + ": head joint"
    assert any("leg" in n for n in names), model + ": leg joint"
    assert any("body" in n or "torso" in n or "pelvis" in n for n in names), model + ": torso joint"
    print(model, "bones:", len(names), "OK")
print("Mob GLB skeleton checks passed")
