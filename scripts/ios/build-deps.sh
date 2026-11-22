#!/usr/bin/env bash
# Builds what the BreezeCAD core needs on iOS and iPadOS — FreeType and Open
# CASCADE, static libraries for one SDK (an app may not carry loose .dylib
# files) — with the common script, the same OCCT as every other system
# (scripts/deps/build-occt.sh). About an hour per SDK on a GitHub macOS
# runner; CI keeps the result in the Actions cache (ios.yml).
#
# Usage: build-deps.sh <sdk> <prefix>
#   sdk     iphoneos (iPhone and iPad, arm64) or iphonesimulator (the
#           simulator on an Apple Silicon Mac, arm64)
#   prefix  where to install: <prefix>/lib/*.a, <prefix>/lib/cmake/opencascade
# Needs: Xcode (xcrun), cmake, ninja, git.
set -euo pipefail
SDK=${1:?sdk: iphoneos or iphonesimulator}
exec bash "$(dirname "$0")/../deps/build-occt.sh" "ios-$SDK" "${2:?install prefix}"
