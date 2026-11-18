#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
io_build_dir=$(mktemp -d /tmp/colab-io-tests.XXXXXX)
trap 'rm -rf -- "$io_build_dir"' EXIT
for name in pcie_mmio_test io_polling_test io_interrupt_test io_dma_test nvme_test io_integration_test io_fault_test; do
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror "tests/$name.cpp" -o "$io_build_dir/$name"
    "$io_build_dir/$name"
done
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror examples/io_modes.cpp -o "$io_build_dir/io_modes"
"$io_build_dir/io_modes"
