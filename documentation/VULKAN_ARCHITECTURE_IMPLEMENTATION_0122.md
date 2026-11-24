# OptiCraft 0.12.2 — Vulkan backend: implementation stage 1

## Статус

**Сделано:** исходный OpenGL-проект сохранён без замены Game/Window/Renderer. Добавлен самостоятельный C++20 `OptiCraft_VulkanCore` и отдельная Vulkan compute-библиотека `OptiCraft_VulkanBackend`. Это **не готовый игровой Vulkan-рендер**: Game пока запускает OpenGL.

**Протестировано:** `tools/tests/run_vulkan_core_checks.sh` — аллокатор диапазонов, отложенное освобождение, стабильные handles, переход CPU/GPU ownership, упаковка ID блоков, потокобезопасная пакетная очередь, ограничения радиусов. Vulkan SDK, GLFW и glslang отсутствуют в тестовой среде; компиляция Vulkan-файлов, SPIR-V и запуск на GPU здесь не выполнялись.

**Осталось:** подключение backend к игровому Window/Game, swapchain/present, полные материалы и атлас, GPU greedy/shaped/flora/liquid meshing, culling, LOD, перенос текущего World_Generator с проверкой идентичности, полноценный Chunk Residency Manager, асинхронные readback/save, динамическое освещение и тесты Vulkan на Intel/RTX.

## Реализованное устройство

- `src/rendering/vulkan/GpuWorldCore.{h,cpp}` — независимая от графического API модель резидентных чанков, арены под блоки и геометрию, stable handle `(slot,generation)`, GPU timeline retirement, coalesced GPU job batching. Producers передают `span<GpuWork>` под одним mutex; consumer забирает ограниченный пакет только при наличии задач.
- `src/rendering/vulkan/VulkanGpuMesher.{h,cpp}` — Vulkan 1.2 compute-диспетчер: Count и Emit, общий SSBO descriptor set, `vkCmdDispatch(512, N, 1)` для N чанков, один `vkCmdFillBuffer` на батч, корректные compute/transfer/indirect/vertex memory barriers. Количество `N` ограничено размером буферов и лимитами физического устройства, проверяемыми однократно при инициализации.
- `assets/vulkan_shaders/voxel_count.comp`, `voxel_emit.comp`, `voxel_finalize.comp`, `mesher_common.glsl` — получение видимых граней на GPU из полных 32³ блоков, с учётом данных непосредственно соседних резидентных чанков. Включены точный подсчёт/выделение, ограничение записи при превышении буфера, сборка GPU indirect draw для `VkDrawIndirectCommand`.
- `assets/vulkan_shaders/voxel_debug.vert/.frag` — диагностический vertex pulling путь для визуальной проверки результатов compute. Внимание: это **не** окончательная система освещения, текстур и материалов.
- `VulkanParallelDraw.{h,cpp}` — параллельная запись secondary command buffers через существующий `Render_Worker_Pool`, отдельный VkCommandPool на (worker, frame), пакетные `vkCmdDrawIndirect` и один `vkCmdExecuteCommands`. Требуется, чтобы вызывающий рендерер дождался завершения GPU-работы соответствующего frame-slot перед `vkResetCommandPool`.
- `ChunkGpuExport.{h,cpp}` — преобразование CPU-authoritative `Chunk::get_blocks().unpack()` в 2×16-битные ID на слово и таблицу свойств простых непрозрачных кубов. CPU не строит грани, вершины, индексы или LOD.

## Формат

- Один оригинальный чанк — **32×32×32** блоков (реальные `Config::chunk_size/chunk_height`, не 16³).
- Текущий плотный GPU-формат — **65 536 байт** на чанк, 16-битные `Block_ID` без потерь. Это промежуточный формат: палитры и разреженные секции ещё не перенесены на GPU. Метаданные и свет хранятся отдельными каналами в старом движке; новая Vulkan-система их пока не импортирует.
- `GpuChunkSlot` — 64 байта, `source={blockBaseWords, faceBaseRecords, faceCapacityRecords, flags}`, `origin` и 6 смещений соседних исходных данных (или -1, когда сосед не резидентен).
- Packed face — 8 байт (`uvec2`). Локальные x/y/z по 5 бит; face 3 бита; материал 16 бит; стабильный GPU-слот 30 бит.
- Счётчики Count/Emit относятся к позициям **батча**; `faces[].y` и `draws[]` — к стабильному GPU-слоту. Индексы батча передаются отдельным `slotIndices[]`; перемешивание очереди не меняет принадлежность старой геометрии.

## Последовательность работы compute (необходимо реализовать в вызывающем Vulkan frame graph)

1. CPU/GPU генератор поставляет полный массив исходных блоков. Данные живут в постоянном `Block Arena`. CPU импортирует только свои ближние чанки; GPU-worldgen для дальних чанков **ещё не перенесён**.
2. При изменении: загрузить актуальный `GpuChunkSlot`, соседские смещения, flags и пакет `slotIndices`. Нужны Vulkan transfer-to-compute барьеры; соседние секции при изменениях у края также помечаются dirty.
3. `recordCount` — один запуск на пакет; затем `vkCmdCopyBuffer` из per-job counters в readback staging; отправить в очередь с timeline signal. Проверять timeline только при наличии незавершённых пакетов, без ожидания в игровом цикле.
4. После завершённого readback выделить **точные диапазоны** из Geometry Arena с помощью `RangeArena`, записать их в `GpuChunkSlot`. Не изменять исходный массив блоков, пока Emit использует данную версию.
5. `recordEmit` — одна/несколько GPU dispatch на весь пакет, затем Finalize пишет массив `VkDrawIndirectCommand`. Overflow (`count > capacity`) формирует `instanceCount=0`, но **публикацию такой версии нужно отклонить**, сохранив предыдущий меш. Часть реализации copy-on-write/publish в вызывающем frame graph **не завершена**.
6. Когда timeline подтверждает завершение, опубликовать новое поколение и отложить освобождение старого через `ChunkRegistry::publishMesh/collect`. Многопоточное рисование выполняется через VulkanParallelDraw на независимых secondary command buffers.
7. Устаревшие задачи фильтровать по `(handle,generation)` и `version`. Блоки остаются на GPU для будущего ремешинга и LOD.

## Принципиальные ограничения этапа 1

1. **GPU-only meshing:** реализованы только видимые грани непрозрачных полных кубов (без greedy-слияния). Другие типы блоков нельзя выпускать через этот pipeline без соответствующих compute шейдеров. Не включать этот backend вместо OpenGL в основном исполняемом файле, пока не реализовано полное покрытие форм.
2. **GPU Worldgen:** старый OpenGL `Gpu_Worldgen.cpp` генерирует только часть подземных материалов. В исходном `World_Generator` есть региональные речные и пещерные системы, структуры и климат. Для дальних GPU-чанков нужен *точный* перенос логики и CPU/GPU parity-тесты, включая пещеры под землёй. Генератор только поверхности либо другая случайная генерация разрушила бы идентичность мира, поэтому такого фиктивного fallback в этом этапе **нет**.
3. **Две дальности:** `GraphicsDistance` хранит и нормализует `renderChunks` и `actualChunks`; UI и самостоятельные GPU запросы чанков пока не подключены. Семантика: `actual` = полноценное CPU world-state/save, `render` = верхняя граница видимости GPU+CPU.
4. **Сохранения:** GPU-only чанки могут быть потеряны при краше, CPU-authoritative чанки должны обеспечиваться обычной гарантией фактически завершённых save. `GpuWorldCore` содержит состояния Promotion/Eviction, но сам процесс асинхронного чтения GPU и очередь записи пока не присоединены.
5. **Графика:** `VulkanParallelDraw` готов записывать командные буферы после создания VkRenderPass/VkFramebuffer/VkPipeline/VkPipelineLayout; эти объекты и swapchain нужно создать на следующем этапе. `recordEmit` строит команды, но пока они не передаются игровому рендереру.
6. **Потоки:** Vulkan permits concurrent *recording* в разных `VkCommandPool`; доступ к одному `VkQueue` и изменяемому descriptor set всё равно синхронизируется владельцем. Никаких `vkDeviceWaitIdle()` при потоковой генерации/мешинге.

## Сборка

Пакеты на Linux: Vulkan SDK headers+loader (`libvulkan-dev`), `glslangValidator` (`glslang-tools`), а также существующие OpenGL/GLFW/GLM зависимости игры.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OptiCraft_VulkanCore OptiCraft_VulkanBackend OptiCraft_Vulkan_Shaders OptiCraft_Vulkan_Core_Tests -j 8
ctest --test-dir build -R VulkanWorldCore --output-on-failure
# Без Vulkan SDK для независимых CPU-тестов:
tools/tests/run_vulkan_core_checks.sh
```

Если Vulkan SDK не установлен, `OptiCraft_VulkanBackend` не создаётся (без поломки исходной OpenGL-сборки). При отсутствии `glslangValidator` SPIR-V не собирается и выводится предупреждение.

Версия проекта сохраняется **0.12.2**. Это отдельный инженерный этап, не новый релиз.

## Vulkan device requirements / memory limits

- Для графического пути с ненулевым `VkDrawIndirectCommand.firstInstance` необходимо включить `VkPhysicalDeviceFeatures.drawIndirectFirstInstance` при создании устройства. При его отсутствии потребуется отдельный fallback: сейчас этот случай **не реализован**.
- `maxStorageBufferRange` ограничивает **размер диапазона SSBO-дескриптора**, даже если весь VkBuffer больше. `VulkanGpuMesher::initialize` отвергает превышение вместо неопределённого поведения. Для десятков тысяч чанков будет нужен сегментированный Block/Geometry Arena (descriptor array / buffer device address) и GPU таблица адресов.
- Count и Emit требуют `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`; counters дополнительно `TRANSFER_SRC|TRANSFER_DST`, draw commands дополнительно `INDIRECT_BUFFER`, и для staging readback нужны `TRANSFER_DST`, видимость и правильные queue-family transitions.
- VulkanParallelDraw не делает `vkQueueSubmit` из воркеров. Запись вторичных команд — параллельная, отправка и управление GPU timeline — централизованные. Это исключает ненужные сабмиты и гонки на VkQueue.
