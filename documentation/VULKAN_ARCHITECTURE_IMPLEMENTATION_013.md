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

