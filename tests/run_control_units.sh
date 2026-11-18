#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
control_build_dir=$(mktemp -d /tmp/colab-control-tests.XXXXXX)
trap 'rm -rf -- "$control_build_dir"' EXIT
for name in decoder_test hardwired_control_test microprogrammed_control_test microcode_binary_test vertical_control_test dual_control_test; do
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror "tests/$name.cpp" -o "$control_build_dir/$name"
    "$control_build_dir/$name"
    printf 'PASS %s\n' "$name"
done
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/control_unit/compare.cpp -o "$control_build_dir/compare"
"$control_build_dir/compare"
