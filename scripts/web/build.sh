#!/usr/bin/env bash
# Build the browser kernel and stage the files served by breezecad-site/preview.
# Usage: scripts/web/build.sh <OCCT wasm install> <build dir> <runtime output dir>
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
occt=$(cd "${1:?pass the OCCT wasm install directory}" && pwd)
build=${2:?pass the build directory}
output=${3:?pass the runtime output directory}

emcmake cmake -S "$root/src/web" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$occt" -DOpenCASCADE_DIR="$occt/lib/cmake/opencascade"
cmake --build "$build" --parallel "${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
mkdir -p "$output"
cp "$build/breezecad-web.mjs" "$output/"
gzip -n -9 -c "$build/breezecad-web.wasm" > "$output/breezecad-web.wasm.gz"
cp "$root/tests/data/кронштейн.bzc" "$output/bracket.bzc"
