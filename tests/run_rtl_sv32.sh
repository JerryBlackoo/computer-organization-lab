#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
rtl_build_dir=$(mktemp -d /tmp/colab-rtl-sv32.XXXXXX)
trap 'rm -rf -- "$rtl_build_dir"' EXIT
rtl_options=()
if [[ -n ${IVERILOG_IVL_DIR:-} ]]; then rtl_options=(-B "$IVERILOG_IVL_DIR"); fi
"${IVERILOG:-iverilog}" "${rtl_options[@]}" -g2012 -s rtl_sv32_tb -o "$rtl_build_dir/test.vvp" \
    rtl/rv32im/sv32.sv tests/rtl_sv32_tb.sv
"${VVP:-vvp}" "$rtl_build_dir/test.vvp"
