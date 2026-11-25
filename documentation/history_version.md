# История версий OptiCraft

## 0.13 — Vulkan render (2026-11-23, разработка)

### Этап 31 — exact GPU plane compaction / indirect mesh dispatch

**Сделано:** для геометрических LOD2/3 с coarse cache Count выполняет prepass 48 workgroups/job, по четыре плоскости на группу. `buildCoarseRows` проверяет все 192 плоскости; пустые не записывают маски и не добавляются в список. GPU атомарно формирует количество и ID непустых плоскостей; `vkCmdDispatchIndirect` запускает greedy только для них. Резервный Emit использует тот же список и точные row masks без повторной классификации видимости. Для получения representative bits используются сохранённые macrocell codes. Rectangle cache hit и fast copy сохраняют прежний путь, включая отдельные режимы 6/8 groups. Сохраняются проверки поколения/ревизии, COW publication, исходные воксели, границы и packed rectangle ABI. Binding 10 получает suffix 6339 words/job (25356 bytes; 811392 bytes на 16 jobs × два lanes), флаг indirect buffer usage; новый push-flag не меняет размер push constants. Нет новых descriptors, shader modules или CPU readback. Compute → compute/indirect dependency, корректная инициализация {0,1,1}, zero dispatch, stale jobs. Default on; `OPTICRAFT_VULKAN_PLANE_COMPACTION=0` возвращает Stage30, также fallback при отсутствии coarse cache или недостаточном SSBO range. Scheduler budget не изменён.

**Протестировано:** Stage31 adapted Mesa EGL actual indirect Count/Emit with cache 0/6/8192, mixed LOD1/2/3, inactive and stale jobs, nonidentity physical voxel addresses, exact packed rectangles, row masks, active plane lists, guards, zero/6/8-group alias tests. Active planes for sparse LOD3 / cave / special / threshold / random: 20/29/27/26/192; synthetic empty/single/two blocks: 0/6/8. Stage16/27/29 adapted shader regressions and 48 vertex representative combinations PASS. C++ mock integration: indirect Count/Emit, compute→indirect barrier, cache-copy and live LOD3 job routing PASS; core/stream, mesh transactions and Stage6 contracts, 130277 CPU worldgen scenarios PASS. Release CMake blocked by missing OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR; no hardware Vulkan/SPIR-V validation or FPS timing.

**Осталось:** Count проверяет все плоскости, но запускает 48 + N workgroups вместо 192 (общий 8-group macrocell prepass не включён в сравнение); uncached Emit запускает N вместо 192. При N=192 Count получает 240 groups и дополнительные barriers/SSBO traffic; количество workgroups не равно времени GPU. Cached Emit уже был дешёвым. Реальная выгода и выбор адаптивного порога требуют профилирования на Intel/RTX. GPU-only worldgen, GPU publication, adaptive residency остаются открытыми.

### Этап 30 — direct coarse row masks

**Сделано:** LOD2/LOD3 `voxel_row_runs.glsl` строит 32-битные маски видимости непосредственно из 2×2 tiles плоскости. 64 lanes вычисляют по 4 tiles, четыре lanes формируют две строки; 32 lanes объединяют частичные маски. Known-air own tile пропускает front classification, known occluder front исключает весь tile, known/known обрабатывается одной проверкой, EXACT стороны сохраняют индивидуальные выборки и межчанковую адресацию. Binary greedy не использует `planeKeys` и `rowBreak` на coarse path. Устранены 1024 записи/1024 чтения ключей и один shared barrier на plane (добавлены 128 записей/128 чтений partial masks); shared footprint уменьшается на 512 байт. `lodOwn` сохраняет representative bits, формат и содержание геометрии не меняются. LOD0/1, кэш, dispatch ABI и scheduler budget сохранены. Работает по умолчанию на геометрических LOD, включая режим без coarse cache.

**Протестировано:** Stage30 adapted Mesa EGL: 49152 row masks (4 scenes × 192 planes × 32 rows × 2 cache modes), exact CPU reference, все face directions, independent physical addresses, mixed neighbor, inactive job, guard words. Stage29/27/16 Count/Emit full packed rectangles, cache overflow, 48 vertex representative combinations, Stage18 fast opaque and Stage23 addressing regressions, 130277 CPU worldgen scenarios PASS. Release CMake blocked by missing OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. No Vulkan/SPIR-V validation or hardware timing.

**Осталось:** 192 plane workgroups по-прежнему dispatch на coarse job. Сокращена работа внутри групп, а не количество dispatch. Статически выделенный `planeKeys` остаётся для LOD0/1. Cost of tile divergence, shared masks and actual GPU performance unmeasured; scheduler estimates unchanged. Plane compaction/indirect dispatch, GPU-only worldgen and GPU publication remain open.

### Этап 29 — conservative 4³ geometric LOD3

**Сделано:** LOD3 включается с 20 чанков и возвращается к LOD2 ниже 18 (квадраты 400/324). Для palette-proven opaque/air chunks ячейки 4³ в [4,28)³ с максимум 16 opaque voxels и без special materials становятся воздухом только в виртуальном поле мешинга; остальные наследуют LOD2. Исходные воксели, двухблочная точная оболочка, исходные связные воздушные проходы и packed 8-byte face ABI сохранены. Bit 10 в slot flags; LOD-only mask включает все три уровня, CPU dispatch mask признаёт LOD2 и LOD3. В существующем prepass 512 lanes проверяют по одной 4³ ячейке и записывают восемь 2³ codes в прежний suffix 4096 words/job. Без cache классификация повторяется локально, результат идентичен. Новых GPU buffers, descriptors, readbacks и shader modules нет. Планировщик использует консервативные 24 work units на LOD3 вместо 16 для LOD2. Default on; `OPTICRAFT_VULKAN_GEOMETRY_LOD=0` отключает оба геометрических LOD.

**Протестировано:** Stage29 adapted Mesa EGL: 5 synthetic scenes (4 LOD3 и LOD2 контроль), 6 job positions включая inactive, cache capacities 0/8192, exact macro codes, guards, Count/Emit и fallback, uncached indirect LOD3. Сравнение с CPU reference: sparse 468→36, cave 58→58, special guard 472→52, threshold guard 468→48; exact exterior boundary faces unchanged. Stage27/16 EGL regressions and 48 vertex combinations PASS. C++ core/stream, actual feed compact/dense, live LOD3 mock dispatch and barrier, mesh transactions UBSan, Stage6 contracts, 130277 worldgen scenarios PASS. Release CMake blocked by missing OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. No hardware Vulkan/SPIR-V validation or timings.

**Осталось:** это выборочное упрощение, не полноценная coarse grid mesh на 8³; разреженные конструкции (включая тонкие стены) могут исчезать вдали. LOD2 заполнение изолированного воздуха остаётся, LOD3 нового заполнения не добавляет. 192 plane workgroups и существующий формат прямоугольников сохранены. Без coarse cache классификация 4³ добавляет повторную работу; производительность и выбранные пороги не измерены на Intel/RTX. Adaptive residency, plane dispatch reduction, GPU-only worldgen/publication остаются открытыми. FPS не измерен.

### Этап 28 — batch-private LOD2 macrocell cache

**Сделано:** opaque Count получает дополнительный режим классификации: 8 workgroups × 64 lanes на LOD2 job, 4096 выровненных ячеек 2³. Коды (exact / air / representative) записываются в отдельный suffix существующего binding 10, 16 КиБ/job. Count/uncached Emit используют эту классификацию через shared plane arrays вместо повторных вызовов `macroCode`; обычный rectangle-cache copy и fast opaque copy не меняются. CPU dispatch mask сохраняет признак LOD2, который раньше терялся при сужении meshKinds до uint8_t. Prepass только для eligible jobs, compute write→compute read barrier, прежние revision/generation checks исключают stale jobs до Emit, batch job indices сохраняются. Нет новых descriptors, SPIR-V modules, CPU readbacks или изменения packed mesh ABI. При default 16 jobs × 2 batches дополнительный VkBuffer payload 512 КиБ. Кэш не зависит от наличия rectangle cache; при недостаточном maxStorageBufferRange сохраняется Stage27. Default on, `OPTICRAFT_VULKAN_COARSE_CACHE=0` отключает только prepass. LOD thresholds, точная оболочка, сохранение исходных вокселей и OpenGL не меняются.

**Протестировано:** Stage28 adapted Mesa EGL (cap=0/8192, mixed LOD1/LOD2 jobs, inactive slot, nonidentity batch ordering, exact macro codes, overflow/uncached and cached Emit, full packed records and guard words); Stage27 seven geometry cases and 48 vertex combinations, Stage16–18 adapted EGL regressions. Actual C++ mock checks dispatch constants, compute barrier, no prepass for ordinary/disabled jobs, Count→Emit without repeated prepass, live stream LOD2 routing; core/stream, Stage6 UBSan mesh transactions, catalog, compact/dense feed and 130277 CPU worldgen scenarios PASS. Release CMake blocked by missing OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR; real Vulkan/SPIR-V and hardware benchmarks not executed.

**Осталось:** в обычном LOD2 Count старый путь вызывает macroCode 73728 раз на 192 плоскостях (включая быстрые возвраты на оболочке); новый 4096 раз, но добавляет 8 workgroups, scratch writes/reads и compute dependency. Это расчёт числа вызовов, не время выполнения. Shared memory и 192 plane groups остаются прежними, conservative LOD2 geometry unchanged. Планировщик сохраняет консервативные 16 work units до аппаратных измерений. Кэш живёт только в пределах транзакции, не между ремешингами. LOD3, дальнейшее сокращение plane dispatches, адаптивная резидентность, GPU-only worldgen и GPU publication остаются открытыми. FPS не измерен.

### Этап 27 — conservative geometric LOD2

**Сделано:** GPU-мешинг LOD2 на выровненных ячейках 2×2×2 для palette-proven opaque/air chunks. Переход LOD1→LOD2 на 14 чанках, обратно ниже 12; LOD0/1 hysteresis 10/8 сохранён. Оболочка шириной два блока остаётся точной, включая соседние границы. Внутри ячейки с 1–3 solid voxels заменяются воздухом; 7 solid voxels заполняют единственный air voxel только при шести исходных occluding neighbors; остальные неоднородные ячейки остаются точными. Связные по граням исходные воздушные проходы не заполняются. Misc и неизвестные/сложные материалы не упрощаются. Count/Emit используют общий алгоритм, два shared 16×16 массива; GPU cache/fallback сохраняют packed ABI 8 bytes/rect. Bits 29–31 хранят XOR-смещение реального представительного вокселя для metadata vertex fetch, bit 28 — LOD1 texture reconstruction; исходные блоки, свет, метаданные не меняются. LOD-only revisions не обновляют плотные воксели и соседние границы. Оценка стоимости LOD2 job увеличена с 8 до 16 work units. Default on; `OPTICRAFT_VULKAN_GEOMETRY_LOD=0` отключает LOD2, `OPTICRAFT_VULKAN_LOD=0` отключает оба LOD.

**Протестировано:** семь adapted Mesa EGL LOD2/LOD1 scenarios, включая exact boundary coverage, connected cave, isolated voids, sparse interior, indirect pages, mixed fallback, non-occluding guard, cached/uncached Emit, guard words; 48 combinations actual vertex transform feedback. Synthetic speckles 16500→36 rectangles, cave/voids 1318→58, random 31425→22511. Stage16 six exact scenes, Stage23 30 ABI comparisons and vertex feedback, C++ core/stream/feed, Stage5 mock, catalog, mesh transaction and Stage6 contracts, 130277 worldgen scenarios PASS. Release CMake blocked by missing OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. Additional Stage7 run terminated by time limit. No real Vulkan/SPIR-V or hardware FPS checks.

**Осталось:** это консервативный локальный LOD2, а не полная coarse voxel grid. Тонкая opaque-геометрия внутри distant chunk может исчезнуть визуально; isolated air can be filled; coarse light is approximate. Нет LOD3, отдельного persistent coarse cache или сокращения dispatch count. Дополнительные 2048 bytes shared memory и повторная классификация macrocell per plane могут увеличить стоимость Count/Emit; проверить на Intel/RTX. Нет measured FPS. GPU-only worldgen и GPU publication не реализованы.

### Этап 26 — scalable exact misc draw ordering / logical residency

**Сделано:** снят фиксированный предел сортировки в 2048 logical slots. До 2048 live slots сохраняется один compact bitonic workgroup; для больших списков каждый workgroup независимо фильтрует и сортирует до 2048 входных слотов, записывает depth key/slot в scratch suffix. Второй dispatch выполняет точное слияние по rank: binary lower bound в остальных runs, без atomics, CPU readback и approximate depth bins. Tie-break по slot ID, invalid tail обнуляется; compute→compute RAW dependency и final compute/draw dependency. На 4096 slots scratch 64 КиБ на frame (128 КиБ для двух кадров), без новых descriptors; Hi-Z и alpha используют только command prefix. `VulkanGameMirror` запрашивает 4096 логических слотов по умолчанию при dynamic voxel arenas; fixed/legacy диагностические режимы остаются ограничены 2048 и SSBO range. Dense pages каждого канала ограничены 2048 и maxStorageBufferRange, поэтому стационарный raw-buffer budget не превышает прежние 256 МиБ (без retired buffers/alignment). Inline channels не требуют страниц. `OPTICRAFT_VULKAN_MAX_RESIDENT=2048` возвращает прежний logical ceiling. Версия 0.13, OpenGL без изменений.

**Протестировано:** 54 adapted Mesa EGL misc sort cases до 4096 команд, exact depth/slot order, empty runs, invalid slots, ties, scratch and guard ranges; Stage15 compact culling, Stage20 split/vertex и Stage21 opaque EGL regressions. Actual C++ mock stream/culler: 4096 резидентных inline slots, dense update slot 4095, neighbor ID, correct single/two dispatch and compute barriers, reject missing scratch, fixed path range failure, bounded SSBO admission/eviction reuse; Stage2–7 contracts, core/stream, mesh transactions UBSan, catalog 534 IDs, 130277 worldgen cases PASS. Release CMake заблокирован OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR; Vulkan SDK/SPIR-V validation и hardware FPS не проверены.

**Осталось:** 4096 — logical capacity, не гарантия 4096 плотных или видимых чанков: dense cap, geometry arenas, CPU feed, memory pressure и реальные данные мира ограничивают загрузку; блокированные пакеты остаются pending до освобождения страниц. Нет automatic physical-memory-driven eviction, общего compacting allocator или adaptive device budget. Новая сортировка требует второй dispatch и дополнительных SSBO reads при >2048 slots; shared memory 16388 bytes для bitonic shader, на устройствах с меньшим лимитом Vulkan init не проходит. Стоимость на Intel/RTX не измерена. Геометрические LOD2+, GPU-only worldgen и GPU publication остаются открытыми.

### Этап 25 — Vulkan voxel arena trimming / eviction admission

**Сделано:** независимые физические block/meta/light арены сокращаются после 120 последовательных записей кадра без uploads, releases и pending packets при заполнении не более 25%. Нижняя граница — стартовая ёмкость канала; новый размер не меньше половины прежнего и максимального занятого page index + 1. Сокращается только свободный хвост без перемещения живых страниц. Миграция копирует min(old,new) bytes, старые VkBuffer/VkDeviceMemory живут до fence кадра миграции; descriptor epochs работают для роста и сокращения. При OOM сохраняется исходный буфер, retry cooldown 30 кадров. `ReleaseNotice` планировщика освобождает dense pages до admission всех новых пакетов, в том числе если освобождённый slot не переиспользован; GPU writes по-прежнему упорядочены same-queue barriers. Live slot snapshot строится за одну блокировку вместо вызова slotInfo под mutex для каждого потенциального слота. FPSCOUNTER: `shrinks`. По умолчанию включено; `OPTICRAFT_VULKAN_SHRINK_VOXEL_ARENAS=0` отключает только shrink. ABI, OpenGL и версия не изменены.

**Протестировано:** actual C++ stream с mock Vulkan под UBSan: idle hysteresis, high live page, data/offset preservation, fence retirement, regrowth, shrink OOM и retry, queued work blocking shrink, same-frame eviction credit при обновлении другого слота. Stage23–24 mock regressions, core/stream, Stage2–7 contracts, CPU greedy coverage, mesh transactions, 534 ID catalog, compact/dense feed, 130277 CPU worldgen scenarios — PASS. Release CMake заблокирован OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. GLSL не менялся; Vulkan hardware/validation и FPS не проверены.

**Осталось:** shrink не уплотняет занятые страницы и требует временного сосуществования allocations; при фрагментации экономия может быть ограничена. Порог 120 записей кадра и 25% эвристический, не привязан к wall clock. Автоматическое вытеснение по device memory budget не реализовано. Логический лимит 2048 не увеличен: misc depth sort в VulkanGpuCuller и шейдере имеет фиксированную ёмкость. Следующая задача — масштабируемая GPU-сортировка и согласование логического бюджета с feed/geometry arenas; затем LOD2+ и GPU-only worldgen. FPS не измерен.

### Этап 24 — Vulkan growable physical voxel arenas (2026-11-24)

**Сделано:** `VulkanChunkStreamGpu` создаёт независимые физические block/meta/light арены на 256/64/256 плотных страниц соответственно (ограничены логической ёмкостью и maxStorageBufferRange). При 2048 логических слотах начальная суммарная ёмкость VkBuffer — 26 МиБ вместо 256 МиБ; требования VkDeviceMemory могут добавить alignment. `VoxelChannelPages` сохраняет физические offsets и отдельно учитывает allocated/occupied pages. Перед коммитом upload planner проверяет admission: при нехватке страницы увеличивает только нужный буфер (обычно x2, с запасом на upload batch); при VK_ERROR_OUT_OF_DEVICE_MEMORY/OUT_OF_HOST_MEMORY пробует минимальную ёмкость, затем откладывает пакет с 30-frame cooldown. Непринятый пакет не меняет generation/revision/slot ownership, остаётся в очереди; остальные подходящие пакеты могут пройти. Уже освобождённые inline transitions учитываются в batch admission; нерециркулированные eviction pages консервативно учитываются на следующем кадре. На одну арену не более одного grow за кадр. Старый буфер GPU→GPU копируется на прежние offsets с transfer RAW/WAW dependencies, сохраняется до fence кадра миграции; новые upload writes выполняются после копирования. `FrameVoxelDescriptors` выделяет два descriptor sets на каждый compute/graphics owner, выбирает кадр после stream.record и обновляет только его три voxel bindings при изменении epoch; оба mesh batches и opaque graphics используют фактические размеры буферов. В render/stream path не добавлены vkDeviceWaitIdle/vkQueueWaitIdle. Slot/geometry/shader ABI не меняется. `voxel dense KiB`, `allocated KiB` (сумма VkDeviceMemory allocations включая retired buffers), `grows`, `migrated KiB`, `admission misses` разделяют фактические allocations, занятые страницы и стоимость миграций. По умолчанию включено; `OPTICRAFT_VULKAN_GROW_VOXEL_ARENAS=0` возвращает fixed-capacity Stage23, `OPTICRAFT_VULKAN_VOXEL_PAGES=0` оставляет Stage22 dense ABI. Версия 0.13.

**Протестировано:** host-only mock Vulkan с настоящими исходниками stream/device buffer/mesh transactions/opaque+misc compute/opaque graphics owners: рост 1→3→6 страниц всех трёх арен, сохранение offsets и каждого слова, две последовательные миграции в обоих frame slots, retirement, epoch updates и диапазоны descriptors, минимальная retry allocation, полный отказ после частичного роста, сохранение queued packet, admission однородных пакетов за заблокированным, eviction/reuse. Проверено создание и переключение двух frame descriptor sets для обоих mesh batches и opaque graphics, без обновления другого кадра. C++ core/stream, mesh transactions UBSan, Stage2–7 contracts, 534 ID catalog, real chunk feed/mock compact+dense, 130277 CPU worldgen scenarios; Stage23 adapted Mesa EGL 30 Count/Emit comparisons и vertex transform feedback повторно PASS. Debug/Release CMake остановлены на OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY и OPENGL_INCLUDE_DIR; Vulkan SDK/glslangValidator/spirv-val и аппаратная Vulkan validation недоступны. Mock API не исполняет реальные GPU-команды; EGL не проверяет Vulkan synchronization.

**Осталось:** логический лимит live renderer пока 2048 слотов (также ограничен SSBO); shrink/compaction и memory-budget extension не реализованы. Рост требует временного сосуществования старой и новой allocations и GPU-копирования старой ёмкости; стоимость VkAllocateMemory в render owner и transfer migration на Intel/RTX не измерена. При устойчивом OOM обновление может оставаться отложенным до освобождения страницы; автоматического eviction по бюджету физической памяти нет. Дальнейшее масштабирование логических слотов, LOD2+, GPU-only worldgen и GPU-only mesh publication остаются открытыми. FPS не измерен.

### Этап 23 — logical voxel addresses / inline uniform channels (2026-11-24)

**Сделано:** `VoxelChannelPages` выделяет независимые dense pages для blocks, metadata и light по требованию. Логические slot ID больше не определяют физические адреса каналов. Повторяющиеся uint32_t хранятся непосредственно в GPU slot tables; dense pages освобождаются при переходе в uniform и переиспользуются после eviction/reset. Slot ABI сохраняет размеры 64/96 байт и существующие descriptor bindings: origin.w bit 5 — новый формат; bits 6/7/8 — inline blocks/meta/light. Opaque source.x и neighborB.z/w, misc source.x/y и neighborBlocksB.z содержат base либо полный uint32_t. Соседи представлены stable slot IDs с -1 для отсутствующих; legacy dense offsets поддерживаются для диагностического режима. Обновлены opaque/misc Count/Emit, vertex lighting/metadata, fragment AO/LOD surface и neighbor fast path; same-queue transfer barriers защищают reuse от предыдущих compute/graphics accesses. `voxel copy/fill/reuse/inline KiB` и `voxel dense KiB` в FPSCOUNTER отражают логический плотный payload, а не VkDeviceMemory allocation. По умолчанию включено; `OPTICRAFT_VULKAN_VOXEL_PAGES=0` возвращает Stage22 ABI и fill/copy. Версия и OpenGL fallback сохранены.

**Протестировано:** mock Vulkan transfer/slot ABI с побайтовой проверкой всех трёх каналов, inline words с high bit и чередующимися значениями, independent page ownership, dense↔inline, full-capacity reuse, eviction/reset/LOD-only, absence of voxel fills; C++ mesh transactions UBSan, Stage2–7, 11–21 contracts и доступные EGL regressions, chunk feed в обоих режимах, каталог 534 IDs, 130277 CPU worldgen scenarios. Stage23 Mesa EGL: 30 парных сравнений actual Count/Emit (opaque/misc, nonidentity page placement, все шесть neighbor links, independent inline flags, high block IDs, mixed forms, guard words); transform feedback opaque/misc vertex positions и packed light совпали с legacy, включая соседние чанки. Debug/Release CMake остановлены на OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY и OPENGL_INCLUDE_DIR; glslangValidator/spirv-val отсутствуют.

**Осталось:** VkBuffer/VkDeviceMemory и descriptor ranges по-прежнему резервируют worst-case 128 КиБ/slot; лимит резидентных слотов не увеличен. Сокращён объём активного dense payload и записей, но не физически выделенной VRAM. Следующий шаг — ограниченные/растущие физические арены с безопасным admission, descriptor lifetime и обработкой memory pressure без потери чанков. Новый ABI добавляет indirect table reads для соседей, стоимость на Intel/RTX и FPS не измерены. Реальные Vulkan SPIR-V/validation, GPU-only allocation/publication, LOD2+, GPU-only worldgen остаются открытыми.

### Этап 22 — compact voxel upload / CPU boundary residency (2026-11-24)

**Сделано:** `LiveChunkPacket` принимает для каждого канала либо полный packed array, либо один повторяющийся uint32_t. `VulkanGameChunkFeed` использует uniform block/light storage без распаковки; sparse metadata пакуется непосредственно из записей, с сохранением last-nonzero semantics. Пустой/однородный чанк с однородным light и metadata занимает 12 байт payload вместо 128 КиБ (без заголовков и allocator overhead). `VulkanChunkStreamGpu` восстанавливает повторяющиеся слова через `vkCmdFillBuffer`, а при точном совпадении с известным повторяющимся каналом того же slot/generation пропускает запись; dense upload и reuse/instance reset инвалидируют соответствующий cache. Slot offsets, raw GPU storage, shader ABI, staging capacity и mesh publication не меняются. Добавлена transfer→transfer зависимость для последующих обновлений. `VulkanChunkStreamPlan` хранит однородные границы одним uint32_t вместо 24 КиБ; heterogeneous snapshot выделяется по необходимости и освобождается при uniform transition или eviction. Точное сравнение шести граней поддерживает dense/compact и чередующиеся packed values. FPSCOUNTER выводит накопительные `voxel copy/fill/reuse KiB` (только voxel payload, не slot tables). По умолчанию включено; `OPTICRAFT_VULKAN_COMPACT_UPLOAD=0` возвращает плотный producer/upload для сравнения, CPU boundary compression остаётся активной.

**Протестировано:** C++ core/stream, UBSan stream, 100 randomized sparse metadata fixtures, dense/compact LOD и boundary transitions, malformed payload rejection, exact slot lifecycle; mock Vulkan fill/copy/reuse с проверкой каждого слова трёх каналов, transitions и соседних slots; actual `VulkanGameChunkFeed.cpp` с mock chunk classes (compact/dense modes, uniform solid, lit air halo, mixed high block IDs, metadata, light и unpack counters); Stage 2–7 contracts/CPU greedy coverage/mesh transactions UBSan, catalog 534 IDs, 130277 CPU worldgen scenarios. Debug/Release CMake остановлены на OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. Новых GLSL/SPIR-V нет; реальные GPU validation и timings не выполнены.

**Осталось:** GPU allocation по-прежнему 128 КиБ/slot, staging allocation не уменьшена; fill также пишет весь диапазон GPU, его скорость и стоимость дополнительных transfer commands не измерены. CPU snapshot dense channels при LOD changes пока строится; изменения плотных каналов не имеют dirty-range protocol. Компактный физический GPU voxel pool требует отдельного logical-to-physical ABI для blocks/meta/light и соседей, безопасного обновления descriptors и работы при memory pressure. GPU-only allocator/publication, LOD2+, GPU-only worldgen и аппаратные benchmarks остаются открытыми. FPS не измерен.

### Этап 21 — Vulkan opaque culling / static draw cache (2026-11-24)

**Сделано:** live opaque pipeline включает `VK_CULL_MODE_BACK_BIT`, CCW для существующей Y-flip clip conversion и positive viewport; diagnostic, misc, entity и OpenGL pipelines не меняются. Для non-Hi-Z списков до 512 live slots `voxel_cull.comp` в одном workgroup выполняет frustum culling и 32-bin front-to-back ordering по projected center depth. Shared min/max и histogram/cursors формируют dense output; compact count записывается напрямую, fixed-size tail обнуляется. Дополнительных dispatch/SSBO/descriptor нет; compact reset fill не нужен в этом пути. Большие списки и Hi-Z используют прежний алгоритм. Frame-local static cache по VP/revision/live count теперь переиспользует также opaque commands/count; invalidation при stream changes, publication, Hi-Z, menu и swapchain сохранена. `opaque reuse` в FPSCOUNTER. Misc frustum расширен на 2 блока с каждой стороны, как Hi-Z, для выступающих shaped geometry. AO пропускает диагональную выборку, когда обе боковые occupancy samples истинны, формула AO не меняется. По умолчанию включено; диагностические отключения `OPTICRAFT_VULKAN_BACKFACE=0`, `OPTICRAFT_VULKAN_OPAQUE_BINS=0`, `OPTICRAFT_VULKAN_OPAQUE_CACHE=0`.

**Протестировано:** 176 Mesa EGL compute scenarios (compact/fixed, legacy/near/ordered, empty/invalid slots, guards, 1–2048 jobs, depth bin monotonicity и точное множество команд); actual adapted opaque vertex transform feedback: 24 triangles, 6 faces, 1×1 и 7×3 rectangles, nonzero baseInstance, наружные normals/winding и projected sign для Y flip. Misc culling: 28 EGL cases + targeted boundary. Stage20 partition: 48 EGL cases и transform feedback; AO 8 occupancy combinations (CPU reference); static draw cache C++ test; fake Vulkan syntax-only VulkanGpuCuller.cpp; core/stream, Stage2–7/11–13 contracts, UBSan mesh transactions, catalog 534 IDs, 130277 CPU worldgen scenarios. CMake Debug/Release блокируются OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR.

**Осталось:** аппаратная Vulkan/SPIR-V validation, измерения Intel/RTX; порог 512 эвристический. Bins добавляют shared memory около 4.8 КиБ/workgroup даже для fallback shader и могут снизить occupancy; один workgroup может оказаться дороже распределённого culling. Сортировка приближённая (порядок внутри bin не гарантируется), Hi-Z и >512 slots остаются несортированными. Backface cull не проверен в реальном Vulkan rasterizer; проверить при нахождении камеры внутри блоков. Расширенные misc bounds могут оставить больше пограничных команд. AO экономит максимум одну диагональную выборку только при двух закрытых сторонах; перенос AO в meshing не выполнен. Физическое разделение материалов, GPU-only allocator/publication, unified scheduler, compact residency, LOD2+, GPU-only worldgen остаются открытыми.

### Этап 20 — Vulkan GPU cutout/blended draw-list partition (2026-11-24)

**Сделано:** `voxel_alpha_sort.comp` формирует отдельные frame-local indirect commands для depth-writing misc (opaque shaped/cutout) и blended. Raw misc arena и meshing ABI не меняются. Alpha сохраняет back-to-front chunk order и exact/binned per-quad sort; depth list обращает порядок чанков (front-to-back). Mixed jobs размещают cutout indices в свободном хвосте того же arena-indexed permutation span, без увеличения permutation SSBO. Pure depth jobs используют прямой contiguous draw без записи permutation; pure blended jobs получают zero depth command. Vertex pulling использует `firstVertex=6` для indexed depth, `firstVertex=0` для direct depth/alpha, с корректным локальным индексом 0–5. Неподходящие blended vertices больше не запускаются в depth pass при включённом split; safety guards и alpha-test сохранены. Cutout/alpha команды размещены в двух половинах существующего frame-local command buffer, дополнительные 16 байт/resident/frame (64 КиБ на два кадра при 2048 slots), без нового descriptor binding. Разделение выполняется после frustum/Hi-Z, переиспользуется вместе с существующим static misc cache. `OPTICRAFT_VULKAN_SPLIT_MISC=0` возвращает legacy depth list и порядок, по умолчанию split включён.

**Протестировано:** 48 multi-command Mesa EGL dispatches в обоих режимах (0/1), различные resident capacity/live counts, nonzero arena offsets, pure/mixed/empty commands, exact 1024 и bucket >1024, отрицательное/нулевое/положительное направление, disjoint permutation ranges и guard words. Transform feedback исполняет адаптированный vertex shader с direct/indexed depth и alpha, проверяет позиции и UV при nonzero baseInstance. Fake Vulkan syntax-only `VulkanAlphaSort.cpp`; core/stream, Stage 2–7/11–15 contracts и EGL, Stage19 direct misc emit, каталог 534 ID, 130277 CPU worldgen scenarios. CMake Debug/Release блокируются отсутствующими OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. EGL не является Vulkan runtime validation.

**Осталось:** физическое разделение материала при meshing, устранение повторной классификации при изменении камеры и уплотнение нулевых indirect commands. Mixed chunks требуют дополнительного обхода material IDs при small exact alpha sort, vertex pulling добавляет одну index load для mixed depth; для large alpha используется уже необходимый bin scatter. Эффективность на Intel/RTX не измерена, порядок depth draws может влиять на coplanar surfaces; нужна визуальная проверка. GPU-only allocation/publication, unified scheduler, LOD2+, GPU-only worldgen и SPIR-V/validation остаются открытыми.

### Этап 19 — Vulkan mixed fast mesh / bounded direct misc emit (2026-11-24)

**Сделано:** fast mesh транзакция поддерживает смешанные opaque/misc batches. CPU резервирует в двух существующих аренах непересекающиеся диапазоны; GPU выполняет opaque Count/cache/prefix copy и сразу полный misc Emit с точным счётчиком. `voxel_misc_emit.comp` использует локальную копию slot с ограниченным `fastBase + job * fastCapacity` вместо изменения live slot range. До 4096 misc quads/job, максимум 2 МиБ временного диапазона misc на батч из 16 jobs, плюс существующая opaque резервация до 1 МиБ; дополнительной VRAM allocation нет. После одного fence CPU проверяет opaque candidate metadata, точные misc counts, generation/revision, сохраняет только использованные диапазоны и освобождает gaps/tails. Переполнение любой части отклоняет обе части конкретного чанка, сохраняет старый меш и запускает точный fallback для этой ревизии. Если резервация misc не удалась, opaque allocation откатывается. Диагностика `fast mesh ... mixed ... misc overflow ...`; mixed считает успешные jobs смешанного батча, а не исключительно jobs с ненулевой misc geometry. `OPTICRAFT_VULKAN_FAST_MIXED=0` возвращает fast opaque-only Stage18; `OPTICRAFT_VULKAN_FAST_OPAQUE=0` отключает оба fast paths. Включено по умолчанию.

**Протестировано:** C++20/UBSan mesh transactions (две смешанные lanes, оба типа overflow, stale publication, rollback, cancel, exact ownership); syntax-only VulkanChunkMeshing/VulkanGpuMesher/VulkanMiscMesher с fake Vulkan headers; core/stream, mocked transfer, 534 ID catalog, Stage 2–7 contracts, 130277 worldgen scenarios. Mesa EGL: повторно Stage16–18 exact greedy/cache/opaque slab и новые три direct misc emit scenarios (capacities 4096/2/1, counts 2/32768/0/19/0, включая alpha/flora/liquid/shape/opaque, LOD skip, split dispatch, exact fit, overflow, guard words). Packed 32-byte records для помещающихся мешей совпадают с legacy Count/Emit; частичная геометрия overflow не публикуется по протоколу CPU. CMake Debug/Release конфигурация остановилась на OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY и OPENGL_INCLUDE_DIR. EGL и fake API не являются Vulkan runtime validation.

**Осталось:** CPU резервирует диапазоны и выполняет финальный readback/validation/publication. Misc fast allocation использует фиксированные временные strides с возвратом неиспользованной памяти, а не GPU compact prefix. При overflow выполняется дополнительный legacy pass; hit rate, стоимость combined dispatch и влияние на frametime/fragmentation не измерены. Для смешанных jobs с forceLegacy или нехваткой slab остаётся двухфазный конвейер. Нужны Vulkan SPIR-V compilation/validation, аппаратный benchmark Intel/RTX, общий GPU scheduler, реальное разделение cutout/blended, LOD2+, GPU-only worldgen. FPS не измерен.

### Этап 18 — Vulkan bounded GPU suballocation / fast opaque (2026-11-24)

**Сделано:** для батчей, содержащих только opaque jobs, CPU резервирует выровненный slab в существующей RangeArena. GPU Count строит точные rectangles, после compute→compute dependency GPU рассчитывает compact prefix offsets из неизменных counters и копирует cache в slab восемью workgroups/job. Отдельные Count readback, CPU allocation между проходами, slot-range upload, Emit counter reset и finalize dispatch в этом пути не нужны. GPU записывает candidate indirect commands; после одного frame fence CPU проверяет counts, offsets и generation/revision, переводит ownership slab в отдельные точные диапазоны, возвращает хвост, padding и устаревшие ranges. Старые draw и deferred release сохранены. Cache overflow / некорректная metadata направляют текущую ревизию в точный fallback без бесконечных fast retries; mixed opaque/misc batches, отсутствие непрерывного slab и отключённый cache используют прежний конвейер. Включено по умолчанию, `OPTICRAFT_VULKAN_FAST_OPAQUE=0` отключает только объединённый путь. Добавлена диагностика `fast opaque ... fallback ... reserve miss ...`. Дополнительная постоянная память — 256 байт readback на батч при batchLimit=16; временная резервация в существующей арене до 1 МиБ/батч (C=8192, 16 jobs), не новая VRAM allocation.

**Протестировано:** C++20/UBSan transactions, slab partition/coalescing, odd count alignment, zero-count commands, независимые lanes, stale generation/revision, overflow→fallback, corrupt metadata rejection, reservation pressure, cancellation; fake Vulkan syntax checks для VulkanChunkMeshing/VulkanGpuMesher; core/stream, mock transfer, catalog 534 ID, Stage 2–7 contracts, 130277 worldgen cases. Mesa EGL: предыдущие 6 exact Count/Emit и 3 cache scenarios, новые 3 fast slab scenarios (capacities 8192/6/5, counts 6/98304/6/0/951/954); packed rectangles, prefix across split Y dispatches, guard words, empty/overflow, selective publication совпали. EGL не является Vulkan runtime/validation. Debug/Release CMake остановлены на OpenGL development headers/GLX libraries.

**Осталось:** GPU-аллокация ограничена opaque-only batches и временным slab; глобальный allocator/retirement и final publish/readback остаются CPU-controlled. Mixed batches сохраняют две стадии readback; эффективность при смешанной очереди, hit rate и временное давление на арену не измерены. Нужны реальные Vulkan SPIR-V compilation, validation, Intel/RTX frametime, общий GPU scheduler, cutout/blended split, geometric LOD2+, GPU-only worldgen. FPS не измерен.

### Этап 17 — Vulkan opaque rectangle cache / bounded upload selection (2026-11-24)

**Сделано:** batch-private GPU cache сохраняет готовые 8-byte opaque rectangles во время Count. При количестве до 8192/job Emit копирует их восьмью workgroups вместо повторного обхода 192 плоскостей (6 для solid fast path); точный GPU count сохраняется GPU→GPU перед сбросом счётчиков. Если cache capacity превышена, выполняется исходный полный Emit без потерь; ошибочный cache count/candidate capacity отклоняется. Generation/revision, старый draw и deferred retirement сохранены. При двух батчах по 16 jobs кэш занимает 2 МиБ + 128 байт. Диагностика `OPTICRAFT_VULKAN_MESH_CACHE=0` отключает кэш; FPSCOUNTER выводит количество cache-copy dispatch jobs и overflow fallback jobs. Быстрый Emit больше не увеличивает Count-бюджет, медленный по-прежнему уменьшает его. Upload planner удаляет устаревшие пакеты и использует partial_sort первых K кандидатов вместо сортировки всей очереди, сохраняя приоритет resident updates, distance и deterministic tie-break. Версия 0.13 и OpenGL fallback сохранены.

**Протестировано:** C++20 mesh transactions/UBSan, syntax-only VulkanGpuMesher/VulkanChunkMeshing на fake API, stream queue/priority/dispatch, mock upload, каталог 534 ID, Stage 2–7 checks, CPU greedy coverage (5 сцен), 130277 worldgen scenarios. Mesa EGL: предыдущие 6 точных Count/Emit сцен и 3 новых multi-job сценария с cache capacity 8192/6/5 (counts 6, 0, 98304, 6, 0, 954, 951), включая cache hits, exact capacity, overflow, LOD, stable slots, guard words, copy, finalize/publish и mismatch rejection. Это не Vulkan/SPIR-V validation. CMake Debug/Release не прошли конфигурацию из-за OpenGL development headers/GLX libraries.

**Осталось:** измерить cost Count packing/cache writes и hit rate на Intel/RTX. GPU-only allocation и устранение двух host readback, параллельное вертикальное объединение, полноценный общий scheduler worldgen/uploads/meshing, разделение cutout/blended, геометрические LOD2+ и GPU-only worldgen не выполнены. GPU runtime, SPIR-V и фактические FPS/frametime не подтверждены.

### Этап 16 — Vulkan overlapping mesh transactions / exact bitmask greedy (2026-11-24)

**Сделано:** два независимых Count/Emit/Publish-батча с отдельными job indices, counters, readback и candidate draws; общие GPU-арены и stable slots. Каждый кадр допускает не более одной новой тяжёлой фазы Count либо Emit по общему адаптивному бюджету, а завершённая публикация больше не занимает отдельный кадр. Загрузка оригинальных вокселей разрешена во время ожидания fence, кроме фазы готового Emit и превышения backlog. Слот не допускается в другой батч до завершения предыдущего; проверка generation и revision до Emit и Publish, сохранение опубликованной геометрии при устаревании кандидата, отложенное освобождение диапазонов. Добавлены зависимости для записи shared indirect-команд после предыдущих чтений, устранены повторные сбросы recycled slots. Opaque greedy: 32-битные маски занятости и границ ключей строятся параллельно по строкам, точное 2D-объединение по маскам; Count не формирует packed rectangles. Формат и правила ID/meta/light/LOD сохранены. При LOD-only обновлении с совпадающими исходными ревизиями и границами GPU больше не получает повторные 128 КиБ raw voxel data. FPSCOUNTER показывает количество mesh jobs в полёте. Диагностика `OPTICRAFT_VULKAN_MESH_BATCHES=1` ограничивает число батчей одним; по умолчанию два. Версия 0.13, Vulkan-first и OpenGL fallback сохранены.

**Протестировано:** C++20 mesh transactions под UBSan (включая два батча, блокировку повторного слота, изменение ревизии, reuse, непересекающиеся диапазоны, OOM и retirement), syntax-only интеграции на fake Vulkan declarations; core/stream, LOD-only mock transfer, каталог 534 ID, Stage 2–6, 11–13 и primary window. CPU greedy reference — 5 сцен. Адаптированные GLSL Count/Emit выполнены в Mesa EGL в 6 сценах (0 / 6 / 98304 / 68615 / 39676 / 954 прямоугольников), полученные packed records точно совпали с предыдущим greedy-алгоритмом. Stage 14/15 EGL: 6 alpha и 28 misc сценариев; worldgen — 130277 CPU сценариев. CMake Debug/Release остановлены отсутствием OpenGL development headers/GLX libraries. Vulkan headers, glslangValidator, spirv-val и целевые GPU недоступны.

**Осталось:** аппаратная сборка/SPIR-V/validation, сравнение Intel/RTX и OpenGL. GPU allocation/publish без обязательного readback пока не реализованы; обе фазы Count/Emit повторяют поиск видимости, точное вертикальное объединение выполняет один invocation. Отдельная compute queue, общий бюджет worldgen+meshing, геометрические LOD2+, native GPU-only worldgen/save, разделение cutout/blended, дальнейшие AO/transfer/lighting оптимизации и удаление GL-контекста остаются открытыми. Ускорение FPS не измерено; изменения не объявляются аппаратно стабильными.

### Этап 15 — Vulkan performance diagnostics / selective depth sort (2026-11-24)

**Сделано:** misc culling компактизирует непустые видимые команды перед bitonic sort; порядок по глубине и ID сохраняется. При неизменной VP и ревизии опубликованной геометрии, без Hi-Z, frame-local misc/alpha commands переиспользуются; при изменении мира, камеры, resize или режиме Hi-Z кэш не используется. Early fragment tests для непрозрачного textured pass. Начальный порог Hi-Z на iGPU — 1024 резидента, на остальных устройствах — 256; `OPTICRAFT_VULKAN_HIZ_MIN_RESIDENT` (1–2048) позволяет сравнить пороги, `OPTICRAFT_VULKAN_HIZ=0` отключает Hi-Z. Консоль: фактический present mode, GPU total/cull/alpha/hiz, CPU update/render, misc reuse; timestamps читаются только после fence, без WAIT. Время стадий диагностическое, возможны перекрытие конвейера и задержки очереди; CPU render включает ожидания Vulkan. Качество, мир, LOD, Vulkan-first и GL fallback сохранены.

**Протестировано:** 28 сценариев исполнения адаптированного misc culler в Mesa EGL, 6 alpha сценариев, C++ cache smoke, core/stream, greedy/transactions, mock upload, catalog 534 ID, Stage 2–13, primary window, 130277 worldgen сценариев. Исправлены устаревшие строковые проверки Stage 7/13. EGL не заменяет Vulkan SPIR-V и аппаратное тестирование.

**Осталось:** измерить GPU и CPU на целевой Intel в Release с одинаковой дальностью/разрешением; сравнить Hi-Z, LOD и GPU worldgen по одному. Порог 1024 эвристический, реальный выигрыш FPS не измерен. Полная Vulkan-сборка здесь не подтверждена.

### Этап 14 — Vulkan material worldgen / surface LOD / alpha / lighting (2026-11-24)

**Сделано:** нативный Vulkan FP64 compute для глубоких материалов по существующим OpenSimplex2S таблицам. Подключён к очереди `Chunk_Manager`, дальность от 8 чанков, не более двух глубинных уровней за колонку; fence опрашивается без блокировки, результат проходит существующую выборочную CPU-проверку. При отсутствии FP64, ошибке или превышении GPU-бюджета — CPU fallback. Hi-Z текущего кадра применяется также к misc/alpha с консервативно расширенным AABB. GPU alpha sort выделяет blended quads и сортирует центры внутри чанка: exact bitonic до 1024 граней, 32 depth bins для больших мешей. Сохраняется межчанковый порядок. LOD1 непрозрачных поверхностей от 10 чанков (возврат при <8): прямоугольники объединяются без ограничения одинакового материала/света, fragment shader восстанавливает ID/meta/UV/light из исходного вокселя. Силуэт и границы остаются на исходной сетке; ABI 8 байт. Добавлены ближнее приближённое AO для непрозрачных сложных форм и цветной локальный свет от выбранного светящегося блока в voxel/entity/item shaders. Vulkan-first и OpenGL fallback сохранены. `OPTICRAFT_VULKAN_LOD=0` отключает LOD1.

**Протестировано:** C++ core/stream, LOD hysteresis и версии, greedy/transactions, mock upload, каталог 534 ID, Stage 2–13, worldgen 130277 сценариев, оригинальный OpenGL worldgen parity (4 seed × 64 samples). 11 изменённых Vulkan GLSL shaders компилируются после адаптации к headless EGL OpenGL; исправлено зарезервированное имя `packed`. Адаптированный alpha compute выполнен на EGL в 6 сценариях (0–2048 граней), включая exact и bucket sort. Это не Vulkan SPIR-V или аппаратный тест.

**Осталось:** полноценная Vulkan-компиляция/validation и измерения Intel/RTX. Поверхность, пещеры, структуры, финализация и сохранение GPU-only чанков остаются CPU; FP64 материал-стадия не является полным GPU worldgen. LOD1 не уменьшает разрешение воксельной сетки и не увеличивает лимит исходных GPU-слотов. Прозрачность приближённая между чанками и для больших мешей; нет глобальной точной сортировки. AO ограничено ближней зоной, цветной динамический свет только от выбранного блока, без теней и произвольных источников. Визуальный перенос моделей и интерфейса не подтверждён аппаратно.

### Этап 13 — Vulkan same-frame Hi-Z (2026-11-24)

**Сделано:** при поддержке sampled depth и необходимых compute limits основной Vulkan-кадр при >=256 резидентах разделяется на два совместимых render pass. Сначала frustum culler выбирает непрозрачные чанки с центрами в пределах 96 блоков по каждой оси от камеры; они записывают цвет и depth. `voxel_hiz.comp` строит иерархию MAX глубины из реальных пикселей текущего кадра (базовые тайлы 8×8, затем 2×2, включая нечётные размеры). Дальние непрозрачные AABB проверяются по проекции восьми углов с расширением области на пиксель и запасом глубины 0.0001. При пересечении near plane, неопределённой проекции или неполном перекрытии объект сохраняется. Команды дальнего opaque прохода формируются на GPU в compact/fixed режиме; затем восстанавливается color/depth attachment layout и продолжается отрисовка дальних блоков, misc, entities/items, alpha и HUD. Pyramid SSBO и descriptor set отдельны на frame slot; синхронизация depth→compute→depth и indirect read→write→read. При resize ресурсы пересоздаются после штатного device idle. На несовместимом устройстве или при `OPTICRAFT_VULKAN_HIZ=0` сохраняется обычный Vulkan frustum path. Vulkan-first/OpenGL fallback не изменены.

**Протестировано:** 6000 CPU reference запросов к пирамиде, 175 вариантов границы near/far, 1000 перспективных AABB, нечётные размеры и отверстия в depth, source contracts; C++20 syntax-only `VulkanHiZ.cpp`/`VulkanGpuCuller.cpp` на fake Vulkan declarations; предыдущие greedy, GPU mesh transactions, stream/core, mock upload, catalog 534 ID, Stage 2–6, primary window, Stage 11–12 и 130277 worldgen scenarios. Это не компиляция GLSL/SPIR-V или исполнение GPU.

**Осталось:** проверить validation layers, качество и время кадра на Intel/RTX; Hi-Z сейчас отсеивает только дальнюю opaque геометрию (misc/alpha не используют пирамиду). Порог 256 и радиус 96 — начальная эвристика, ускорение не гарантировано. Нативный GPU Worldgen, геометрические LOD, per-quad alpha, цветные динамические источники/тени, AO сложных форм и оставшиеся GL-ресурсы не завершены.


### Этап 12 — Vulkan entities, chunk depth sort, near AO (2026-11-24)

**Сделано:** misc culler сортирует видимые команды по глубине центра чанка (bitonic, один workgroup, до 2048 слотов, 16 КиБ shared memory); нулевые команды уходят в конец. Сортировка межчанковая, не per-quad. `VulkanItemPipeline` рисует существующую динамическую геометрию выпавших предметов с depth test/write, альфа-отсечением, исходным атласом, светом и туманом. `VulkanEntityPipeline` кэширует геометрию и RGBA-текстуры GLB, сохраняет primitive ranges, скрытые материалы, hurt flash и матрицы до 64 костей; GPU skinning в vertex shader, индексированные draw с firstInstance как индексом pose SSBO, отдельный pose buffer на frame slot. Через `Mob::ScopedEntitySink` подключены обычные мобы, боссы, их активные части и клинки, жители, големы и тело игрока. Порядок проходов: opaque, cutout, entities/items, alpha, HUD. OpenGL-ресурсы всё ещё загружаются для fallback; preview игрока не перенесён. AO непрозрачных блоков приближённое, выборка соседних вокселей (включая доступные диагональные чанки) в fragment shader, без изменения greedy ABI; плавное выключение 20–32 блока. Направление солнца меняет вклад небесного света; локальный блочный свет не умножается на этот коэффициент.

**Протестировано:** 56 CPU reference случаев bitonic, 512 вариантов соседних координат и 8 AO комбинаций; C++ layout EntityVertex/EntityPose и владение asset, source contracts 21 GLB-модели, mock Vulkan raw RGBA image upload, предыдущие core/greedy/stream/mesh/catalog/primary window/compact, регрессии боссов и 130277 worldgen cases. Это не исполнение GLSL или аппаратный Vulkan тест.

**Осталось:** Hi-Z, нативная GPU-генерация дальних чанков, геометрические LOD, per-quad прозрачность, точный AO сложных форм, цветные динамические источники/тени, preview игрока и остальные эффекты. Сборка Vulkan/SPIR-V и визуальное совпадение моделей на драйвере не подтверждены; возможна дополнительная GPU-стоимость сортировки и AO. Vulkan-first/OpenGL fallback сохранены.


### Этап 11 — Vulkan compact indirect / atmosphere (2026-11-24)

**Сделано:** исправлено несовпадение объявления и реализации `VulkanMiscPipeline::recordDraws`. При наличии Vulkan 1.2 `drawIndirectCount`, `multiDrawIndirect` и достаточного `maxDrawIndirectCount` непрозрачный GPU culler уплотняет ненулевые видимые команды через atomic append в буфер текущего frame slot. `vkCmdDrawIndirectCount` читает GPU-счётчик без CPU readback. На остальных устройствах сохранён fixed indirect. Счётчик обнуляется transfer перед compute, включая зависимость transfer/compute → indirect для пустого результата. Misc/alpha пока сохраняет старый порядок стабильных слотов. Отсечение неправильной категории alpha/cutout перенесено также в vertex stage через specialization constant; неподходящие треугольники не растеризуются. Параметры камеры/ambient/тумана передаются в 112-байтном push constant block. Туман начинается на 70% настроенной дистанции, конец ограничен camera_far; небесный свет зависит от времени/погоды, блочный свет не умножается на ambient. В лог запуска Vulkan добавлены имя физического устройства и режим compact/fixed indirect.

**Протестировано:** 28 CPU reference случаев компактации и статические контракты feature/barrier/clip/view; C++ проверка layout push constants; core/stream, greedy reference, mesh transactions (UBSan), mock transfer, каталог 534 ID, Stage 2–6 и primary window; worldgen 130277 сценариев. CMake остановлен на отсутствующих OpenGL dev-библиотеках. GLSL/SPIR-V, визуальная проверка и измерение ускорения на GPU не выполнены.

**Осталось:** прозрачность не отсортирована по глубине, native GPU worldgen, LOD, Hi-Z, AO/динамический свет, мобы/боссы/предметы и эффекты не перенесены. Туман использует настроенный радиус, а не фактическую границу резидентности. Vulkan-first и OpenGL fallback сохранены.


### Этап 10 — Vulkan selective meshing / sparse air residency (2026-11-24)

**Сделано:** Count/Emit группируют непрерывные диапазоны задач по категориям материалов, сохраняя исходные индексы readback. Для полностью непрозрачного чанка dispatch содержит 6 внешних плоскостей вместо 192; opaque/misc dispatch отсутствует для задач без соответствующей категории. Счётчики пропущенных задач обнуляются transfer-командами с необходимыми зависимостями до readback/finalize. Границы сравниваются точно, по ID/meta/light каждой клетки шести плоскостей; изменение внутри чанка не инвалидирует соседей, изменение грани затрагивает только её соседа. Новые/заменённые/выгруженные чанки обновляют все соответствующие соседние слоты. CPU-хранилище границ: 24 КиБ на слот (48 МиБ при 2048 слотах). При замороженном upload не сортируется очередь источников. Пустые чанки вдали от геометрии исключены из выбора GPU-резидентности; сохраняются пустые чанки, соседние с непустыми, в пределах общего бюджета. При изменениях выбор обновляется по topology/dirty epoch и движению; проверка ревизий в неизменном мире ограничена с резервным сканированием через 250 мс. При большом backlog мешинга приостанавливаются новые uploads. Счётчик pending O(1); неудачные аллокации мешей повторяются при фактическом безопасном освобождении GPU-памяти. В консоли добавлены selected, air skip, retry.

**Протестировано:** C++ core/stream (точные границы: ID/meta/light на каждой из 6 сторон, угол, внутреннее изменение; dispatch masks; air halo), mesh transactions с UBSan (повтор OOM после освобождения, отмена при выгрузке), mock Vulkan transfer, каталог блоков, статические контракты Stage 2–6, primary window, пять эталонных CPU greedy сцен, worldgen 130277 сценариев. CMake остановлен на отсутствующих OpenGL development libraries/headers; полный Vulkan build, GLSL и измерения на GPU не подтверждены.

**Осталось:** дальняя нативная GPU-генерация и LOD, хранение большего числа непустых чанков, перенос сущностей/эффектов, аппаратная проверка; Vulkan-first и OpenGL fallback сохранены. Лимит 2048 исходных слотов остаётся, увеличение фактической дальности зависит от состава загруженного мира. Не заявляется гарантированное ускорение FPS.

### Этап 9 — Vulkan streaming / meshing hotfix (2026-11-24)

**Сделано:** при активном Vulkan отключены legacy GL mesh jobs, GL neighbor mesh scan и GL GPU-worldgen; при переключении на OpenGL все резидентные чанки помечаются для штатной пересборки. Vulkan raw chunk storage увеличен до 2048 слотов (с учётом maxStorageBufferRange, до 256 МиБ исходных данных), источник получает фактический лимит. Ближайшие снимки получают приоритет; выбор резидентов кэшируется, снимки ограничены 4 на проход и 32 ожидающими загрузками. Загрузка источников приостановлена на период Count/Emit/Publish; выгрузки продолжают инвалидировать устаревшие задачи. Пустые и полностью закрытые непрозрачные чанки сбрасывают draw без compute; непрозрачные чанки с палитрой только occluder-блоков пропускают внутренние плоскости и закрытые соседями границы; misc и opaque пропускаются независимо. Все категории материалов остаются в каталоге. Стоимость батча взвешивается по типу чанка; при поддержке timestamps корректируется по завершённым GPU замерам, цель 2.5 мс, без WAIT на запросах. В консоли FPS по успешным Vulkan present, резиденты, очереди, опубликованные/устаревшие меши, ошибки выделения, время последнего замера и бюджет.

**Протестировано:** C++ core/stream, mesh transactions (UBSan, включая новый weighted budget, stale token, empty transitions), CPU reference greedy, Stage 6 integration; ранее в этом проходе mock Vulkan, catalog, Stage 2–4 и primary-window contracts прошли. Повторный полный запуск остановлен тайм-аутом после Stage 6 integration. Реальные SPIR-V, Vulkan timestamps, сборка игры и производительность на драйвере не проверены.

**Осталось:** измерить на Intel iGPU и RTX, завершить перенос сущностей/эффектов, нативную дальнюю генерацию и LOD. 2048 слотов не означают поддержку полной максимальной дальности; GL worldgen временно заменён CPU в Vulkan-режиме. Скрытый GL контекст остаётся.

### Этап 8 — основное Vulkan-окно, HUD и двумерный GPU greedy meshing (2026-11-24)

**Сделано:** `Window` первым пытается создать скрытое GLFW_NO_API окно при доступном Vulkan; после подготовки legacy GL-ресурсов выполняется инициализация Vulkan device/swapchain, world и UI pipelines. При успехе показывается только Vulkan-окно, при неудаче — OpenGL. Ошибка Vulkan-кадра вызывает отсоединение GPU-подсистемы, освобождение Vulkan-ресурсов и переключение ввода на OpenGL без пересоздания мира. Скрытый GL-контекст пока обязателен для существующих игровых ресурсов; это не чистый Vulkan-only запуск. `OPTICRAFT_RENDERER=opengl` принудительно выбирает GL; `OPTICRAFT_VULKAN_GPU=integrated|discrete` и `OPTICRAFT_VULKAN_VALIDATION=1` служат для проверки устройства. Software CPU Vulkan не выбирается. Отдельный `OptiCraft_Vulkan` остаётся тестом swapchain.

Пакеты HUD (меню, текст, хотбар, иконки, инвентарь) передаются в нативный Vulkan UI graphics pipeline, объединяются по последовательным одинаковым атласам без нарушения порядка прозрачности; отдельный vertex buffer на каждый кадр. OpenGL-кадр не копируется в Vulkan. GPU-резидентность больше не зависит от GL mesh readiness или CPU frustum, исходные блоки отправляются по block revision/dirty epoch. Greedy compute теперь объединяет прямоугольники 32×32 с одинаковыми ID/meta/light, формат остаётся 8 байт; 192 workgroups на чанк, 64 lanes на плоскость. Реальная производительность не измерена.

**Протестировано:** CPU reference пяти сцен (включая сплошной чанк: 6144 грани → 6 прямоугольников), C++20 UI batching, core/stream, mesh transactions, fake Vulkan, контракты Stage 2–6 и каталог блоков, регрессии боссов, 130277 worldgen сценариев. CMake не завершает конфигурацию из-за отсутствующих OpenGL dev-зависимостей; SPIR-V и работа Vulkan на драйвере не проверены.

**Осталось:** аппаратная сборка и проверка всех состояний блоков, перенос GL-зависимых ресурсов, мобов/боссов, выпавших предметов, preview игрока, неба и постэффектов; AO, динамические источники света, прозрачность, полный GPU-worldgen и GPU-only сохранение, геометрический LOD и два радиуса. GL-compatible подготовка мешей ещё присутствует в Chunk_Manager. Текущий лимит GPU-резидентности 512 чанков. Для полноценного survival до переноса сущностей использовать OpenGL.

### Этап 7 — GPU row meshing, блоковый и небесный свет (2026-11-24)

**Сделано:** compute-мешинг непрозрачных блоков вместо отдельной записи для каждой видимой грани строит полосы длиной до 32 блоков, параллельно по 6144 строкам (по 1024 на направление). Соседние грани объединяются только при совпадении ID, metadata и света перед поверхностью. Геометрия остаётся 8 байт на запись; добавлены 5 бит длины полосы, стабильный slot ограничен 16 битами (до 65536 слотов). Count и Emit используют один `voxel_row_runs.glsl`, сохраняют прежнюю Count → Allocate → Emit → Validate → Publish схему и не выполняют CPU-мешинга. Количество compute workgroups на чанк уменьшено с 512 до 96, но стоимость одного потока увеличена из-за поиска полос — выигрыш времени GPU **не измерен**.

Обычные, полупрозрачные и сложные блоки теперь используют имеющийся в `Chunk_Light_Grid` блочный и небесный свет из GPU persistent buffer; для непрозрачных граней свет считывается из открытой клетки перед гранью, в том числе из соседнего резидентного чанка. Переписаны opaque vertex/fragment shaders для повторения текстуры на длинных полосах и выборки с `textureGrad` / защитой от захвата соседних тайлов атласа. Дополнены descriptor SSBO bindings; компиляция шейдеров по CMake учитывает новый include `voxel_row_runs.glsl`. Лицо блока внутри подземной пещеры больше не должно получать только фиксированную дневную яркость.

**Протестировано:** Python CPU reference `test_vulkan_gpu_rows.py` сравнивает восстановленные из packed полос грани с полным эталоном для пустой, сплошной, шахматной, различающейся по metadata/light и случайной секции; совпадение полное. Тесты Stage 2–6, ядра, каталога, мок-передачи Vulkan пройдены. `test_worldgen_fastpaths.py`: 130277 сценариев пройдены. **Не проверено на устройстве:** SPIR-V compilation, graphics/Vulkan Validation Layers, качество UV, производительность, точность световых швов.

**Осталось:** полноценный двумерный greedy meshing (текущий алгоритм склеивает только линейные полосы), точный AO/угловое освещение, динамические цветные источники света, корректная сортировка прозрачности, проверки всех 534 определений на GPU, перенос UI/sky/мобов/боссов, GPU worldgen всех дальних чанков с caves/structures parity, геометрические LOD и сохранение GPU-only чанков. Vulkan продолжает работать в режиме отдельного диагностического окна; это не завершённая замена OpenGL.



### Этап 6 — живой Vulkan-кадр из Game, GPU-мешинг и публикация геометрии (2026-11-24)

**Сделано:** `VulkanChunkMeshing` подключает резидентные GPU-буферы блоков к обеим compute-веткам Count → Allocate → Emit → Publish. `GpuMeshTransactions` отслеживает поколения/ревизии, проверяет количество граней после GPU readback, сохраняет опубликованный меш до подтверждения новой версии и откладывает освобождение старых диапазонов на несколько подтверждённых кадров. Непрозрачная геометрия передаётся в текстурированный Vulkan vertex-pulling pipeline, включая UV-атлас и metadata. `VulkanLiveWorldRenderer` записывает один кадр: пакетная загрузка → пересборка изменённых чанков → GPU culling для обычных и сложных граней → indirect draws непрозрачных, alpha-cutout и полупрозрачных блоков. GPU не пересчитывает неизменные чанки. Поддержаны изменения размеров swapchain и повторное создание graphics pipelines.

`VulkanGameMirror` подключён к настоящему `Game::render_gameplay` через прежний `VulkanGameChunkFeed`: создаётся отдельное окно Vulkan (`GLFW_NO_API`) с тем же игровым миром и матрицей камеры. Основное окно OpenGL остаётся работающим и сохраняет управление, UI, небо, мобов и боссов. При доступном Vulkan SDK **и** glslangValidator диагностический Vulkan-просмотр включается автоматически при запуске `OptiCraft` из CLion; `OPTICRAFT_VULKAN_PREVIEW=0` отключает его без пересборки. При ошибке Vulkan превью отключается, но OpenGL-игра продолжает работать. `VulkanGameChunkFeed` выбирает ближайшие к камере чанки в пределах резидентного бюджета (по умолчанию 512) и выгружает устаревшие, чтобы не переполнять очередь при очень большой OpenGL-дальности. У Vulkan-окна нет второго CPU-мешинга или повторного запроса списка чанков.

**Протестировано:** C++20 тесты `GpuMeshTransactions` с UBSan, core/frustum, stream queue, каталог форм/материалов, fake Vulkan upload/atlas и источниковые контракты Vulkan frame, а также 130277 сценариев worldgen — пройдены. GLSL→SPIR-V, сборка Vulkan/GLFW/GLM, драйверная и визуальная проверки **не проводились**: в тестовой среде отсутствуют Vulkan SDK, glslangValidator и графические dev-заголовки (CMake не находит OpenGL). Сравнение пикселей с OpenGL на реальном железе не проведено.

**Осталось:** аппаратная сборка и исправления по validation; GPU greedy meshing (текущий мешер создаёт отдельные грани), корректная сортировка прозрачных поверхностей, идентичность всех состояний жидкостей/сложных блоков, свет/AO, HUD, sky, мобы/боссы, прямое Vulkan-окно как основной игровой renderer без OpenGL, GPU worldgen дальних чанков, LOD и асинхронные сохранения. Этот этап — живой **диагностический** Vulkan-путь, НЕ полноценная замена OpenGL.


### Этап 5 — резидентные GPU-чанки, staging и Vulkan-атлас (2026-11-24)

**Сделано:** `VulkanChunkStreamPlan` принимает настоящие `LiveChunkPacket` из `Game`, ограничивает очередь, объединяет повторные ревизии, защищает идентификаторы поколениями и при появлении/выгрузке чанка помечает соседей на GPU-ремешинг. `VulkanGameChunkFeed` экспортирует не более 16 изменённых чанков за кадр вместо полной сериализации мира за один вызов. `VulkanChunkStreamGpu` создаёт постоянные VkBuffer под полные данные блоков 32³, meta, свет, две таблицы слотов, массив активных слотов и indirect-команды. Плотный список активных слотов обновляется только при появлении/выгрузке резидента, а не при изменении его блоков. Копии группируются в один bounded пакет через переиспользуемый staging-буфер для каждого из двух кадров, с transfer↔compute/graphics barriers. Нет запросов и GPU-копий при неизменном состоянии, нет CPU-мешинга и отдельных VkBuffer на каждый чанк. Переработанные/выгруженные слоты очищают устаревшую indirect-команду; существующий чанк при изменении блоков сохраняет старый меш до публикации новой версии.

`VulkanMaterialTables` один раз переносит каталог реальных материалов и форм (`GpuBlockCatalog`) в GPU storage buffers. `VulkanBlockAtlas` декодирует существующий blocks_main PNG без OpenGL, создаёт `VkImage`, `VkSampler` и набор дескрипторов; mip-цепочка для спрайтов 16×16 строится на видеокарте отдельными областями blit для каждого тайла, чтобы не смешивать соседние текстуры. Исправлена ориентация UV для Vulkan, поскольку оригинальный OpenGL-атлас загружался с вертикальным отражением. Все изменения относятся к **0.13 — Vulkan render**, история 0.12.x сохранена.

**Протестировано:** независимые C++20 тесты очереди, дедупликации, neighbour invalidation и переполнения; fake-Vulkan имитация выделения памяти, копирования настоящих packed blocks/meta/light, таблиц слотов, публикации смещений, каталога и PNG атласа (включая проверку всех 4 уровней раздельных blit); UBSan на тестовом контуре; статическое покрытие реестра 534 определений и 130277 сценариев worldgen — успешно. Fake-Vulkan тест **не является сборкой настоящего Vulkan или проверкой видеодрайвера**. Vulkan SDK и glslangValidator не доступны в тестовом окружении.

**Осталось:** связать `VulkanChunkStreamGpu::record()` и `VulkanMaterialTables` с активным циклом `Game`, выполнить реальный GPU Count→Allocate→Emit→Publish по всем видам блоков, построить и отсортировать indirect draw lists, перенести непрозрачные блоки с debug-цветов на атлас, а затем HUD, небо, мобов, боссов и остальные игровые проходы. Дальний GPU-worldgen, LOD и сохранение GPU-only чанков пока не реализованы. **Вулкан-версия игры ещё не готова к запуску; OpenGL остаётся активным рендерером.**

### Этап 4 — подключение игровых данных и каталог всех блоков (2026-11-24)

**Сделано:** из существующего `Game::render_gameplay` подключена пакетная отправка исходных данных реально загруженных видимых чанков в `VulkanGameChunkFeed` (только новые/изменённые ревизии; без повторного запроса списка чанков и без CPU-мешинга). При выгрузке и смене мира снимки аннулируются. Чтение JSON-атласов и UV теперь возможно без OpenGL-контекста. `VulkanBlockCatalog` формирует GPU-таблицы для всех размещаемых определений `Block_Registry`: непрозрачные и прозрачные кубы, растения, вода и лава, сложные блоки (в том числе состояния и ориентации). Shape boxes берутся из общего `shape_boxes()`; отдельный opaque-mesher flags ABI предотвращает путаницу с битами прозрачности.

Добавлены GPU compute count/emit для нетипичных блоков и Vulkan graphics pipeline с UV-атласом, cutout и отдельным alpha-blend проходом. Зависимости от соседних блоков вычисляются в шейдере для лестниц, заборов, стенок, панелей и проводов. Эти файлы являются частями подсистемы, **не подключёнными пока к кадру VulkanFrameRuntime**.

**Протестировано:** автономные C++20 тесты каталога и GPU World Core, исходные/ABI-контракты Stage 2–3, статическая проверка 534 определений блоков/спрайтов (включая 231 размещаемый shaped блок) и 130 277 сценариев worldgen — пройдены. Настоящие шейдеры Vulkan и рендеринг на Intel/RTX **не проверены** из-за отсутствия Vulkan SDK, заголовков, glslangValidator и графического оборудования в среде.

**Осталось:** интеграция драйвера Vulkan в `Game` как активного графического пути (прежние Renderer/HUD/мобы всё ещё OpenGL), фактические выделение/заполнение SSBO из packet feed, загрузка атласа в VkImage и VkDescriptorSet, таблица indirect-команд по типам, сортировка прозрачности, свет и стили жидкостей, реальная корректность всех форм, LOD и полноценный GPU Worldgen дальних чанков. Нельзя считать Vulkan готовой игровой заменой.

### Этап 3 — окно Vulkan, swapchain и кадры в полёте (2026-11-24)

**Сделано:** отдельный запускаемый target `OptiCraft_Vulkan` на библиотеке `OptiCraft_VulkanWindow` (не подменяет `Game` и не зависит от сборки mesh shaders). GLFW `NO_API`, создание Vulkan 1.2 instance/device/surface, выбор подходящей graphics+compute+present очереди и GPU, swapchain с обработкой resize/minimize/out-of-date, color+depth attachments, кадры в полёте (2), command pools и command buffers на кадр, per-image presentation semaphores и fences для повторного использования изображений. Поддержан режим Vulkan Validation Layers, выбор Intel iGPU (`--integrated`) / дискретного GPU (`--discrete`). Runtime предоставляет основной primary command buffer и render pass, совместимый с будущим vertex pulling/secondary draws. OpenGL и игровые системы не изменены.

**Протестировано:** отдельные core/frustum и source-contract тесты, регрессии worldgen и боссов — пройдены. Vulkan SDK, заголовки, драйверная/визуальная проверка, GLSL→SPIR-V и запуск отдельного Vulkan-окна в среде сборки не доступны. `OptiCraft_Vulkan` — проверка presentation, ещё **не полнофункциональная игра**: пока отрисовывает только меняющийся фон, без чанков.

**Осталось:** подключить реальные persistent storage buffers + staging/readback, GPU mesher/culler к primary command buffer, материалы/формы/свет, swapchain lifecycle интерфейс для pipeline recreation; перенести GPU worldgen и интегрировать новую ветку в `Game` после проверки совместимости.

### Сделано

- Начата новая графическая подсистема Vulkan без удаления текущего OpenGL-рендерера. Исходный игровой процесс пока работает через OpenGL; отдельный Vulkan-бэкенд ещё не подключён к `Game` и не имеет собственного swapchain.
- **GPU-only meshing:** библиотека Vulkan рассчитывает видимые грани полных непрозрачных кубов в compute shaders из исходных блоков 32³; упакованные грани остаются в GPU-буферах. CPU не формирует вершины или индексы. Greedy, жидкие/нестандартные формы и материалы ещё не перенесены.
- **Безопасная публикация мешей:** `Count → Emit → Finalize` создаёт кандидат, не изменяя опубликованную indirect-команду. Только после асинхронной проверки результата отдельный `voxel_publish.comp` может заменить активную команду. Старая геометрия удерживается до завершения использующих её GPU-команд.
- **GPU frustum culling:** `voxel_cull.comp` принимает пакет стабильных слотов и строит независимый плотный массив `VkDrawIndirectCommand` для текущего кадра. Невидимые чанки получают `instanceCount=0`; GPU не требуется возвращать список видимых чанков на CPU.
- **Graphics pipeline:** добавлен диагностический vertex-pulling рендер packed-граней без CPU vertex/index buffers, с динамическими viewport/scissor и глубиной; подготовлена совместимость с записью secondary command buffers в несколько потоков и пакетами `vkCmdDrawIndirect`.
- **Управление памятью:** аллокатор проверяет точное владение выделенным диапазоном, отвергает повторное освобождение/освобождение поддиапазона. Массовое отложенное освобождение выполняется линейно, без квадратичных сдвигов элементов.
- Сохранены две концептуальные дистанции: `render` (видимые GPU+CPU чанки) и `actual` (полноценные CPU-authoritative данные). Переключение зон, экран настроек и GPU worldgen в Vulkan остаются неподключёнными.

### Протестировано

- C++20 автономные проверки Vulkan World Core, потокобезопасной очереди, arena/retirement и консервативного Vulkan frustum — пройдены.
- Python-регрессии старых боссов, деревень и контрактов 0.12 — пройдены; Vulkan SDK, SPIR-V компилятор и драйвер в проверочной среде отсутствуют, поэтому **сборка Vulkan-библиотеки и запуск кадров на GPU не подтверждены**.

### Осталось

- Создать Vulkan instance/device, swapchain, frame graph, кадры в полёте и синхронизацию GPU meshing → publish → culling → drawing → present, подключить к `Game`.
- Реализовать полноценную генерацию дальнего мира на GPU с CPU/GPU parity для пещер, рек и структур, GPU LOD без перекрытия проходов, материалы/текстуры и прозрачность, сохранения GPU-only чанков.
- Проверить компиляцию SPIR-V, Vulkan Validation Layers, корректность отрисовки и FPS на Intel iGPU и RTX 4060 Laptop.

Подробнее: `documentation/VULKAN_ARCHITECTURE_IMPLEMENTATION_013.md`.

## 0.12.2 — HUD и летающие боссы (2026-11-21)

### Сделано

- Полоска опыта перенесена под здоровье и голод, непосредственно над хотбаром.
- Верхняя полоска босса с именем, здоровьем и состоянием (до трёх ближайших боссов в пределах 120 блоков); поддержан и существующий Рыцарь.
- Принцесса Луны: модель высотой около 3,1 блока, 300 HP, 4 самостоятельных щита по 45 HP. Пока есть хотя бы один щит, урон по основному телу не проходит. Воздушные лезвия летят к игроку и исчезают при столкновении с твёрдыми блоками. Периодические сближения позволяют атаковать щиты даже без дальнобойного оружия.
- Принцесса Солнца: модель высотой около 3,3 блока, 250 HP, 4 орбитальные ладони по 70 HP. После потери каждой ладони скорость перемещения увеличивается, а время между ударами уменьшается. Без ладоней начинается вторая фаза с ближними атаками.
- Спавн командой `/boss moon_princess` или `/boss sun_princess` (сокращения `moon`, `sun`), а также отдельными яйцами из Creative (`Moon Princess Spawn Egg`, `Sun Princess Spawn Egg`).
- Летающие боссы используют существующую 3D-навигацию; для моделей учтён реальный размер тела при проверке препятствий.

### Доработка 0.12.2 — боевые комбинации (2026-11-21)

- Рыцарь обучается на завершённых тактических эпизодах, исходах своих ударов и времени повторных уклонений игрока. Может задерживать взмах с остановкой анимации и отходить при слишком близком противнике, а также подбирать комбинацию из доступных приёмов на основе наблюдений.
- Принцесса Солнца получила 9 боевых техник с подготовкой/активным окном/восстановлением, воздушные шаги и рывки, анимированные движения конечностей; все четыре ладони могут остаться целыми при её убийстве.
- Принцесса Луны получила три режима залпа воздушными лезвиями и отдельные скелетные анимации; щиты сохраняют прежнюю уникальную механику.
- Обе принцессы получили по семь костей, Солнце — 10 анимационных клипов, Луна — 3. Старые модели сохраняют геометрию и материалы.
- Колосс и Теневой охотник получили различающиеся по таймингам упрощённые комбинации, чтобы все существующие боссы имели читаемые окна атак.
- Подробности: `documentation/Boss_Combat_Choreography_0122.md`.

### Протестировано

- C++20 smoke-тесты HP, отдельных щитов/ладоней, перехода фазы и ускорения; Python-проверки текстур, моделей, команд, HUD и конфигурации.
- Регрессия генерации мира: 130 277 сценариев. Полный запуск OpenGL и проверка игрового боя в CLion не проводились из-за отсутствия системных графических dev-зависимостей.

### Осталось

- Полная сборка и ручная проверка в CLion: dev-зависимости OpenGL/GLFW/GLM в проверочной среде отсутствуют.
- Колоссу и Теневому охотнику нужны отдельные скелетные анимации ударов; звуки, частицы и баланс требуют игрового тестирования.

## 0.12.1 — Nether, огонь, связи порталов и навигация мобов (2026-11-20)

### Деревни, жители, торговля и големы (дополнение 0.12.1, 2026-11-21)

- Добавлены жители с профессиями и рабочими местами: фермер/компостер, лучник/стол стрелодела, библиотекарь/кафедра, охотник/стол охотника, повар/коптильня, кузнец/кузнечный стол. Для каждой профессии предусмотрены три сделки за товары и изумруды, открываемые ПКМ.
- Распознавание закрытых домов по кровати, двери, крыше и факелу; привязка жителей к свободным кроватям, дневная работа и ночной возврат по A*. Новые генерируемые дома содержат кровать, факел и рабочее место; на ферме компостер.
- Размножение при свободном доме, наличии фермера и зрелого урожая; сохранение жителей и големов между сеансами.
- Железные големы защищают деревни от враждебных мобов и реагируют на атаку игрока. Лимит по численности деревни: 5/12/22/35/51/70/92 жителей, максимум 7 големов.
- Проверены отдельные C++20-тесты правил домов/профессий/порогов големов и Python-контракты ресурсов. Полная сборка игры и визуальная проверка **не проводились**: отсутствуют GLFW/GLM dev-зависимости. Известные ограничения описаны в `documentation/VILLAGE_SIMULATION_0121.md`.

### Навигация мобов (дополнение 0.12.1)

- Наземный A* с учётом размеров моба, уступов, препятствий и опасных блоков; трёхмерный A* для летающих видов.
- Асинхронный поиск по безопасному снимку мира, ограниченная очередь, интервалы перестроения маршрутов и отмена при смене измерения.
- Маршруты подключены к свиньям, овцам, коровам, лошадям без всадника, врагам и обычному движению рыцаря.
- Небольшие waypoint-отрезки пересчитываются при изменении цели/блоков, без изменения боевых анимаций и физики езды.
- Детали и проверенные ограничения: `documentation/MOB_NAVIGATION_0121.md`.

### Сделано

- Генерация Nether в диапазоне Y=0…512, сплошная крыша Mantle Stone на Y=512. Выше неё воздух; до общего предела OptiCraft Y=1024 можно строить, как и в остальных измерениях.
- Отдельный предмет **Lighter** (ID 528), рецепт: железный слиток + древесный уголь, прочность 128 применений. Добавлен временный блок **Fire** (ID 529) с отдельной пиксельной текстурой, свечением, слабым распространением на горючие блоки и сгоранием. Ограничение: до 2048 активных огней и до 128 обработанных за секунду.
- Сохранение огня отдельно для каждого измерения (`fires[_dimension].txt`); огонь наносит периодический урон игроку в Survival и ближайшим мобам.
- Nether Portal / End Portal не доступны в Creative и не могут ставиться как предмет: они создаются через активацию рамки или отладочную команду.
- Порталы связываются через сохранённые точки в `portal_sites.txt`; возврат использует ближайший подходящий существующий портал, а не строит новый рядом при каждом переходе. Для старых миров предусмотрен локальный поиск незарегистрированного портала и проверка разрушенных привязок.
- Из End в Overworld игрок возвращается на исходную точку спавна обычного мира; новый портал в Overworld при этом не создаётся.
- После перехода нельзя автоматически вернуться обратно, пока игрок не выйдет из активного портала.

### Протестировано

- Отдельные C++20 smoke-тесты реестра порталов и симуляции огня; Python-контракты JSON/генерации/взаимодействий.
- Полный запуск в CLion/OpenGL, реальная производительность Nether Y=512 и переходы со старыми сохранениями здесь не проверены.

### Осталось

- Проверить игровые переходы на железе, рендер огня и FPS при больших расстояниях прорисовки.
- Старые уже сгенерированные чанки Nether не переписываются (необходим новый мир/неисследованная область, чтобы увидеть новую высоту). Перенос существующих миров и привязок требует отдельной проверки.
- Порталы остаются в бета-тесте; перед обновлением рекомендуется резервная копия мира.


## 0.12 (2026-11-19)

Дата: 19 ноября 2026 г. Предыдущая версия: 0.11.

## Новое

- **Система измерений:** регистрация встроенных Overworld, Nether, End, определение дополнительных миров из `assets/dimensions.json`; независимое хранение изменённых чанков, позиций игрока, посевов и саженцев.
- **Nether:** незерак, базальт, лавовые пространства, бедроковый потолок, пещеры, стартовая каверна и собственные настройки света/фона.
- **End:** центральный остров, внешние острова, обсидиановые монолиты, собственная генерация и атмосфера.
- **Порталы (бета):** обсидиановая рамка 4×5 с активацией огнивом, горизонтальная рамка End 5×5 с глазами Эндера, задержка перехода, создание обратной точки и масштаб координат Overworld/Nether 8:1.
- **Команды:** `/portal status`, `/portal build nether`, `/portal build end`, `/portal nether`, `/portal end`, `/portal overworld`, `/portal ignite`, автодополнение команды и очередь постройки при незагруженных соседних чанках.

## Внешний вид и взаимодействие

- Рычаги и факелы получили объёмную геометрию; у дверей появился проём, у стола зачарования отдельные текстуры граней.
- Объёмные иконки в хотбаре и инвентаре для дверей, кроватей, нажимных плит, факелов, рычагов и кнопок.
- Кнопки: крепление к шести сторонам, тело 6×4 пикселя, уменьшенная толщина при нажатии.
- Вода: устранён зазор у полной поверхности, согласованы верх и боковые грани.
- Лава: исправлен выбор спрайта жидкостей, вода остаётся прозрачной, лава получает собственные UV и режим непрозрачности.

## Рендеринг и генерация

- Сохранён формат `uvec4` по 16 байт на GPU-квад; для признака лавы использован свободный бит без расширения записи.
- Уточнены счётчики GPU-culling и meshlet: `Submitted tris`, Frustum и Hi-Z отображают отправляемую геометрию, а не пиксели, реально видимые после depth-test.
- Оптимизирована генерация профиля островов End по X/Z, без повторения вычислений для каждого Y.
- Снижена нагрузка от skylight в измерениях без неба и от большого количества светящихся клеток лавы.
- Обновления жидкости и сохранения объединяются в пакеты с ограничением операций за кадр.

## Стабильность и диагностика (0.12)

- **Большие миры:** загрузка при высокой горизонтальной дальности сохраняет полный вертикальный диапазон рядом с игроком, но сокращает его на дальних кольцах. При `R=48, H=8` теоретическое число позиций в окне уменьшается со 159 953 до 20 961 (модель окна, не RAM-бенчмарк).
- **Выгрузка:** вместо создания огромного массива всех удалённых чанков при каждом проходе используется ограниченная куча кандидатов; при избытке резидентных чанков размер пакета выгрузки увеличивается. Это снижает временные выделения памяти и ускоряет ликвидацию накопившегося backlog.
- **Переход в Survival из полёта:** при падении через ещё не загруженный чанк вертикальное перемещение приостанавливается, пока соответствующий чанк не будет доступен, без ложного приземления или урона.
- **F3 (Linux):** добавлен `Process RAM` — RSS процесса в MiB, опрос не чаще одного раза в секунду. Число загруженных чанков показано отдельно от отрисованных.
- Сохранены ранее внесённые исправления раздельных координат измерений, фоновых задач, пакетной постройки порталов, очередей света/жидкости и вертикальной загрузки Nether/End.

## Статус бета-теста порталов

**Nether и End с порталами доступны в бета-версии. Стабильность ещё не достигнута.**
Возможны зависания при смене измерений, проблемы с догрузкой и обратными порталами. Не считать порталы готовой стабильной системой; резервное копирование миров обязательно.

## Протестировано

- `tools/tests/chunk_stream_window_smoke.cpp` (C++20): границы и монотонность вертикального окна, сценарий R=48/H=8 — пройдено.
- Регрессионные Python-тесты из `tools/tests/` — результаты фиксируются при упаковке релиза.
- Полная сборка игры, GPU-рендеринг и переход в Survival на машине пользователя **не проверены**; диагностика показывает вероятную перегрузку по количеству чанков, но снимок диалога CLion сам по себе не является доказательством OOM/deadlock в OptiCraft.

## Осталось


- Проверить визуальное появление дальних вертикальных слоёв после изменения профиля резидентности.
- Записать стек всех потоков `OptiCraft` при следующем настоящем зависании, проверить системные сообщения OOM.
- Доделать стабильные переходы в Nether/End, портальные структуры, уникальных мобов и боссов.

## 0.11 - стабильная

В этом обновлении продолжена работа над графикой и потоковой загрузкой мира. Построение мешей чанков выполняется на GPU асинхронно: прежний меш остаётся видимым до готовности нового. При изменении блоков перестраиваются затронутые секции; также доработаны отсечение невидимой геометрии, LOD и отдельный проход отрисовки воды.

Генерация чанков стала гибридной: часть вычислений подземных материалов для дальних чанков может выполняться на видеокарте с помощью compute-шейдера. Подготовка данных и завершение генерации, включая пещеры, реки, поверхность и структуры, остаются на CPU. При недоступности или недостаточной скорости GPU используется CPU-путь. Подробнее — в [описании гибридной генерации](documentation/GPU_WORLDGEN_HYBRID.md).

По результатам игрового теста на Intel Core i5-13420H с 16 ГБ DDR5 при дальности прорисовки 48 чанков во время полёта наблюдалось около **80–100 FPS**. При этом перед игроком успевало появляться большое количество чанков: скорость генерации превышала **100 чанков в секунду**. Это результаты конкретного теста; производительность зависит от оборудования и условий игры.

На данный момент циклы оптимизации графики приостановлены: дальнейшая работа над ней уже становится чрезмерной, и пришло время уделить внимание другим частям проекта.

## Сводка изменений 0.10 из README

**Исправлено:** LOD использует octree занятости 32³: узел считается занятым, если занят хотя бы один из его восьми потомков; при упрощении занятая область больше не может быть заменена воздухом. Вертикальная детализация Y сохраняется, поэтому LOD не заполняет пещеры по высоте. Видимость воды отделена от достижимости непрозрачных секций не только в Meshlet-проходе, но и в общем GPU-culler: water-only чанки и секции больше не теряются до генерации indirect-команд.

**Исправлено в 0.10.8.5:** найдено несоответствие двух depth targets: GPU-culling строил текущую Hi-Z из отдельного half-resolution depth FBO, но полноразмерный colour pass после возврата в основной framebuffer использовал `GL_LEQUAL` без записи глубины. Основной framebuffer не содержал этот depth prepass, поэтому opaque-поверхности могли перекрывать друг друга неверно, а вода — просвечивать через берег. Colour pass снова использует обычный полноразмерный `GL_LESS` с записью глубины; half-resolution depth остаётся только источником консервативного GPU-culling. Убрана старая CPU reachability/portal-фильтрация секций: CPU оставляет только chunk-level frustum, а GPU решает видимость по текущему depth pass. Для LOD отключено coarse Hi-Z rejection (оставлены frustum и финальная полноразмерная depth test), а octree occupancy дополнен одноблочным X/Z halo, чтобы соседняя тонкая стена/гребень не превращала coarse sample в Air.

**Асинхронная замена GPU-меша:** CPU по-прежнему меняет voxel state и передаёт packed snapshot, но GPU строит новый меш в отдельный COW handle, пока старый остаётся видимым. После GPU fence CPU проверяет готовые quad counters без ожидания (poll с нулевым timeout) и переключает active handle только если поколение полностью готово; данные старого handle освобождаются после commit. В GPU не выполняется полный readback меша — обратно читаются только счётчики/статус. Это не GPU-владение voxel state: CPU по-прежнему нужен для физики, света, жидкостей, AI и сохранения.

**Архитектура правки блоков:** CPU остаётся авторитетным хранилищем voxel-state для физики, жидкостей, освещения, AI и сохранения; GPU выполняет построение и отрисовку меша. Полный GPU-first edit pipeline не включён: синхронное чтение GPU обратно на CPU после каждого редактирования создавало бы stall. Конкретная безопасная схема следующего этапа описана в `documentation/GPU_FIRST_BLOCK_EDITS_DESIGN.md`; она предусматривает persistent voxel SSBO, sparse edit queue, двойную буферизацию секционных мешей и асинхронный readback, не блокирующий кадр.

**Добавлено:** вода переведена на отдельный Meshlet/MDI-проход с сохранением обратного порядка прозрачной отрисовки; water meshlets используют консервативные bounds и не отсекаются по Hi-Z непрозрачной геометрии. Ступеньки получили ориентацию по сторонам света, автоматические внешние/внутренние углы и общую геометрию для меша, коллизии и луча. GPU-геометрия при изменении блока обновляет только затронутые вертикальные секции; полностью закрытая замена непрозрачного куба не запускает мешинг, если все шесть соседних клеток закрывают грани. CPU-подготовка packed snapshot пока всё ещё анализирует и упаковывает весь чанк.

**Исправлено в 0.10.8.2:** жидкие Meshlet-команды больше не проходят через недетерминированную atomic-компактизацию: для воды сохраняется фиксированный back-to-front MDI-порядок, тогда как opaque-команды остаются compact. При невозможности частичного обновления Renderer повышает dirty mask до всех секций, чтобы не очищать счётчики всего чанка при перестроении только одной секции. LOD дополнительно сохраняет продолжающиеся тонкие стены/гребни, если материал встречается в соседних coarse-ячейках; одиночные разреженные блоки по-прежнему остаются воздухом. GPUSTAT отдельно показывает `partial_chunks` и `full_chunks`.

**Сохранено:** Meshlet Renderer переиспользует существующий greedy quad pool и GPU Vertex Pulling. Flora остаётся на штатном прозрачном пути; Standard renderer доступен для A/B-сравнения. Переполнение GPU-потоков, LOD и освещение продолжают использовать существующие механизмы.

Также сохранены изменения версии 0.10.4: расширенный набор из 39 биомов, разнообразные деревья и небесные острова, а также деревянные ступеньки и полублоки для 16 пород древесины с рецептами крафта.

## 0.10.8.6

- Обычные мобы удаляются без дропа за пределами дальности прорисовки; верховая лошадь сохраняется. Новые появляются в радиусе до двух третей дальности, при наличии загруженной подходящей поверхности.
- Спавн врагов работает в Creative и Survival. Сложность в настройках меняет ночной лимит врагов и сохраняется между запусками.
- При высоком полёте поиск поверхности учитывает высоту рельефа.
- Настройки кода распределены по `src/utils/configs/Config_*.h`.


## 0.10.8.5 — полноразмерная глубина, текущие camera rays и безопасный LOD

- **Исправлен depth-state между prepass и colour pass.** Hi-Z строится из отдельного half-resolution depth-only FBO. Ранее после возвращения в основной framebuffer цветовой проход включал `GL_LEQUAL` и отключал depth writes, хотя depth prepass в основной framebuffer не копировался. Основной depth buffer оставался пустым/неполным: дальние opaque-поверхности могли рисоваться поверх ближних, а прозрачная вода проходила сквозь берега. Теперь отдельная глубина применяется только для GPU visibility; полноразмерный colour pass использует `GL_LESS` и пишет глубину обычным способом.
- **Видимость текущего кадра.** Opaque prepass rasterизует реальные ближайшие поверхности для каждого sample/pixel camera view; depth test hardware выбирает первый opaque hit на этом camera ray. GPU строит из неё conservative MAX Hi-Z. В Hi-Z целиком отклоняются только meshlet/section AABB, доказанно находящиеся за всеми depth samples своего screen region. Финальный полный проход всё равно выполняет обычный per-pixel depth test. Это screen-space ray visibility через raster depth, не отдельный software ray marcher по всем voxels.
- **Устранена ненадёжная CPU portal reachability.** `Chunk_Manager::get_renderable_chunks` теперь проверяет только AABB целого чанка по frustum и не удаляет отдельные секции по связности воздуха. Реальные section/meshlet bounds и Hi-Z обрабатываются GPU. Это консервативнее и не скрывает горы/воду из-за неверной топологии порталов.
- **Меньше ложных LOD-дыр.** Coarse LOD stream помечается отдельным флагом и не проходит Hi-Z rejection; GPU frustum culling остаётся, а скрытые пиксели удаляет полноразмерный depth test. В `Chunk.cpp` octree запрос occupancy расширен на один voxel по X/Z вокруг coarse cell; пустой coarse sample рядом с тонкой стеной/гребнем выбирает ближайший реальный terrain block вместо Air. Y не расширяется — вертикальные cave gaps остаются.
- **Асинхронный COW upload подтверждён в коде.** Новый GPU mesh строится в отдельный pending handle; active mesh остаётся drawable. После fence Renderer проверяет только quad counters через non-blocking poll и переключает CPU metadata на pending handle, когда batch не переполнен и версия совпадает. GPU mesh bytes не копируются обратно на CPU. CPU по-прежнему авторитетен для voxel simulation и packed snapshot; полной GPU-owned voxel map эта версия не добавляет.
- **Проверка.** Surfaceless Mesa OpenGL 4.5: compile проверены greedy/misc mesher, meshlet builder/culler, section culler, Hi-Z copy/reduce; link проверены оба terrain shader pairs. Synthetic dispatch проверил solid/shaped/liquid bounds, fixed/compact MDI order и тест Hi-Z: LOD остаётся видимым, детальная opaque-геометрия под искусственной near-depth маской отклоняется, liquid остаётся видимым. Полная C++ сборка и запуск на пользовательской Intel UHD не выполнялись в этом окружении.

## 0.10.8.4 — независимая жидкая reachability в общем GPU-culler

- **Причина оставшихся пропусков целых водяных чанков.** В `Renderer::prepare_gpu_frame` общий GPU-culler получал единую маску `reachable_mask`. Если в чанке не было достижимой непрозрачной секции, кандидат целиком отбрасывался, даже когда там находилась вода. При пустом списке кандидатов сбрасывался и сформированный ранее Meshlet draw list. Исправление: добавлена независимая `liquid_reach_mask` (4 бита секций, упакована в `candidate.w[16..19]`). Чанк попадает в GPU candidate list, если есть достижимая opaque-секция ИЛИ секция с жидкостью.
- **Раздельный culling.** `gpu_cull_commands.comp` формирует отдельные `frustum_visible_mask` для solid/shaped и `liquid_frustum_visible_mask` для воды. Hi-Z и legacy opaque occlusion-query mask влияют только на solid/shaped; liquid использует собственный frustum и затем обычный depth test. Это не заставляет показывать закрытую непрозрачную геометрию: у неё прежняя маска достижимости.
- **LOD.** Сохранено octree 32³ из версии 0.10.8.3: занятый потомок не может исчезнуть при выборе coarse-представителя в соответствующей X/Z-группе и Y-слое. Вертикальное разрешение не упрощается.
- **GPU-first редактирование.** В этой версии не заявляется как реализованное. CPU остаётся источником voxel state; GPU получает подготовленный snapshot и строит меш. Проектирование следующего этапа вынесено в `documentation/GPU_FIRST_BLOCK_EDITS_DESIGN.md`, включая предотвращение синхронного readback stall.
- **Проверка.** Выполняется standalone C++ проверка octree occupancy и brute-force сравнение пересечений. Изменённая масочная логика проверяется отдельными сценариями: water-only section, opaque unreachable + liquid reachable, opaque reachable + liquid reachable, empty chunk. Полная сборка игры и визуальный запуск на пользовательской видеокарте всё ещё требуют проверки в среде пользователя.

## 0.10.8.3 — octree occupancy для LOD и независимая видимость воды

- **Octree занятости LOD.** Добавлено полное octree для 32×32×32 блоков (1 + 8 + 64 + 512 + 4096 внутренних узлов и листья). Лист хранит занятость terrain-геометрии, внутренний узел — OR восьми дочерних узлов. Для каждой coarse X/Z-ячейки и конкретного Y вычисляется пересечение с octree; если в её исходной области есть хотя бы один подходящий блок, coarse-представитель не может стать Air. Материал всё ещё выбирается голосованием. Вертикальное разрешение не уменьшается, поэтому это не заполняет вертикальные полости/пещеры.
- **Целые пропадающие водяные секции.** Исправлена ошибка в `Renderer::prepare_gpu_meshlet_frame`: старый CPU `section_visible` (результат достижимости/opaque section culling) применялся до построения water stream. Теперь этот фильтр действует только на непрозрачные streams. Жидкость проходит собственный frustum/Meshlet AABB culling и затем тест глубины GL, но не наследует opaque occlusion-query rejection.
- **GPU-first редактирование — границы архитектуры.** Текущий GPU pipeline уже выполняет meshing/culling/draw, но CPU хранит voxel state и формирует packed snapshot. Полный перенос voxel state на GPU с последующим readback не добавлен: физика, жидкости, свет, AI и сохранения требуют согласованного CPU state, а синхронный readback создавал бы GPU stall. Правильный следующий этап — persistent GPU voxel SSBO + sparse edit command queue + асинхронный snapshot/readback, с CPU state остающимся авторитетным для симуляции.
- **Проверка.** Для octree выполнен отдельный C++ тест построения и запросов прямоугольных сечений (2×2×1 и 4×4×1), сравнение с brute-force OR на пустом, полном, разреженном и псевдослучайном чанке. Статическая проверка подтверждает, что opaque `section_visible` не может отбрасывать прозрачные liquid streams. Полная C++ сборка проекта и визуальный запуск на пользовательской Intel UHD требуют проверки в пользовательском окружении: в текущей среде нет GLM/GLFW/OpenGL development headers.

## 0.10.8.2 — порядок воды, безопасность секционного обновления и непрерывность LOD

- **Мерцание прозрачной воды.** Opaque Meshlet streams по-прежнему используют compact atomic append. Liquid kind=3 исключён из compaction и записывает MDI-команды только в заранее назначенные offsets. Renderer рисует весь фиксированный жидкий диапазон после обратной сортировки чанков (far-to-near); жидкость не использует opaque Hi-Z или occlusion-query rejection. Это делает порядок alpha blending детерминированным.
- **Потеря секций при fallback.** Если частичное обновление невозможно из-за первой GPU-резиденции, смены LOD, изменения политики флоры или необходимости увеличить/перераспределить буферы, Renderer теперь повышает `section_update_mask` до всех секций. Раньше в таких случаях очищались все stream counters, но compute mesher продолжал видеть маску только изменённой секции; остальные секции оставались пустыми. Это могло проявляться как исчезновение кусков чанка, включая поверхность океана, и отверстия в стенах.
- **Тонкие детали дальнего LOD.** При голосовании coarse X/Z-ячеек сохраняется не только победитель, но и сильнейший terrain-кандидат. Если голосование выбрало Air, но как минимум `lod_stride` voxels того же материала продолжаются в одной из восьми соседних coarse-ячеек, материал сохраняется в текущей ячейке. Одиночные слабые пятна без соседнего продолжения остаются Air, чтобы не вернуть прежние песчаные плиты над океаном.
- **Диагностика пересборки.** GPUSTAT разделяет `light_chunks`, `partial_chunks` и `full_chunks`; прежний `full_chunks` ошибочно включал и настоящие частичные замены.
- **Точная граница оптимизации.** Quad ranges, GPU counters и Meshlet bounds неизменённых секций сохраняются. Но `Chunk::build_chunk_gpu_mesh` пока разворачивает полное packed-представление блока/света/metadata, пересчитывает connectivity и формирует payload всего чанка. То есть локальна именно генерация/замена GPU-геометрии, а CPU snapshot/packing ещё не полностью секционный; это остаётся отдельной оптимизацией, не маскируется под готовую.
- **Проверка.** Mesa OpenGL 4.5: compute-тест проверяет solid/shaped/liquid AABB, фиксированный порядок двух жидких MDI-команд в compact-режиме, отдельные opaque counters, opaque query rejection и независимую видимость жидкости. Compile/link тесты greedy/misc compute и обоих terrain shader pairs прошли. Полная C++-сборка и проверка на целевой Intel UHD здесь не выполнены: GLM/GLFW/OpenGL development dependencies отсутствуют.


Этот файл содержит подробную хронологию развития проекта. Актуальное краткое описание находится в корневом `README.md`, техническое устройство — в `TECHNICAL_DOCUMENTATION.md`.


## 0.10.8 — жидкость в Meshlet, угловые ступеньки и локальная пересборка

- **Вода на Meshlet-пути.** Добавлен отдельный kind=3 stream в Meshlet builder/culler и свой диапазон MDI-команд/счётчик. Вода собирается в обратном порядке относительно opaque chunk order, не использует Hi-Z и не наследует opaque occlusion-query rejection. Bounds водяного Meshlet охватывают всю исходную клетку с вертикальным запасом 0.08 блока под волну vertex shader. Альфа-смешивание и отсутствие записи в depth buffer остаются штатными.
- **Ориентированные ступеньки.** `meta` задаёт одно из четырёх направлений по сторонам света. Нижняя половина строится из четырёх четвертей, верхняя — из двух четвертей для прямой ступеньки, одной для внешнего угла или трёх для внутреннего. Угол автоматически определяется по соседней ступеньке впереди/позади; внутренние соприкасающиеся грани частей формы не эмитятся. Один `shape_boxes()` используется для меша, коллизии игрока и проверки попадания луча.
- **Секционная пересборка.** Dirty mask хранит затронутые вертикальные секции (по 8 блоков); при изменении на границе секции добавляется соседняя секция. GPU greedy/misc mesher пропускают остальные секции. Renderer сохраняет их quad ranges и counters, обнуляет счётчики только изменяемых потоков и пересобирает Meshlet bounds только для них. Полная пересборка остаётся для первого меша, смены LOD, изменения ёмкости или несовместимого состояния GPU-ресурсов.
- **Изменение без видимой грани.** Замена полного непрозрачного куба на другой такой же тип геометрии не ставит секцию на пересборку, когда все шесть соседних клеток внутри чанка закрывают грани. На границе чанка проверка консервативно считает грань потенциально видимой.
- **Проверка.** Surfaceless Mesa OpenGL 4.5: прошли compile/link текущих terrain shader pairs, компиляция greedy/misc compute shaders и синтетические builder/culler dispatch tests для solid, shaped и liquid. Проверены liquid AABB, отдельный MDI range/count, compact mode и независимость воды от opaque query rejection. Полная C++-сборка и визуальный запуск на Intel UHD не подтверждены в этой среде из-за отсутствующих OpenGL/GLFW/GLM development headers/libraries; производительность не заявляется до игрового A/B-теста.

## 0.10.7 — исправления LOD и жидких GPU-мешей

- **Дальний LOD.** Воздух теперь участвует в голосовании coarse-группы. Solid-представитель выбирается только при строгом большинстве подходящих блоков; при равенстве выигрывает Air. Это не позволяет одной-двум ячейкам раздуваться в целую горизонтальную площадку и уменьшает прямоугольные песчаные/каменные пятна над водой.
- **Переполнение жидких потоков.** Стартовая ёмкость liquid-stream увеличена с 1024 до 2048 квадов на секцию. Переполненные resident-потоки теперь обрабатываются перед обычными фоновыми заменами мешей, чтобы усечённая первая сборка не оставалась на экране за очередью новых чанков.
- **Консервативное отсечение секций.** AABB секции расширяется на 0.10 блока для vertex displacement воды и покачивания флоры. Это предотвращает отбрасывание целой секции, если анимированные вершины выходят за статические voxel bounds.
- **Ограничения проверки.** Изменения прошли статические проверки и GLSL-проверку, если доступен локальный OpenGL test context. Полный запуск игры и сравнение на целевой видеокарте должны быть подтверждены в пользовательской сборке.


## 0.10.6 — интеграция Meshlet Renderer в текущий GPU renderer

- **Интеграция в актуальный quad pool.** Самостоятельный Meshlet Runtime адаптирован к фактическим интерфейсам проекта 0.10.4: используется существующий GPU greedy mesher, packed quad pool, `gpu_counts`, `gpu_origins`, текущий MDI draw path и temporal Hi-Z. `Chunk_Manager`, генератор мира и освещение не переписывались ради Meshlet-подсистемы.
- **Meshlet descriptors.** Добавлены persistent AABB descriptors по 32 quad records. Обычные greedy faces и shaped-box records декодируются разными путями; для shaped bounds используются координаты `rec.z/rec.w` в масштабе 1/64 клетки. Bounds строятся только после обновления соответствующего GPU geometry range.
- **Fine-grained GPU culling.** Добавлен compute pass для per-meshlet frustum и temporal Hi-Z отсечения. Output — отдельные MDI command ranges для solid и shaped. Поддерживаются fixed-count команды и compact append с `glMultiDrawArraysIndirectCount`, если runtime entry point доступен.
- **Legacy query compatibility.** Stream record содержит бит результата существующего асинхронного section occlusion query. Meshlet culler не может повторно показать секцию, уже скрытую legacy query path.
- **Safe fallback.** Meshlet pass подготавливается до legacy chunk culler. Если подготовка команд не проходит проверку capacity или allocation, флаг Meshlet mode снимается до генерации стандартных opaque-команд в том же кадре.
- **Ограничения текущего режима.** Solid и shaped geometry идут через Meshlet MDI + существующий Vertex Pulling; flora и liquid остаются на прежнем пути. Нативные EXT/NV Mesh Shader draws не смешиваются с Meshlet MDI в одном кадре. Это GPU-driven имитация Meshlet culling/scheduling, а не утверждение о native Mesh Shader output path.
- **Проверка.** Синтетический surfaceless Mesa OpenGL 4.5 dispatch подтвердил solid/shaped AABB, fixed и compact MDI, а также наследование legacy occlusion-query rejection. Новые Compute programs и текущие classic vertex/fragment pairs compile/link успешно. Полная сборка C++ не подтверждена: в среде проверки отсутствуют OpenGL development libraries/headers (CMake `FindOpenGL` останавливается до компиляции проекта). Intel UHD FPS не измерялся; производительный выигрыш не заявляется до реального A/B-теста.

Подробные ABI, pass bindings, синхронизация и A/B протокол: `documentation/MESHLET_RENDERER.md`.

## 0.10.5 — экспериментальный GPU Meshlet Renderer

- **Отдельный режим рендеринга.** Внутриигровые настройки получили переключатель `Путь рендера`: `Стандартный` или `GPU Meshlet (эксперимент.)`. Выбор сохраняется в `options.txt` ключом `meshlet_renderer`. Если драйвер не позволяет инициализировать Compute/SSBO-путь Meshlet, пункт показывает, что режим недоступен, и стандартный рендерер остаётся активным.
- **Переиспользование существующей геометрии.** Meshlet-режим не меняет `Chunk_Manager`, GPU greedy mesher, освещение и формат quad pool. Он группирует уже созданные 16-байтные quad records в кластеры по 32 записи. Persistent AABB descriptor занимает 48 байт и индексируется через `firstQuad / 32`; границы пересчитываются при обновлении геометрии, а не каждый кадр.
- **Два формата геометрии.** Для обычных greedy-граней AABB восстанавливается из оси, направления, ширины, высоты и нормальной толщины. Для shaped-блоков используется отдельное декодирование шести 7-битных координат bounding box в `rec.z/rec.w` (масштаб 1/64 клетки); смешивать эти packed-layout нельзя. Флора и жидкости пока не переводятся в Meshlet-путь.
- **GPU culling и MDI.** Новый Compute-проход проверяет отдельные Meshlet по frustum и переиспользует существующую temporal Hi-Z пирамиду, когда её кадр валиден. Он формирует `DrawArraysIndirectCommand` непосредственно на GPU. В compact-режиме потоки сначала считают локальные видимые элементы в shared memory, затем резервируют глобальный диапазон командами одной атомарной операцией на batch из 32 Meshlet; при наличии `glMultiDrawArraysIndirectCount` количество Draw-команд также остаётся на GPU. Иначе используется фиксированное число команд с нулевыми `count` у невидимых кластеров.
- **Совместимость.** В Meshlet-режиме opaque solid/shaped draws выполняются через существующие vertex-pulling шейдеры и MDI. Нативный EXT/NV Mesh Shader путь не смешивается с этими командами в одном кадре; при выключенном Meshlet-переключателе исходный выбор стандартного backend сохраняется. Команды флоры и жидкости остаются на существующем рендер-пути.
- **Dispatch limits.** Построение AABB и culling разбиваются на пачки по `GL_MAX_COMPUTE_WORK_GROUP_COUNT.x`; абсолютный индекс передаётся uniform base-offset, поэтому большой список stream/candidate не требует Dispatch за пределом лимита одной оси.
- **Проверка.** Compute shader compile/link и синтетический dispatch двух новых GPU-проходов выполнены в surfaceless Mesa OpenGL 4.5: проверены solid и shaped AABB, frustum culling, две MDI-команды и счётчики compact-режима. Полная C++-сборка в среде разработки не подтверждена из-за отсутствующих OpenGL/GLFW/GLM development dependencies. На Intel UHD и в реальном игровом кадре FPS ещё не измерялся; прирост производительности не заявляется до A/B-теста.

Документация по переключателю, структурам SSBO, барьерам и ограничениям: `documentation/MESHLET_RENDERER.md`.

## 0.10.4 — обновление биомов и строительных форм

- **Климатическая классификация.** Переработан выбор поверхностного биома в `World_Generator::surface_biome_for_column`. Биом теперь выбирается не только по отдельному климатическому значению: учитываются температура, влажность, эрозия, выраженность рельефа, необычность климатической области, высота поверхности, уклон и положение относительно уровня моря. Для прибрежных и водных зон используются реальная высота дна и близость водоёма. Благодаря этому горные участки определяются формой рельефа, а берег, болото и мангровые заросли — сочетанием высоты, уклона и водной близости.
- **Полный набор биомов.** Список `Biome_Id` и таблица имён теперь содержат 39 типов: Ocean, Shallow Water, Beach, Plains, Forest, Birch Forest, Taiga, Jungle, Savanna, Desert, Stone Desert, Steppe, Tundra, Ice Fields, Mountains, High Mountains, Plateau, Slime, Solar, Sky, Underground, Deep Underground, Flooded Underground, Temperate Rainforest, Swamp, Mangrove Swamp, Cherry Grove, Beech Forest, Aspen Grove, Meadow, Shrubland, Badlands, Cold Desert, Snowy Taiga, Alpine Meadow, Cold Ocean, Warm Ocean, Coral Reef и Mushroom Fields. Имена используются в F3 и при обратном преобразовании имени в `Biome_Id`; список биомов для режима одного биома расширен наземными вариантами.
- **Наземные экосистемы.** Тёплые и влажные области распределяются между джунглями и дождевыми лесами, более сухие тёплые — между саванной, степью и пустынями. Для холодных областей разделены ледяные поля, тундра, снежная тайга, холодная пустыня и обычная тайга. Умеренные зоны дополнены буковыми лесами, осинниками, вишнёвыми рощами, лугами и редколесьями. Низкие влажные участки рядом с водой могут стать болотом или мангровым болотом; редкие подходящие влажные участки выделяются в грибные поля. Для этих зон подобраны разные поверхностные блоки и растительность: мох, папоротники, луговые цветы, камыш, рогоз, болотная трава, сухие кусты и грибы.
- **Океаны и рифы.** Океанические биомы выбираются по глубине морского дна и температуре: холодный, обычный, тёплый океан, мелководье и коралловый риф. Рифы получают кораллы-мозговики, трубчатые, ветвистые и веерные кораллы, а также актинии и морскую траву. На морском дне в зависимости от глубины и типа воды размещаются ламинарии и морская трава; состав дна меняется от песка у берега до гравия и камня на глубине.
- **Подземные области.** Обычная сеть пещер остаётся в диапазоне Y = -60…130. Отдельно генерируются большие закрытые пещерные полости в диапазоне Y = -150…-63: они строятся независимо от сети ходов, между диапазонами оставлен каменный разделительный слой. Основная камера имеет неровный профиль, к ней добавляются 2–3 соединённых боковых зала; шум деформирует стенки и убирает форму идеального эллипсоида. Полости, расположенные достаточно высоко, могут быть затоплены, а ниже Y = -120 они остаются сухими. В сухих участках высотой от 10 блоков могут появляться светящиеся пещерные деревья: ствол проходит почти от пола до потолка, ветви и светящиеся ягоды/цветы распределены несколькими ярусами по высоте зала.
- **Небесные острова.** Для измерения с парящими островами добавлена отдельная экосистема `Sky`: острова строятся из облачного камня с травяным верхним слоем, а декорации включают небесные деревья, цветы и свисающие лозы. Проверка биома `Sky` выполняется и для самих островов, даже если у мира включён режим единого наземного биома.
- **Разнообразие деревьев.** Помимо дуба, берёзы, ели, джунглевого, акации и слизового дерева используются отдельные генераторы пальмы, бука, секвойи, мангрового дерева, ивы, небесного и светящегося дерева, баобаба, сакуры и осины. Привязка к биомам сделана по экологии: Forest → дуб, Birch Forest → берёза, Aspen Grove → осина, Beech Forest → бук, Cherry Grove → сакура, Temperate Rainforest → секвойя, Taiga/Snowy Taiga → ель, Jungle → джунглевое дерево, Swamp → ива, Mangrove Swamp → мангровое дерево, Savanna → акация и реже баобаб, Slime → слизовое дерево. В пустыне редко встречается пальма; небесное дерево и светящееся дерево ставятся отдельными декораторами на островах и в пещерах. Деревья отличаются не только блоками/цветом, но и силуэтом: у мангрового дерева есть опорные корни, у ивы — свисающие побеги, у небесного — изогнутый ствол и лозы, у баобаба — массивный ствол, у сакуры — разнесённые цветущие кроны, у осины — высокая узкая крона. Для пород добавлены соответствующие саженцы и сопоставление с типом дерева в `Sapling_Manager`.
- **Ступеньки и полублоки для древесины.** Добавлены 32 блока (ID 225–256): ступеньки и полублоки для 16 пород — дуба, берёзы, ели, джунглевого дерева, акации, слизового дерева, бука, секвойи, мангрового дерева, ивы, небесного дерева, светящегося дерева, баобаба, пальмы, сакуры и осины. Для каждого блока задана форма `stairs` или `slab`, текстура соответствующих досок, ориентация ступенек и дроп `self`. В `Block_Shapes` ступенька описывается нижней половиной клетки и верхней четвертью со стороны, заданной `meta`; полублок — нижней половиной клетки. Эти формы используются при построении геометрии и проверке формы блока.
- **Рецепты строительных форм.** Добавлены рецепты для всех 16 деревянных вариантов, а также восстановлены/добавлены рецепты каменных ступенек и полублоков. Итого 17 рецептов ступенек и 17 рецептов полублоков: 6 соответствующих блоков/досок дают 4 ступеньки, 3 — 6 полублоков. Деревянные рецепты используют доски той же породы, а не общий материал.
- **Превью в инвентаре.** `Hud::draw_slot_icon` больше не рисует ступеньки и полублоки как полный куб. Для полублока строятся отдельные верхняя и боковые грани уменьшенной высоты; для ступеньки — нижняя половина, верхняя площадка, боковые стенки и подступенок. Используются UV соответствующих граней текстуры блока и отдельное затенение сторон; обычные блоки остаются изометрическими кубами, а предметы и растения — плоскими иконками.
- **Разрушение в Creative.** В творческом режиме блок разрушается мгновенно независимо от его прочности, в том числе если у него отрицательная прочность. Жидкости (`Block_State::Liquid`) исключены из разрушения. Для удалённого блока отправляется событие без выпадения предмета, чтобы связанные с растениями и логикой служебные состояния могли корректно очиститься.

## 0.10.3 — настройки графики

- Длительность появления чанка вынесена в настройки. Константа 0.42 заменена uniform `appearDuration` во всех шести шейдерах появления (`terrain_gpu_vertex.glsl`, `terrain_gpu_misc_vertex.glsl`, `terrain_mesh.mesh`, `terrain_mesh_nv.mesh`, `terrain_misc_mesh.mesh`, `terrain_misc_mesh_nv.mesh`). Диапазон 0.10–0.50 с задан в `Config::chunk_fade_seconds_*`, по умолчанию 0.42 с.
- Дальность по высоте: `Chunk_Manager::set_vertical_radius` задаёт обычную полуширину окна вместо жёсткой 4. Быстрый вертикальный полёт по-прежнему сжимает окно (до половины, не меньше 2), а при малой горизонтальной дальности окно остаётся 2. Режим «Авто» = `Config::auto_vertical_render_distance` = round(горизонтальная / 3), но не меньше 4: дальность 20 даёт 7 (15 уровней), дальность по умолчанию 5 даёт прежние 4. Пул слотов чанков рассчитан на максимум (12).
- Постобработка кадра: `Renderer::begin_scene` и `end_scene_and_post` выводят мир в промежуточный RGBA8-буфер с 24-битной глубиной. Проход `post_fragment.glsl` применяет FXAA (упрощённая версия, сглаживает только там, где яркость соседей резко меняется) и зерно (амплитуда до ±4% яркости, новый рисунок каждый кадр). Хотбар, прицел и HUD рисуются поверх готового кадра. Если сглаживание и зерно выключены, промежуточный буфер не создаётся, и кадр идёт прежним путём.
- Подсветка выделенного блока и виньетка рисуются внутри промежуточного буфера: подсветка использует глубину сцены.
- Виньетка: `Hud::set_vignette_strength`. Уровни: выкл (0), слабая (0.15), средняя (0.30, как раньше), сильная (0.50).
- Меню «Настройки»: пункты «Дальн. по Y», «Появление», «Сглаживание», «Зерно», «Виньетка». Значения сохраняются в `options.txt` ключами `vertical_distance`, `chunk_fade`, `antialiasing`, `film_grain`, `vignette`.
- Не реализовано: SMAA. Он требует предвычисленных таблиц area/search texture, которых в проекте нет. MSAA потребовал бы отдельного resolve глубины для Hi-Z-пирамиды, поэтому пока не добавлен.

## 0.10.2 — визуальное обновление и ускорение GPU hand-off

- Добавлено плавное блочное проявление чанка в течение ~0.42 с. Эффект реализован детерминированным dither/discard в fragment shader, а не масштабированием геометрии: мировые координаты и общие границы соседних чанков не двигаются, поэтому анимация не создаёт временных щелей.
- Добавлен мягкий дистанционный туман, смешиваемый с текущим цветом неба/погоды. Начало и конец тумана вычисляются из выбранной дальности прорисовки, чтобы край видимости не оставался жёсткой линией при разных настройках.
- В solid/misc GPU shader-пути добавлено направленное солнечное освещение: оно усиливает объём граней днём и плавно отключается у горизонта/ночью. Существующая нижняя граница яркости 0.32 сохранена.
- При GPU upload новые чанки (ещё не имеющие GPU handle) отправляются перед фоновой заменой уже resident-мешей; в каждой группе сохраняется порядок от ближних к дальним. В текущем кадре compute-meshing и GPU culling/draw preparation выполняются последовательно в одном GL command stream.
- Приоритет мешинга учитывает знаковое направление движения игрока, расстояние и видимость. Результаты уже завершённых задач также пересортировываются после смены чанка игрока; при быстром движении приоритет обновляется примерно каждые 120 мс даже внутри того же чанка.
- LOD-переходы больше не блокируются условием полного простоя streaming/generation/meshing очередей. Очередь LOD работает независимо и имеет отдельный приоритет; старый меш остаётся на экране до готовности нового.

## 0.10.1 — GPU-driven рендеринг и подготовка кадра

- Переработан рендеринг мира: мешинг секций перенесён на GPU через compute-шейдеры, с greedy meshing, GPU-пулом квадов и генерацией indirect-команд; при доступности `GL_EXT_mesh_shader`/`GL_NV_mesh_shader` включается Task/Mesh Shader-путь, иначе используется indirect draw с vertex pulling. Добавлены GPU frustum culling и temporal Hi-Z occlusion culling, вершинный AO и исправления актуализации освещения на границах чанков. Сокращена передаваемая с CPU полезная нагрузка; реализованы квоты/переполнение GPU-мешера с безопасным CPU fallback, адаптивные бюджеты стриминга и подготовки кадра, приоритет чанков по расстоянию и видимости, а также диагностика F3/F4. Исправлены гонки при асинхронной пересборке мешей и применении результатов к выгруженным/повторно загруженным чанкам.

## Предыдущая история

Ниже сохранён ранее ведшийся журнал изменений. Его записи перенесены из старого README без сокращения.

# OptiCraft

> **Статус: первый прототип.** Это учебный pet-проект: воксельный движок в духе Minecraft на C++ и OpenGL.
> Прототип нужен, чтобы показать, какие возможности уже есть, и понимать, куда развивать проект дальше.
>
> **Об авторстве кода:** большая часть кода написана мной вручную. Часть кода написана
> с помощью нейросетей.

Единая актуальная версия проекта собирается через CMake и использует код из `src/`.
Актуальная линия разработки находится в `src/` и собирается через CMake. Старый legacy-код удалён после завершения переноса; он больше не является частью проекта.

## Сборка

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 4
```

апдейт 0
проект уже имеет может рисовать минимальную воксельную графику, создавать 9 плоскиx чанков, по ним можно бегать
если выйти за пределы центрального то спереди появятся новые, а те что слишком далеко будут удалены
был оптимизирован рендер чанков, теперь весь чанк рендерится за один вызов, а не каждый блок по отдельности,
блоки внутри чанка, которые не видны, не рендерятся, также были добавлены текстуры блокам


версия 0.1
добавлена: генерация мира с помощью шума перлинга, созданы зачатки для реализации различных биомов
реализована система текстур с помощью атласа текстур, а также общее улучшение кода для простоты разработки
Иллюстрация к исторической записи отсутствует в репозитории.
забавный баг, исправлен
(сейчас есть графические артефакты, в виде заглушек текстур, вскоре исправлю)

версия 0.2
реализовано: оптимизация за счет разделения кода на потоки данных.
исправлены: графические артефакты с текстурами

версия 0.3
добавлено: экранно-игровая state machine в `Application`, главное меню, пауза и экран настроек поверх OpenGL HUD.
из настроек можно переключать набор генерации Classic/Islands; параметры мира (имя, seed, generation folder)
сохраняются в `worlds/default/world.json` рядом с запущенным бинарником.
пирамиды включены как редкие структуры биома Mountains.

версия 0.3.1
добавлено: debug HUD по `F3` (FPS, время кадра, seed, чанки, треугольники, позиция игрока),
vignette (радиальное затемнение экрана, перенесено из legacy как процедурный шейдер вместо текстуры).
битмап-шрифт HUD дополнен цифрами и недостающими буквами — раньше числа в HUD вывести было нечем.

версия 0.3.2
добавлено: динамическая регистрация блоков — `blocks.json` может объявлять блоки с id за пределами
старого фиксированного enum `Count`, без пересборки движка.
исправлено: зарезервированный id 8 больше не попадает в инвентарь как выбираемый "призрачный" блок
с несуществующей текстурой.

версия 0.3.3
добавлено: полноценный инвентарь в стиле Minecraft — хотбар из 9 слотов (клавиши `1`-`9`) и полноэкранная
сетка по `E` со всеми зарегистрированными блоками, с настоящими текстурными иконками из атласа.
debug HUD (`F3`) теперь показывает состояние игрока: `GHOST` (полёт, `F1`) или `WALKER` (обычная ходьба).

версия 0.3.4
добавлено: секции по Y внутри меша чанка (`Chunk_Section_Range`) — покомпонентный frustum-culling
без раздувания числа GL-буферов на чанк, плюс layer culling при постройке меша (пропуск полностью
пустых Y-слоёв). debug HUD (`F3`) показывает `SECTIONS: отрисовано/всего`.
исправлено: иконки блоков в HUD (хотбар/инвентарь) не отображались из-за обратного обхода вершин
квада при глобально включённом `GL_CULL_FACE` — найдено и починено через реальный headless-прогон
движка под Xvfb, а не только по компиляции.
примечание: высота мира (`Config::chunk_height = 32`) не менялась — секции по Y относятся только
к рендеру внутри существующего чанка-колонки, а не к вертикальному размеру мира.

версия 0.4.0
добавлено: настоящая высота мира — 256 блоков (`Config::world_height_chunks = 8`). Чанки теперь
адресуются в 3D (`x, y, z`), стекуются по вертикали как отдельные единицы (по той же концепции вертикального стекования, но без привязки к старой реализации — секции меша из версии 0.3.4 остались
внутри одного чанка неизменными). Высота рельефа, уровень воды и потолок для декораций теперь
считаются от полной высоты мира, а не от высоты одного чанка — игрок спавнится на Y≈71 вместо Y≈2.
проверено: headless-прогон под Xvfb — игрок стабильно стоит на поверхности при потоковой подгрузке
всех 8 вертикальных слоёв чанков, debug HUD показывает корректно растущую статистику.
известное ограничение: загрузка полного радиуса чанков стала тяжелее (8× объём генерации).

версия 0.4.1
добавлено: асинхронная пересборка меша при правке блока и при подгрузке нового соседа — раньше
`build_mesh()` вызывался синхронно в главном потоке (до 4 раз за одну правку у границы чанка),
теперь это чистая функция без побочных эффектов, считается в фоновом воркере по снимку блоков,
результат забирается и применяется главным потоком на следующем кадре.
проверено: стресс-тест под ThreadSanitizer — 14500+ правок блоков и 48 сбросов мира подряд
без единой гонки. Класс `Chunk_Manager` документирован как небезопасный для вызова с нескольких
потоков в целом (только внутренние очереди задач синхронизированы) — так и было задумано.
известное ограничение: пересборка всё ещё на уровне целого чанка, а не отдельной Y-секции.

версия 0.4.2
исправлено: реальный баг backface culling, живший в коде с самого начала — из 6 направлений
граней блока верно был закручен обход вершин только у X-граней (лево/право); Y и Z грани
отсекались GPU именно с той стороны, откуда их положено видеть. Найден изолированным тестом
(рендер одной грани в чистом OpenGL-контексте, чтение пикселя framebuffer'а), не на глаз.
На террейне разница видна невооружённым глазом — силуэт стал гладким сплошным вместо рябого.
добавлено: анимированный шейдер жидкости (`assets/shaders/liquid_*.glsl`) — волна на вершинах
верхней грани воды, скролл текстуры течения (обёрнут строго внутри тайла атласа, проверено
отдельным тестом на утечку в соседние тайлы), мерцание яркости, честная прозрачность.

версия 0.4.3
добавлено: анимированный шейдер флоры (`assets/shaders/flora_*.glsl`) — верхушка куста/цветка
качается на ветру, низ укоренён неподвижно. Верх/низ различается через отдельный флаг в
неиспользуемой компоненте нормали, а не через UV — первая версия через UV содержала реальный
баг (v_max одного тайла атласа численно совпадает с v_min соседнего, различить по значению
на границе невозможно), пойманный отдельным изолированным тестом на позицию силуэта во времени.

версия 0.4.4
добавлено: приоритетная очередь загрузки чанков — свой этаж и ближайшая горизонталь грузятся
почти сразу, дальнее небо/подземелье в той же колонке отъезжает в конец очереди (весь столбец
по-прежнему грузится целиком, поменялся только порядок). Раньше была обычная FIFO-очередь,
из-за которой при первой загрузке мира игрок мог долго стоять посреди пустоты.
проверено: изолированный тест на порядок появления чанков + перепрогон стресс-теста под
ThreadSanitizer после смены типа очереди — 0 гонок. На headless-скриншоте разница разительна:
впервые игрок сразу видит плотный террейн вокруг себя, а не в основном небо.
примечание: тестовая песочница оказалась однопроцессорной (nproc=1) — предыдущие оценки
"медленной" загрузки чанков в headless-тестах были следствием этого, а не кода; на реальном
железе с несколькими ядрами воркеры генерации по-настоящему параллельны.

версия 0.5.0
добавлено: цикл дня и ночи (`Day_Night_Cycle`), 20 минут реального времени на полный круг.
Солнце и луна — процедурные billboard-диски без текстуры, всегда друг напротив друга. Цвет
неба и общая яркость сцены (`ambient` во всех трёх фрагментных шейдерах чанков) плавно меняются
по высоте солнца над горизонтом; ночью яркость не падает до нуля. `F10` (только при включённом
F3-debug) сдвигает время на минуту вперёд — 10 нажатий переводят день в ночь и обратно. F3
показывает текущее время суток.
уточнение: "освещение по legacy-модели" из плана переноса переименовано — в legacy реального
алгоритма освещения не было вообще (только неиспользуемая структура `CLight` и пустой uniform),
так что день/ночь и грядущий свет от блока — новая система с нуля, а не перенос.
проверено: headless-прогон — 15×F10 переводят 06:10 в 00:11, та же точка мира заметно темнеет.
Диски солнца/луны отдельно проверены изолированным тестом на тех же самых шейдерных файлах.

версия 0.5.1
добавлено: свет от блока — Sun Stone (`assets/blocks.json`, id 11, радиус ~14, тёплый
бело-жёлтый, без мерцания, появляется только в инвентаре игрока). BFS-распространение
через границы чанков (`Chunk_Manager::recompute_lighting`), затухание на 1 уровень за блок,
останавливается на непрозрачных блоках. Не тускнеет вместе с ambient день/ночь — светит
одинаково и днём, и ночью.
известное ограничение: пересчёт освещения полный (по ограниченной области), а не инкрементальный
де-пропагейшн — осознанный компромисс ради надёжности.
текстура блока — временная жёлтая заглушка, настоящую пришлёт пользователь позже.
проверено: изолированный тест на реальном Chunk_Manager (точное затухание, остановка на стенах,
корректное снятие света при поломке источника) — первая версия теста поймала два настоящих
бага (стена не блокировала свет, снятие источника не гасило его), оба исправлены. Стресс-тест
под ThreadSanitizer после этих правок — 16360 правок блоков (2338 — Sun Stone), 0 гонок.


версия 0.6.0
интегрирован новый biome-ready генератор мира из handoff: OpenSimplex2S, независимые климатические поля
(temperature/humidity/continentalness/erosion/weirdness), mountainness, 3D-пещеры, физические сигналы
water/surface connection, 23 биома и отдельная генерация подземных/особых биомов. Добавлены семейства
деревьев (oak/birch/spruce/jungle/acacia/slimewood), новые породы/руды/флора/пещерные блоки в
`assets/blocks.json`. Старые `Info.biome`/`*.biome` оставлены в assets для совместимости, но основной
`World_Generator` теперь использует новый генератор напрямую. Текстурные координаты новых блоков взяты
из handoff как placeholders и требуют финальной привязки к реальному атласу.

версия 0.7.0 (играбельный вертикальный срез)
исправлено: слишком тёмные тени. Минимальная освещённость в шейдерах блоков/флоры/жидкости/предметов
поднята с 0.08 до 0.32 (мобы — до 0.4), ночной ambient 0.18 -> 0.4, виньетка слабее. Всё видно, просто темно.
добавлено: земледелие. Трава/папоротник иногда роняют Wheat Seeds (blocks.json: drop + drop_chance), цветы
падают сами. Мотыга (ПКМ по траве/земле) делает грядку, ПКМ семенами/морковью по грядке сажает росток.
Каждый росток растёт 3-6 минут (случайно при посадке, `Crop_Manager`), вдвое быстрее, если вода в радиусе 4
блоков (грядка при этом меняет вид на «влажную»). Растут только посевы в загруженных чанках; состояние в
`worlds/<мир>/crops.txt`. Дикая морковь генерируется на грядках (Wild_Farmland) на равнинах/степи (чаще) и в
лесах (реже); когда игрок её собирает, грядка исчезает и на её месте снова трава.
добавлено: корова (`assets/mobs/cow.glb`, модель сгенерирована `tools/gen_cow_glb.py` — можно заменить своей
с теми же клипами Idle/Walk), кормление ПКМ: коровы — Wheat, свиньи — Carrot, лошади — Raw Horse Meat, Wheat или
Carrot. Два накормленных животных одного вида идут друг к другу и рождают детёныша (растёт 15 минут, размер
плавно 0.5 -> 1.0; перезарядка родителей 5 минут). Общие таймеры — `entities/Breeding.h`. Лошадь помнит
ПОСЛЕДНЮЮ съеденную еду (`Horse::get_last_food()`), а жеребёнок — чем кормили родителей
(`get_parent_food_a/b()`, `born_from_horse_meat()`). Жеребёнка нельзя оседлать. Команды: `/summon cow`,
`/breed <pig|cow|horse>` (сразу «влюбляет» взрослых — для проверки без кормёжки).
добавлено: крафт с тегами — в recipe.json есть `"tags"`, в рецептах пишется `"#planks"`/`"#logs"`: верстак,
сундук и палки делаются из ЛЮБЫХ досок. Брёвна всех пород -> доски. Рецепты инструментов (кирка, топор, лопата,
меч, мотыга) из досок/камня/железа/алмаза, палки, печь. Плавка: железо, золото, говядина, конина.
добавлено: инструменты с прочностью (дерево 59, камень 131, железо 250, алмаз 1561) — поля `durability`,
`tool`, `tool_tier`, `tool_speed`, `attack_damage` в blocks.json; у блоков `effective_tool` и `harvest_tier`.
Подходящий инструмент ускоряет добычу в `tool_speed` раз, руды требуют кирку нужного уровня (железо — каменная,
золото/алмаз — железная), оружие даёт урон по мобам. Каждая добытая «твёрдая» клетка/удар тратит 1 прочности.
Инструменты не стакаются, прочность хранится в `Item_Stack::damage` (инвентарь, сундуки, дроп) и рисуется
полоской под иконкой. Новая алмазная руда генерируется глубоко под землёй.
отладка: `OPTICRAFT_AUTOSTART=survival|creative` (сразу создать мир AutoTest), `OPTICRAFT_CMDFILE=<файл>`
(выполнить команды из файла в игре), `OPTICRAFT_CROP_SPEED=N` (ускорить рост посевов).

версия 0.7.1
добавлено: изометрический снимок мира на клавишу F7, как в Minecraft Indev. Снимается квадрат чанков
вокруг игрока (`Config::iso_capture_radius_chunks`, сейчас 14), ортогональная камера под углом 35.264° и 45°,
без HUD и мобов; результат сохраняется PNG (`src/rendering/capture/Isometric_Capture.*`). (03ed448, e016590)

версия 0.7.2
система миров: рефакторинг структуры `src/` (OpenSimplex2S в `external/opensimplex`, математика в `src/math`,
пустые `Mesh.h/.cpp` удалены) и измерения. `Dimension_Id` и `Dimension_Info` задают режим рельефа: `Noise`,
`Flat` или `Void`. Ключ хранения (`storage_key`) отделяет сохранения разных измерений, `World_Generator`
выбирает измерение через `set_dimension`. Тестовые `Flat_Test` и `Void_Test` уже зарегистрированы.
Перемещения между измерениями пока нет. (4348bc2, 44f718d)

версия 0.7.3
палитровые чанки, жадный меш, жидкости, взрывоустойчивость блоков, ленивый шум руд. (c6b6903, 2026-10-12)

версия 0.7.4
лошадь: функция атаки `Horse::attack` (кулдаун, досягаемость, урон от скорости). Пока не вызывалась из игры.
(44f718d, 2026-10-12)

версия 0.7.5
лошадь: прыжок по Space. Игрок, который крадётся, замедляется и останавливается у края блоков. (c96c1a6, 2026-10-12)

версия 0.7.6
более жадный меш листвы, стекла и льда; рейкаст целится во флору; лошадь: ручник по Ctrl. (5640bde, 2026-10-12)

версия 0.7.7
окклюзия секций выключена по умолчанию (самоссылочный тест мерцал). (1c7a409, 2026-10-12)

версия 0.7.8
быстрее генерация мира: плотное поле пещер на чанк, точный отсев клеток. (361cca2, 2026-10-12)

версия 0.7.9
загрузчик OpenGL перегенерирован под версию 4.3. (6e554bb, 2026-10-12)

версия 0.7.10
жадный меш секций строится на GPU (OpenGL 4.3). (3ec89df, 2026-10-12)

версия 0.7.11
перерисованы текстуры блоков в реалистичном стиле 16x16, инструменты, печи, слитки и алмаз в атласе;
слияние ветки с текстурами. (40c7973, 1ee65b6, dbe1b0a, 3211b36, 27893b5, ecfe599, 2026-10-13)

версия 0.7.12
отсечение невидимых пещер по достижимости секций от камеры. (81230c5, 2026-10-13)

версия 0.7.13
Python-инструменты удалены из репозитория: `tools/gen_atlas.py`, `tools/gen_ui_atlas.py`, `tools/gen_cow_glb.py`,
`tools/make_pig_glb.py`. Упоминания этих скриптов выше относятся к прошлым версиям. (90aed5c, 2026-10-13)

версия 0.7.14
глобальное обновление системы мобов: мобы переписаны под общий `Mobs_Update`. (c6088f6, 2026-10-15)

версия 0.8
переход к обновлению боевки.
- у каждого инструмента в `assets/blocks.json` свой `attack_cooldown` (перезарядка удара) и `speed_damage_scale`
  (во сколько раз бонус урона от скорости больше или меньше обычного). Кулак — `Config::player_attack_cooldown`.
- бонус урона от скорости игрока без потолка, считается только скорость сверх ходьбы (`player_move_speed`).
- скорость игрока ограничена `Config::player_max_speed` = 50 блоков/с, включая импульсы инструментов и течение
  воды. Механика импульса (ПКМ, `impulse_speed`) уже есть, предметов с ней пока нет.
- мобы бьют сильнее быстрого игрока (`Config::mob_speed_bonus_per_speed`). (ee27576)

версия 0.8.1
враждебные мобы в основном цикле: пауки (`Hostile_Kind::Spider`) днём и ночью охотятся на лошадей, на игрока
нападают только ночью (нормализованное время суток >= `night_start_normalized` = 0.5). Лошадь без всадника бежит
к ближайшему пауку в `horse_hunt_range` и бьёт его; паук бьёт ближайшую лошадь. `Hostile_Mob::update` возвращает
`Hostile_Result` (урон игроку и урон лошади раздельно). Команда `/summon spider`. Модели зомби и скелета из
архива лежат в `assets/mobs/`, модель паука ещё не добавлена. (0367b76)

версия 0.8.2
централизация тиков: `Mobs_Update::update_tick` вызывает `update_hostiles`. Раньше этого вызова не было,
поэтому враждебные мобы не спавнились. Лошади и езда обновляются по кадрам в `Mobs_Update::update_frame`. (ce62aa7)

версия 0.8.3
централизация мирных мобов: свинья, корова и овца обновляются и спавнятся из `Mobs_Update`.
корова заменена на новую 3D-модель. модель игрока пока заглушка, будет заменена позже.
(без коммита, правки на локальном диске автора)

версия 0.8.4
техдокументация обновлена под мобов, бой, тики и ночь; история версий. (b781a71, e06e482)

версия 0.8.5
набор оружия (id 106–123, 18 предметов, спрайты в `texture_atlas.png`, ряд 2, колонки 42–59): нож, копьё,
булава, боевой топор, рапира, трезубец. Новый вид `"tool": "weapon"` (оружие ничего не копает).
- нож: самая быстрая перезарядка (0,2 с), малый урон, малый бонус от скорости (x0,5).
- копьё: ПКМ тянет игрока вперёд на суше (`impulse_speed` 9–16) и слабо в воде (`impulse_speed_water` 2,5–4).
- трезубец: наоборот, в воде сильный импульс (13–16), на суше слабый (2,5–3). Только железо и алмаз.
- булава: самый большой множитель урона от скорости (x3,0), медленная (1,2 с). Камень, железо, алмаз.
- боевой топор: самый большой урон за удар (8–12), очень медленный (1,4 с). Камень, железо, алмаз.
- рапира: быстрая (0,55 с), множитель скорости x2,0. Железо и алмаз.
- новые поля `blocks.json`: `impulse_speed_water` (импульс в жидкости), `impulse_cooldown` (пауза между
  импульсами). Выбор импульса по среде — `Game::handle_tool_impulse` (`Player::is_in_liquid`).
- рецепты оружия в `assets/recipe.json` (деревянное — из любых досок).

версия 0.8.6
режимы и полёт: F1 циклически переключает Выживание → Творчество (полёт с коллизиями) → наблюдатель
(полёт сквозь блоки) → Выживание. Полёт привязан к Творчеству, в Выживании его нет (`/fly` там отключён).
Инвентарь един для обоих режимов: предмет, взятый из палитры Творчества, становится обычным стаком и остаётся
в инвентаре/сундуке, его можно использовать в Выживании. Палитра — отдельная сетка 9x3 (ids 400+). Сундуки
показывают хранилище игрока и хотбар в обоих режимах. Команда `/gamemode spectator`. Сохранение: `spectator`
в `world.json`; старый `c`-блок в `inventory.txt` больше не пишется и читается пропуском.

версия 0.8.7
ID блоков без ограничения 256: палитра чанка хранит `Block_ID` (uint16), ширина записи растёт
1 → 2 → 4 → 8 → 16 бит по мере переполнения. Свойства блоков (`Block_Registry`) растут по максимальному id,
а GPU-LUT и таблица флагов — по числу зарегистрированных id. Потолок пространства id — 65536 (`k_max_block_types`).
Региональный формат пишет id как uint16 (форматы 3, 4, 5); старые форматы 0–2 читаются. Неизвестный id при
загрузке превращается в воздух. Ключ слияния граней расширен до 64 бит (id 16 + meta 8 + свет 8 + AO 8);
compute-шейдер `mesh_greedy.comp` читает его парой `uvec2`. Миры на диске прежние читаются без конвертации.

версия 0.8.7.1
цикл дня и ночи принадлежит измерению, а не всему миру. У `Dimension_Info` появился `time_mode`:
`Cycle` — свои часы (Overworld, Flat_Test), `Fixed` — время заморожено на `fixed_time` (Void_Test, полдень).
`Day_Night_Cycle::set_frozen` останавливает ход часов. Для замороженного измерения время не записывается
в `world.json`, чтобы не затереть сохранённое время мира.

версия 0.8.7.2
чанк привязан к измерению: `Chunk::get_dimension_id()` задаётся генератором при создании, поэтому
чанки разных измерений не путаются в памяти. Хранение уже разведено по `storage_key` (отдельные region-файлы).
Ограничение: переходов между измерениями пока нет, в памяти одновременно живёт одно измерение.

версия 0.8.8
типы мира при создании: Обычный, Супер плоский, Один биом, Парящие острова, Тест: плоский, Тест: пустота.
Кнопка «Тип мира» перебирает их; для «Один биом» и «Парящие острова» появляется кнопка «Биом» (14 наземных биомов).
Один биом: обычный рельеф и декорации, без пещер; биом задан на весь мир и хранится в `world.json` (`biome`).
Парящие острова: земли нет, под островами пустота; острова ставятся в каждой ячейке (в обычном мире
они редки, а океанские лежат ниже Y=200 и не видны). Точка появления — ближайший остров к (8, 8).
Супер плоский: слои камень/земля/трава на Y 5, без декораций.
Измерения `superflat`, `single_biome`, `single_islands` добавлены в `Dimension.cpp`; обычный мир — `overworld`.
Старое различие «Классика/Острова» (только имя папки сохранения) убрано из меню.

версия 0.8.9
«Oak Wood» (id 5) удалён из кода и данных; его id 5 отдан целому камню («Stone» переехал с id 3 на id 5).
Id 3 теперь свободен. Стволы деревьев ставятся как «Oak Log» (id 56), рецепт досок и переплавка
используют только «Oak Log». Id 8 по-прежнему зарезервирован под динамит.
Миры, сохранённые до этой версии, читаются так: id 3 (старый камень) станет воздухом, id 5 (старое дерево) — камнем.

версия 0.8.10
каждый сгенерированный чанк сразу пишется в region-файл, поэтому при следующей загрузке он читается с диска без генерации.
Очередь генерации пересортируется при каждом переходе игрока через границу чанка: ближайшие чанки идут первыми,
задачи вне окна загрузки снимаются. Воркер берёт снимок мира под мьютексом; задачи прошлого мира не выполняются.

версия 0.8.11
булыжник («Cobblestone», id 3): выпадает из камня при добыче, в печи переплавляется обратно в камень.
Спрайт булыжника — процедурный, нарисован в свободной ячейке атласа (96, 16); его можно заменить.

версия 0.8.12
исправлено: артефакты на чанках в режиме наблюдателя и сильная просадка FPS при полёте сквозь только что
загруженные области. Сначала подозревали отсечение видимости, устаревшие меши соседей и переполнение id блоков;
попытки править эти места не устраняли причину. Затем жадный мешер усилили: он стал объединять грани разных
блоков, если у них совпадает текстура. Это уменьшало число квадов, но визуальные «структуры» на стыках проявлялись
чаще — алгоритм агрессивнее растягивал один квад через разные типы блоков.

выяснилось, что межчанковое слияние и GPU-мешер используют разные представления данных. GPU строит квады отдельно
для каждого чанка, а совместимость готовых квадов через границу проверяет CPU. Если полностью пропустить эту
проверку для GPU-чанков, FPS стабилизируется, но артефакт возвращается. Если без ограничений отправлять соседние
GPU-чанки на CPU, границы исправляются, однако во время потока пересборок FPS падает примерно до 12; после
переключения режимов очередь успевает закончить работу и FPS подскакивает выше 350.

итог: межчанковая проверка остаётся на CPU только там, где нужна для слияния GPU-чанков; результаты мешинга
кешируются, а версия меша соседнего чанка меняется только если его объединённую CPU-геометрию действительно
пришлось восстановить. Так повторная загрузка GPU-ресурсов не запускается для неизменившихся чанков, очередь
пересборки не раскручивается заново, а текстурные артефакты и нестабильный FPS ушли.

версия 0.9.1
добавлено: новый менеджер чанков — загрузка и выгрузка переписаны вокруг фиксированного пула памяти.
- пул слотов (`Chunk_Slot_Pool`): все чанки живут в слотах, выделенных один раз при старте. Создание и
  освобождение без аллокаций и без замков (стек Трайбера с версией против ABA). Если слоты кончились,
  загрузка откладывается до освобождения, а не падает.
- буфер ввода-вывода (`Chunk_IO`): выгрузка копирует блоки в буфер и сразу возвращает управление. Отдельный
  поток пачками пишет накопившееся на диск, по одному захвату региона на пачку. Загрузка тоже пачками, в
  порядке расположения на диске. Чанк, ещё стоящий в очереди записи, отдаётся из буфера, а не с диска.
  Чтение с диска остаётся под мьютексом: корректность важнее скорости.
- приоритет по игроку: каждый чанк знает своё кольцо (расстояние в чанках до игрока). Очередь генерации
  и очередь мешей пересортировываются при смене чанка. Задачи генерации, оставшиеся вне окна загрузки, снимаются.
  Меши колец 0–1 ставятся с наивысшим приоритетом.
- очереди без замков: `Mpmc_Queue` (кольцо Вьюкова с резервным списком, элементы не теряются) и
  `Priority_Bucket_Queue` (корзины по приоритету).
- вертикальное окно: 8 слоёв по Y в обычном режиме, 4 при быстром вертикальном движении или малом радиусе.
  Приоритет смещается в сторону движения, поэтому чанки впереди грузятся раньше чанков позади.
- загрузка мешей в видеокарту на главном потоке ограничена бюджетом 2 МиБ данных за кадр
  (`Renderer::kGpuFrameBudgetBytes`). Ближние чанки идут первыми, дальние ждут следующего кадра.

проверено: изолированные тесты пула (исчерпание, стресс, отсутствие дублей слотов), буфера ввода-вывода
(сохранность блоков в трёх сценариях), очередей (порядок по кольцам). Сравнение со старой версией без GL, радиус 8, по три прогона на версию: при полёте 200 блоков/с
по горизонтали новая держит около 88 % окна (старая в среднем около 5 %), на стоянке окно ровно 2312 чанков
(старая перегружала до 2872). Главный поток на кадр: в среднем около 6 мс против 10 мс, p99 около 52 мс против 90 мс.

версия 0.9.2
переработана генерация мира: старая логика в `World_Generator` заменена отдельными модулями.
- пещеры (`Cave_Network`): черви и шахты, закрытые пещеры ниже Y = -63, затопление до Y = -120.
- реки (`River_Network`): русло по шуму с искажением координат, уровень воды стекает к морю,
  водопады при перепаде не меньше 3 блоков.
- структуры (`Structure_Placement`): деревни размещаются по Worley-пузырям, но пока это только маркер
  (столб над центром), сами постройки не сделаны.
- кеш регионов (`Region_Cache`): регионы строятся без общего замка, один регион не строится дважды.
проверено: детерминизм генерации, 0 расхождений на 22 260 точках.

версия 0.9.3
подземные биомы теперь выбираются по типу пещеры из `Cave_Network`: закрытые сухие пещеры → Deep_Underground,
закрытые затопленные и водные полости → Flooded_Underground, воздушные полости → Underground.
`Biome.cpp` и `Biome.h` не менялись: изменилась только функция выбора биома в `World_Generator`.

версия 0.9.4
`Region_File`: добавлены пакетные операции. `read_batch` сортирует чанки по позиции на диске и читает их
за один проход, `write_batch` пишет пачку с одним сбросом индекса региона. Одиночные `read` и `write`
теперь обёртки над теми же внутренними функциями.
проверено: изолированный тест буфера ввода-вывода, сохранность блоков в трёх сценариях.

версия 0.9.5
освещение, отладка и вода.
- свет: тени неба, освещение от факела и лавы, исправлены квадраты света.
- отладочный оверлей (F3 и F4): статистика и параметры мира, строки эффектов и биома.
- вода: растекание по уклону, текстура течения смещается по направлению потока.
- факел: рецепт из древесного угля и палки, даёт 4 факела.
- лабиринт: генератор `Maze` (отдельный модуль, пока не подключён к генерации мира), каменные ступеньки
  и полублоки (`Stone_Stairs`, `Stone_Slab`).

версия 0.9.6
деревья и растения.
- дуб: новая форма — неровные кроны, ветки на трёх высотах, стволы поверх листвы. Лес стал реже и проходимее.
- листва дуба: с неё падают яблоко (1 %), палка (2 %) и саженец (5 %). Листва других пород — свой саженец.
- саженцы и доски для всех пород: дуб, берёза, джунгли, акация и новое слизовое дерево. Текстуры в атласе.
- слизовое дерево («ива»): низкое, широкое, четыре ветки на разных высотах, по одной на сторону, свисающие нити.
  Такие деревья редкие: между ними не меньше 50 блоков (`is_sparse_tree_anchor`, сетка 60 клеток).
- `Sapling_Manager`: несколько пород. Большие группы (2×2, 3×3) остались только у ели; остальные растут одиночно.
  Формат сохранения: `x y z size progress total species`. Старые записи считаются елью.
проверено: тесты деревьев и саженцев без GL; минимальное расстояние между ивами 50.0 на трёх сидах.

версия 0.9.7
мобы и эффекты.
- спавн мобов только на земле и в подходящем биоме; над океаном и в воздухе не спавнятся.
- зелья-шарики: бросаются ПКМ и разбиваются о моба или землю. Регенерация (1 HP за тик), скорость, прыжок.
  Эффекты есть у игрока и у свиней, коров, овец, лошадей и мобов-врагов. Строка эффектов видна в F3.
- сбрасываемые предметы: `Dropped_Item` умеет «брошенный» режим, такие предметы не сливаются и не подбираются.
проверено: тесты эффектов и брошенных предметов без GL; обычное освещение без изменений.

версия 0.9.8
данные: бинарный формат JSON (`.ocb`).
- конвертер `json2bin`: `--dir assets` превращает JSON в `.ocb`, `--verify` сверяет, `--dump` печатает.
- при загрузке `.ocb` используется, если он свежее JSON; устаревший или повреждённый файл пропускается
  (при повреждении — предупреждение в лог и чтение JSON).
- замер: декодирование около 0.25 мс, загрузка реестра блоков около 1.1 мс. Выигрыш по времени
  незаметен, поэтому вторая фаза (ускорение реестра) не делалась. Размер ассетов около 26 % от JSON.
проверено: round-trip всех ассетов, сценарии «свежий / устаревший / повреждённый / только .ocb / ничего».

версия 0.9.9
броня, руды, медь и камень.
- броня: кожаная (кожа с коров и лошадей, по 1–2 шт.), алмазная и золотая. Защита и прочность как в
  Minecraft. Рецепты как у железной брони. Железная броня теперь тоже крафтится.
- уголь: руда `Coal Ore` (добывается деревянной киркой) даёт уголь. Уголь — топливо для печи.
- медь: руда `Copper Ore` (нужна каменная кирка) даёт 2–8 сырой меди. Сырая медь и медный слиток плавятся в печи.
  Текстуры руды, самородка и слитка в атласе.
- камень: требует кирку, даёт булыжник. Рукой камень ломается, но ничего не выпадает.
- исправлено: в `blocks.json` id камня и булыжника были перепутаны с `Block_Types`, и генератор ставил
  булыжник вместо камня. Теперь id совпадают. В старых мирах блоки этих двух типов могут выглядеть иначе.
- генерация: уголь и медь появляются в камне отдельным шумом, их жилы не совпадают с железом.
проверено: тесты реестра (ID, броня, дроп 2–8, уголь), тест добычи камня рукой и киркой, состав мира (камня много, булыжника ноль).

версия 0.9.10
фундамент красной логики (`Logic_Manager`).
это только фундамент, и не более. Полноценное обновление логики будет отдельной версией; здесь ничего
сверх перечисленного не заявлено.
- блоки: провод (`Copper Wire`, рецепт — 1 медный слиток → 4 провода), рычаг, кнопка, зажиматель сигнала
  (инвертор вместо редстоун-факела), повторитель. Пары «выкл. / вкл.» — разные блоки, состояние видно по текстуре.
- рычаг, кнопка и зажиматель крепятся к грани опоры, провод и повторитель стоят на полу. Без опоры не ставятся;
  при поломке опоры узел отрывается и падает предметом.
- правила (реализованы по описанию механик Java Edition): сигнал 15, на проводе −1 на шаг; рычаг мгновенный;
  кнопка 10 редстоун-тиков (1 с); зажиматель инвертирует с задержкой 1 тик; повторитель с задержкой 1–4 тика,
  выход только вперёд, отмена переключения при пропавшем входе, блокировка боковым повторителем.
- упрощено: нет квазисвязности, сильного и слабого сигнала, порядка обновления соседей, сгорания зажимателя.
- ограничения: таймеры живут в памяти; узлы ищутся вокруг игрока радиусом 12 блоков, раз в секунду.
проверено: 25 тестов правил на фейковом мире. В игре не запускалось, соответствие поведению Minecraft
не сверялось с исходным кодом и не гарантируется.

## Персистентность чанков

Содержимое чанков хранится в региональном файле `worlds/<world>/chunks/<generation>.region`.

- При подгрузке чанк сначала ищется в региональном файле; если запись найдена и валидна,
  блоки загружаются без процедурной генерации.
- Если записи нет, чанк генерируется текущим `World_Generator`.
- После изменения блока или его метаданных актуальный снимок чанка сразу ставится в
  асинхронную очередь записи. Главный поток не ждёт диска.
- Несколько быстрых правок одного чанка объединяются, пока предыдущая версия ещё не взята
  писателем, поэтому на диск уходит последняя версия, а не очередь устаревших снимков.
- При выгрузке и при выходе из игры изменённые чанки дополнительно проверяются и сохраняются;
  перед закрытием мира очередь записи полностью дожидается завершения.
- Classic и Islands имеют отдельные региональные файлы, чтобы пресеты генерации не смешивались.

## Оптимизация хранения чанка (2026-09-10)

Рантайм-чанк по-прежнему ровно **32x32x32**, но его "горячее" воксельное хранилище
переработано под локальность CPU-кэша:

- id блоков: плоский `uint8_t[32768]` = **32 КиБ** (реестр блоков ограничен 256 id);
- освещение блоков: упакованные 4-битные нибблы = **16 КиБ** (диапазон света 0..14);
- итого "горячих" воксельных данных: **48 КиБ на чанк**, непрерывный блок памяти, без куч;
- задачи пересборки меша копируют целевой чанк целиком плюс только **шесть однослойных
  граней соседей** (по 1 блоку), а не шесть полных соседних чанков;
- формат сохранения чанка v2 хранит id блоков как байты (**32 КиБ** payload вместо 64 КиБ);
  сохранения v1 по-прежнему читаются.

Размеры чанка не изменились: `32 x 32 x 32`.


## Вертикальный диапазон мира / стриминг

- Мировые координаты теперь строго **Y=-256..1024 (включительно)**.
- `Chunk` остаётся ровно **32x32x32**.
- Весь диапазон покрывается chunk-Y от `-8` до `32` (41 позиция чанка; последний чанк
  содержит только мировой Y=1024).
- В памяти держится только вертикальное окно вокруг игрока (`vertical_load_radius_chunks=8`);
  при вертикальном перемещении окно сдвигается. Это не даёт памяти расти бесконтрольно,
  сохраняя при этом полную высоту мира.
- Процедурный рельеф следует пропорциям из biome handoff для диапазона -256..1024.

## Генерация пещер

- Генерация пещер теперь использует два 3D-масштаба шума: шум масштаба туннелей и
  низкочастотный шум камер (залов).
- На большой глубине вклад "камерного" шума усиливается, что даёт настоящие крупные
  пещеры-залы, а не отдельные залитые водой пятна.
- Порог пещер снижен с 0.62 до 0.58, так что обычные пещеры стали заметно чаще встречаться.

## Планирование пересборки меша

- Задачи пересборки меша используют очередь с приоритетом и обгоняют задачи генерации.
- Один воркер зарезервирован под работу с мешами, так что правка блока может пересобраться,
  даже пока остальной пул воркеров занят генерацией рельефа.
- Задачи меша по-прежнему используют компактный снимок (свой чанк + шесть пограничных
  граней), а не полные копии соседей.

## Прозрачность

- Прозрачные/cutout-проходы сохраняют depth-тест и теперь пишут глубину. Это не даёт более
  дальнему полупрозрачному блоку визуально перекрыть более близкий.
- Флора использует alpha discard и запись глубины; жидкость остаётся alpha-blended, но
  сортируется по глубине back-to-front на уровне чанка.

## Небесные острова (Sky islands)

Биом `Sky` теперь опирается на настоящий детерминированный плавающий рельеф. Острова
генерируются как несвязанные формации из Cloud Stone на большой высоте (примерно
Y=260..900), с неровными очертаниями, толстым центром и сужающимся книзу основанием.
Их расположение зависит только от мировых координат + seed, поэтому на границах чанков
не может возникнуть швов или задвоенных/пропавших островов.

Старое дерево исходников `legacy/` и устаревшие поблочные файлы `assets/blocks/*.block`
больше не поставляются. Актуальный реестр блоков — `assets/blocks.json`; чанки мира
используют бинарный формат сохранения `.bin`, которым занимается `World_File`.

## Occlusion culling: убрана самоокклюзия секций (2026-09-12)

Query для occlusion culling (`Renderer::render_chunks`, второй проход) исправлен — раньше
секции мигали (то рисуются, то пропадают): `GL_CULL_FACE` оставался включён во время
запроса, а бокс секции, если камера физически находится внутри него (очень частый случай —
сама секция, где стоит игрок), состоял из одних задних граней и вообще не растеризовался,
из-за чего запрос всегда возвращал "не видно". Дополнительно на дистанциях за пару блоков от
камеры сказывалась низкая точность буфера глубины (near=0.1/far=1000) — прежний
геометрический инсет бокса (0.05 блока) был на порядки меньше одного шага квантования глубины
и не спасал от z-fighting секции с самой собой. Теперь на время прохода `GL_CULL_FACE`
выключается, а вместо/вместе с инсетом используется `glPolygonOffset`, который смещает
глубину в единицах самого буфера и не зависит от дистанции.

## Пещеры: связность тоннелей (2026-09-12)

`cave_density_at` вырезает тоннели там, где два независимых 3D-шума одновременно близки к
нулю (пересечение двух изоповерхностей — приём "ridged/worm caves", как в ваниль-Minecraft
1.18+). Ширина полосы (`kTunnelBand`) и частота шума не были согласованы: при
`freq=0.020/band=0.045` полученная "трубка" пересечения получалась тоньше одного блока, и
воксельная решётка почти всегда проходила мимо неё — на срезе 96×48×96 блоков вырезалось
0.33% объёма, раздробленного на 769 несвязанных кусков (самый большой — 37 вокселей). Видимо
это и есть та самая "пара пустых блоков в чанке" вместо пещеры.

Параметры подобраны замером связности (flood fill по вырезанному объёму) на реальном шуме
движка, не на глаз — см. `tools/cave_tuning/`. При `freq=0.018/band=0.16` тот же замер даёт
3.1% вырезанного объёма и 43% этого объёма в одной связной системе — то есть настоящие
протяжённые извилистые ходы переменной толщины, а не шум. Крупные "каверны" (редкие залы на
большой глубине, отдельная низкочастотная ветка в той же функции) эта правка не трогает — они
и раньше были одной гладкой изоповерхностью без проблемы тонкой трубки.

Если захотите пещеры гуще/реже или другого масштаба — `tools/cave_tuning/README.md`
объясняет, как погонять `sweep`/`slice` по своим числам, прежде чем переносить их в
`cave_density_at()`.

## Worldgen shoreline/cavity fix (2026-09-13)

Теперь формулы для поверхностей океана и суши плавно сопрягаются при значении показателя континентальности `C = -0,19`;
ранее базовое значение `+8` на стороне суши приводило к образованию замкнутого вертикального кольца,
напоминавшего заполненный водой цилиндрический провал посреди обычного ландшафта.
- Исходные выражения для шума пещер и параметры FBM остались без изменений. Вода в
затопленных пещерах (Flooded Underground) по-прежнему добавляется только после того, как
воксель проходит проверку по маске пещеры; этап генерации воды не изменяет рельеф.
- При размещении деревьев запись производится только в существующие воздушные воксели,
что предотвращает перезапись сгенерированной воды или рельефа объектами береговой зоны
на границах чанков.

## Система мобов (glTF/GLB) (2026-09-22)

Загрузка мобов из `.glb` без ручной привязки анимаций/текстур под каждую модель —
см. `src/mobs/Mob_Model.{h,cpp}` (парсинг файла) и `src/mobs/Mob.{h,cpp}`
(экземпляр в мире: transform + проигрывание анимации). Парсинг — через `cgltf`
(`external/cgltf/`).

**Как готовить модель в Blender:**
1. Обычный армейтур + скиннинг (Ctrl+P → With Automatic Weights), имена костей — любые.
2. Каждая анимация — отдельный Action; имя Action'а = имя, по которому её потом
   вызывать в коде (`"Idle"`, `"Walk"`, `"Attack_01"`, `"Death"`...).
3. Материал — Base Color и (опционально) Normal Map подключены в Principled BSDF
   как обычно; роль текстуры подтягивается автоматически из этих слотов, имя
   файла текстуры значения не имеет.
4. Экспорт: File → Export → glTF 2.0, формат **.glb**, галки "Export Animations"
   и "Export Skinning" включены (Pack Textures — если не хочется таскать текстуры
   отдельными файлами).

**Использование в коде:**
```cpp
Mob zombie;
zombie.load("assets/mobs/zombie.glb"); // модель кэшируется по пути — повторные Mob с тем же путём переиспользуют GPU-ресурсы
zombie.set_position({10.0f, 65.0f, 10.0f});
zombie.play_animation("Walk");

// каждый кадр:
zombie.update(delta_time);
zombie.render(mob_shader, view, projection);
```
Шейдеры — `assets/shaders/mob_vertex.glsl` / `mob_fragment.glsl` (стандартный
linear blend skinning, до 4 костей на вершину, до `MAX_MOB_BONES = 64` костей
на модель — см. `Mob_Model.h`).

Загрузчик собирает геометрию ВСЕХ мешей файла в один буфер (частый случай
готовых моделей — тело/ноги/голова отдельными объектами на одном скелете;
проверено на реальном `.glb` с Sketchfab: 7 мешей → один draw call).

Правильно считает трансформацию корневой кости с учётом ВСЕХ предков выше
`skin.joints` (масштабирующие/разворотные обёртки конвертеров FBX→glTF —
Armature/RootNode/и т.п.) — трансформ самого mesh-узла в формуле скиннинга
алгебраически сокращается (`worldPos = jointGlobalTransform *
inverseBindMatrix * localPos`), поэтому значение имеет только полная цепочка
предков КОСТИ от истинного корня сцены, а не узла с мешем.

При загрузке скелета выполняется sanity-check bind-позы: по определению
`inverseBindMatrix` — это `inverse(global_transform кости в bind-позе)`,
значит `global_bind(bone) * inverseBindMatrix(bone)` обязана быть identity.
Если нет — TRS костей в файле не согласуется с их же inverseBindMatrices
(встречается у моделей из дешёвых/автоматических FBX→glTF конвертеров —
типичный симптом: гигантская перекошенная геометрия при в целом корректном
загрузчике). Лечится не багфиксом движка, а переэкспортом модели напрямую
из Blender по конвенции выше. `Mob_Model` в этом случае честно пишет в лог
WARN на каждую "подозрительную" кость с её ошибкой масштаба.

**Известные ограничения текущей версии** (легко расширяется по необходимости):
- Все меши файла должны использовать один и тот же материал — берётся текстура
  только с первого найденного (мультиматериальные мобы пока не поддержаны).
- Только albedo + normal текстуры (occlusion/metallic-roughness/emissive не читаются).
- Анимации типа Shape Keys (morph targets) не поддержаны, только скелетные.
- Нет блендинга между анимациями (переключение мгновенное, без плавного перехода).

**Время жизни ресурсов:** `Mob_Model`/`Mob` держат GL-объекты (VAO/VBO/EBO) и
обязаны быть уничтожены ДО разрушения GL-контекста (`Window`). Это касается
не только самих `Mob` (порядок объявления членов класса решает это
автоматически), но и **статического кэша моделей** внутри `Mob::load()` —
он живёт до конца процесса, пока его не почистить явно. Поймано на практике
(segfault в exit-хендлерах после `glfwTerminate()`), поэтому:
`Game::shutdown()` обязан звать `Mob::clear_model_cache()` ПОСЛЕ очистки
всех `Mob`/сущностей с мобами, но ДО `m_window_ptr.reset()`. Уже сделано —
см. `Game::shutdown()` и комментарий в `Mob.h`.

## Босс-рыцарь Knight_Ai (2026-10-31)

Полноценный трёхфазный босс находится в `src/mobs/Knight_Ai.{h,cpp}` и включён в
общий `Mobs_Update`: прицеливание и получение урона от игрока, урон от взрывов,
зелья скорости/прыжка/регенерации, отладочный счётчик, анимация и удаление после
смерти используют общие игровые пути. Вызов: `/boss knight` (также `/boss knight_ai`).

Обязательная модель — `assets/mobs/knight.glb`: команда спавна проверяет наличие
скелета и всех клипов `Idle`, `Walk`, `Attack_Slash`, `Attack_Double`, `Attack_Slam`,
`Jump`, `Charge`, `Spin`, `Hit`, `Death`; без них рыцарь не создаётся. Внутри GLB
запакованы меш, риг, текстура и анимации. Исходный атлас также лежит в
`assets/mobs/knight_atlas.png`.

Арена ИИ — область 5×5 чанков (32 блока на чанк) со срезанными углами и пределом
высоты 3 чанка. Она ограничивает перемещения босса, но не перестраивает мир и не
создаёт пол/стены автоматически. ИИ включает поле зрения с проверкой Raycast,
предугадывание движения игрока, память о заходах за спину, прыжковое перемещение,
телеграфируемые атаки и наследуемый набор атак в трёх фазах. Подробности и точки
настройки: `documentation/KNIGHT_AI.md`.

## Мобы: свинья с ИИ (Pig / Pig_AI) (2026-09-23)

Первый живой моб на системе выше — свинья с простым конечным автоматом
поведения (голод/энергия/счастье → Idle/Hungry/Eating/Sleepy/Sleeping/
Walking/Happy). Логика ИИ портирована из наброска пользователя (изначально
сгенерирован DeepSeek как консольная демка) в `src/entities/animals/ai/Pig_AI.{h,cpp}`
— это чисто поведенческий слой, ничего не знает о GL/Mob/позиции в мире.
`src/entities/animals/Pig.{h,cpp}` склеивает `Pig_AI` (поведение) с `Mob` (визуал):
переключает анимацию по имени состояния (`Idle`/`Walk`/`Eat`/`Sleep`/`Happy`
— если у модели есть такой клип, см. `Mob::has_animation`) и блуждает по XZ,
пока `Pig_AI` в состоянии `Walking`.

Спавн — `Game::spawn_missing_pigs()`, вызывается каждый кадр в
`update_gameplay()`: если свиней в мире меньше `Config::min_pig_count` (8),
досоздаёт недостающих в случайной точке кольца
`[pig_spawn_radius_min; pig_spawn_radius_max]` вокруг игрока.

**Известное упрощение:** блуждание не сверяется с рельефом — Y не меняется
после спавна (нет raycast до земли, нет коллизий по пути). На плоской
поверхности незаметно, на резком рельефе свинья может зависнуть в воздухе
или провалиться в блок. Для полноценного движения потребуется дать `Pig`
доступ к `Chunk_Manager`.

**`assets/mobs/pig.glb` пересобрана скриптом `tools/make_pig_glb.py`.** Скачанная
с Sketchfab модель была внутренне противоречива (TRS костей не совпадал с их
`inverseBindMatrices`, вершины частей тела лежали в разных единицах) и рисовалась
огромным боксом без ног; заодно в ней не было ни одной анимации. Теперь это
свинья в ванильных пропорциях (1 пиксель = 1/16 блока; 164 вершины, 7 костей,
клипы `Idle`, `Walk`, `Eat`, `Sleep`, `Happy`), начало координат — на земле
между копытами, «вперёд» = +Z. Текстура — `assets/mobs/pig.png` (оригинальная
из Sketchfab-файла, автор None_Yaroslav, CC-BY-4.0 — сохраните указание
авторства). Чтобы поменять размеры/анимации: правьте скрипт и запустите
`python3 tools/make_pig_glb.py` (нужен только numpy).

**Текстуры glTF-моделей грузятся без вертикального переворота.** `Texture` по
умолчанию переворачивает картинку (нужно блокам и HUD — там V идёт вверх), а у
glTF V идёт сверху вниз, поэтому `Mob_Model` вызывает загрузку с
`flip_vertically = false`, а фильтр берёт из sampler'а файла (NEAREST для
пиксель-арта). Если добавите нового моба и его текстура «съехала» — сначала
проверьте это.

## Атласы текстур: сколько угодно, без переписывания кода (2026-09-23)

Раньше был один хардкоженный атлас блоков (`texture_atlas.png`), жёсткая сетка
64×32, все тайлы строго 16×16 — из-за этого несимметричная текстура (факел и
т.п.) в принципе не помещалась нормально. Теперь:

- **`Texture_Atlas`** (`src/rendering/atlas/Texture_Atlas.{h,cpp}`) — одна текстура +
  именованные спрайты с явным пиксельным прямоугольником `{x, y, w, h}`.
  Спрайты внутри одного атласа могут быть КАКОГО УГОДНО размера — 16×16,
  16×24, 32×32, что угодно.
- **`Atlas_Registry`** (`src/rendering/atlas/Atlas_Registry.{h,cpp}`) — синглтон,
  при старте сканирует `assets/atlases/` на файлы `*.atlas.json` и грузит
  каждый. Это и есть механизм "сколько угодно атласов без правки кода":
  чтобы завести новый атлас (UI, 3D-иконки блоков для инвентаря, что
  угодно ещё) — просто кладёшь пару `<name>.png` + `<name>.atlas.json` в
  эту папку. Пересборка не нужна, C++ трогать не надо.

**Формат манифеста** (`assets/atlases/<name>.atlas.json`):
```json
{
  "name": "blocks_main",
  "texture": "../texture/texture_atlas.png",
  "sprites": {
    "grass_top":  { "x": 0,  "y": 0,  "w": 16, "h": 16 },
    "torch":      { "x": 32, "y": 0,  "w": 16, "h": 24 }
  }
}
```
`texture` — путь относительно самого манифеста. `name` — имя, под которым
атлас доступен через `Atlas_Registry::get_atlas(name)`; если не задано,
берётся имя файла без `.atlas.json`.

**Блоки ссылаются на спрайты по имени**, не по `[col,row]`:
```json
"texture": { "atlas": "blocks_main", "top": "tile_27_0", "side": "tile_25_8", "bottom": "tile_21_13" }
```
`atlas` необязателен, по умолчанию `"blocks_main"`. `Block_Registry` резолвит
`atlas+имя` в готовый UV-прямоугольник ОДИН раз при загрузке `blocks.json`
(через `Atlas_Registry`) и кладёт его прямо в `Block_Properties::uv_top/
uv_side/uv_bottom` — `Chunk.cpp` при постройке меша чанка атлас уже вообще
не трогает, просто читает готовые UV. Существующий `blocks.json` и
`assets/atlases/blocks_main.atlas.json` сгенерированы автоматически из
старого `[col,row]`-формата — визуально ничего не поменялось (проверено
рендером образцов тайлов в PNG и сверкой глазами: трава/земля/бок травы —
пиксель в пиксель как было).

**Практическое ограничение:** рендер чанка — один draw call, значит и
текстурой на этот draw call может быть только ОДИН атлас разом. Поэтому
все мировые блоки обязаны жить в одном атласе (`blocks_main` по умолчанию,
имя задаётся в `Renderer::initialize()`). Другие атласы (UI, иконки
инвентаря) — отдельные `Texture_Atlas` из того же `Atlas_Registry`,
используются независимо (HUD, например, спокойно может забиндить свой
атлас для крестика прицела/рамок, не мешая рендеру мира).

**Общий JSON-парсер** вынесен в `src/utils/text/Json.{h,cpp}` (раньше был
захардкожен прямо внутри `Block_Types.cpp`) — теперь им пользуются и
`Block_Types.cpp`, и `Atlas_Registry`, а не только blocks.json.

**Известное упрощение:** "атлас для блоков с ракурса с тремя сторонами"
(псевдо-3D иконки блоков для инвентаря, как в майнкрафте) — система для
этого уже готова (просто ещё один атлас с именованными спрайтами и опциональным
полем `"icon"` в блоке, отдельным от мировых `top/side/bottom`), но самих
иконок ещё нет — это либо рендерить самому (Blender/скрипт), либо рисовать
руками. HUD показывает верхнюю грань блока как иконку (`props.uv_top`).

## Палитры, жадный меш, жидкости, взрывоустойчивость (2026-10-12)

- **Палитра чанка** (`Chunk_Block_Grid`): список уникальных блоков + индексы 0/1/2/4/8 бит в `uint64`. Однородный чанк почти ничего не занимает, типичный — единицы КБ. Освещение — ленивая сетка (однородная не хранится). Формат на диске не менялся, старые миры читаются.
- **Жадный меш** целиком на GPU (compute-шейдеры `mesh_greedy.comp`, `mesh_misc.comp`, OpenGL 4.3): грани сливаются по ключу (блок, meta, свет, поворот); квады не пересекают границы Y-секций. Ступени, плиты, флора и жидкость строятся в `mesh_misc.comp`. Финальный GPU-driven frustum/Hi-Z culling и indirect-команды формирует `gpu_cull_commands.comp`; на CPU остаются упаковка данных и связность секций.
- **Полупрозрачные кубы** (листва, стекло, лёд, слизь): грань к такому же блоку не строится (внутри кроны граней нет), свет таких граней огрублён до 6 уровней на канал, поэтому они сливаются в большие квады. На тестовой кроне вершин в 2–4,5 раза меньше.
- **Жидкости** (`src/world/fluids/Fluid_Simulator`): логика как в Minecraft (`FlowingFluid`) — тики, уровень в meta блока (0 — источник, 1..7 — течение, бит 0x08 — падение), поиск стока до 4 блоков, бесконечные источники, смывание растений. Параметры в `blocks.json`: `fluid_tick_delay`, `fluid_drop_off`, `fluid_slope_distance`, `fluid_infinite`. Настройки: `fluid_tick_seconds`, `fluid_max_updates_per_frame` в `Config.h`.
- **Взрывоустойчивость**: ключ `blast_resistance` в `blocks.json` (шкала Minecraft: по умолчанию твёрдость×5, жидкости 100); доступ через `Block_Types`.

## Измерения (заготовка) и атака лошади (2026-10-12)

- **Измерения** (`src/world/dimensions/Dimension.h/.cpp`): `Dimension_Id` + таблица `Dimension_Info` (имя, режим рельефа `Terrain_Mode` = Noise/Flat/Void, декорации, небесный свет, слои плоского мира, ключ хранения). `World_Generator(folder, seed, dimension)` ветвится по режиму в `get_block`, `generate_chunk`, `chunk_is_definitely_air`, `decorate_column`. Overworld работает как раньше (генерация и хранилище бит-в-бит), тестовые `flat_test` и `void_test` — примеры режимов. Измерение пишется в `world.json` (`"dimension"`), чанки не-Overworld хранятся в своих region-файлах (`<dimension>_<пресет>.region`). Переходов между измерениями пока нет. Новое измерение = значение в enum + запись в `register_dimensions()`.
- **Атака лошади**: `Horse::attack(...)` возвращает `Horse_Attack_Result` (попала ли, урон, импульс), есть кулдаун, дальность и бонус урона от скорости (`Config::horse_attack_*`). Урон к цели применяет вызывающий код; в игровом цикле пока не используется.

## Отсечение невидимых пещер по связности секций (2026-10-13)

- Причина: кадр отсекался только по фрустуму, а окклюзия выключена (см. ниже). Пещеры под землёй, закрытые камнем, попадали в конус обзора и рисовались целиком. При радиусе 5 в загруженной области 4367 секций с геометрией, а достижимых от камеры обычно около 200.
- Что сделано: при построении меша каждая секция получает связность: компоненты проходимых клеток (воздух, вода, стекло, листва, флора) и маски их касания граней. Обход от секции камеры по этим компонентам отмечает достижимые секции; рисуются только они и секции, к которым примыкает достижимый воздух. Обход идёт только наружу от камеры по каждой оси (монотонное правило): иначе через пещеры протекали бы соседние полости.
- Кэш: результат хранится между кадрами и пересчитывается, только если сменилась секция камеры, набор загруженных чанков или связность. Правка блока, которая связность не меняет, кэш не сбрасывает.
- Замер (`get_renderable_chunks`, 48 случайных камер, среднее на камеру, без учёта фрустума): радиус 2 — 909 → 67 секций (−93%); радиус 5 — 4367 → 216 (−95%). Без монотонного правила было бы 415 и 1655. Пересчёт обхода 0,04 мс (радиус 2) и 0,27 мс (радиус 5), из кэша 0,01–0,04 мс.
- Проверка: сверка с эталонным обходом по клеткам (харнесс вне репозитория): на 48 камерах при радиусе 2 и при радиусе 5 нужных секций не пропущено, лишних нет; после правок блоков тоже. Лучевая сверка (720×360 лучей): видимых секций, отсечённых обходом, нет.
- Ограничения: отсечение выключается на кадр, если секция камеры закрыта (нет проходимых граничных клеток), если глаз стоит в непроходимой клетке (камень, застревание после телепорта), или если загруженная область больше 65 536 чанков с рамкой. Точный проход луча ровно через ребро между двумя проходимыми клетками без общей грани обход не учитывает: в одном кадре один пиксель показал фон вместо камня. Учёт рёбер и углов добавляет примерно 8–10% секций (по эталону), в этой сессии не доведён.

## Окклюзия секций выключена по умолчанию (2026-10-12)

- Тест окклюзии секций (`Renderer::issue_occlusion_query`) проверяет секцию по глубине, которую рисуют сами секции. Результат в кадре N переворачивался в кадре N+1, поэтому при неподвижной камере набор отрисованных секций и треугольников менялся каждый кадр (мерцание «треугольников» у земли и листвы). Баг существовал и до этого, гистерезис по кадрам проблему полностью не убрал.
- На момент того изменения `Renderer::m_occlusion_enabled = false`: legacy GL query path был отключён из-за мерцания. В текущем GPU-driven рендерере frustum выполняется на GPU, а temporal Hi-Z доступен отдельно и инвалидируется при движении камеры/изменении мешей.
- Сборка: `Hash_Utils.h` подключает `<functional>` (без него не собиралось на чистом Linux).

## Скорость генерации мира (2026-10-12)

Замер в одной сессии (`gen_bench`, однопоточно, 612 чанков, одинаковая выборка): было **14,9–15,1 мс**
на чанк, стало **5,5–6,9 мс** (разброс из-за загрузки машины). Что сделано:

- **Плотное поле пещер на чанк.** Тоннели считаются в узлах решётки шага 2 (массив 17³ на канал),
  залы — шага 4 (9³). Блоки чанка берут значения из этих массивов без проверок окна. Декорации и
  связность пещер по-прежнему идут через ленивое окно (`lattice_sum`): значение узла — чистая функция
  его координат, поэтому чанк и декорации видят одну и ту же пещеру бит-в-бит.
- **Пропуск невозможных ячеек.** Для каждой ячейки решётки хранятся min и max по её углам.
  Трилинейная интерполяция не выходит за эти пределы, поэтому если тоннель или зал в ячейке заведомо
  невозможен, сумма не считается. Результат тот же.
- **Кэш колонок 4-way.** Декорации (48×48) и сам чанк (32×32) при прямом отображении вытесняли друг
  друга, и `build_column` запускался в разы чаще, чем нужно.
- **`Fast_Rng` (splitmix64) вместо `std::mt19937` на колонку декораций.** Засев mt19937 стоит
  несколько тысяч инструкций, а колонок декораций в чанке тысячи.

Что изменилось в мире (это не только скорость):

- **Пещеры** теперь интерполируются с решётки (тоннели — шаг 2, залы — шаг 4) вместо точного шума в каждом
  вокселе. Замер на 300 000 точках: среднее отклонение поля 0,004, максимум 0,22; около 0,34 % точек меняют
  решение «пещера / не пещера»; объём пещер +2 %. Форма ходов та же, границы чуть сдвинуты.
- **Декорации** (деревья, цветы, морковь) ставятся другим генератором: статистика та же, конкретные позиции
  другие. Старые миры это не сохраняют — так и договорились.
- **Руда не меняется**: шум руды по-прежнему считается в каждом подземном вокселе. Это самая дорогая
  оставшаяся часть генерации (около пятой части времени).

## GPU-мешер секций (2026-10-12)

Мешинг полностью переехал на видеокарту; CPU-мешера нет. На CPU остаются упаковка блоков и света, сборка полезной нагрузки и связность секций для отсечения по окклюзии. Нужен OpenGL 4.3 (compute-шейдеры), откат на 3.3 удалён (`Window.cpp`).

- **Что уходит на GPU.** На каждый чанк отправляется полезная нагрузка из 16 каналов: блоки, meta, свет и блоки со светом с шести граней соседних чанков, а также кольцо рёбер и углов для AO (канал 15; `Chunk.cpp`, `build_gpu_solid_payload`). Из неё квады строятся на GPU. Раньше на видеокарту уходило 44 байта на вершину, шесть вершин на квад.
- **Compute-шейдеры** (`assets/shaders/mesh_greedy.comp`, `mesh_misc.comp`, `gpu_cull_commands.comp`). Слияние идёт по тем же правилам, что в `build_chunk_mesh`: грани сливаются, если совпадают блок, meta, свет и поворот. Квады пишутся в GPU-пул. `gpu_cull_commands.comp` выполняет GPU frustum-culling секций, при стабильной камере применяет предыдущую Hi-Z карту и создаёт indirect-команды для всех видов геометрии.
- **Рисование**: при наличии `GL_EXT_mesh_shader` или `GL_NV_mesh_shader` используются Task/Mesh Shader и соответствующий indirect mesh-task вызов. Иначе сохраняется `glMultiDrawArraysIndirect` + vertex pulling (`terrain_gpu_vertex.glsl`).
- **Пул.** Единый пул квадов с блочным аллокатором (блок = 32 квада). У чанка 16 потоков (вид геометрии × 4 секции). При переполнении точное число квадов читается обратно, выделение растёт, чанк перемешивается; CPU-отката нет.
- **Замер** (проверочная программа вне репозитория, 160 чанков стартовой сцены, сид 12345). Данные для видеокарты при перестроении меша: 47,1 МБ у CPU-пути против 2,3 МБ полезной нагрузки, то есть примерно в 20 раз меньше. Подготовка меша на CPU в одном потоке: 210–285 мс против 90–115 мс на GPU-пути, примерно в 2,3 раза (замер шумный). При капе 512 на CPU уходило 53 из 160 чанков, при 2048 — 4 (2,5 %).
- **Проверка.** Кадр при капе 512 совпал с CPU-кадром пиксель в пиксель (0 из 518 400), хотя 53 чанка уходили на CPU-откат, так что откат тоже проверен. При капе 2048 отличается один пиксель: вероятно, на ребре, где грани стыкуются и глубина совпадает точно, а порядок отрисовки решает, какая грань видна (предположение, не доказано). Позиции, UV и нормали совпали во всех 624 сравнённых секциях. Свет отличается в 37 секциях. Одна секция отличается числом квадов (400 против 398), скорее всего из-за света (см. ниже).
- **Не измерено: время кадра на видеокарте.** Здесь OpenGL работает через llvmpipe, программную реализацию на процессоре, поэтому выигрыш в рисовании и мешинге на видеокарте не проверен. Цифры выше — объём данных и время CPU.
- **Устаревший свет (есть и без GPU-пути).** Исправлено 2026-10-13, см. раздел «Вершинный AO и устаревший свет (2026-10-13)» ниже. До исправления на тестовой выборке 52 из 640 секций отличались от свежего пересчёта только светом, и это не проходило даже после долгого ожидания.
- **Отладка.** `Renderer::debug_gpu_section_quads` читает квады секции из пула. Нужна только проверочным программам, в игре не вызывается.

## Вершинный AO и устаревший свет (2026-10-13)

Вершинный ambient occlusion (AO) и исправление устаревшего света на стыках чанков. CPU- и GPU-пути используют одни и те же правила.

**AO.** Каждый угол грани получает уровень 0..3 (0 — самый тёмный). Для угла берутся две боковые клетки и диагональная клетка в воздушном слое перед гранью: `level = (s1 && s2) ? 0 : 3 - (s1 + s2 + s3)`. Занятой считается клетка с непрозрачным блоком, который видит мешер. Шейдер умножает цвет на `mix(0.45, 1.0, AO / 3)`.

- **CPU** (`Chunk.cpp`, `face_ao`, `flip_quad_diagonal`). AO попадает в вершину (`Terrain_Vertex::ao`, атрибут 5) и в ключ слияния (биты 24..31), поэтому квады с разным AO не склеиваются. Диагональ квада переворачивается по правилу `a0 + a3 > a1 + a2`, иначе на плоскости появляются полоски (`kCorner`, `kCornerFlip`).
- **GPU** (`mesh_greedy.comp`, `terrain_gpu_vertex.glsl`). Те же правила. AO упаковано по 2 бита на угол в старшем байте второго слова квадовой записи (`w1`), размер записи не меняется.
- **Кольцо соседей.** AO читает диагональных соседей, поэтому у снимка меша есть кольцо из 392 клеток рёбер и углов (`Chunk_Ring`, `chunk_ring_index`). Клетки у рёбер и углов берутся только из кольца, а не из граней соседа: иначе клетка диагонального чанка подменялась бы чужой. Если диагонального соседа не было при постановке задачи, клетку отдаёт генератор.

**Устаревший свет.** Граничные грани соседа читают свет через границу, но при смене света соседи не помечались грязными, и стык держал старое значение. Теперь:

- `recompute_lighting` и `recompute_sky_lighting` помечают шесть соседей тех чанков, где свет изменился (`mark_light_neighbors_dirty`). Пометка консервативная: свет, который после пересчёта совпал со старым, тоже приводит к пересборке соседей;
- новый чанк помечает все 26 соседей, потому что появление чанка на ребре или в углу меняет AO соседей;
- правка блока ставит в очередь сам чанк и тех соседей, в чьей окрестности лежит клетка: у границы это грани, у ребра и угла ещё и AO-кольцо;
- прямые сеттеры `set_block` и `set_meta` помечают чанк через `mark_mesh_dirty()`, который увеличивает эпоху грязности. Раньше они её обходили.

**Гонки асинхронной пересборки.** Найдены при разборе AO и исправлены:

- пометка, пришедшая после снимка меша, терялась при применении результата. Теперь снимок запоминает эпоху грязности, и её рост после снимка ставит повторную задачу;
- результат задачи для выгруженного чанка мог лечь на новый чанк той же позиции (его выгрузили и загрузили заново) и оставить призрачный блок. Теперь у каждого объекта `Chunk` есть номер (`instance_id`), и результат применяется только к своему объекту. Чужой результат не трогает `pending` и `requeue` позиции: они принадлежат новой задаче;
- одна только позиция не различает старый и новый объект, потому что счётчик эпохи у каждого объекта свой и может совпасть по значению.

**Замер.** Проверочная программа вне репозитория: 425 чанков, сид 12345, CPU-путь, по три прогона на каждую сборку. Перед каждым прогоном мир сбрасывается, чтобы сохранённые правки прошлых прогонов не меняли стартовое состояние.

| Проверка | До (HEAD) | После |
|---|---|---|
| Расхождения меша со свежим пересчётом после правок (раунды A, B, C) | раунд B: 2 секции в каждом прогоне (1 отличается только светом, 1 — числом вершин) | 0 во всех прогонах |
| AO против эталона из мировых данных | не было | 0 расхождений из 666 372 вершин верхних граней и из 2 946 960 вершин всех граней |
| Квады по секциям (1086 секций с геометрией) | 306 136 | 491 160 (+60 %) |
| Секции больше квоты GPU-мешера 2048 | 4 | 7 |
| Пересборка 425 чанков с нуля, CPU | 0,63–0,67 с | 0,63–0,65 с (в пределах шума) |
| Оседание меша после загрузки | 0,52–0,53 с | 0,57 с |

Раунд C выгружает и заново загружает крайнюю колонку окна, пока по ней летят задачи. Призрачный блок на старой версии в нём не воспроизвёлся (0 из 3 прогонов), поэтому исправление гонки подтверждается разбором кода, а раунд C подтверждает только то, что новый путь не теряет правки.

**Цена AO по квадам.** AO разрывает слияния там, где значение AO меняется между соседними гранями, поэтому квадов становится примерно на 60 % больше. В этой выборке квота GPU-мешера 2048 переполняется в 7 секциях из 1086 (было 4). Переполнение переводит чанк на CPU-мешер до выгрузки, как и раньше, картинка при этом не меняется. Квоту можно поднять через `Renderer::kGpuQuadCap`: 4096 удваивает пул (с 16 МБ до 32 МБ), максимум в выборке сейчас 3444 квада на секцию.

**Не измерено и не настроено.** Время кадра на видеокарте (OpenGL здесь через llvmpipe). Визуально AO и затемнение (нижняя граница 0,45) не подбирались по картинке. Диагональ квада выбирается по правилу из таблицы, без подбора по картинке.

## Сторонние библиотеки (external/)

- **stb_image** — `external/stb/stb_image.h` — https://github.com/nothings/stb
- **glad** — `external/glad/` (сгенерирован под OpenGL 4.3) — https://glad.dav1d.de/
- **OpenSimplex2S** — `external/opensimplex/OpenSimplex2S.{c,h}` — https://github.com/KdotJPG/OpenSimplex2 — шум для генерации мира (C-версия, подключается через `#include "OpenSimplex2S.h"`)
- **cgltf** — `external/cgltf/cgltf.h` — https://github.com/jkuhlmann/cgltf — парсер glTF/GLB, используется для загрузки моделей мобов (геометрия, скелет, анимации, материалы)

## Интерфейс, атлас и локализация

- Интерфейс рисуется из атласа `ui_main` (`assets/texture/ui_atlas.png`). Источник правды ---
  сам PNG; скрипт `tools/gen_ui_atlas.py` удалён из репозитория (90aed5c), атлас правится напрямую.
- Раскладки инвентаря, сундука, верстака и печи --- в `src/hud/layouts/Panel_Layouts.*`, а не в `Game.cpp`.
- Тексты UI --- в `assets/lang/en.json` и `ru.json`, язык переключается в Options.
  Шрифт поддерживает ASCII и русскую кириллицу.

Подробности GPU-driven culling, temporal Hi-Z и границ CPU/GPU приведены в `documentation/RENDERING_PIPELINE.md`.
