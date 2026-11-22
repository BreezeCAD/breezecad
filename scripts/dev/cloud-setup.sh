#!/usr/bin/env bash
# BreezeCAD — подготовка облачной песочницы Claude (Linux x86_64, Ubuntu 24.04)
# к сборке, тестам и UI-сценариям на Xvfb.
#
# Зачем: в песочнице закрыты apt и PyPI, открыт только GitHub. Поэтому OCCT и
# GLFW собираются из исходников с GitHub, недостающие заголовки X11/GL берутся
# оттуда же. Повторный запуск ничего не пересобирает (метки *.done).
#
#   bash scripts/dev/cloud-setup.sh            # зависимости + сборка + тесты
#   bash scripts/dev/cloud-setup.sh deps       # только зависимости
#
# OCCT — тот же, что в сборках CI (scripts/deps/build-occt.sh, ADR-0001).
# Быстрый путь: CI (build.yml, job linux) кладёт его архив
# bz-occt-linux-x64.tar.xz в релиз dev-deps; он скачивается вместо часовой
# сборки, если совпадает хеш скрипта. Иначе OCCT собирается тем же скриптом.
#
# Переменные: BZ_DEPS (по умолчанию $HOME/deps), BZ_JOBS (по умолчанию nproc).
set -euo pipefail

DEPS=${BZ_DEPS:-$HOME/deps}
PREFIX=$DEPS/prefix
SRC=$DEPS/src
JOBS=${BZ_JOBS:-$(nproc)}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
GLFW_TAG=3.4
ARCHIVE=bz-occt-linux-x64.tar.xz
OCCT_HASH=$(sha256sum "$REPO/scripts/deps/build-occt.sh" | cut -d' ' -f1)
mkdir -p "$SRC" "$PREFIX"

log() { printf '\n== %s\n' "$*"; }
clone() { # clone <github owner/repo> <dir> [tag]
    [ -d "$SRC/$2/.git" ] && return 0
    git clone -q --depth 1 ${3:+--branch "$3"} "https://github.com/$1" "$SRC/$2"
}

# --- 1. Заголовки X11 / OpenGL / EGL и ссылки *.so для линковки ------------
system_headers() {
    [ -f "$DEPS/headers.done" ] && return 0
    log "Заголовки X11/GL/EGL (zig-gamedev/system_sdk, NVIDIA/libglvnd)"
    clone zig-gamedev/system_sdk system_sdk
    clone NVIDIA/libglvnd libglvnd
    local S=$SRC/system_sdk/linux/include
    cp -r --update=none "$S/X11/." /usr/include/X11/
    cp -r --update=none "$S/xkbcommon" /usr/include/
    cp -r --update=none "$SRC/libglvnd/include/GL" "$SRC/libglvnd/include/EGL" \
        "$SRC/libglvnd/include/KHR" /usr/include/
    local L=/usr/lib/x86_64-linux-gnu
    for l in GL EGL GLX Xrandr Xinerama Xcursor Xi Xfixes Xtst; do
        [ -e "$L/lib$l.so" ] && continue
        local f; f=$(ls "$L"/lib$l.so.* 2>/dev/null | sort | head -1 || true)
        [ -n "$f" ] && ln -s "$f" "$L/lib$l.so"
    done
    touch "$DEPS/headers.done"
}

# --- 1а. Инструменты UI-тестов: xdotool и Pillow недоступны -> свои заменители
ui_tools() {
    [ -x "$DEPS/bin/xdotool" ] && return 0
    log "UI-тесты: mini-xdotool и заменитель PIL"
    mkdir -p "$DEPS/bin" "$DEPS/pylib"
    cc -O2 "$REPO/scripts/dev/mini-xdotool.c" -o "$DEPS/bin/xdotool" -lX11 -lXtst
    cp -r "$REPO/scripts/dev/pil-shim/PIL" "$DEPS/pylib/"
}

# --- 2. OCCT из релиза dev-deps (тот же архив, что собрал CI) ---------------
# Пересобирается при смене scripts/deps/build-occt.sh: метка хранит его хеш.
occt_ready() { [ "$(cat "$DEPS/occt.done" 2>/dev/null)" = "$OCCT_HASH" ]; }
fetch_prebuilt() {
    occt_ready && return 0
    log "Пробую готовый OCCT из релиза dev-deps"
    local base="https://github.com/BreezeCAD/breezecad/releases/download/dev-deps"
    local sha; sha=$(curl -fsSL "$base/${ARCHIVE%.tar.xz}.sha" 2>/dev/null || true)
    if [ "$sha" != "$OCCT_HASH" ]; then echo "в dev-deps другой OCCT (или его нет) — соберу сам"; return 1; fi
    curl -fsSL -o "$DEPS/$ARCHIVE" "$base/$ARCHIVE" || { rm -f "$DEPS/$ARCHIVE"; return 1; }
    rm -rf "$DEPS/occt" && mkdir -p "$DEPS/occt"
    tar -C "$DEPS/occt" -xJf "$DEPS/$ARCHIVE" && rm -f "$DEPS/$ARCHIVE"
    echo "$OCCT_HASH" > "$DEPS/occt.done"
    cat "$DEPS/occt/occt-build.txt"
}

# --- 3. GLFW ----------------------------------------------------------------
build_glfw() {
    [ -f "$DEPS/glfw.done" ] && return 0
    log "GLFW $GLFW_TAG"
    clone glfw/glfw glfw "$GLFW_TAG"
    cmake -S "$SRC/glfw" -B "$SRC/glfw/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=ON \
        -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF \
        -DBUILD_SHARED_LIBS=OFF >/dev/null
    cmake --build "$SRC/glfw/build" -j "$JOBS" >/dev/null
    cmake --install "$SRC/glfw/build" >/dev/null
    touch "$DEPS/glfw.done"
}

# --- 4. OCCT своей сборкой (если архива нет) ---------------------------------
build_occt() {
    occt_ready && return 0
    log "OCCT — около часа на 2 ядрах"
    SRC="$SRC" JOBS="$JOBS" bash "$REPO/scripts/deps/build-occt.sh" linux "$DEPS/occt"
    echo "$OCCT_HASH" > "$DEPS/occt.done"
}

# --- 6. Сборка BreezeCAD и тесты --------------------------------------------
build_app() {
    log "Сборка BreezeCAD (linux-debug)"
    cd "$REPO"
    cmake --preset linux-debug -DCMAKE_PREFIX_PATH="$DEPS/occt;$PREFIX" \
        -DOpenCASCADE_DIR="$DEPS/occt/lib/cmake/opencascade" >/dev/null
    # Into a file first: `| head` ended the build with SIGPIPE after 40 warnings.
    cmake --build --preset linux-debug -j "$JOBS" > "$DEPS/build.log" 2>&1 || true
    grep -E 'error|warning:' "$DEPS/build.log" | head -40 || true
    log "Тесты"
    LD_LIBRARY_PATH="$DEPS/occt/lib:$PREFIX/lib:${LD_LIBRARY_PATH:-}" ./build/linux-debug/bin/bz_tests 2>&1 | tail -3
}

env_hint() {
    cat <<EOF

Готово. В каждой новой оболочке для запуска программы и UI-тестов:
  export LD_LIBRARY_PATH=$DEPS/occt/lib:$PREFIX/lib:\$LD_LIBRARY_PATH
  export PATH=$DEPS/bin:\$PATH PYTHONPATH=$DEPS/pylib
UI-сценарии: scripts/ui-test/ui.sh (Xvfb уже стоит в образе).
EOF
}

case "${1:-all}" in
    deps) system_headers; ui_tools; build_glfw; fetch_prebuilt || build_occt; env_hint ;;
    all)  system_headers; ui_tools; build_glfw; fetch_prebuilt || build_occt; build_app; env_hint ;;
    *)    echo "usage: $0 [all|deps]"; exit 2 ;;
esac
