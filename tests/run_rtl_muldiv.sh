#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
rtl_build_dir=$(mktemp -d /tmp/colab-rtl-muldiv.XXXXXX)
trap 'rm -rf -- "$rtl_build_dir"' EXIT
rtl_compiler=${IVERILOG:-iverilog}
rtl_runner=${VVP:-vvp}
rtl_options=()
if [[ -n ${IVERILOG_IVL_DIR:-} ]]; then rtl_options=(-B "$IVERILOG_IVL_DIR"); fi
python3 tests/rtl_muldiv_vectors.py "$rtl_build_dir/vectors.txt"
"$rtl_compiler" "${rtl_options[@]}" -g2012 -s rtl_muldiv_tb -o "$rtl_build_dir/test.vvp" \
    rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv tests/rtl_muldiv_tb.sv
"$rtl_runner" "$rtl_build_dir/test.vvp" "+vectors=$rtl_build_dir/vectors.txt"
