#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
rtl_build_dir=$(mktemp -d /tmp/colab-rtl-cache.XXXXXX)
trap 'rm -rf -- "$rtl_build_dir"' EXIT
rtl_options=()
if [[ -n ${IVERILOG_IVL_DIR:-} ]]; then rtl_options=(-B "$IVERILOG_IVL_DIR"); fi
"${IVERILOG:-iverilog}" "${rtl_options[@]}" -g2012 -s rtl_cache_tb -o "$rtl_build_dir/test.vvp" \
    rtl/rv32im/axi_cache.sv tests/rtl_cache_tb.sv
"${VVP:-vvp}" "$rtl_build_dir/test.vvp"
