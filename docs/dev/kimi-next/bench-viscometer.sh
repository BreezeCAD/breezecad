#!/usr/bin/env bash
# Замер открытия регрессионных файлов: новая сборка (fix/slow-open) против базы 0.2.6-beta.
set -uo pipefail
export LD_LIBRARY_PATH=/root/deps/prefix/lib
DIR=/root/briz/bc-slow/tests/data/regression

echo "### NEW — fix/slow-open 15ee71e (= установленный 0.2.6-dev)"
for i in 1 2 3; do
  echo "--- прогон $i"
  /root/briz/bc-slow/build/linux-release/bin/bz_openbench "$DIR" 2>&1 | grep -E "^=== |итого" || true
done

echo "### BASE — 0.2.6-beta f79acf8 (до фикса, тот же harness)"
for i in 1 2 3; do
  echo "--- прогон $i"
  /root/briz/bc-base/build/linux-release/bin/bz_openbench "$DIR" 2>&1 | grep -E "^=== |итого" || true
done
