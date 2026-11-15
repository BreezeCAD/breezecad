#!/usr/bin/env bash
# One Open CASCADE for every BreezeCAD build: Windows (MSYS2/MinGW64), Linux,
# macOS (arm64), Android, iOS and Claude's cloud sandbox. Same tag, same
# toolkits, same options — only the toolchain differs.
#
# Usage: build-occt.sh <platform> <prefix>
#   platform  linux | windows | macos | android-arm64-v8a | android-x86_64 |
#             ios-iphoneos | ios-iphonesimulator
#   prefix    where to install: <prefix>/lib (+ bin/ on Windows),
#             <prefix>/include/opencascade, <prefix>/lib/cmake/opencascade
#
# Needs: cmake, ninja, git, a C++ compiler. Desktop systems take FreeType and
# fontconfig from their package manager (MSYS2, conda-forge, Homebrew; the
# caller sets CMAKE_PREFIX_PATH); Android and iOS build FreeType here.
#   Android: ANDROID_NDK_HOME.  iOS: Xcode (xcrun).
#
# CI keeps the result in the Actions cache; the key is the hash of this file,
# so changing the tag or a toolkit here rebuilds OCCT everywhere.
#
# Toolkits (ADR-0001): the modeling core, STEP/STL, and what BreezeCAD's
# render, text outlines and drawing views link (TKService, TKV3d, TKOpenGl or
# TKOpenGles, TKHLR). OCCT adds what they depend on by itself — STEP in 8.0
# needs TKXCAF and with it TKCAF, TKLCAF, TKCDF, TKVCAF. Nothing else: no
# Draw, IGES, OBJ/PLY/GLTF/VRML, no OCAF storage (TKBin*, TKXml*, TKStd*).
set -euo pipefail

PLATFORM=${1:?platform: linux, windows, macos, android-<abi> or ios-<sdk>}
mkdir -p "${2:?install prefix}"
PREFIX=$(cd "$2" && pwd)

OCCT_TAG=V8_0_1
FREETYPE_TAG=VER-2-13-3     # Android and iOS only
ANDROID_API=28              # Android 9; keep in step with CMakePresets.json
IOS_MIN=16.3                # std::format needs iOS 16.3; as CMakePresets.json
MACOS_MIN=14.0              # as the "macos" preset
SRC=${SRC:-$PREFIX/../occt-src}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}

TOOLKITS=(
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo
    TKPrim TKBO TKBool TKFeat TKFillet TKOffset TKShHealing TKMesh
    TKXSBase TKDE TKDESTEP TKDESTL
    TKService TKV3d TKHLR
)

log() { echo "== $*"; }
clone() { [ -d "$SRC/$2/.git" ] || git clone -q --depth 1 --branch "$3" "https://github.com/$1" "$SRC/$2"; }
mkdir -p "$SRC"
SRC=$(cd "$SRC" && pwd)

# Paths for CMake: MinGW's cmake wants C:/… rather than /c/….
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(native "$PREFIX")")
LIBTYPE=Shared
FREETYPE=system

case "$PLATFORM" in
    linux)
        TOOLKITS+=(TKOpenGl)
        # $ORIGIN: the libraries find each other wherever they are unpacked
        # (the package's lib/, the sandbox's prefix). In a conda environment
        # (CI) also its lib/: FreeType and libstdc++ come from there, not from
        # the runner's newer system. package-linux.sh resets it to $ORIGIN.
        ARGS+=(-DUSE_XLIB=ON -DUSE_OPENGL=ON -DUSE_GLES2=OFF "-DCMAKE_INSTALL_RPATH=\$ORIGIN${CONDA_PREFIX:+;$CONDA_PREFIX/lib}")
        ;;
    windows)
        TOOLKITS+=(TKOpenGl)
        ARGS+=(-DUSE_D3D=OFF -DUSE_OPENGL=ON -DUSE_GLES2=OFF)
        ;;
    macos)
        TOOLKITS+=(TKOpenGl)
        # Absolute install names: dylibbundler (package-macos.sh) rewrites them.
        ARGS+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN"
               -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" -DUSE_OPENGL=ON -DUSE_GLES2=OFF)
        ;;
    android-arm64-v8a|android-x86_64)
        : "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK folder}"
        TOOLKITS+=(TKOpenGles)
        # c++_shared: OCCT is many shared libraries, a static C++ runtime in
        # each would not work. Flexible page sizes: 16 KB page devices too.
        ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
               -DANDROID_ABI="${PLATFORM#android-}" -DANDROID_PLATFORM="android-$ANDROID_API"
               -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON)
        FREETYPE=build
        ;;
    ios-iphoneos|ios-iphonesimulator)
        TOOLKITS+=(TKOpenGles)
        # Static: an app may not carry loose .dylib files. arm64 only: every
        # device on iOS 16 and the simulator on Apple Silicon.
        ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$(xcrun --sdk "${PLATFORM#ios-}" --show-sdk-path)"
               -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN")
        LIBTYPE=Static
        FREETYPE=build
        ;;
    *) echo "unknown platform: $PLATFORM" >&2; exit 2 ;;
esac

# --- FreeType (mobile): fonts for OCCT's text (kernel/TextGeometry) ---------
# Nothing optional (no zlib, png, HarfBuzz, Brotli): OCCT only reads outlines.
FT_ARGS=()   # may stay empty: expanded as ${FT_ARGS[@]+…} for bash 3.2 (macOS) under set -u
if [ "$FREETYPE" = build ]; then
    log "FreeType $FREETYPE_TAG ($PLATFORM)"
    clone freetype/freetype freetype "$FREETYPE_TAG"
    [ "$LIBTYPE" = Shared ] && ft_shared=ON ft_lib=libfreetype.so || ft_shared=OFF ft_lib=libfreetype.a
    cmake -S "$SRC/freetype" -B "$SRC/freetype/build-$PLATFORM" "${ARGS[@]}" -DBUILD_SHARED_LIBS=$ft_shared \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
    cmake --build "$SRC/freetype/build-$PLATFORM" -j "$JOBS" >/dev/null
    cmake --install "$SRC/freetype/build-$PLATFORM" >/dev/null
    FT_ARGS=(-D3RDPARTY_FREETYPE_DIR="$PREFIX"
             -D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build="$PREFIX/include/freetype2"
             -D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2="$PREFIX/include/freetype2"
             -D3RDPARTY_FREETYPE_LIBRARY_DIR="$PREFIX/lib"
             -D3RDPARTY_FREETYPE_LIBRARY="$PREFIX/lib/$ft_lib")
fi

# --- Open CASCADE -----------------------------------------------------------
# Every module off; the toolkits above are asked for one by one and OCCT
# builds them with their dependencies.
log "OCCT $OCCT_TAG ($PLATFORM): ${TOOLKITS[*]}"
clone Open-Cascade-SAS/OCCT occt "$OCCT_TAG"
start=$(date +%s)
cmake -S "$SRC/occt" -B "$SRC/occt/build-$PLATFORM" "${ARGS[@]}" ${FT_ARGS[@]+"${FT_ARGS[@]}"} \
    -DINSTALL_DIR="$(native "$PREFIX")" -DINSTALL_DIR_LAYOUT=Unix -DBUILD_LIBRARY_TYPE=$LIBTYPE \
    -DBUILD_MODULE_FoundationClasses=OFF -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_ModelingAlgorithms=OFF -DBUILD_MODULE_Visualization=OFF \
    -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF \
    -DBUILD_MODULE_Draw=OFF -DBUILD_ADDITIONAL_TOOLKITS="${TOOLKITS[*]}" \
    -DBUILD_DOC_Overview=OFF -DBUILD_GTEST=OFF -DBUILD_USE_PCH=OFF \
    -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_TBB=OFF -DUSE_FREEIMAGE=OFF -DUSE_RAPIDJSON=OFF \
    -DUSE_DRACO=OFF -DUSE_VTK=OFF -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_FREETYPE=ON \
    > "$SRC/occt-configure-$PLATFORM.log" 2>&1 || { tail -40 "$SRC/occt-configure-$PLATFORM.log"; exit 1; }
cmake --build "$SRC/occt/build-$PLATFORM" -j "$JOBS"
cmake --install "$SRC/occt/build-$PLATFORM" >/dev/null

# The folder of OpenCASCADEConfig.cmake differs between layouts: one place.
config=$(dirname "$(find "$PREFIX" -name OpenCASCADEConfig.cmake | head -1)")
[ -n "$config" ] && [ -d "$config" ] || { echo "OpenCASCADEConfig.cmake not installed" >&2; exit 1; }
[ "$config" = "$PREFIX/lib/cmake/opencascade" ] || { mkdir -p "$PREFIX/lib/cmake"; ln -sfn "$config" "$PREFIX/lib/cmake/opencascade"; }

# What was built, for the logs and for anyone who unpacks the archive.
version=$(awk '/define OCC_VERSION_COMPLETE/ {gsub(/"/, "", $3); print $3}' "$PREFIX/include/opencascade/Standard_Version.hxx")
built=$(find "$PREFIX/lib" "$PREFIX/bin" -maxdepth 1 -name '*TK*' 2>/dev/null | sed -E 's,.*/(lib)?(TK[A-Za-z0-9]+).*,\2,' | sort -u)
minutes=$(( ($(date +%s) - start) / 60 ))
{
    echo "OCCT $version ($OCCT_TAG), $PLATFORM, $LIBTYPE, built in $minutes min"
    echo "toolkits: $(echo $built)"
} > "$PREFIX/occt-build.txt"
log "Готово: OCCT $version, $(echo "$built" | wc -l | tr -d ' ') тулкитов, $(du -sh "$PREFIX" | cut -f1), $minutes мин"
cat "$PREFIX/occt-build.txt"
