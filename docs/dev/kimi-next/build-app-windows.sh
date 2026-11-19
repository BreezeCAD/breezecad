#!/usr/bin/env bash
# Сборка BreezeCAD (windows-mingw) из-под ASCII-джанкшна D:\bc (обход багов ld/windres
# с не-ASCII путями; репозиторий живёт в C:\Users\Мишаня\briz\breezecad).
# Тулчейн — твой MSYS2 из D:\ (ASCII). OCCT уже собран.
# Запуск: MSYSTEM=MINGW64 /d/usr/bin/bash.exe -lc 'bash /d/bc/../briz/build-app-windows3.sh 2>/dev/null || bash /c/Users/Мишаня/briz/build-app-windows3.sh'
set -euo pipefail
cd /d/bc
export MSYSTEM=MINGW64
export PATH="/d/mingw64/bin:$PATH"
export PATH="/d/bc/.deps/occt/bin:$PATH"

rm -rf build/windows-mingw
OCCT_DIR="/d/bc/.deps/occt/lib/cmake/opencascade" cmake --preset windows-mingw -DBZ_SETUP_CHANNEL=next-build

if grep -q 'isystem "D:/mingw64/include"' build/windows-mingw/build.ninja; then
  sed -i 's|-isystem "D:/mingw64/include"||g' build/windows-mingw/build.ninja
fi

cmake --build --preset windows-mingw --target breezecad bz_tests
ctest --preset windows-mingw --output-on-failure --timeout 1200
echo "APP-BUILD-AND-TESTS-OK"
