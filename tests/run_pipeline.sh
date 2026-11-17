#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
pipeline_build_dir=$(mktemp -d /tmp/colab-rv32-pipeline.XXXXXX)
trap 'rm -rf -- "$pipeline_build_dir"' EXIT
for name in rv32_datapath_test rv32_muldiv_test pipeline_test rv32_subword_test rv32_trap_test pmp_test rv32_privilege_test rv32_csr_test rv32_fence_test clint_test rv32_wfi_test rv32_supervisor_test; do
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror "tests/$name.cpp" -o "$pipeline_build_dir/$name"
    "$pipeline_build_dir/$name"
    printf 'PASS %s\n' "$name"
done
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror soc/core/pipeline/example.cpp -o "$pipeline_build_dir/demo"
"$pipeline_build_dir/demo"
