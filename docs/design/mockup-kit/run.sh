#!/usr/bin/env bash
# Пересобирает макеты окна BreezeCAD в PNG 1440×900.
# Нужны: python3 (numpy, Pillow) и Chrome/Chromium.  Запуск из корня репозитория:
#   bash docs/design/mockup-kit/run.sh            # только HTML → PNG
#   RENDER=1 bash docs/design/mockup-kit/run.sh   # заново посчитать картинки модели (долго, ~10 мин)
set -euo pipefail
KIT="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$KIT/../../.." && pwd)"
OUT="${OUT:-$ROOT/build/mockups}"; IMG="$ROOT/docs/design/viewport-look/img"
mkdir -p "$OUT/html/icons"
cp "$ROOT"/assets/icons/*.svg "$KIT"/badge-*.svg "$OUT/html/icons/"
cp "$ROOT"/assets/fonts/*.ttf "$ROOT/assets/brand/breezecad-mark.svg" "$KIT/cube.svg" "$KIT/R-gallery.html" "$OUT/html/"
if [[ "${RENDER:-0}" == 1 ]]; then
  for m in now live render glass preview; do ZOOM=1.5 python3 "$ROOT/docs/design/viewport-look/prototype_render.py" $m 2192 1324 "$IMG/$m.png"; done
  BG=dark ZOOM=1.5 python3 "$ROOT/docs/design/viewport-look/prototype_render.py" render 2192 1324 "$IMG/render_dark.png"
fi
cp "$IMG"/{now,live,glass,preview,render,render_dark}.png "$OUT/html/"
cp "$KIT/gen.py" "$KIT/build.py" "$OUT/"
(cd "$OUT" && python3 build.py)
CHROME="$(command -v google-chrome || command -v chromium || command -v chromium-browser)"
for f in "$OUT"/html/*.html; do
  "$CHROME" --headless=new --no-sandbox --disable-gpu --hide-scrollbars --window-size=1440,900 \
    --screenshot="${f%.html}.png" "file://$f" 2>/dev/null
done
echo "Готово: $OUT/html/*.png"
