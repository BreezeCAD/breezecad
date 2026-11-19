#!/usr/bin/env bash
# Установка Windows-сборки BreezeCAD 0.2.6-dev в D:\BreezeCAD-0.2.6-dev (рядом с 0.2.5.1).
# Раскладка как в CI: breezecad.exe + assets + DLL через deploy-runtime.sh.
# Запуск: MSYSTEM=MINGW64 /d/usr/bin/bash.exe -lc 'bash /c/Users/Мишаня/briz/install-windows-026.sh'
set -euo pipefail
cd /d/bc
export MSYSTEM=MINGW64
export PATH="/d/mingw64/bin:$PATH"
export PATH="/d/bc/.deps/occt/bin:$PATH"

DIST=/d/BreezeCAD-0.2.6-dev
BIN=build/windows-mingw/bin

mkdir -p "$DIST/shell"
cp "$BIN/breezecad.exe" "$DIST/"
cp -r "$BIN/assets" "$DIST/"
bash scripts/deploy-runtime.sh "$DIST" breezecad.exe "/d/bc/.deps/occt/bin"
cp LICENSE "$DIST/"
[ -f "$BIN/bz_thumbs.dll" ] && cp "$BIN/bz_thumbs.dll" "$DIST/shell/"
cp installer/icons/*.ico "$DIST/shell/"

# Зелёное лого — только в этой установке.
sed -i 's/#1D4ED8/#16A34A/g' "$DIST/assets/brand/breezecad-mark.svg" \
                             "$DIST/assets/brand/breezecad-app-icon.svg" \
                             "$DIST/assets/icons/brand-mark.svg" \
                             "$DIST/assets/icons/brand-app.svg"
sed -i 's/#6E9BFF/#4ADE80/g' "$DIST/assets/brand/breezecad-mark-dark-ui.svg"

cat > "$DIST/installed.json" <<EOF
{
  "version": "0.2.6-dev",
  "branch": "fix/slow-open",
  "commit": "15ee71ec63aa0b9cf8e71381beb35c9a086d228c",
  "pr": 126,
  "logo": "green (#1D4ED8->#16A34A, #6E9BFF->#4ADE80, this install only)",
  "installed": "$(date -Iseconds)"
}
EOF

cp "tests/data/regression/Вискозиметр ВЗК.05.bzp" "$DIST/"

echo "INSTALL-OK: $DIST"
ls "$DIST" | head -25
