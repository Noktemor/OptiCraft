OptiCraft — экспериментальный гибридный GPU worldgen (этап 1)

Новые файлы:
  src/world/generation/Gpu_Worldgen.h/.cpp
  src/world/generation/Gpu_Worldgen_Data.h
  assets/shaders/worldgen_material.comp
  tools/tests/test_gpu_worldgen_parity.py
  documentation/GPU_WORLDGEN_HYBRID.md

Изменены:
  src/world/generation/World_Generator.h/.cpp
  src/world/chunks/Chunk_Manager.h/.cpp
  CMakeLists.txt

Запуск:
  по умолчанию — AUTO (по 2 дальних подземных Y-чанка за пакет);
  OPTICRAFT_GPU_WORLDGEN=off    — CPU-only;
  OPTICRAFT_GPU_WORLDGEN=force  — GPU без лимита автоотключения по времени.

Проверка:
  python3 tools/tests/test_worldgen_fastpaths.py
  python3 tools/tests/test_gpu_worldgen_parity.py
  GPU_WORLDGEN_FULL=1 python3 tools/tests/test_gpu_worldgen_parity.py

Перед заменой сохраните текущий проект/закоммитьте его в Git.
Подробнее: documentation/GPU_WORLDGEN_HYBRID.md
