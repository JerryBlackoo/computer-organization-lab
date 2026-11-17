#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
rtl_build_dir=$(mktemp -d /tmp/colab-rtl-virtual.XXXXXX)
trap 'rm -rf -- "$rtl_build_dir"' EXIT
rtl_options=()
if [[ -n ${IVERILOG_IVL_DIR:-} ]]; then rtl_options=(-B "$IVERILOG_IVL_DIR"); fi
for pipeline in 0 1; do
    "${IVERILOG:-iverilog}" "${rtl_options[@]}" -g2012 -s rtl_virtual_tb -P "rtl_virtual_tb.PIPELINED=$pipeline" \
        -o "$rtl_build_dir/test.vvp" rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv rtl/rv32im/core.sv \
        rtl/rv32im/pipeline.sv rtl/rv32im/pipeline_axi.sv rtl/rv32im/sv32.sv rtl/rv32im/sv32_axi.sv \
        rtl/rv32im/virtual_core.sv tests/rtl_virtual_tb.sv 2>"$rtl_build_dir/compile.log" || {
            cat "$rtl_build_dir/compile.log" >&2; exit 1;
        }
    for scenario in normal flush_walk flush_stalled execute_fault accessed_fault store_fault load_fault physical_fault deny_pte deny_final; do
        "${VVP:-vvp}" "$rtl_build_dir/test.vvp" "+scenario=$scenario"
    done
done
