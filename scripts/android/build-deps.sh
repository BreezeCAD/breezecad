#!/usr/bin/env bash
# Builds what the BreezeCAD core needs on Android — FreeType and Open CASCADE,
# shared libraries for one ABI — with the common script, the same OCCT as
# every other system (scripts/deps/build-occt.sh). About an hour per ABI on a
# GitHub runner; CI keeps the result in the Actions cache (android.yml).
#
# Usage: build-deps.sh <abi> <prefix>
#   abi     arm64-v8a (phones) or x86_64 (the emulator the tests run on)
#   prefix  where to install: <prefix>/lib/*.so, <prefix>/lib/cmake/opencascade
# Needs: ANDROID_NDK_HOME, cmake, ninja, git.
set -euo pipefail
ABI=${1:?abi: arm64-v8a or x86_64}
exec bash "$(dirname "$0")/../deps/build-occt.sh" "android-$ABI" "${2:?install prefix}"
