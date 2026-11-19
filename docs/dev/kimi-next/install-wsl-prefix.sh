#!/usr/bin/env bash
# Установка release-сборки BreezeCAD 0.2.6-dev (ветка fix/slow-open, PR #126)
# в отдельный префикс /root/.local/breezecad-0.2.6-dev внутри WSL.
# Раскладка — как у scripts/package-linux.sh: breezecad + assets + lib/ ($ORIGIN/lib).
# Установленная 0.2.5.1 (/mnt/d/BreezeCAD) не трогается.
set -euo pipefail

BIN=/root/briz/bc-slow/build/linux-release/bin
PREFIX=/root/.local/breezecad-0.2.6-dev
DESKTOP_DIR=/root/.local/share/applications
SRC=/root/briz/bc-slow

mkdir -p "$PREFIX/lib" "$DESKTOP_DIR"
cp "$BIN/breezecad" "$PREFIX/"
cp -r "$BIN/assets" "$PREFIX/"
cp "$SRC/installer/icons/png/app.png" "$PREFIX/breezecad-icon.png"

is_system() {
    case "$1" in
        linux-vdso.so*|ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*) return 0 ;;
        libGL.so*|libGLX*|libGLdispatch*|libOpenGL*|libEGL*|libGLU*|libdrm*|libgbm*|libwayland-*) return 0 ;;
        libX11*|libxcb*|libXext*|libXau*|libXdmcp*|libXfixes*|libXrender*|libXrandr*|libXinerama*|libXcursor*|libXi.so*|libXxf86vm*) return 0 ;;
        libfontconfig*) return 0 ;;
    esac
    return 1
}

export LD_LIBRARY_PATH=/root/deps/prefix/lib   # иначе ldd не показывает косвенные OCCT-зависимости

copied=0
while read -r name path; do
    is_system "$name" && continue
    [ -f "$path" ] || { echo "нет библиотеки: $name ($path)"; exit 1; }
    [ -e "$PREFIX/lib/$name" ] && continue
    cp -L "$path" "$PREFIX/lib/$name"
    copied=$((copied + 1))
done < <(ldd "$PREFIX/breezecad" | awk '/=>/ && $3 ~ /^\// {print $1, $3}')
echo "пакетировано библиотек: $copied"

patchelf --set-rpath '$ORIGIN/lib' "$PREFIX/breezecad"
for lib in "$PREFIX"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
strip --strip-unneeded "$PREFIX/breezecad" "$PREFIX"/lib/*.so* 2>/dev/null || true

if LD_LIBRARY_PATH= ldd "$PREFIX/breezecad" | grep -E "not found"; then
    echo "ошибка: есть неразрешённые зависимости"; exit 1
fi

# --- Перекраска лого в зелёный (только в этой установке) ---
# Синий фирменный -> зелёный: #1D4ED8 -> #16A34A, светло-синий -> #4ADE80.
# Исходники и сборочное дерево не трогаем.
sed -i 's/#1D4ED8/#16A34A/g' "$PREFIX/assets/brand/breezecad-mark.svg" \
                             "$PREFIX/assets/brand/breezecad-app-icon.svg" \
                             "$PREFIX/assets/icons/brand-mark.svg" \
                             "$PREFIX/assets/icons/brand-app.svg"
sed -i 's/#6E9BFF/#4ADE80/g' "$PREFIX/assets/brand/breezecad-mark-dark-ui.svg"

# Иконка для .desktop и окна — поворот оттенка синего к зелёному (PIL).
python3 - "$PREFIX/breezecad-icon.png" <<'PY'
import sys, colorsys
from PIL import Image
p = sys.argv[1]
img = Image.open(p).convert("RGBA")
px = img.load()
for y in range(img.height):
    for x in range(img.width):
        r, g, b, a = px[x, y]
        if a == 0:
            continue
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s > 0.25:  # цветные пиксели; серое/белое не трогаем
            h = (h + (140 - 221) / 360.0) % 1.0  # ~221° (синий) -> ~140° (зелёный)
            r, g, b = colorsys.hsv_to_rgb(h, s, v)
            px[x, y] = (round(r * 255), round(g * 255), round(b * 255), a)
img.save(p)
PY

cat > "$DESKTOP_DIR/breezecad-0.2.6-dev.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=BreezeCAD 0.2.6-dev (fix/slow-open)
Comment=BreezeCAD из ветки fix/slow-open (PR #126), рядом с 0.2.5.1
Exec=$PREFIX/breezecad %f
Icon=$PREFIX/breezecad-icon.png
Terminal=false
Categories=Graphics;Engineering;
MimeType=application/x-breezecad;
EOF

cat > "$PREFIX/installed.json" <<EOF
{
  "version": "0.2.6-dev",
  "branch": "fix/slow-open",
  "commit": "15ee71ec63aa0b9cf8e71381beb35c9a086d228c",
  "pr": 126,
  "logo": "green (recolored in this install only; #1D4ED8->#16A34A, #6E9BFF->#4ADE80)",
  "source": "git bundle (work/fix-slow-open) -> origin/fix/slow-open",
  "installed": "$(date -Iseconds)"
}
EOF

echo "готово: $PREFIX"
