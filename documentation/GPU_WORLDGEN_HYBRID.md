# OptiCraft — гибридная GPU-генерация (экспериментальный этап 1)

## Зачем и что перенесено

Движок уже умеет готовить X/Z-данные колонок и вычислять региональную сеть пещер и рек в CPU-worker'ах. Перенос всего `World_Generator` в GLSL сейчас нарушил бы детерминизм: гидрология (`River_Network`) и региональные пещеры (`Cave_Network`) — это не независимые 3D-шумы. Поэтому реализован **реальный GPU compute backend для подземных материалов (руда, камень, глубинные слои)** с последующим CPU-финализатором.

1. Дисковая загрузка всегда выполняется первой и имеет приоритет над процедурной генерацией.
2. `Chunk_Manager` готовит общие климатические/поверхностные данные, реки, структуры и карту пещер один раз на колонку, как и раньше.
3. Дальние запросы (не ближе 8 чанков от игрока) при наличии свободного места в GPU-очереди идут в `Gpu_Worldgen`.
4. На **главном GL-потоке** выполняется `assets/shaders/worldgen_material.comp`, вычисляющий OpenSimplex2S FP64 и тип материала по тем же порогам руд, что и CPU. Одна отправка ограничена **двумя** Y-чанками, чтобы не перегрузить Intel UHD. Другие уровни имеют обычный CPU fallback.
5. GPU устанавливает fence; CPU проверяет его с нулевым timeout каждый игровой кадр, без `glFinish` и ожидания GPU в worker'ах. После получения результата специальные значения `0` обрабатываются обычным CPU генератором, остальные значения переиспользуются.
6. CPU добавляет пещеры, реки, поверхность и структуры, упаковывает блоки, делает освещение и передаёт результат существующей очереди сохранений. Формат чанков и save-файлов **не меняется**.

Это **не** полная генерация классического мира целиком на видеокарте: CPU-стадия подготовки колонки и заполнение остальных уровней пока остались. Такой этап нужен, чтобы измерить отдачу от FP64 OpenSimplex2S на интегрированной видеокарте, прежде чем переносить климат/пещеры/реки.

## Режимы

Управляются переменной окружения при запуске OptiCraft:

| Переменная | Эффект |
| --- | --- |
| `OPTICRAFT_GPU_WORLDGEN=off` | Исходный CPU-путь для A/B-сравнения |
| *(не задана)* | Автоматический GPU/CPU, с ограничением очереди и защитой FPS |
| `OPTICRAFT_GPU_WORLDGEN=force` | Принудительно использовать GPU при наличии OpenGL 4.3 и успешном шейдере; не отключать автоматически из-за медленных пакетов |

Примеры из терминала (из корня проекта):

```sh
OPTICRAFT_GPU_WORLDGEN=off ./cmake-build-release/OptiCraft
OPTICRAFT_GPU_WORLDGEN=force ./cmake-build-release/OptiCraft
```

Для CLion: Run → Edit Configurations → Environment variables. Не путайте с CMake-опциями: переменная проверяется **при запуске игры**.

Режим auto выключает GPU-путь, если два подряд пакета заняли >2,5 мс по `GL_TIME_ELAPSED`. Если fence не сработал 500 мс, активная колонка возвращается CPU-воркерам и GPU выключается. Ошибки компиляции шейдера, OpenGL <4.3 и несовпадения материала с CPU-выборкой тоже переключают на CPU. Очередь ограничена двумя колонками (включая выполняемую).

## Что смотреть в логах

- `GPU worldgen: FP64 OpenSimplex2S material backend initialized` — GLSL удалось загрузить и подготовить.
- `GPU worldgen materials: ... GPU avg ... ms per batch` — прошли реальные GPU-пакеты. Логируется первый и затем каждый 32-й пакет.
- `GPU worldgen: batch exceeds 2.5 ms` — автоотключение ради плавности.
- `GPU worldgen: material parity check failed` — вместо GPU использован CPU, сохранения не повреждены.

**Проверка производительности:** на одном и том же сиде и маршруте полёта сравнивать `off` и `force` в **Release**. Измерять FPS, 1% low, количество завершённых колонок/с, время колонки (Tracy) и GPU-время пакетов. В `Debug` сравнение CPU/GPU может сильно исказиться.

## Тесты

```sh
python3 tools/tests/test_worldgen_fastpaths.py
python3 tools/tests/test_gpu_worldgen_parity.py
GPU_WORLDGEN_FULL=1 python3 tools/tests/test_gpu_worldgen_parity.py
```

GPU-тесту нужны OpenGL через EGL/GL и установленный C-компилятор. Он создаёт временную библиотеку **из настоящего** `external/opensimplex/OpenSimplex2S.c`, передаёт таблицы на GLSL, и сравнивает результат при нескольких сидах и X/Z/Y, включая отрицательные координаты. FULL проверяет 4 × 4096 случайных вокселей из 4 полных GPU-чанков. Это проверка математического ядра, **не** замена игрового теста полностью собранного движка.

## Расширение до следующего этапа

Когда появятся измерения на Intel UHD/RTX 4060, можно расширять backend до GPU-генерации полных воксельных массивов, передавая финализатору также подготовленные региональные маски пещер, рек и поверхностные материалы. Более радикальное перенесение климатических полей и 2D OpenSimplex требует детерминированного GPU/CPU сравнения. Время мешинга и плавность кадра важнее абсолютной пропускной способности генератора.


## Vulkan 0.13 — Stage32

Vulkan-путь использует тот же CPU-финализатор, но выводит два 16-bit material ID на uint в x-major порядке. `Gpu_Material_Result` сохраняет только отправленные Y-уровни (максимум два), без разворачивания всей колонки. Ноль по-прежнему означает CPU fallback; это не готовый блок воздуха. Legacy GL шейдер не изменён. `OPTICRAFT_GPU_WORLDGEN=off` сохраняет CPU-путь. Полноценный GPU-only worldgen и прямой вывод в резидентную арену ещё не реализованы.

Дополнительные проверки (адаптация Vulkan GLSL к EGL, не Vulkan validation):

```sh
GPU_WORLDGEN_VULKAN_PACKED=1 GPU_WORLDGEN_FULL=1 python3 tools/tests/test_gpu_worldgen_parity.py
python3 tools/tests/test_vulkan_stage32_worldgen.py
g++ -std=c++20 -fsanitize=undefined -I src tools/tests/test_gpu_material_result.cpp -o /tmp/oc-material-test && /tmp/oc-material-test
```

## Vulkan 0.13 — Stage36

Base mode: региональный заголовок содержит `water | (floor_mantle << 16)`. GPU возвращает мантию на нижней границе до шума и масок. Validation worker полностью проверяет слои вне мира, нижний слой и sky markers; для остальных слоёв проверяет ID и исправляет численный fallback непосредственно в упакованном массиве, затем выполняет CPU parity samples. Обычные принятые слои больше не содержат 0xffff. CPU Noise build пропускает нижний слой до финального заполнения в publish(). Сохраняется `OPTICRAFT_VULKAN_BASE_WORLDGEN=0` для material-only режима. Небесные острова, структуры, освещение и непосредственная GPU-резидентность пока не перенесены.

Проверки: `GPU_WORLDGEN_VULKAN_BASE=1 python3 tools/tests/test_gpu_worldgen_parity.py`, `python3 tools/tests/test_vulkan_stage33_base_worldgen.py`, `python3 tools/tests/test_vulkan_stage34_worldgen_handoff.py`.

## Vulkan 0.13 — Stage37

Декорации и деревни формируют `Generation_Overlay` с локальным индексом 15 bit (y,z,x), признаком meta 1 bit и значением 16 bit. Пакет содержит операции в исходном порядке, включая очистку Air и meta=0. CPU replay подключён к `Chunk::finish_generation` и предшествует освещению/сохранению. Готовый формат пригоден для будущего GPU overlay; параллельный replay обязан сохранять порядок на одном вокселе и избегать гонок при записи соседних 16-bit IDs / 8-bit meta в общие 32-bit words. Shader dispatch и прямой resident output ещё не реализованы.

Проверки: `python3 tools/tests/test_vulkan_stage37_overlay.py` (production replay с mock generator/registry, UBSan, 524288 сравнений блоков и метаданных), Stage34/35, CPU fastpaths.

## Vulkan 0.13 — Stage38

`Generation_Overlay_Packet` стабильно группирует команды по 4 соседним вокселям (два ID words + одно meta word). Каждый `worldgen_overlay.comp` invocation эксклюзивно владеет своими словами, сохраняет порядок и выдаёт logical edit count. CMake включает shader в SPIR-V target, но Vulkan runtime его пока не dispatch'ит: вход обязан быть полностью resolved. Dense CPU finalization >=4096 commands использует тот же reducer и однократное построение палитры; sparse path сохранён. Порог не бенчмаркирован. Проверки: `python3 tools/tests/test_vulkan_stage38_overlay_packet.py` (EGL + UBSan), `python3 tools/tests/test_vulkan_stage37_overlay.py`. GPU lighting и прямая resident publication не реализованы.

## Vulkan 0.13 — Stage39

**Сделано:** `Gpu_Overlay_Work` формирует ограниченный пакет для одного принятого Vulkan base-уровня на колонку (максимум 131072 команд и 8192 на владельца). Уровни с 0xffff и пустые пакеты идут через CPU. Общий `collect_generation_overlay` сохраняет порядок деревья/растения → деревни. Validation worker повторно ставит батч в существующую GPU очередь с тем же admission credit. `VulkanMaterialWorldgen::start_overlay` лениво создаёт пять HOST_VISIBLE SSBO (736 КиБ payload), отдельный pipeline/descriptor set, переиспользует command buffer и fence. Dispatch по активным владельцам, compute→host barrier, readback blocks/meta/edit counts после fence. Worker проверяет полный результат точным reducer, включая untouched groups, и передаёт его только соответствующему build job. `Chunk::adopt_generation_overlay` восстанавливает палитру, метаданные и версии без повторного декорирования; CPU skylight/save остаются штатными. При ошибке или смене backend CPU replay использует принятую базу. По умолчанию включено, `OPTICRAFT_VULKAN_WORLDGEN_OVERLAY=0` возвращает Stage38.

**Протестировано:** Stage39 extracted Vulkan runtime mock под UBSan: descriptors, persistent buffers, dispatch, fence/readback, submit failure PASS; Stage35 extended queue mock: кредит, adoption, corruption/submit/sky fallback PASS; Stage37 extended production adoption 262144 block/meta comparisons, layer/heightmap/version parity, corrupted blocks/meta/count and length rejection PASS. Stage38 EGL shader regression 262144 comparisons, Stage37 CPU regression 524288 comparisons, Stage34 131072 voxels, Stage33 host adoption, 130277 CPU fastpaths PASS. Release CMake blocked by OPENGL_opengl_LIBRARY, OPENGL_glx_LIBRARY, OPENGL_INCLUDE_DIR. Native Vulkan/SPIR-V and full game launch not tested.

**Осталось:** переходный проверочный путь добавляет вторую GPU submission, upload/readback и полное CPU сравнение; ускорение не заявляется, возможна регрессия. Максимум один уровень на колонку, существующий бюджет base worldgen действует. Планирование структур, освещение, resident arena output и GPU-only chunks остаются CPU/не реализованы. Нужны аппаратные проверки, после них устранение дополнительного обмена и parity replay в production.
