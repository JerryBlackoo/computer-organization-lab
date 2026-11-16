#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
cache_build_dir=$(mktemp -d /tmp/colab-cpu-cache.XXXXXX)
trap 'rm -rf -- "$cache_build_dir"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror tests/cpu_cache_test.cpp -o "$cache_build_dir/test"
"$cache_build_dir/test"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/memory_system/cpu_cache.cpp -o "$cache_build_dir/demo"
"$cache_build_dir/demo"
