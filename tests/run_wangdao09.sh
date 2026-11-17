#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
output_dir=$(mktemp -d /tmp/colab-wangdao09.XXXXXX)
trap 'rm -rf -- "$output_dir"' EXIT
for name in tests/wangdao09_execution_test.cpp tests/wangdao09_rv32_test.cpp examples/wangdao09/compare.cpp; do
    binary="$output_dir/$(basename "${name%.cpp}")"
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror "$name" -o "$binary"
    "$binary"
done
