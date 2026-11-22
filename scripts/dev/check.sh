#!/usr/bin/env bash
# Сборка (пересобирает только изменившееся) + полный прогон тестов + сводка
# одной строкой. Использование: bash scripts/dev/check.sh [preset]
#   preset: linux-release (по умолчанию) | linux-debug
set -euo pipefail
cd "$(dirname "$0")/../.."
export LD_LIBRARY_PATH="$HOME/deps/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
preset="${1:-linux-release}"

cmake --build --preset "$preset" --parallel "$(nproc)"

out="$(./build/$preset/bin/bz_tests 2>&1)"
status=$?
echo "$out" | grep -E "\[doctest\]" | tail -3
# doctest пишет сводку в stdout; код возврата — из прямого запуска
exit $status
