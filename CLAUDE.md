# BreezeCAD — памятка для Claude

Бесплатная кроссплатформенная САПР: C++20, OpenCASCADE, Dear ImGui.
Подгружается сама в каждой сессии. Отвечать по-русски.

## Нельзя нарушать
- В `next` и `main` сливает только автор (MikeVerx). Работа — в своей ветке
  от `next` (глобальные переделки — от `Pre-next`), PR в `next`.
- Модули с основой не сливаются никогда. `modules/mcp` не трогать; iOS-порт
  ведёт второй разработчик — тоже не трогать.
- Офлайн, без телеметрии и сетевых запросов.
- Видимый текст — только `Tr("…")` / `Trf("… {}", x)` / `N_("…")`; после новых
  строк — `python3 scripts/i18n/extract.py` (на Windows — `python`).
- Цвета — только токены темы; переменные не называть `far`/`near` (макросы windows.h).
- Должно работать на ARM (Apple Silicon, Android): без x86-интринсиков.
- Старые файлы не ломать: новые поля читать через `ReadMember` (отсутствие = норма).
- Без автора не трогать: форматы файлов (`document/BzcFile*`, `JsonIO*`,
  `assembly/AssemblyFile*`), решатель сборки, `kernel/AssemblyModel*`, `AutoDrop*`,
  `ComponentPattern*`, `CMakeLists.txt`, `.github/`, `third_party/`.
- Новые интерфейсы — сначала макет, в код после согласия автора. Спорное
  поведение — по логике Inventor, если автор не сказал иначе.
- Тестовые модели автора — только в закрытом `BreezeCAD/bz-test-corpus`.

## Где подробности (экономия токенов)
- **База знаний** — приватный `MikeVerx/breezecad-vault`. Если подключена к
  сессии — начинать с её `00-Index/Home.md` и `Status.md`, документы ниже —
  только за деталями.
- Без неё: `docs/dev/HANDOFF.md` — только нужный раздел через `grep -n`
  (§0 старт, §2 экономия токенов, §3 UI-тест, §3а мониторинг, §4 карта кода,
  §7 известные баги); `docs/ARCHITECTURE.md`; `docs/dev/FIX-BRANCH.md`
  (правила кода и коммитов); решения — `docs/ADR/`.
- `docs/dev/WORKLOG.md` и большие исходники (`SketchTools.cpp`, `SketchEditor.cpp`,
  `Model.cpp`, `Application.cpp`) — только `grep -n` и чтение кусками.
  `docs/legacy/` не читать.
- Граф кода — `graphify-out/` (если собран), документация библиотек —
  Context7 («use context7»); см. `docs/dev/AI-TOOLS.md`.

## Сборка и проверка
`bash scripts/dev/cloud-setup.sh` в облачной песочнице, затем
`cmake --preset linux-debug`, сборка, `ctest --preset linux-debug` (или
`./build/linux-debug/bin/bz_tests`). Windows и macOS — только CI.
Сделал — удостоверься: скилл `/check`. Упал CI — скилл `/ci-triage`.
Крупный PR перед сдачей — субагент `bz-reviewer`.

Коммиты по-русски: `тип(область): что` + What / Why / Where / Tested,
одна логическая часть на коммит.
