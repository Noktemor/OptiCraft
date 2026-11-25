# Статистика треугольников / GPU occlusion

## Сделано

- В HUD счётчик переименован из `Tris` в `Submitted tris`. Это треугольники, переданные рисующим командам после GPU-отсечения, **не** треугольники, видимые как пиксели после depth test.
- В режиме GPU Meshlet счётчик теперь суммируется из реально прошедших frustum/Hi-Z проверки мешлетов, а не из всех квадов крупных секций. Источником служит `gpu_cull_meshlets.comp`, отдельно для первого (frustum) и второго (post Hi-Z) проходов.
- Исключён двойной учёт solid/shaped/liquid мешлетов между `gpu_cull_meshlets.comp` и `gpu_cull_commands.comp`; flora остаётся в стандартном счётчике.
- Сброс GPU stats перенесён до meshlet-прохода, иначе результаты были бы затёрты перед чтением.
- В HUD выведены `Sections: drawn, frustum, Hi-Z` с раздельными причинами исключения секций.
- Добавлен `tools/tests/test_triangle_stats_contract.py` — исходные контракты и реальная компиляция обоих compute-шейдеров в headless OpenGL 4.3, если окружение поддерживает EGL.

## Почему при взгляде на близкую стену число может оставаться высоким

- GPU-фрейм-билдер формирует indirect draw commands на основе секций (32x32 в горизонтали) или мешлетов (32 квада в GPU Meshlet режиме).
- Если bounds примитива пересекают near-plane или содержат камеру, conservative Hi-Z не может надёжно объявить его невидимым.
- Даже когда в экране виден только один блок, draw-команда может содержать скрытые полигональные поверхности других блоков, которые отсеет позднее аппаратный depth test.
- Количество реально покрытых видимых пикселей и количество подготовленных треугольников — разные величины; OpenGL `GL_PRIMITIVES_GENERATED` также не измеряет окончательную pixel visibility.

## Протестировано

- `python tools/tests/test_triangle_stats_contract.py` — PASS, компиляция обоих compute-шейдеров в EGL/OpenGL 4.3.
- `python tools/tests/test_render_stability.py` — PASS.
- `python tools/tests/test_gpu_near_render_compat.py` — PASS.
- `python tools/tests/test_liquid_button_render_contracts.py` — PASS.
- `python tools/tests/test_worldgen_fastpaths.py` — PASS (130 277 сценариев).

## Осталось

- Сравнить Standard и GPU Meshlet на настоящей игровой сцене, желательно в Release.
- Проверить новую статистику при закрытом обзоре, в открытой пещере и над поверхностью. Оценивать также GPU frame time, а не только `Submitted tris`.
- Полная сборка в этой среде не выполнена из-за отсутствия системных заголовков GLM/GLFW; готовность проекта к запуску пока не подтверждена.
