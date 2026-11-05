#!/usr/bin/env bash
# Host tests for seal-maze's plain C parts (no device, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_game" test_game.c test_ring.c ../main/game.c ../main/maze.c ../main/ring.c -lm
"$OUT/test_game"
rm -rf "$OUT"
