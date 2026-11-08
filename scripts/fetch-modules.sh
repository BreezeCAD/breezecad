#!/usr/bin/env bash
# The module packages of one system, from the latest release of every module
# repository in modules/list.txt — what the installer offers and the release
# page "modules" lists.
#
#   scripts/fetch-modules.sh <windows|linux|macos> <folder>
#
# Needs gh with a token (GH_TOKEN). A module without a release, or without a
# package for this system, fails the run: the installer must not lose a module
# silently.
set -euo pipefail
system="$1"
dest="$2"
cd "$(dirname "$0")/.."
mkdir -p "$dest"
count=0
while IFS= read -r repo; do
    repo="${repo%%#*}"; repo="$(echo "$repo" | tr -d '[:space:]')"
    [ -z "$repo" ] && continue
    tmp="$(mktemp -d)"
    gh release download -R "$repo" --pattern "*-${system}-*.bzmod" -D "$tmp" \
        || { echo "::error title=Modules::$repo: no release with a $system package"; exit 1; }
    ls "$tmp"/*.bzmod > /dev/null 2>&1 \
        || { echo "::error title=Modules::$repo: the latest release has no *-${system}-*.bzmod"; exit 1; }
    mv -f "$tmp"/*.bzmod "$dest"/
    rm -rf "$tmp"
    count=$((count + 1))
done < modules/list.txt
echo "Modules for $system: $count repositories → $(ls "$dest"/*.bzmod | xargs -n1 basename | tr '\n' ' ')"
