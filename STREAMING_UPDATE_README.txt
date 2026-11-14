OptiCraft streaming/worldgen fast paths — 14.11.2026

Included: latest full enchanting version + atlas deduplication replacement + Mobs_Update compile fix.
Changes to C++ worldgen are limited to:
  src/world/chunks/Chunk_Manager.cpp
  src/world/generation/World_Generator.cpp
  src/world/generation/World_Generator.h

Test script: tools/tests/test_worldgen_fastpaths.py
Explanation: documentation/STREAMING_FASTPATHS_2026_11.md

Run test: python3 tools/tests/test_worldgen_fastpaths.py

IMPORTANT: This archive does NOT contain GPU-based OpenSimplex2S generation yet.
This first performance pass retains CPU generation and reduces repeated work.
It does not alter save format or chunk scheduling, and GPU meshing is preserved.
Full CLion build / GPU runtime FPS not tested in this container.
