# kimi-next — что это и из чего собрано

Срез состояния 0.2.6-dev, собранного и проверенного на машине автора (Windows 11 +
WSL Ubuntu, ноябрь 2026). Ветка = `fix/slow-open` (вершина `15ee71e`, голова PR #126)
плюс документация и скрипты воспроизведения. В `next`/`main` не сливалась.

## Что вошло

- База: `release/0.2.6-beta` (`f79acf8`).
- **`fix/slow-open`** (PR #126, `15ee71e`):
  регрессионные файлы в `tests/data/regression/`, headless-замер `bz_openbench`,
  OBB в булевых, бюджет времени булевых (сторож-поток), параллельные сборки
  пересечений, параллельная загрузка деталей сборок + настройки, строгие
  EdgeMatch/FaceMatch, откат `Clean()` при сломанном слиянии, fuzzy-допуски.
- Связанные ветки (в этот срез НЕ входят, для контекста):
  `fix/visual-gaps` уже влита в `next`; вердикт CI по PR #126 на момент среза
  ожидал автора.

## Замеры (bz_openbench, вискозиметр ВЗК.05, 3 прогона)

| | база 0.2.6-beta | fix/slow-open |
|---|---|---|
| Вискозиметр ВЗК.05 | 4523 / 5171 / 5421 мс | 1174 / 1188 / 1214 мс |

Итог ≈ 5,2 с → ≈ 1,2 с (~4,3×). Нативный Windows-прогон с реальным открытием
проекта: 0.2.6-dev — 5,8 с полного цикла (старт + открытие + 60 кадров + выход);
0.2.5.1 под тем же тестом не открыла за 120 с.

## Сборка на Windows (нативно, MSYS2 MinGW64)

Нужны: MSYS2 с `mingw-w64-x86_64-{gcc,cmake,ninja,glfw,freetype,binutils}`,
OCCT 8.0.1 — собственная сборка (`scripts/deps/build-occt.sh windows .deps/occt`),
далее `cmake --preset windows-mingw` с `OCCT_DIR=<...>/lib/cmake/opencascade`,
`cmake --build --preset windows-mingw --target breezecad bz_tests`.

**Грабли с не-ASCII путями** (пользовательская папка `C:\Users\Мишаня`):
- `-isystem <mingw>/include` от glfw3 ломает `#include_next <stdlib.h>` в imgui;
- `windres` не открывает файлы по путям с кириллицей (rc с иконкой не собирается);
- `ld` не находит `libTK*.dll.a` по кириллическому пути.
Обход: собирать из-под ASCII-джанкшна (`mklink /J D:\bc <репо>`) и тулчейном
из ASCII-путей. Скрипты: `scripts/` рядом с этим файлом.

## Сборка в WSL

Ворктри на `15ee71e`, `bash scripts/dev/check.sh` (linux-release):
сборка 7/7, тесты зелёные. Установка в префикс по мотивам
`scripts/package-linux.sh` — см. `scripts/install-breezecad-0.2.6-dev.sh`.

## Статус проверок

- WSL: полный прогон `bz_tests` — зелёный.
- Windows: exe собран и запускается (старт-чек с чистым PATH), открытие проектов
  проверено; полный `ctest` на машине автора уперся в таймаут 1200 с — кейсы
  идут, но медленно; итог не зафиксирован.
- Отличительный знак dev-сборок: лого перекрашено в зелёный только в установке
  (`#1D4ED8` → `#16A34A`, `#6E9BFF` → `#4ADE80` в brand-SVG), исходники чистые.

## Файлы

- `build-app-windows3.sh` — configure + сборка targets `breezecad bz_tests` + ctest.
- `install-windows-026.sh` — portable-установка (`deploy-runtime.sh`) + зелёное лого.
- `install-breezecad-0.2.6-dev.sh` — установка в WSL-префикс `~/.local`.
- `bench-viscometer-026.sh` — замер новой сборки против базы через bz_openbench.

Пути в скриптах — машинозависимые (примеры с `D:\bc`), править под себя.
