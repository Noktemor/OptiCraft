# OptiCraft

OptiCraft — учебный C++-проект воксельного игрового движка в духе Minecraft. Проект использует OpenGL, потоковую загрузку чанков, процедурную генерацию мира и GPU-driven рендеринг.

## Быстрый старт

Нужны CMake 3.16+, компилятор C++20, OpenGL и GLFW; на Linux также X11 и Threads. Зависимости и опции описаны в [технической документации](documentation/TECHNICAL_DOCUMENTATION.md).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
cd build && ./OptiCraft
```

Ресурсы копируются в каталог сборки. Для переносимого исполняемого файла задайте `-DOPTICRAFT_NATIVE_ARCH=OFF`. Настройки игры сохраняются в `options.txt` относительно рабочего каталога. При включённой опции `OPTICRAFT_VULKAN_CORE_TESTS` CTest регистрирует независимые тесты Vulkan World Core.

### Vulkan 0.13 (экспериментальный бэкенд)

Основная цель `OptiCraft` в CLion автоматически пытается создать окно Vulkan (`GLFW_NO_API`), если сборка включает Vulkan SDK и SPIR-V shaders (`glslangValidator`). При успешной инициализации показывается только Vulkan-окно с миром и HUD; при ошибке используется OpenGL. Скрытый OpenGL-контекст пока необходим для старых ресурсов и резервного пути. Это экспериментальный гибрид: нативные проходы мобов, боссов и предметов подключены, но аппаратно ещё не подтверждены; ряд эффектов не перенесён. При >=256 GPU-резидентах на поддерживаемом устройстве автоматически включается same-frame Hi-Z для дальних непрозрачных чанков; `OPTICRAFT_VULKAN_HIZ=0` отключает только этот проход. Для обычной полной игры принудительно используйте `OPTICRAFT_RENDERER=opengl`. Для проверки Intel iGPU: `OPTICRAFT_VULKAN_GPU=integrated`, для дискретной карты: `OPTICRAFT_VULKAN_GPU=discrete`; диагностика `OPTICRAFT_VULKAN_VALIDATION=1` (нужны validation layers). Отдельная цель `OptiCraft_Vulkan` остаётся тестом swapchain, не игрой. Техническое состояние: [документация Vulkan](documentation/VULKAN_ARCHITECTURE_IMPLEMENTATION_013.md).

Настройка сложности доступна внутри мира и влияет на лимит враждебных мобов ночью. Мирные и враждебные мобы появляются в обоих режимах; удалённые обычные мобы исчезают без добычи. Подробности — в [разделе о мобах](documentation/TECHNICAL_DOCUMENTATION.md#47-мобы).

Документация: [архитектура и сборка](documentation/TECHNICAL_DOCUMENTATION.md), [рендеринг](documentation/RENDERING_PIPELINE.md), [профилирование](documentation/PROFILING.md), [дорожная карта](documentation/ROADMAP.md), [история изменений](documentation/history_version.md). Исторические замеры и планы описывают состояние на момент их создания.

Альтернативный вариант, для удобной компиляции под Windows и Linux

https://github.com/Noktemor/OptiCraft_Installer

инструкция по использования внутри репозитория

## Версия 0.13 — Vulkan render (в разработке)

В разработке: основной Vulkan-путь для GPU-мешинга, GPU-culling, текстурированных граней и пакетного HUD. При ошибке Vulkan игра переходит на OpenGL. Переход не завершён: сохраняется скрытый GL-контекст, а дальний GPU-worldgen, LOD, сохранение GPU-only чанков и некоторые графические проходы не перенесены. Аппаратная проверка Vulkan ещё требуется.

Подробности: [история обновлений](documentation/history_version.md) и [технический статус Vulkan 0.13](documentation/VULKAN_ARCHITECTURE_IMPLEMENTATION_013.md).

## Версия 0.12.1 — расширенный Nether, огонь и навигация

Nether теперь генерируется на высоте Y=0…512 с крышей на Y=512; выше разрешено
строительство вплоть до общего потолка Y=1024. Добавлена самостоятельная зажигалка
`Lighter`, временный огонь с небольшим распространением, сгоранием древесины и уроном.
Переходы Nether/Overworld используют сохранённые привязки порталов вместо строительства
новой рамки при каждом возвращении. Из End игрок возвращается на спавн Overworld.
Подробнее: `documentation/history_version.md` (раздел 0.12.1).

**Статус:** порталы — бета; сохраните резервную копию мира перед проверкой.

Добавлена асинхронная навигация мобов: наземный A*, подготовка пути для 3D-полёта,
автоматический пересчёт при препятствиях и ограничение нагрузки на кадр.
Подробности: [навигация мобов](documentation/MOB_NAVIGATION_0121.md).

## Версия 0.12 — миры, порталы, косметика и устойчивость потоковой загрузки

Добавлены независимые измерения **Nether** и **End**, порталы между ними и обычным миром,
сохранение состояний по измерениям и регистрация дополнительных миров через `assets/dimensions.json`.
Новые порталы сейчас находятся в **бета-тестировании**: стабильность переходов и работы под
большой нагрузкой пока не достигнута. Перед тестами необходимо создать резервную копию сохранения.

Также обновлены меши и иконки предметов, текстуры лавы и воды, а обработка света/жидкостей
и потоковая загрузка чанков получили ряд исправлений. На больших дистанциях обновление 0.12
ограничивает далёкие вертикальные слои, чтобы не накапливать десятки тысяч резидентных чанков.
На экране F3 появился показатель потребления RAM процессом (Linux).

**Подробности:** [История версий](documentation/history_version.md).

### Известные ограничения 0.12

- **Порталы — бета:** при переходах Nether/End ещё возможны зависания или ошибки загрузки.
- При очень большой дальности прорисовки на удалённых колонках количество слоёв по высоте
  ограничено; возможен поздний показ отдельных высоких/низких участков рельефа.
- Смена Creative на Survival на большой высоте запускает падение и догрузку мира.
  Пока чанки под игроком не готовы, падение временно приостанавливается, чтобы не провалиться сквозь мир.
- Полную сборку и стресс-тест на целевом оборудовании необходимо выполнить отдельно.


### Vulkan render 0.13 — Stage 8 (development)

The main `OptiCraft` target attempts a Vulkan window by default when built with the Vulkan backend and SPIR-V. It presents the world and batched HUD in one visible window; OpenGL is the fallback. A hidden GL context still owns legacy resources. Entity rendering, distant GPU generation, geometric LOD and GPU-only saves remain unfinished. Use `OPTICRAFT_RENDERER=opengl` for the complete legacy game. The standalone `OptiCraft_Vulkan` target is still a swapchain probe. See `documentation/VULKAN_ARCHITECTURE_IMPLEMENTATION_013.md`.

### Vulkan 0.13 — Stage 14
Native FP64 material worldgen with CPU fallback, conservative misc Hi-Z, per-chunk alpha quad sorting, exact-surface LOD1, near misc AO and held-block light. Full terrain generation, coarse voxel LOD and dynamic shadows are not implemented. See `documentation/history_version.md` for validation limits.
