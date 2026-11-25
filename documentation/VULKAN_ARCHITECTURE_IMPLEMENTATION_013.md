# OptiCraft 0.13 — Vulkan render: этапы 2–8

## Статус

**Сделано:** расширен compute-бэкенд Stage 1 отдельной фазой безопасной публикации мешей, GPU frustum culling, диагностическим graphics pipeline (vertex pulling), точным учётом владения GPU-диапазонами и линейным timeline retirement. `VERSION` и CMake обновлены на 0.13, история версий обновлена в существующем `documentation/history_version.md`. OpenGL остаётся рабочим путём приложения.

**Протестировано:** независимые C++20 core+frustum unit tests; Python source/asset regression contracts из 0.12.x. Запуск Vulkan, компиляция SPIR-V, визуальная проверка, swapchain и интеграция с Game не проверялись. Среда без Vulkan SDK, glslangValidator и GLFW/GLM dev headers.

**Осталось:** device/swapchain/frame scheduling/descriptor lifetimes, полный material+shape GPU mesher, LOD, GPU worldgen, GPU-only persistent saving, lighting, GUI и аппаратное тестирование.

## Новые файлы

- `src/rendering/vulkan/GpuFrustum.{h,cpp}`: извлечение шести плоскостей из column-major матрицы Vulkan с clip Z=0..W. Матрица рассчитывается только при изменении камеры; внутри каждого кадра GPU самостоятельно проверяет границы чанков.
- `assets/vulkan_shaders/voxel_publish.comp`: по подтверждённой таблице кандидатов коммитит indirect-команды стабильных чанков. Считать `voxel_emit.comp`/`voxel_finalize.comp` публикацией **запрещено**.
- `assets/vulkan_shaders/voxel_cull.comp` и `VulkanGpuCuller.{h,cpp}`: отдельный GPU compute dispatch с шестью clip planes и проверкой AABB чанков 32³, выпускает `VkDrawIndirectCommand` в кадровый выходной буфер.
- `VulkanVoxelPipeline.{h,cpp}`: диагностический graphics pipeline: shader modules из SPIR-V, pipeline layout с тем же descriptor set, что у GPU mesher, depth test, vertex pulling, динамические viewport/scissor. Материалы/атлас, солнце и динамические огни на этом этапе отсутствуют.

## Корректная последовательность кадра

1. Streaming scheduler передаёт блоки CPU-authoritative чанков только при появлении/изменении; GPU-generated дальние чанки в этом этапе **ещё не готовы**. GPU Block Arena хранит исходный массив резидентных чанков.
2. После загрузки данных и transfer→compute barrier `VulkanGpuMesher::recordCount` считает число видимых граней в пакете. Результат копируется в readback staging и асинхронно завершается timeline semaphore. Никаких ожиданий в render loop.
3. CPU по завершённому readback выделяет под candidate ровно нужную область в `Geometry Arena` и обновляет per-job записи слотов. `recordEmit` сбрасывает counters, строит packed faces и записывает `candidateDraws[batchIndex]` (новый buffer binding 7). Действующий `publishedDraws[slot]` не изменяется.
4. После завершения Emit и проверки `count <= candidateCapacity` и актуального `(slot,generation,version)` caller вызывает `recordPublish` с подтверждённым батчем. Перед вызовом необходимо установить и синхронизировать `slotIndices[]` и `candidateDraws[]` **именно для выбранных кандидатов**. Если хоть одна запись невалидна — исключить её из батча публикации; при переполнении сохранить старый меш. GPU timeline должен защищать старый и новый ranges.
5. `VulkanGpuCuller::record` вычисляет видимость текущего набора резидентных слотов, копируя опубликованную indirect-команду в **кадровый** compact-indexed массив draw-команд. Этот массив нельзя перезаписывать, пока GPU ещё использует frame slot. Классификация AABB консервативная: может оставить лишний чанк, но не должна отрезать пересекающий frustum.
6. После compute→draw-indirect barrier и начала совместимого render pass `VulkanParallelDraw::record` создаёт один или несколько secondary command buffers через независимые VkCommandPool и рисует массив из этапа 5. Для каждого secondary буфера устанавливаются динамические viewport/scissor. Caller выполняет `vkCmdExecuteCommands` в primary (beginRenderPass с `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`).
7. Frame fences/timeline защищают `frameSlot` и per-worker VkCommandPool до reset. Нельзя использовать `vkDeviceWaitIdle()` при стриминге. Destroy pipeline/buffers только после завершения использующих их GPU команд.

## Контракты ABI

- `GpuMesherBuffers`: **8 storage buffers** (было 7), новый binding 7 — candidate indirect array размером `maxJobs * 16` байт. SPIR-V `voxel_publish.comp` обязателен; `VulkanGpuMesher::initialize` теперь принимает четыре compute shader modules.
- `VulkanCullerBuffers`: `chunkSlotBuffer`, `publishedIndirectBuffer`, `liveSlotIndicesBuffer`, `frameVisibleDrawBuffer`. Последний создаётся с `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`, отдельным на каждый in-flight frame.
- `GpuFrustum`: 6 `vec4` / 96 байт, push constants 112 байт включая `[firstJob,totalJobs,pad0,pad1]`. `VulkanGpuCuller` требует Vulkan push-constant limit >= 112 bytes (Vulkan 1.0 baseline >=128).
- Активный draw command — GPU-owned (vertex count 6, instance count N, firstVertex 0, firstInstance = packed face base). Включить `drawIndirectFirstInstance` при создании VkDevice. При отсутствии требуется fallback; его нет в текущем этапе.
- `VulkanVoxelPipeline` использует descriptor layout мешера, shader push constant `mat4` (= 64 байта), 1 color attachment и совместимый depth. Pipeline не создаёт render pass и swapchain.
- `VulkanParallelDraw::record` теперь принимает `VkExtent2D viewportExtent` для корректных dynamic viewport/scissor внутри secondary buffers.

## Известные ограничения

1. `VulkanGpuMesher` по-прежнему формирует **отдельные грани простых непрозрачных кубов**. Это не GPU greedy meshing и не окончательный дизайн материалов/LOD; подключение к Game до покрытия сложных форм приведёт к пропавшим блокам.
2. Ключевой GPU worldgen не завершён: старые генераторы содержат биомы, пещеры, структуры, реки и размерные различия, их нужно перенести с проверкой одинакового результата CPU/GPU. Нельзя выдавать только поверхность за полноценный дальний чанк.
3. Новый Vulkan graphics pipeline — диагностический. Реальный Window/Present/FrameGraph ещё отсутствует, поэтому `OptiCraft` запускается на OpenGL.
4. Frustum culling — не Hi-Z occlusion. `instanceCount=0` у невидимой команды не означает, что её command slot полностью исключён из indirect batch; дальнейшая GPU-компактизация возможна после профилирования.
5. В прежнем Stage 1 есть слабый контракт на передачу origin/дополнительных block metadata. При переходе к сохраняемым GPU-only чанкам необходим более строгий readback/versioned save protocol.
6. Без Vulkan headers/loader и glslangValidator здесь не выполнялась ни сборка backend, ни валидация шейдеров. В CLion сначала собрать target `OptiCraft_Vulkan_Shaders`, затем `OptiCraft_VulkanBackend` и проверить shader-validation ошибки.

## Проверка

```bash
tools/tests/run_vulkan_core_checks.sh
python3 tools/tests/test_vulkan_013_contracts.py
python3 tools/tests/test_celestial_0122_contracts.py
python3 tools/tests/test_boss_combat_0122_contracts.py
python3 tools/tests/test_worldgen_fastpaths.py
# На машине с Vulkan SDK:
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target OptiCraft_Vulkan_Shaders OptiCraft_VulkanBackend OptiCraft_Vulkan_Core_Tests
ctest --test-dir cmake-build-release -R VulkanWorldCore --output-on-failure
```

---

# Этап 3: Vulkan window / swapchain / frame runtime (2026-11-24)

**Сделано:** `VulkanFrameRuntime.{h,cpp}` и отдельный `OptiCraft_Vulkan` executable (`VulkanProbe.cpp`). Он открывает настоящее Vulkan окно без OpenGL контекста, выбирает GPU с Vulkan 1.2, graphics+compute+present queue family, `VK_KHR_swapchain`, `drawIndirectFirstInstance`; настраивает цветные swapchain images и отдельную depth attachment для каждого изображения. Кадровые primary buffers и command pools создаются заранее; 2 кадра в полёте. `vkAcquireNextImageKHR` → запись команд (в том числе compute перед pass) → `vkQueueSubmit` → `vkQueuePresentKHR`. `VkFence` защищает повторное использование per-frame pools и изображений, а semaphores `presentReady_` привязаны **к изображению**, не к frame index. Нет per-draw VkCreate*/VkAllocate*, нет `vkDeviceWaitIdle` в обычном кадре.

**Демонстрационный путь:** отдельная библиотека `OptiCraft_VulkanWindow` без зависимости от компиляции mesh shaders; clear-only rendering (не фейковые блоки, не OpenGL). Он нужен для аппаратного тестирования window/surface/present/depth synchronization до подключения настоящих GPU chunk pipelines. `VulkanParallelDraw` можно выполнить в render pass, передав `recordSecondaryDraws=true` в `beginColorPass`; при этом нельзя смешивать INLINE-команды и secondary draws в одном subpass. Pipeline графики должен использовать `hasDepth=true` и корректный render pass. Culling/meshing после этого подключаются через предоставленный primary command buffer и GPU buffers с необходимыми barriers.

**Запуск в CLion (Linux):**

```bash
# Нужны Vulkan SDK headers/loader, GLFW dev headers, OpenGL dev для старого Game,
# GLM и glslangValidator для .spv (они в этой среде отсутствовали).
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target OptiCraft_Vulkan
./cmake-build-release/OptiCraft_Vulkan --integrated --validation
# Проверить без GUI после N отрисованных кадров (нужен дисплей):
./cmake-build-release/OptiCraft_Vulkan --frames 180
```

**При тестировании:** проверить на интегрированной и дискретной графике, изменение размера окна, сворачивание/восстановление, закрытие, `--frames`, валидацию, быстрое переключение между окнами. Если iGPU не поддерживает present на данном дисплее/драйвере, `--integrated` должен сообщить ошибку вместо молчаливой подмены видеокарты.

**Протестировано:** `tools/tests/run_vulkan_core_checks.sh`, `test_vulkan_013_contracts.py`, `test_vulkan_013_stage3_contracts.py`, тест генератора мира (130277 сценариев), boss/celestial contracts. На данной машине CMake-конфигурация полного проекта блокируется отсутствием OpenGL dev headers; компиляция `VulkanFrameRuntime.cpp`, SPIR-V и runtime-проверка на GPU **не выполнялись**.

**Осталось:** GPU buffer allocations, staging ring, mesher GPU job submission + readback/versioned publication, culler+vertex pulling drawing of chunks, карты материалов/текстуры, GUI, полноценные GPU-generated дальние чанки и переход CPU/GPU-ownership. Ни один из этих пунктов не отмечен как готовый на этом этапе.


## Stage 4 (0.13, 2026-11-24) — Game voxel stream + all placed block categories

**Сделано:**
- Реальный `Game::render_gameplay` получает renderable список один раз, передаёт его `VulkanGameChunkFeed::acceptFrame`, и только при подключённом Vulkan sink извлекает блоки/metadata/light из чанка. Неизменённые экземпляры пропускаются по instance ID и ревизиям. При выгрузке, смене/закрытии мира записи инвалидируются. Feed — источник данных будущего Vulkan резидентного мира; GPU-мешинг в CPU-слое отсутствует.
- Атласы поддерживают headless-загрузку размеров и UV без вызовов GL (`load_all(..., false)`).
- Каталог GPU-визуализации из реального `Block_Registry` охватывает все размещаемые блоки, все 256 metadata-вариантов при необходимости, 33 shape ID и 256 shape metadata. Shape boxes берутся из общего `shape_boxes()` и переводятся в 1/64. Отдельная `opaqueMesherFlags` конвертация между несопоставимыми enum-битами.
- В `voxel_misc_*` compute shaders добавлена генерация граней прозрачных кубов, растений, жидкостей и shaped blocks, включая вычисление соседских соединений лестниц, заборов, стен, стёкол-панелей и медных проводов. Count/emit использует две проходные стадии и лимиты записи. CPU не создаёт для этих блоков вершины.
- `VulkanMiscMesher` создает descriptor/pipeline для новых compute shaders. `VulkanMiscPipeline` строит отдельные графические pipeline для cutout (depth writes) и transparent (alpha blending, без depth writes), читает GPU quad buffer и подаёт batched indirect draws. Данные атласа должны поставляться отдельным VkDescriptorSet.

**Протестировано:**
- `bash tools/tests/run_vulkan_catalog_checks.sh` — PASS.
- `bash tools/tests/run_vulkan_core_checks.sh` — PASS.
- `python3 tools/tests/test_vulkan_013_stage4_asset_coverage.py` — PASS: 534 ID, в том числе 106 ordinary, 80 flora, 2 liquids, 231 shaped. Эти числа — определённые в assets типы, не результат GPU рендеринга.
- `python3 tools/tests/test_vulkan_013_contracts.py`, `python3 tools/tests/test_vulkan_013_stage3_contracts.py` — PASS.
- `python3 tools/tests/test_worldgen_fastpaths.py` — PASS: 130277.
- Нельзя здесь собрать Vulkan и SPIR-V и увидеть кадр: Vulkan SDK headers, GLFW/GLM dev и glslangValidator отсутствуют.

**Осталось (критические блокеры реальной игры):**
1. `Game` по-прежнему создает OpenGL-контекст и использует OpenGL renderer, HUD и mob models. `Game::set_vulkan_chunk_sink` предоставляет соединение, но Vulkan не зарегистрирован как активный backend: реальной выдачи GPU пока нет.
2. Vulkan GPU arena residency, staging upload, дескрипторы `VulkanMiscMesher`, атлас `VkImage`/sampler, indirect-buffer command publisher ещё не прикручены к `VulkanFrameRuntime`/Game.
3. Непрозрачные кубы в Stage 2 по-прежнему рисуются диагностическими цветами, не UV-атласом. Реализация water corners, корректная сортировка прозрачных поверхностей, освещение и некоторые хитрые грани форм требуют проверки соответствия OpenGL.
4. Мобы, боссы, HUD, инвентарь, sky, particles и прочие OpenGL проходы должны быть перенесены до возможности переключить окно на `GLFW_NO_API`. GPU-only full far-worldgen/LOD и фоновое сохранение остаются отдельно.

Не использовать диагностический `OptiCraft_Vulkan` как доказательство готовности игровой Vulkan-версии. Стандартный `OptiCraft` остается рабочей OpenGL веткой.


---

# Этап 5: постоянное хранение и пакетные загрузки (2026-11-24)

## Сделано

- `VulkanChunkStreamPlan.{h,cpp}` — потокобезопасная очередь raw-чанков из `VulkanGameChunkFeed`. Дубликаты по `(dimension,x,y,z,instance,revision)` объединяются, старые ревизии игнорируются. Вместимость и количество отправок за кадр ограничены. Соседи по 6 направлениям помечаются для повторного GPU-мешинга. При занятости всех слотов обновления уже резидентных чанков получают приоритет над новыми. Защита от переполнения координат `ivec4(origin)` и от устаревших `generation`.
- `VulkanDeviceBuffer.{h,cpp}` — RAII для постоянных VkBuffer/VkDeviceMemory. Поддержка HOST_VISIBLE coherent и noncoherent flush, без вызовов `vkDeviceWaitIdle` и аллокаций на чанк.
- `VulkanChunkStreamGpu.{h,cpp}` — реальное хранение блоков (2×16 бит в uint32), meta и света (4×8 бит в uint32) в раздельных persistent device-local буферах; GPU слот-таблицы 64/96 байт, draw commands и dense live-slot list. Размер чанка фиксирован 32×32×32. По умолчанию 512 резидентных слотов (64 МиБ только на block/meta/light), 8 загрузок за кадр и 1024 ожидающих ключа; лимиты задаются при инициализации. Два persistent staging buffers соответствуют двум `VulkanFrameRuntime::kFramesInFlight`. Один командный пакет делает COPY для всех изменённых источников, transfer barriers и invalidate старых published draws при выгрузке/переиспользовании слота. Для обновления того же экземпляра старая геометрия остаётся до publish. Плотный список live slots копируется только при изменении набора резидентных чанков; обновление ревизии уже существующего слота не вызывает повторную полную отправку списка. В неизменном кадре `record` сразу возвращается без Vulkan-команд.
- `VulkanMaterialTables.{h,cpp}` — одноразовая загрузка shader flags, block descriptors, shape ranges, shape boxes и UV-вариантов по исходному `Block_Registry`. Использовать `opaqueFlags()` для `voxel_count.comp` (биты отличаются от VisualFlags). Staging материализуется до Fence успешной загрузки и затем освобождается вызывающим кодом.
- `VulkanBlockAtlas.{h,cpp}` — независимая от OpenGL загрузка текущего PNG в `VkImage` c 5 mip-уровнями для спрайтов 16×16. Каждый mip строится одним `vkCmdBlitImage` со списком независимых регионов спрайтов, никакой CPU-перепаковки пикселей; clamp maxLod=4. Предусмотрена проверка доступности SRGB linear blit. `voxel_misc.vert` конвертирует OpenGL UV в Vulkan UV, не отражая пиксели CPU. Дескриптор совмещён с `VulkanMiscPipeline::atlasSetLayout()`.
- `VulkanGameChunkFeed` — ограниченная пакетная сериализация с `maxPackets=16` по умолчанию. Не сериализует все тысячи новых чанков за один кадр. `VulkanChunkStreamGpu::uploadSink()/evictSink()` можно напрямую передать в уже существующий `Game::set_vulkan_chunk_sink(...)`.

## Порядок использования в будущем игровом Vulkan-кадре

1. Создать `VulkanFrameRuntime` и `VulkanChunkStreamGpu` с одинаковым числом кадров в полёте (2); загрузить `VulkanMaterialTables` и `VulkanBlockAtlas` **однократно** на старте. Удержать staging обеих статических загрузок до завершения их fence, затем освободить.
2. Только после этого подключить `Game::set_vulkan_chunk_sink(stream.uploadSink(), stream.evictSink())`. Снять callbacks до уничтожения `stream`.
3. В Vulkan primary command buffer, полученном через `beginFrame()`, вызвать `StreamBatch batch=stream.record(frame.cmd,frame.frameSlot)`. Возвращённые `remeshSlots` передать GPU meshing scheduler; `opaqueSlots()` и `miscSlots()` готовы для descriptor bindings 1 и 2 соответствующих compute-пайплайнов.
4. Для каждого dirty slot: Count на GPU → асинхронная проверка и выделение геометрии → `setOpaqueFaceRange`/`setMiscQuadRange` с актуальным generation → Emit → Validate → Publish; GPU frustum-culling и indirect drawing работают **после** публикации. Этот конвейер пока не связан автоматически с `record()` — caller обязателен.
5. Два graphics/compute устройства или отдельные очереди требуют явных queue-family ownership transfers и timeline; текущий streamer поддерживает **одну graphics+compute очередь** из Stage 3. Он корректно использует frame-slot fence для staging reuse, но не делает cross-queue sync.

## Протестировано

```bash
bash tools/tests/run_vulkan_core_checks.sh
bash tools/tests/run_vulkan_stage5_mock_checks.sh
bash tools/tests/run_vulkan_catalog_checks.sh
python3 tools/tests/test_vulkan_013_stage4_asset_coverage.py
python3 tools/tests/test_worldgen_fastpaths.py
```

`test_vulkan_stage5_mock.cpp` содержит **локальные имитации Vulkan-функций** и проверяет байтовую точность копирования, смещения, актуальность поколений и регионы всех blit. Это не Vulkan SDK/validation layers, не тест реальной shader compilation и не измерение FPS. На ПК с SDK обязательно собрать `OptiCraft_VulkanBackend` и шейдеры, запустить Validation Layers и проверить драйвер Intel и RTX.

## Осталось

Настоящее подключение основного `Game` к активному Vulkan-рендереру, GPU-мешинг всех категорий с публикацией indirect, готовые draw passes, GUI/мобы/небо и точная проверка кадров; полная GPU-генерация дальних чанков, LOD и надёжная readback→save очередь. Стадия 5 — работающий по структуре кода GPU upload/resource layer, **не завершённый Vulkan игровой рендерер**.


---

# Этап 6: GPU Count/Emit/Publish и подключение к живой Game (2026-11-24)

## Сделано

- `GpuMeshTransactions` — транзакционный планировщик count/readback/allocate/emit/verify/publish для стабильных слотов, защищающий от OOM/переполнения, устаревших ревизий и повторного использования ID; старый меш остаётся опубликованным до замены. Старые GPU-арены освобождаются после трёх успешно подтверждённых frame-slot fences. CPU обрабатывает **только количество граней и адреса аллокации**, а не создаёт геометрию.
- `VulkanChunkMeshing` — GPU buffer bindings и record команд реального Vulkan compute, раздельные opaque и misc quad arenas, staging счетчиков и indirect-команды. Перестраиваются только помеченные слоты; при отсутствии работ compute не отправляется. Текущий алгоритм ещё **не greedy**; размер 8 B/opaque face, 32 B/misc quad.
- `VulkanOpaqueMaterialPipeline` + `voxel_textured.vert/frag` — текстурированная отрисовка GPU PackedFace через vertex pulling. `VulkanMiscPipeline` — отдельные cutout/alpha passes (без сортировки геометрии). `VulkanGpuCuller` генерирует dense непрозрачные/сложные indirect-команды на GPU.
- `VulkanLiveWorldRenderer::recordFrame` — последовательность frame-fence wait → completed meshes → streaming uploads → mesher dispatch/publish → culling → color/depth pass → indirect drawing → present (вызывает владелец `VulkanFrameRuntime`). Все GPU-ресурсы и дескрипторы создаются в `initialize()`, а не каждый кадр.
- `VulkanGameMirror` — интеграция с фактическим `Game::render_gameplay`, одна игра, два независимых графических окна, без дублирования CPU-мешинга. Vulkan использует тот же вид и проекцию с преобразованием OpenGL NDC в Vulkan NDC. `Game` автоматически запускает Vulkan preview при наличии SDK+GLSL toolchain; при неудаче безопасно возвращается к GL. При закрытии preview или завершении игры callbacks снимаются **до** уничтожения GPU-ресурсов. В обычном режиме без Vulkan SDK остаётся прежний `OptiCraft` и те же игровой цикл/ресурсы.
- Live feed ограничен бюджетом 512 GPU-чанков; выбранные чанки отсортированы по расстоянию до камеры, при перемещении устаревшие освобождаются. Это временный бюджет для превью, **не** будущая полная GPU-генерация на расстоянии 48 чанков.

## Протестировано

`bash tools/tests/run_vulkan_stage6_checks.sh`, `bash tools/tests/run_vulkan_core_checks.sh`, `bash tools/tests/run_vulkan_catalog_checks.sh`, `bash tools/tests/run_vulkan_stage5_mock_checks.sh`; статические контракты предыдущих этапов и `test_worldgen_fastpaths.py` (130277 сценариев). Проверки изоляции и CPU-логики не эквивалентны сборке GLSL и Vulkan запуску. Реальная сборка заблокирована отсутствием OpenGL development headers, Vulkan SDK/GLFW headers/glslangValidator.

## Осталось

Настоящая аппаратная проверка на Intel iGPU и RTX 4060, валидация SPIR-V/descriptor ABI, сравнение картинки всех 534 block IDs (статическое покрытие каталога само по себе недостаточно), прозрачность и корректное визуальное отображение сложных граней, динамический свет, GPU greedy meshing, полный GPU worldgen + cave/structure parity, LOD, GPU-only save queue, sky/HUD/animations/mobs/bosses для отказа от OpenGL окна.

**Важно:** если Vulkan-компилятор шейдеров отсутствует, `OptiCraft_VulkanBackend` не подключается к основному target, а Vulkan preview не создаётся. Обновление остаётся **0.13 — Vulkan render**.


---

# Этап 7: GPU-объединение полос и освещение исходного мира (2026-11-24)

## Сделано

- `voxel_row_runs.glsl`: compute-алгоритм для непрозрачных кубов, 6 направлений × 32 × 32 полосы, каждая длиной до 32. Слияние разрешено только при одинаковых block ID/meta/переднем свете; геометрия и индексы строятся исключительно GPU. Шейдеры `voxel_count.comp` и `voxel_emit.comp` используют один код.
- `mesher_common.glsl`: 8-byte packed face с длиной 1..32 (битовое поле 18..22 второго слова), 16-битным stable slot ID; проверки вместимости для 65536 слотов. GPU memory/Count→Emit→Publish остаются прежними. Два новых SSBO bindings 8 metadata и 9 light у opaque GPU mesher; у misc mesher binding 10 packed light.
- `voxel_textured.vert/frag`: декодирование полосы и повторение UV внутри неё, выборка `textureGrad` с производными до `fract`, clamp на полтекселя внутри исходного тайла. Обычные и сложные блоки используют существующий блоковый+небесный свет, без CPU light reconstruction. Для обычных граней свет берётся из переднего вокселя, а на границе — из соседнего GPU-чанка.
- `CMakeLists.txt`: SPIR-V dependencies включают `voxel_row_runs.glsl`, иначе изменение общего шейдерного алгоритма не вызвало бы перекомпиляцию.

## Протестировано

- `bash tools/tests/run_vulkan_stage7_checks.sh` и `python3 tools/tests/test_worldgen_fastpaths.py` — автономные тесты пройдены.
- `tools/tests/test_vulkan_gpu_rows.py`: проверка обратного распаковывания GPU row strips и полного покрытия видимых граней на 5 видах секций, без дублирования/пропусков, с материалами 16 bit и различным metadata/light. Тест является эталонной проверкой логики на CPU, **не исполнением GLSL**.
- Запуск CMake конфигурации в этой среде остановился на отсутствующих OpenGL/GLX dev-библиотеках. Vulkan SDK / `glslangValidator` также отсутствуют. Реальная компиляция SPIR-V, Vulkan driver и Intel/RTX не проверены.

## Осталось

Графика в Vulkan всё ещё диагностическая: аппаратное подтверждение, точные текстуры/свет/AO для всех блоков, 2D greedy, прозрачность, динамические огни, HUD/небо/мобы/боссы и переход основного окна на Vulkan. Полноценный удалённый GPU worldgen с детерминированными пещерами/рудами/структурами, LOD (без перекрытия проходов) и readback/save GPU-only резидентов не реализованы. **Нельзя выдавать Stage 7 за законченную 0.13.**


## Текущий статус — этап 8 (заменяет ранние описания состояния)

**Сделано:** `Window` первым пытается создать скрытое GLFW_NO_API окно при доступном Vulkan; после подготовки legacy GL-ресурсов выполняется инициализация Vulkan device/swapchain, world и UI pipelines. При успехе показывается только Vulkan-окно, при неудаче — OpenGL. Ошибка Vulkan-кадра вызывает отсоединение GPU-подсистемы, освобождение Vulkan-ресурсов и переключение ввода на OpenGL без пересоздания мира. Скрытый GL-контекст пока обязателен для существующих игровых ресурсов; это не чистый Vulkan-only запуск. `OPTICRAFT_RENDERER=opengl` принудительно выбирает GL; `OPTICRAFT_VULKAN_GPU=integrated|discrete` и `OPTICRAFT_VULKAN_VALIDATION=1` служат для проверки устройства. Software CPU Vulkan не выбирается. Отдельный `OptiCraft_Vulkan` остаётся тестом swapchain.

Пакеты HUD (меню, текст, хотбар, иконки, инвентарь) передаются в нативный Vulkan UI graphics pipeline, объединяются по последовательным одинаковым атласам без нарушения порядка прозрачности; отдельный vertex buffer на каждый кадр. OpenGL-кадр не копируется в Vulkan. GPU-резидентность больше не зависит от GL mesh readiness или CPU frustum, исходные блоки отправляются по block revision/dirty epoch. Greedy compute теперь объединяет прямоугольники 32×32 с одинаковыми ID/meta/light, формат остаётся 8 байт; 192 workgroups на чанк, 64 lanes на плоскость. Реальная производительность не измерена.

**Протестировано:** CPU reference пяти сцен (включая сплошной чанк: 6144 грани → 6 прямоугольников), C++20 UI batching, core/stream, mesh transactions, fake Vulkan, контракты Stage 2–6 и каталог блоков, регрессии боссов, 130277 worldgen сценариев. CMake не завершает конфигурацию из-за отсутствующих OpenGL dev-зависимостей; SPIR-V и работа Vulkan на драйвере не проверены.

**Осталось:** аппаратная сборка и проверка всех состояний блоков, перенос GL-зависимых ресурсов, мобов/боссов, выпавших предметов, preview игрока, неба и постэффектов; AO, динамические источники света, прозрачность, полный GPU-worldgen и GPU-only сохранение, геометрический LOD и два радиуса. GL-compatible подготовка мешей ещё присутствует в Chunk_Manager. Текущий лимит GPU-резидентности 512 чанков. Для полноценного survival до переноса сущностей использовать OpenGL.



## Stage 9 — streaming hotfix

См. актуальную запись этапа 9 в `history_version.md`. Vulkan raw residency: до 2048 слотов, до 256 МиБ raw storage. При активном Vulkan нет GL mesh jobs / GL worldgen dispatch; при fallback восстанавливается штатная очередь OpenGL. Count/Emit/Publish работают на стабильном наборе источников; измерение GPU timestamp выполняется только после fence, без WAIT. GPU mesh cost budget: интегрированная графика 16 → 4–32, дискретная 32 → 4–64 условных единиц. Один плотный чанк неделим; целевое время 2.5 мс не является гарантией. Диагностика `[FPSCOUNTER]` отображает успешные present и Vulkan queues.


## Stage 10 — selective dispatch, exact boundaries, sparse air residency

**Сделано:** `MeshDispatchRuns.h` планирует диапазоны работ без изменения индексов Count/Emit. 6 групп для полностью непрозрачного чанка, 192 для обычного opaque, 512 для misc только при наличии таких блоков. `VulkanChunkStreamPlan` хранит точные границы ID/meta/light (24 КиБ/слот) и инвалидирует только затронутых соседей. `MeshResidency.h` исключает удалённый воздух без геометрических соседей. `GpuMeshTransactions` повторяет allocation failures после retirement и поддерживает O(1) pending count. При очереди >= 2×batchLimit приём новых upload приостанавливается до её сокращения.

**Протестировано:** см. этап 10 в `history_version.md`. Автономные тесты не заменяют Vulkan/SPIR-V проверку.

**Осталось:** Vulkan GPU-worldgen, LOD, сущности и визуальные эффекты; текущий бюджет raw slots и fallback не изменены.


## Stage 11 — compact opaque draws, atmosphere and early pass rejection

**Сделано:** опциональный `drawIndirectCount` Vulkan 1.2; frame-local counters и GPU append только видимых opaque draws, fixed indirect fallback. Misc сохраняет прежний порядок (без depth sort). `WorldViewParams` 112 B, одинаковый layout vertex/fragment, ambient для skylight, туман с цветом неба. Specialization constant ALPHA_PASS в vertex/fragment отсекает неподходящий класс до растеризации. Исправлена сигнатура `VulkanMiscPipeline::recordDraws`. Лог показывает выбранный GPU и indirect режим.

**Протестировано:** `test_vulkan_stage11.py`, C++ layout, предыдущие core/stream, mesh transactions, mock, greedy, catalog, source contracts, 130277 worldgen cases. Аппаратное исполнение/GLSL не проверено.

**Осталось:** см. этап 11 в `history_version.md`.


## Stage 12 — entity and item passes, chunk depth sort, near AO

**Сделано:** см. этап 12 в `history_version.md`. Порядок кадра: streaming → mesh → GPU frustum/opaque compaction + misc depth sort → opaque/cutout → entities/items → alpha → HUD. Сортировка: 64 lanes, padded power-of-two до 2048, 128 B push constants, 16 KiB shared keys/IDs; opaque culler прежний. Сущности: 64 B vertex, 4176 B pose, статические VB/IB и изображения на модель, frame-local SSBO поз; staging текстур освобождается после frame fence. AO: выборка original block words в fragment, чтение axial neighbor links; добавлены fragment stages в stream barriers. Солнечный вектор в ранее резервных компонентах `WorldViewParams::fogEnd`; размер view остаётся 112 B.

**Протестировано:** `python3 tools/tests/test_vulkan_stage12.py`, `g++ -std=c++20 -Isrc tools/tests/test_vulkan_entity_abi.cpp -o /tmp/oc_entity_abi && /tmp/oc_entity_abi`, `run_vulkan_stage7_checks.sh`, `run_vulkan_stage5_mock_checks.sh` (расширен raw RGBA), `test_vulkan_stage11.py`, `test_vulkan_primary_window.py`, `test_mob_bones.py`, `test_boss_combat_0122_contracts.py`, `test_celestial_0122_contracts.py`, `test_worldgen_fastpaths.py`. Без аппаратной валидации.

**Осталось:** Hi-Z, GPU worldgen, LOD, per-quad alpha, AO сложных форм, динамические источники и тени, preview и эффекты; производительность и картинка на драйвере.


## Stage 13 — current-frame hierarchical depth

**Сделано:** см. этап 13 в `history_version.md`. `VulkanFrameRuntime` создаёт совместимые clear/store и load/present render passes для sampled depth. `VulkanHiZ` владеет frame-local pyramid SSBO и descriptor set. Normal depth (near=0, far=1), max reduction (8×8 base / ceil 2×2 reduction), консервативная проверка полного прямоугольника проекции AABB; до четырёх ячеек выбранного уровня. Используется глубина именно текущей камеры, без history reprojection/readback. Near opaque pass и far opaque pass не пересекаются по центрам чанков. Остальные проходы сохраняют прежний порядок. `OPTICRAFT_VULKAN_HIZ=0` отключает этот путь, не меняя выбор Vulkan.

**Протестировано:** `python3 tools/tests/test_vulkan_stage13_hiz.py`; `g++ -std=c++20 -fsyntax-only -Wall -Wextra -Werror -Itools/tests/fake_vulkan -Isrc src/rendering/vulkan/VulkanHiZ.cpp src/rendering/vulkan/VulkanGpuCuller.cpp`; `run_vulkan_stage7_checks.sh`, `run_vulkan_core_checks.sh`, `test_vulkan_primary_window.py`, `test_vulkan_stage11.py`, `test_vulkan_stage12.py`, `test_worldgen_fastpaths.py`. Реальная сборка и аппаратная проверка недоступны в текущей среде.

**Осталось:** native GPU worldgen, geometric LOD, per-quad transparency, dynamic colored lights/shadows, AO сложных форм, preview/эффекты и замеры Hi-Z. Текущий Hi-Z не уменьшает исходную GPU residency и не ускоряет генерацию/мешинг напрямую.

### Этап 14 — Vulkan material worldgen / surface LOD / alpha / lighting (2026-11-24)

**Сделано:** нативный Vulkan FP64 compute для глубоких материалов по существующим OpenSimplex2S таблицам. Подключён к очереди `Chunk_Manager`, дальность от 8 чанков, не более двух глубинных уровней за колонку; fence опрашивается без блокировки, результат проходит существующую выборочную CPU-проверку. При отсутствии FP64, ошибке или превышении GPU-бюджета — CPU fallback. Hi-Z текущего кадра применяется также к misc/alpha с консервативно расширенным AABB. GPU alpha sort выделяет blended quads и сортирует центры внутри чанка: exact bitonic до 1024 граней, 32 depth bins для больших мешей. Сохраняется межчанковый порядок. LOD1 непрозрачных поверхностей от 10 чанков (возврат при <8): прямоугольники объединяются без ограничения одинакового материала/света, fragment shader восстанавливает ID/meta/UV/light из исходного вокселя. Силуэт и границы остаются на исходной сетке; ABI 8 байт. Добавлены ближнее приближённое AO для непрозрачных сложных форм и цветной локальный свет от выбранного светящегося блока в voxel/entity/item shaders. Vulkan-first и OpenGL fallback сохранены. `OPTICRAFT_VULKAN_LOD=0` отключает LOD1.

**Протестировано:** C++ core/stream, LOD hysteresis и версии, greedy/transactions, mock upload, каталог 534 ID, Stage 2–13, worldgen 130277 сценариев, оригинальный OpenGL worldgen parity (4 seed × 64 samples). 11 изменённых Vulkan GLSL shaders компилируются после адаптации к headless EGL OpenGL; исправлено зарезервированное имя `packed`. Адаптированный alpha compute выполнен на EGL в 6 сценариях (0–2048 граней), включая exact и bucket sort. Это не Vulkan SPIR-V или аппаратный тест.

**Осталось:** полноценная Vulkan-компиляция/validation и измерения Intel/RTX. Поверхность, пещеры, структуры, финализация и сохранение GPU-only чанков остаются CPU; FP64 материал-стадия не является полным GPU worldgen. LOD1 не уменьшает разрешение воксельной сетки и не увеличивает лимит исходных GPU-слотов. Прозрачность приближённая между чанками и для больших мешей; нет глобальной точной сортировки. AO ограничено ближней зоной, цветной динамический свет только от выбранного блока, без теней и произвольных источников. Визуальный перенос моделей и интерфейса не подтверждён аппаратно.


### Этап 15 — Vulkan performance diagnostics / selective depth sort (2026-11-24)

**Сделано:** misc culling компактизирует непустые видимые команды перед bitonic sort; порядок по глубине и ID сохраняется. При неизменной VP и ревизии опубликованной геометрии, без Hi-Z, frame-local misc/alpha commands переиспользуются; при изменении мира, камеры, resize или режиме Hi-Z кэш не используется. Early fragment tests для непрозрачного textured pass. Начальный порог Hi-Z на iGPU — 1024 резидента, на остальных устройствах — 256; `OPTICRAFT_VULKAN_HIZ_MIN_RESIDENT` (1–2048) позволяет сравнить пороги, `OPTICRAFT_VULKAN_HIZ=0` отключает Hi-Z. Консоль: фактический present mode, GPU total/cull/alpha/hiz, CPU update/render, misc reuse; timestamps читаются только после fence, без WAIT. Время стадий диагностическое, возможны перекрытие конвейера и задержки очереди; CPU render включает ожидания Vulkan. Качество, мир, LOD, Vulkan-first и GL fallback сохранены.

**Протестировано:** 28 сценариев исполнения адаптированного misc culler в Mesa EGL, 6 alpha сценариев, C++ cache smoke, core/stream, greedy/transactions, mock upload, catalog 534 ID, Stage 2–13, primary window, 130277 worldgen сценариев. Исправлены устаревшие строковые проверки Stage 7/13. EGL не заменяет Vulkan SPIR-V и аппаратное тестирование.

**Осталось:** измерить GPU и CPU на целевой Intel в Release с одинаковой дальностью/разрешением; сравнить Hi-Z, LOD и GPU worldgen по одному. Порог 1024 эвристический, реальный выигрыш FPS не измерен. Полная Vulkan-сборка здесь не подтверждена.



## Stage 16 — аудит горячих путей и устранение части сериализации

### Подтверждённые исходниками ограничения Stage 15

| Файл / функция | Результат аудита | Состояние Stage 16 |
|---|---|---|
| `VulkanChunkMeshing::beginFrame/record/acceptsUploads` | Один глобальный Count→readback→Allocate→Emit→readback→Publish, при ожидании блокируются новые uploads; Publish занимает кадр | Два батча, один тяжёлый dispatch-проход в кадр; публикация совмещается с Count/Emit. Readback остаётся |
| `GpuMeshTransactions::startCount/finishCount/verifyEmit` | Единственная глобальная фаза; CPU выделяет точные диапазоны; revision проверялась при Count/Publish, но перед Emit проверялось только generation | Общая arena, независимые фазы, защита слота от повторного батча; revision проверяется перед Emit |
| `voxel_row_runs.glsl::buildPlane` | Lane 0 последовательно сравнивает/обнуляет клетки каждого прямоугольника, Count повторяет работу Emit | Параллельные 32-bit row masks; точная вертикальная greedy-сборка по-прежнему lane 0, Count без записи прямоугольников |
| `VulkanChunkStreamGpu::record` | При LOD-only изменении повторно отправляются block/meta/light | Пропуск GPU-копии 128 КиБ/чанк при совпадении instance/source revisions/границ. CPU snapshot по-прежнему формируется |
| `VulkanFrameRuntime::beginFrame/endFrame` | Один graphics+compute queue; штатные frame/image fences; device-idle при resize/shutdown; VSync по умолчанию false, фактический present mode выводится | WSI не изменялся. Отдельный compute queue не внедрён |
| `VulkanMaterialWorldgen`, `Chunk_Manager` | Материалы GPU→CPU, затем полная финализация/передача на GPU; отдельный submit на той же queue | Не GPU-only worldgen; общий GPU time scheduler ещё не реализован |
| `VulkanMiscPipeline`, `voxel_misc.vert`, `VulkanAlphaSort` | Blended геометрия отсеивается в non-alpha vertex shader; alpha имеет отдельный индексный список | Полное разделение геометрических арен cutout/blended не выполнено |
| `voxel_textured.frag`, `voxel_misc.frag` | AO и LOD обращаются к исходным вокселям в fragment shader | Не изменено |
| `VulkanGameChunkFeed::acceptFrame` | Unpack/pack исходного снапшота на CPU; ограничение 2048 резидентов. Нативный GL mesh jobs/worldgen при активном Vulkan выключены за исключением совместимых capture paths | Формат/лимит сохранены; hidden GL context остаётся |

### Новый путь

Состояние батча: Idle → Count submitted → fence → allocation → Emit submitted → fence → validate/publish → Idle. `GpuMeshTransactions` хранит две независимые группы jobs, но только один resident record и общие RangeArena для всех слотов. `VulkanChunkMeshing` владеет отдельными scratch/readback/descriptor sets каждого батча, graphics читает общие геометрические арены. Пример при достаточной очереди без инвалидирования: Count A, Count B, Emit A, Emit B, Publish A + Count A, Publish B + Count B. Не гарантируется, что каждый кадр содержит dispatch: лимит нагрузки, fences, устаревшие задачи и нехватка памяти могут оставить кадр без работы. На graphics queue задачи выполняются последовательно; независимые CPU/GPU транзакции не означают параллельное выполнение физических вычислительных блоков.

При новой ревизии in-flight slot остаётся заблокированным для новой транзакции до освобождения старой. Ревизия ставится в pending; после fence неактуальные диапазоны отбрасываются. Проверка поколения и ревизии перед Emit исключает запуск устаревших задач. После Emit устаревший кандидат не публикуется. Old draw остаётся до корректной публикации; при реальной выгрузке/замене инстанса или доказанно пустом/закрытом чанке draw обнуляется. При OOM повторная попытка выполняется после освобождения памяти. Новый batch не выделяет дополнительную геометрическую арену.

`OPTICRAFT_VULKAN_MESH_BATCHES=1` — сравнительный режим с одним scratch batch; не полный откат Stage 15. По умолчанию 2. Лог `mesh Q ... flight ...` разделяет ожидающие и активные записи. Обе фазы используют существующий timestamp budget; Count и Emit по-прежнему требуют двух readback. Для устранения readback нужен отдельный проверенный GPU allocation/publication protocol, а для оценки его выгодности — распределение размеров мешей и измерение на целевых устройствах.

### Проверки и ограничения

См. Stage 16 в `history_version.md`. `tools/tests/test_vulkan_stage16_mesher.py` исполняет адаптированные GLSL Count/Emit в EGL и сравнивает packed records с прежним точным CPU greedy, включая neighbor light, metadata, 16-bit material ID, LOD и full-width masks. Не является Vulkan/SPIR-V проверкой. `run_vulkan_stage6_checks.sh` проверяет транзакции под UBSan и синтаксис интеграции на fake API. Debug и Release CMake не прошли конфигурацию: отсутствуют OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR; Vulkan development headers, glslangValidator и spirv-val недоступны. Не измерены GPU frametime/FPS, 1% low, реальная queue latency или скорость генерации на Intel/RTX; критерий превосходства над OpenGL пока не подтверждён. Следующий P0: реальная Vulkan validation, устранение обязательных count/emit readback, повторного обхода вокселей и расширение GPU workload scheduler на worldgen/uploads.

## Stage 17 — ограниченный кэш результатов opaque Count

Каждый из двух scratch batches владеет собственным SSBO binding 10. Для N jobs и capacity C буфер содержит N uint headers и N сегментов по C записей uvec2. Индекс сегмента — batch job index, а не stable chunk slot. `voxel_count.comp` сохраняет packed rectangles по точному atomic reservation, ограничивая запись C; счётчик продолжает учитывать все прямоугольники. На Emit `vkCmdCopyBuffer` сохраняет Count counters в headers до `vkCmdFillBuffer`, с зависимостями compute-write→transfer-read/compute-read и transfer-read/write→transfer-write→compute-read/write. CPU использует уже прочитанный count для выбора 8-group cache-copy dispatch либо прежнего 192/6-group plane dispatch. Cache-copy сравнивает сохранённый GPU count с выделенной capacity, пишет sentinel при расхождении. GPU Count/Emit readback и проверки поколения/ревизии остаются; candidate publication выполняется прежним протоколом. Переполнение не сокращает меш. C=8192 по умолчанию, 16 jobs × 2 batches дают 2097280 байт дополнительного device storage. `OPTICRAFT_VULKAN_MESH_CACHE=0` — диагностический uncached path. Кэш добавляет запись и shared memory в Count; выигрыш зависит от распределения размеров мешей и GPU, не измерен.

`MeshWorkBudget::sample` получает фазу по завершённому frame slot: быстрый Emit не увеличивает Count budget, медленный Emit может его уменьшить. `VulkanChunkStreamPlan::take` фильтрует устаревшие snapshots до частичной сортировки, кэширует priority/resident для comparator, сохраняет bounded pool и порядок по ключу. См. тесты и ограничения в history_version.md. Для patch rebuild обновить timestamps изменённых исходников и шейдеров; CMake shader dependencies включают mesher_common.glsl и voxel_row_runs.glsl.

## Stage 18 — bounded GPU suballocation для opaque-only batches

`VulkanChunkMeshing::record` выбирает fast path только если у всех jobs `meshKinds & 3 == 1`, cache включён, ревизии не помечены `forceLegacy` и `GpuMeshTransactions::reserveFastOpaque` получил непрерывный slab в общей opaque RangeArena. Его размер `N * alignUp(C,2) * 8`, base выровнен на 16 байт. При отсутствии диапазона выполняется исходный Count/Emit, без удаления старой геометрии и без отдельной GPU allocation. 2 MiB — максимальная одновременная временная резервация при 2×16 jobs и C=8192; фактическая arena capacity не растёт.

GPU записывает count/cache обычным Count shader. `recordFastOpaque` передаёт slab base через четвёртое 32-bit push constant `fastBasePlusOne` (0 означает legacy ABI mode), ставит compute/transfer-write→compute-read/write dependency и вызывает `voxel_emit.comp` в fast mode с восемью группами по 64 invocation на job. Каждая группа получает deterministic prefix `sum(alignUp(count[j],2))` для предыдущих jobs с `count[j] <= C`; overflow не потребляет диапазон. Lane 0 группы 0 записывает `{6,count,0,base}` или нулевой command при overflow. Все группы копируют свои непересекающиеся packed records. Counts неизменны; headers copy, count reset и finalize не вызываются. Барьер после записи включает transfer, compute и vertex reads. Published draw пока не меняется. Legacy branch сохраняет исходный формат и поведение.

После fence считываются count и 16-byte candidate command (20 bytes/job; раньше два readback по 8 bytes/job). CPU независимо восстанавливает prefix, проверяет все candidate commands и границы slab. `RangeArena::partition` проверяет упорядоченность и отсутствие пересечений до изменения allocation records, переводит только актуальные успешные диапазоны в индивидуальное ownership и возвращает gaps/tail. Новое освобождение памяти учитывается retry mechanism. `verifyEmit` после обработки изменённых слотов повторно проверяет revision; публикация и deferred retirement остаются прежними. Fast fallback для overflow/ошибочной metadata ставится в очередь с `forceLegacy` до следующей ревизии, поэтому не зацикливается на fast reservation. Slot face range для fast jobs не нужен: vertex/culling используют published indirect firstInstance и stable slot encoded in face; legacy Emit обновляет slot range перед использованием. Никаких изменений исходных блоков/формата геометрии.

`OPTICRAFT_VULKAN_FAST_OPAQUE=0` — диагностический откат к Stage17 mesh scheduling; `OPTICRAFT_VULKAN_MESH_CACHE=0` автоматически исключает fast path. Три новых Mesa EGL сценария и C++ ownership tests описаны в history_version.md. Это bounded GPU suballocation, не глобальный GPU allocator. Промежуточный fence/readback устранён только у fast batches; итоговая host validation/publish сохраняется. Худший размер временной резервации и долю подходящих batches необходимо измерить; большой смешанный backlog может ограничить coverage. GPU validation и реальные FPS/frametime не выполнены из-за окружения. При применении патча пересобрать изменённые GLSL/SPIR-V и C++ (патч обновляет timestamps).

## Stage 19 — bounded mixed fast mesh

Stage18 fast opaque protocol распространён на batches с `meshKinds & 2`. При ненулевом `Config::fastMiscQuads` (4096) резервируется второй slab из существующей misc RangeArena размером `N * M * 32`; при неудаче opaque reservation откатывается, затем выполняется обычный Count. Оба slab живут внутри batch до его fence. Opaque Count/cache/copy остаётся прежним (включая zero-count jobs). Misc Count не вызывается: `VulkanMiscMesher::recordFastEmit` запускает оригинальный Emit с 16-byte compute push constants `{firstJob, jobCount, fastBase, fastCapacity}`. В shader `MiscSlot` копируется локально; `source.z = fastBase + job * fastCapacity`, `source.w = fastCapacity`. Каждый job получает независимый диапазон; все атомарные счётчики продолжают учитывать полную геометрию, а запись ограничена capacity. Legacy Count/Emit передают нулевую fastCapacity; ABI graphics descriptors/vertex buffers и 32-byte quads не изменяются. Misc-only/opaque-only jobs корректно пропускают ненужные dispatch, счётчики обнуляются для всех jobs.

После одного fence CPU читает opaque counts, candidate commands и misc counts (24 bytes/job в mixed path, 20 в opaque-only). `finishFastOpaque` проверяет prefix opaque, допустимость обоих counts и current revision; сохраняет только точные используемые диапазоны в обеих аренах с помощью `RangeArena::partition`. При overflow любой категории отвергаются обе категории данного job, revision получает `forceLegacy`; старые draws не меняются. `verifyEmit` повторяет проверку актуальности после изменений мира, `acceptPublish` сохраняет прежнюю deferred retirement. Misc published indirect firstInstance берётся из проверенного фиксированного stride; slot range для fast path не обновляется и не используется при отрисовке. Новый постоянный SSBO не нужен; 2 MiB временной misc reservation на batch при 16 jobs и M=4096 (4 MiB для двух batches), плюс до 2 MiB opaque reservations на две lanes. Это не увеличение arena capacity.

`OPTICRAFT_VULKAN_FAST_MIXED=0` отключает только расширение Stage19; существующие отключения cache/fast opaque действуют как раньше. Параметр M выбран эвристически, не на основании измеренного распределения мешей. Overflow повторно строится полным fallback, combined Count/Emit измеряется как Count для существующего адаптивного бюджета. Известные ограничения: нет GPU compact allocation misc, глобального GPU allocator, GPU-only publish и общего worldgen/mesh scheduler. Mesa EGL проверяет алгоритм GLSL, а fake Vulkan — только CPU syntax/contracts; реальная Vulkan синхронизация и производительность не подтверждены. Пересобрать GLSL/SPIR-V и C++ после применения patch.

## Stage 20 — depth/blended draw-list partition

`VulkanAlphaSort` хранит две таблицы `VkDrawIndirectCommand` в одном frame-local SSBO/indirect buffer: alpha в `[0, capacity)`, depth в `[capacity, 2*capacity)`. Descriptor binding 3 покрывает весь buffer; binding 0 raw visible list имеет `capacity` записей. Compute запускается по `live` jobs после frustum/Hi-Z. Alpha job index остаётся back-to-front, depth index `capacity + live - 1 - job` меняет порядок на front-to-back. Empty/culled jobs обнуляют обе команды. При `OPTICRAFT_VULKAN_SPLIT_MISC=0` depth table копирует прежние raw commands в исходном порядке; четвёртый float compute push constants теперь split flag, xyz остаются depth direction. Глубинное смещение проекции не влияет на относительную сортировку.

Для published span `[base, base+N)` GPU считает A blended quads по существующему material flag 128. Alpha пишет permutation в `[base,base+A)` (exact bitonic до 1024, иначе bins). Если `0<A<N`, depth пишет оставшиеся индексы в `[base+A,base+N)` и получает `{6,N-A,6,base+A}`. Интервалы не пересекаются даже при нескольких jobs, поскольку опубликованные arena ranges независимы. Pure depth (`A=0`) рисуется напрямую raw command, pure alpha (`A=N`) не рисуется в depth pass. `firstVertex=6` — признак indexed depth; `voxel_misc.vert` выбирает permutation и вычитает 6 из `gl_VertexIndex` для CORNERS. Alpha и direct depth используют `firstVertex=0`. Vertex/fragment guard оставлен для diagnostics и корректности. Fragment alpha-test, depth write/test и blending pipeline не меняются.

`VulkanLiveWorldRenderer` использует обе половины нового списка; compute→draw-indirect/vertex read dependency и frame fences остаются прежними. При неизменной VP/revision и отключённом Hi-Z кэшируются обе команды и permutation. Дополнительная память — 16 bytes/resident/frame, до 65536 bytes для двух frames × 2048 slots; новая geometry arena/descriptor не нужны. Mixed small alpha требует второго material traversal; large alpha совмещает partition со scatter. Это GPU разделение draw lists, не физическая классификация при meshing. Оно исключает blended vertex invocations из depth pass, но влияние на FPS, bandwidth, coplanar surfaces и качество на Vulkan не измерено. Проверки описаны в history_version.md. Пересобрать C++ и SPIR-V после применения патча.

## Stage 21 — opaque raster/cull hot path

Packed greedy vertex mapping имеет наружный winding во всех шести направлениях. `VulkanGameMirror` преобразует OpenGL VP (Y flip, Z 0..1), live viewport имеет положительную высоту; `VulkanOpaqueMaterialPipeline` использует CCW front face и backface cull. Изменение ограничено opaque atlas pipeline. Диагностический `OPTICRAFT_VULKAN_BACKFACE=0` отключает его.

`VulkanGpuCuller` с `opaqueDepthBins` выбирает mode 2 в прежнем 128-byte push constant только при `count<=512 && !nearOnly`; eye.xyz в этом режиме содержит row W VP (направление depth), для Hi-Z near mode 1 остаётся позиция камеры. Один 64-lane workgroup хранит slot/depth для видимых jobs, редуцирует min/max через lane bounds, строит 32-bin histogram, prefix cursors и scatter. В compact mode lane записывает exact draw count, без transfer reset; в fixed mode хвост до live count обнуляется. Не меняются published draws, буферы, descriptor bindings, Hi-Z ABI, indirect command fields. Все output writes уникальны (atomic cursor per bin, tail после validCount), compute→draw indirect dependency сохранена. Порядок внутри bin произвольный. Большие списки и near/far Hi-Z не сортируются. `OPTICRAFT_VULKAN_OPAQUE_BINS=0` возвращает старый culling. Порог 512 выбран эвристически: single workgroup и ~4.8 KiB shared memory могут стоить дороже исходного варианта (в том числе shared footprint fallback shader), нужен A/B benchmark.

Opaque draw list/count переиспользуются вместе с frame-local static misc cache только при exact VP, drawRevision и live count, без Hi-Z. Frame slot fence уже завершён; изменение stream batch или publication повышает revision, swapchain/menu/Hi-Z инвалидируют cache. При `OPTICRAFT_VULKAN_OPAQUE_CACHE=0` opaque пересчитывается, misc reuse сохраняется. `opaque reuse` показывает фактический выбор пути. В misc frustum bounds используются `origin-2 .. origin+34`, аналогично Hi-Z; shapes у границы не теряются из-за слишком узкого AABB. AO в `voxel_textured.frag` не читает diagonal occupancy, если обе side samples закрыты, поскольку dark уже равен 3. Остальные вычисления AO и light unchanged.

Проверены EGL compute и vertex transform feedback, CPU cache и ABI contracts (см. историю). Это не Vulkan raster/validation test; CMake Debug/Release и SPIR-V заблокированы окружением. FPS не измерен. Пересобрать изменённые C++ и GLSL/SPIR-V после применения patch.

## Stage 22 — lossless compact upload and host boundary storage

Producer `LiveChunkPacket` has two lossless representations per channel: dense packed words (16384 for block IDs, 8192 for each metadata/light channel) or one repeated packed word. The latter can represent arbitrary periodic 16-bit IDs or 8-bit values, not just identical voxels. `VulkanGameChunkFeed` emits compact uniform block/light channels directly from the authoritative grids; `packSparseMetadata` packs sparse records directly and preserves the original `unpack_meta` nonzero/overwrite rule. Empty or exactly repeated metadata compresses to one word. Dense producer path can be selected with `OPTICRAFT_VULKAN_COMPACT_UPLOAD=0` before starting the process.

`VulkanChunkStreamPlan` validates lengths, extracts exact 6×1024 combined boundary words irrespective of encoding and uses one uint32_t for uniform borders. Nonuniform borders allocate their 24 KiB payload lazily; transitions compare exact values per face, no hashes/collision risks. Eviction frees the boundary allocation. This reduces host queue and planner memory, not physical GPU residency. The worst-case dense boundary payload is unchanged, plus small per-slot pointer/metadata and allocator overhead. Uniform snapshots can skip voxel boundary extraction altogether. The diagnostic toggle only affects producer encoding, not this exact host boundary storage.

`VulkanChunkStreamGpu` expands repeated channels with `vkCmdFillBuffer` into the unchanged full-sized device-local arrays. Exact repeated value previously written in a live slot is tracked independently for each channel; identical repeated uploads require no GPU write, dense copies and new-instance/recycled slot reset invalidate tracking. Slot updates, generation checks, transfer ordering, mesh revision and publication rules remain intact. No writes overlap within a batch. Before transfer, the existing barrier also includes prior transfer writes to cover sequential overwrites. After transfer, existing shader and indirect visibility barrier applies to fill and copy. Staging is still sized for all-dense uploads. `voxel copy/fill/reuse KiB` are cumulative bytes of dense source copies, GPU fill range, and skipped repeated range respectively, excluding slot tables and existing LOD-only skip. Not measured PCIe bytes or GPU time.

An all-repeated snapshot carries 12 bytes of vector payload instead of 131072; a common heterogeneous block chunk without metadata omits the 32768-byte metadata staging copy, though the first upload fills the target range. At 2048 uniform active slots the logical CPU boundary payload is 8 KiB vs 48 MiB; actual savings depend on allocator overhead and uniformity. Device allocations remain 128 KiB per resident slot and GPU shader ABI is unchanged. A physical compact voxel pool needs logical slot indirection separate from physical offsets (neighbor lookups currently divide block offsets by 16384), per-channel addressing, bounded capacity/eviction and descriptor lifetime safety. Do not count this stage as GPU-side memory compression. CPU and mock tests are recorded in history; no real Vulkan runtime or performance claim. Rebuild C++ after applying patch; no GLSL files changed.


## Stage 23 — independent voxel page addressing

- Raw block/meta/light storage has independent fixed-stride page pools, each with worst-case capacity equal to the logical slot count. `VoxelChannelPages` maintains per-slot ownership and a free-page stack. A full-capacity pool always admits a dense transition; no eviction or truncation on page pressure is introduced.
- A repeated packed uint32_t channel is resident directly in the existing GPU slot record, including arbitrary alternating block IDs and byte values. No raw-buffer copy or fill is recorded for this channel. The existing slot transfer publishes it together with flags. Dense pages are released/reused on uniform transition, unload or instance change; the pre/post transfer barriers on the single graphics+compute queue order this reuse against earlier shader accesses.
- Slot ABI (std430 strides unchanged): `origin.w & 32` selects logical links; 64/128/256 mark inline blocks/meta/light. In opaque records `source.x`, `neighborB.z`, `neighborB.w` are channel bases or inline words. In misc records `source.x`, `source.y`, `neighborBlocksB.z` are the same values. Axial neighbour links hold stable slot IDs; -1 means absent. All current live records share the selected ABI. Legacy diagnostic records (bit 5 clear) retain original dense offsets. Geometry arena offsets and packed face slot IDs are unchanged.
- `voxel_address.glsl` decodes both formats. Opaque/misc compute, vertex light/metadata, fragment AO and LOD sampling use the same mapping. In particular, neighbour light uses its own channel mapping, never the physical block page. Dense own-chunk opaque samples reuse the already-loaded ChunkSlot.
- Default enabled; `OPTICRAFT_VULKAN_VOXEL_PAGES=0` restores Stage22 upload/address path. `voxel dense KiB` counts occupied dense pages, not Vulkan allocations; inline KiB counts avoided full-channel writes, excluding table traffic. No new buffer/descriptors, and no change to buffer allocation sizes in Stage23. Actual memory reservation and slot limit remain unchanged pending safe grow/shrink and memory-pressure admission.
- Validation: mock transfer and page lifecycle, 30 adapted EGL Count/Emit equivalence comparisons, opaque/misc vertex transform feedback (including cross-chunk light), prior regression suites and worldgen tests. CMake configuration blocked by missing OpenGL development dependencies; no Vulkan/SPIR-V or hardware benchmark. Potential cost: extra table lookup for cross-chunk samples in the new ABI; verify on target GPUs before declaring performance improvement.


## Stage 24 — growable physical voxel arenas

- The three raw channel buffers have independent physical page capacities, initially at most 256/64/256 pages (64/32/32 KiB each). The live renderer keeps its previous logical slot ceiling. `VoxelChannelPages::grow` appends free page indices; existing page offsets, stable slot IDs and shader ABI remain unchanged. Initial capacity is 26 MiB for 2048 logical slots, compared with 256 MiB in Stage23; actual device allocation sizes are queried from memory requirements.
- The stream planner performs resource admission before committing a packet or advancing its generation. A dense channel requires one page only when transitioning from non-dense; accepted inline transitions can free pages for later uploads in the batch. LOD-only updates need no page. Pages freed by an eviction but not immediately recycled are conservatively available for new admissions on the next frame. On allocation failure, try minimum sufficient capacity; if that also fails, defer the packet and suppress repeated allocation attempts for 30 recorded frames. Other eligible packets may pass. Device-lost/other unexpected errors are not treated as recoverable OOM. Growth is bounded by logical capacity and `maxStorageBufferRange`.
- Single queue migration: frame fence complete → retire that frame slot's migration sources → planner admission (allocate larger buffer, preserve page offsets) → transfer barrier with old shader/transfer dependencies → GPU copy old capacity to offset 0 of new buffer → transfer WAW barrier → regular voxel/table uploads → transfer-to-compute/graphics dependency. Old buffers are retained until the migration frame's fence completes, including consecutive growth on different frame slots. At most one resize per channel per recorded frame. No device/queue-idle call added. Growth may temporarily increase actual memory and may cost driver allocation time and GPU bandwidth; no timing claim.
- All five active voxel descriptor owners (opaque+misc for each of two mesh lanes and opaque material graphics) use two sets, one per frame. Initial and replacement descriptor ranges use actual buffer sizes. After `stream.record`, `selectVoxelFrame` binds the fence-completed slot and updates its three voxel descriptors only on epoch mismatch. The other slot's sets are not modified while pending. Misc vertex/fragment paths use the selected misc set; culling/alpha sorting use unchanged geometry/table buffers. Diagnostic probe still uses its fixed default set. No GLSL or SPIR-V source changes.
- Diagnostics: `voxel dense KiB` counts occupied pages; `allocated KiB` sums actual raw channel VkDeviceMemory allocations including retired sources, excluding staging, geometry and other resources; `migrated KiB` counts old capacities copied, separately from uploaded payload; `grows` and `admission misses` are cumulative counters. Default enabled, `OPTICRAFT_VULKAN_GROW_VOXEL_ARENAS=0` disables growth (Stage23), `OPTICRAFT_VULKAN_VOXEL_PAGES=0` disables indirect pages (Stage22).
- Validation: host-only mock executes real C++ buffer/stream/descriptor owner initialization and frame selection, including consecutive growth, preserved offsets and bytes, independent frame epochs, recoverable allocation failure and retry. Prior core/stream/mesh/catalog/feed/worldgen checks and Stage23 adapted EGL regression passed. Actual Vulkan SDK compilation/validation and hardware tests remain unavailable. No physical shrinking, no automatic memory-budget-driven eviction, and no increase to live renderer logical slot limit yet. Do not count this stage as measured FPS improvement.


## Stage 25 — idle physical arena trimming and admission credit

- A dynamic voxel channel becomes eligible for shrink after 120 consecutive stream records with no accepted uploads, releases or queued packets and at most 25% dense occupancy. The target is max(initial capacity, half current capacity, highest live page + 1). A reduction of at least one quarter (integer rounding) is required. Only free tail pages are discarded; addresses, slot IDs, neighbor references and geometry remain unchanged. Lower free holes are reused first after trimming. No compaction or live relocation.
- The existing same-queue migration protocol handles both growth and shrink; copy min(oldSize,newSize), retain source through migration fence, update current frame descriptors by epoch. On recoverable OOM, leave the original buffer and page allocator unchanged and apply a 30-record retry cooldown. Shrink does not run in a frame with upload/release activity. Temporary peak memory includes both allocations.
- The planner calls a release notification on the render owner before admission while holding its lock. The stream credits all freed channel pages before considering candidate packets; it does not release them again in the later upload loop. No new GPU write is submitted without the existing transfer dependency. Dense live-slot IDs are collected in one locked snapshot, not one lock per potential slot.
- `shrinks` counts successful channel resizes down; `migrated KiB` reflects bytes actually copied. Default on; `OPTICRAFT_VULKAN_SHRINK_VOXEL_ARENAS=0` disables shrink only. Growth and fixed/legacy diagnostics retain their prior semantics.
- Mock Vulkan with UBSan covers shrink hysteresis, high live pages, exact offsets/bytes, retirement, regrowth, OOM/retry, pending-work suppression and same-frame reuse of an evicted page by a different slot. CPU/core/stream/catalog/feed/worldgen and Stage2–7 checks pass. No shader change; real Vulkan validation and hardware performance remain untested. The 2048-slot depth-sort limit remains; scaling it requires a separate culling/sorting redesign. There is no automatic device-budget eviction or general compaction. Thresholds are heuristic and measured in records rather than elapsed time.


## Stage 26 — exact multi-run misc sorting and logical slot scaling

- For up to 2048 live IDs, keep the single-workgroup compact bitonic sort and fixed-size output. For >2048, dispatch one 64-lane workgroup per 2048 input IDs, independently frustum-test/compact/sort each run by projected center depth descending and slot ID ascending. Store `(floatBits(depth), slot, unique scratch index, 0)` records in a disjoint suffix of the per-frame misc command buffer. Invalid entries sort after valid records; NaN depth keys are normalized to zero. A second dispatch computes each record's exact global rank via lower-bound searches in other runs, adds the position within its own run, then writes the original published command or zero to the unique output position. No GPU/CPU readback, no global atomics in the merge, no approximation. Live IDs are unique. This two-run path uses an extra 64 KiB per frame at 4096 slots; no new descriptor binding. The alpha/Hi-Z descriptors still expose only the original command prefix, preserving the existing output layout and ordering. Compute write→compute read/write barrier between passes; final compute write→compute/indirect visibility. Existing frame-slot fence prevents overlapping writes between frame reuses. The 16388-byte shared-memory requirement is checked explicitly; no new shader subgroup feature.
- Default game requests 4096 logical slots when independent pages and growth are enabled; fixed/legacy modes clamp to the previous 2048 and dense SSBO limit. Each dynamic channel remains capped at 2048 dense pages or the device SSBO limit, whichever is lower. At full dense budget, pending work is deferred using existing admission; inline work can pass. Active raw-buffer capacity is at most 256 MiB (retired allocations and alignment excluded), initial default 26 MiB unchanged. Queue/table/staging capacities follow logical slots; geometry arenas unchanged. `OPTICRAFT_VULKAN_MAX_RESIDENT=2048` restores previous logical limit for comparisons. Console logs selected logical capacity. The Hi-Z threshold override accepts up to 4096.
- Validation: adapted Mesa EGL executes both passes against exact CPU reference in 54 cases, including 2049/4096, fully visible and empty runs, tie keys, invalid slots, arbitrary capacity, untouched scratch/guard words. Previous compact culling/alpha split/opaque tests pass. Real C++ mock Vulkan executes 4096-slot stream and high slot dense updates, checks actual culler dispatch constants, groups and barriers, rejects insufficient scratch/fixed oversized allocation, exercises bounded SSBO admission and eviction reuse. Prior core/Stage2–7/worldgen checks pass. Release CMake fails for missing OpenGL development dependencies; no hardware Vulkan/SPIR-V validation or measured FPS. No automatic memory-budget-driven eviction; when all dense pages are occupied, pending dense updates can remain deferred. Large lists require extra merge bandwidth; logical capacity does not guarantee rendered residency or distance. No new geometric LOD or GPU-only worldgen.


## Stage 27 — conservative 2³ geometric LOD

- `SurfaceLod.h` selects exact LOD0, existing material-independent LOD1 or LOD2 with 14/12 chunk-distance hysteresis (squared thresholds 196/144). The feed enables geometry LOD only for known opaque/air palettes with no misc geometry and not fully solid; renderer slot flags use bit 9, independently of addressing bits 5–8. LOD-only changes mask both LOD bits in the boundary comparison and do not copy original channels or invalidate unrelated neighbors. Existing source revision/generation checks and retained old mesh continue to apply. The feed still constructs a complete CPU snapshot on an LOD change.
- LOD2 virtual occupancy is evaluated entirely in opaque GPU Count/Emit. Within aligned 2³ cells wholly inside [2,30)³, 0–3 opaque voxels become air; 7 opaque voxels may fill the remaining cell only if all six original axial neighbors occlude. All other cells retain exact occupancy. Non-air, non-occluding or special material forces exact handling; mixed chunks use LOD1. The two-voxel shell stays exact at every LOD, so original boundary visibility matches across all six neighbor directions; an original face-connected air passage cannot be filled. Thin solid details may disappear in the interior. No original voxel mutation or coarse persistent storage.
- Two shared arrays of 256 uint each cache own/front macro classification within a plane. Each 32² plane still uses existing greedy rectangle generation, cache, fast allocation, fallback and publication. Packed record remains 8 bytes; bits 29–31 contain XOR of anchor low coordinates and the actual representative voxel low coordinates, so vertex metadata reads use a real sample even if a coarse rectangle spans former air. LOD1 fragment reconstruction retains original surface samples where valid and uses the representative fallback otherwise. Light on approximated geometry is not exact. This encoding is independent of the mutable slot LOD flags, including old published meshes during remeshing.
- LOD2 Count receives a conservative 16-unit scheduler estimate versus 8 for an ordinary opaque chunk. Shared storage grows by 2048 bytes in Count/Emit even for non-LOD2 dispatch; macro classifications are repeated between planes. No extra descriptors, buffer allocation or dispatch. Default on; `OPTICRAFT_VULKAN_GEOMETRY_LOD=0` retains LOD1; existing `OPTICRAFT_VULKAN_LOD=0` disables both. No OpenGL changes.
- Validation: seven adapted EGL cases compare exact packed records and boundary coverage against a CPU virtual-field reference, including dense/indirect paths, isolated voids, connected tunnel, sparse details, misc fallback, non-occluder guard and cache/legacy Emit. Synthetic counts: sparse 16500→36, cave 1318→58, random 31425→22511. Actual vertex transform feedback covers all 48 face/XOR offsets with LOD flags already reset, verifying positions and texture metadata. Stage16/23 EGL regression, core/stream/feed, Stage5 mock, catalog, mesh transactions/Stage6 and worldgen 130277 pass. Release configuration blocked by missing OpenGL development libraries; additional Stage7 run timed out. No Vulkan SDK/SPIR-V validation or hardware measurements. LOD3 and more efficient reusable macrocell classification remain open; no FPS claim.


## Stage 28 — batch-private macrocell classification cache

- The LOD2 virtual occupancy rules and packed face ABI remain Stage27. Instead of reclassifying aligned 2³ cells for each of the 192 face planes, the opaque Count pipeline has a prepass mode: 8 groups of 64 lanes per eligible job classify all 16³ cells. Exact shell codes are retained. Non-LOD2 jobs do not dispatch the prepass. CPU job mask bit 6 is private to dispatch scheduling (not the chunk slot ABI). The high bit of `DispatchParams.totalJobs` enables scratch lookup only for plane/prepass dispatches; cache offsets mask it, and finalize/publish/fast-copy parameters retain their normal count. Count prepass uses `fastBasePlusOne=1` as a Count-only discriminator.
- Binding 10 layout for J jobs and rectangle capacity C (uint words): J count headers, 2*J*C rectangle words, then J*4096 coarse codes. Each mesh lane owns its own scratch; indices are batch positions, not stable slots. Count writes codes, compute→compute RAW dependency makes them visible to plane groups; uncached Emit reuses codes from the corresponding Count, with an explicit compute dependency and existing generation/revision rejection of stale jobs. Header copy and rectangle emission never overlap the suffix. Slot tables, descriptor count, old-mesh COW publication and fast suballocation protocol are unchanged. No CPU-generated geometry.
- Additional payload 16384 bytes/job (512 KiB at default 16 jobs × two lanes). The capacity check preserves rectangle cache and disables coarse caching if both do not fit maxStorageBufferRange. Coarse caching also works when rectangle cache is disabled. `OPTICRAFT_VULKAN_COARSE_CACHE=0` restores Stage27 classification. No prepass for ordinary jobs. No per-chunk persistent coarse allocation. At full 192 planes Stage27 does 73728 macroCode invocations including boundary early returns; Stage28 performs 4096, plus cached loads. No claim about actual speed; shared memory size and plane group count unchanged and prepass adds 8 groups/job, bandwidth and a barrier. Conservative scheduler cost is unchanged pending profiling.
- Validation: `test_vulkan_stage28_coarse_cache.py` executes adapted EGL Count prepass / Count / Emit with cache capacities 0 and 8192, nonidentity job order, skipped job, mixed LOD1 and LOD2, exact macro codes, packed output, overflow and guard checks. It runs Stage27/16 regressions including vertex transform feedback. Stage18 fast opaque and Stage17 cache adapted tests also pass. C++ mock tests use actual mesh owner for dispatch and dependency checks plus live stream LOD2 routing; core/stream, Stage6 UBSan, catalog, chunk feed and 130277 worldgen checks pass. Release configuration blocked by missing OpenGL dev libs; no real Vulkan SPIR-V validation or hardware timings. LOD3 and dispatch count reduction remain open.


## Stage 29 — conservative 4³ geometric LOD3

- `SurfaceLod.h` adds bit 10, LOD3 enter/exit at 20/18 chunk units, inherits both LOD2 flags. All LOD bits are masked in voxel-only comparisons, and slot table upload preserves the new flag. Dispatch eligibility uses `kLod2Flags`, so both geometric levels reach the existing prepass. Scheduler estimates 24 units per LOD3 job (16 LOD2); not a measured cost.
- An aligned 4³ cell inside [4,28)³ becomes virtual air if it contains at most 16 solid voxels and every non-air voxel is a full occluding cube. Otherwise its eight 2³ cells use Stage27 rules. The exact two-block shell remains unchanged; LOD3 introduces no additional fill of original air, so the LOD2 face-connected air guarantee holds. No source data mutation, representative encoding change or neighbor remesh on LOD-only transitions. Sparse solid structures may disappear at distance.
- Prepass uses the existing 8×64 invocation dispatch: each LOD3 lane owns one 4³ cell and writes eight disjoint 2³ codes into the same 4096-word scratch. Count and uncached Emit share that scratch; the no-prepass diagnostic path evaluates the identical rule locally. No extra memory, descriptor, dispatch or readback compared with Stage28. 192 face planes remain; this is selective geometry simplification, not a full 8³ coarse mesh. Default on through existing geometry LOD setting.
- Validation: `test_vulkan_stage29_lod3.py` adapted Mesa EGL tests mixed levels, inactive job, cache capacities 0/8192, exact code/record/guard comparisons, sparse threshold and special material guard, connected cave and independent voxel addresses in uncached mode. Sparse synthetic case 468→36 rectangles, cave unchanged 58, special 472→52, threshold 468→48. Stage27/16 shader and vertex feedback regressions, actual C++ feed/stream and mock dispatch, Stage6 UBSan and CPU worldgen (130277) pass. Release configuration blocked by missing OpenGL dev libraries. No real Vulkan validation or FPS results. Coarse cache disabled path may be substantially more expensive; further plane reduction and full GPU-only worldgen remain open.


## Stage 30 — direct binary row masks for geometric LOD

- `buildCoarseRows` replaces the coarse path's 1024 per-cell keys and subsequent key scan. Each of 64 lanes handles four 2×2 plane tiles; four lanes own disjoint 8-column segments of a pair of rows. `coarseTileVisibility` short-circuits known empty/occluded tiles, reads individual voxels only on EXACT sides, retains all six face mappings and existing `cellAt` neighbor behavior. Two shared arrays of 64 partial masks replace the 256-word front-code array, reducing declared shared storage by 512 bytes. Thirty-two lanes OR four disjoint segments into `rowLive`; binary greedy consumes these masks without `rowBreak` or `planeKeys`. `lodOwn` still supplies the representative for packed rectangles. The exact LOD0/1 path is unchanged.
- Per coarse plane: 1024 key stores and 1024 key reads eliminated, 128 partial-mask stores and 128 reads introduced, one fewer workgroup barrier. These are source-level operation counts, not memory transactions or measured timings. The `planeKeys` allocation remains in the unified shader for ordinary jobs. No descriptor/scratch/packed ABI changes. Both cached and uncached classification paths use the new builder. Dispatch remains 192 plane groups per coarse job, no indirect plane compaction yet. No change to scheduler budget or LOD semantics.
- Validation: `test_vulkan_stage30_coarse_rows.py` executes the production builder through an adapted EGL test entry point and checks 49152 full uint row masks against CPU virtual occupancy, four scenes, all directions, mixed neighbor, independent addresses, inactive job, guard words, both coarse cache modes. It also runs Stage29/27/16 full Count/Emit and vertex regressions; Stage18 fast opaque, Stage23 addressing and 130277 worldgen cases pass. Release CMake blocked by missing OpenGL development libraries; Vulkan/SPIR-V validation and hardware timing not performed. Actual performance and benefit of reduced shared memory remain unmeasured.


## Stage 31 — exact plane compaction and indirect mesh dispatch

- Coarse jobs with a valid macrocell cache use Count prepass mode 2 and private push flag `0x40000000`. 48 workgroups each scan four consecutive planes using Stage30 `buildCoarseRows`, all 192 planes evaluated exactly. Each active plane atomically appends its ID and stores its 32 row masks; empty planes leave scratch untouched. GPU indirect args are initialized to {0,1,1}, with X incremented once per nonempty plane. Count and uncached Emit invoke `vkCmdDispatchIndirect` per eligible job, read the list and row masks, and use the original greedy algorithm and representative encoding. Ordinary jobs, six-plane solid jobs, cached Emit and fast opaque copy retain their previous direct modes. The explicit compact flag prevents 6 or 8 indirect groups from being interpreted as special direct modes. Old mesh COW and revision/generation validation unchanged.
- The suffix of binding 10 after coarse codes contains J×3 uint indirect args, followed by J×6336 uint job regions (192 plane IDs + 192×32 masks). Added payload 25356 bytes/job, 811392 bytes for 16 jobs × two lanes; no new bindings, shader modules, readbacks or packed mesh changes. Buffer includes `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`. Transfer initialization and compute write→compute read/indirect command read dependencies cover Count; uncached Emit reuses the scratch with a corresponding dependency. Per-lane scratch lifetime follows the existing transaction fence. `maxStorageBufferRange` check disables compaction without removing the existing coarse cache when the suffix cannot fit. Default on in game; `OPTICRAFT_VULKAN_PLANE_COMPACTION=0` disables it. No OpenGL engine changes.
- For N active planes Count uses 48 scan + N greedy groups rather than 192 (the common 8 macrocell prepass groups excluded), uncached Emit uses N groups rather than 192 and no visibility reconstruction. Dense N=192 may add overhead; 48 scan groups still evaluate all planes. No claim about GPU time or FPS. Cached Emit is unchanged. Workgroup size and scheduler estimates are unchanged, profiling needed to determine an adaptive policy.
- Validation: `test_vulkan_stage31_plane_compaction.py` executes adapted production Count/Emit shaders and actual GL indirect compute in Mesa EGL, capacity 0/6/8192, exact row mask/list/guard and packed geometry comparisons, inactive/stale jobs, mixed levels, nonidentity voxel addresses, empty and 6/8-group cases. Active plane counts: sparse LOD3 20, cave 29, special 27, threshold 26, dense random 192. Stage16/27/29 GLSL regression and 48 vertex combinations, C++ mock command/barrier and live LOD3 routing, core/stream, mesh transactions and Stage6 source contracts, 130277 CPU worldgen cases PASS. Release configuration blocked by missing OpenGL development libraries; no real Vulkan/SPIR-V validation or hardware FPS. GPU-only worldgen/publication and adaptive residency remain open.
