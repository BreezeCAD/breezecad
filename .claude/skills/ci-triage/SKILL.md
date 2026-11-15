---
name: ci-triage
description: Разбор упавшей сборки GitHub Actions в BreezeCAD (Windows, Linux, macOS, Android) — достать аннотации, найти причину, объяснить автору по-человечески и предложить исправление. Использовать, когда CI красный или автор присылает ссылку на упавший прогон.
---

# Разбор упавшего CI

Логи прогонов из сессии часто не скачать (403), поэтому читаем **аннотации**:
шаги пишут туда `::notice`, а `scripts/ci-annotate.sh` — ошибки компиляции и
хвост лога тестов (до 4 КБ на аннотацию, до 10 на шаг).

## 1. Найти прогон
```bash
gh run list --branch <ветка> --limit 3
gh run view <run id> --json jobs --jq '.jobs[] | select(.conclusion=="failure") | "\(.databaseId) \(.name)"'
```

## 2. Прочитать причину
```bash
gh api repos/BreezeCAD/breezecad/check-runs/<job id>/annotations \
  --jq '.[] | "\(.annotation_level) \(.title): \(.message)"'
```
Если аннотаций нет — попробовать `gh run view <run id> --log-failed | tail -80`.

## 3. Классифицировать
- Джобы упали за ~6 с — биллинг GitHub («recent account payments have
  failed…»), не код: сказать автору, ничего не чинить.
- Отменён (cancelled) — его перебил новый push (concurrency), не ошибка.
- Ошибка компиляции только на Windows — проверить ловушки: `far`/`near`,
  `min`/`max` из windows.h, пути с кириллицей, CRLF.
- Только macOS — OCCT 7.9 вместо 8 (другой API), clang строже к шаблонам.
- Только Android — NDK, ARM (никаких x86-интринсиков), отсутствие десктопного GL.
- Падение тестов — найти тест и воспроизвести на Linux (`/check`).

## 4. Итог автору
Одной-двумя фразами: что сломалось, на какой системе, почему, что предлагаю.
Исправление — отдельным коммитом в ту же ветку; повторно CI не ждать в цикле.
