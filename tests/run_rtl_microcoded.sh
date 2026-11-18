#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
test_dir=$(mktemp -d /tmp/colab-microcoded-rtl.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror examples/control_unit/export_microcode.cpp -o "$test_dir/export"
"$test_dir/export" > "$test_dir/rv32_control.hex"
cmp rtl/microcoded/rv32_control.hex "$test_dir/rv32_control.hex"
"$test_dir/export" vertical > "$test_dir/rv32_vertical.hex"
cmp rtl/microcoded/rv32_vertical.hex "$test_dir/rv32_vertical.hex"
compiler=("${IVERILOG:-iverilog}")
if [[ -n "${IVERILOG_IVL_DIR:-}" ]]; then compiler+=(-B "$IVERILOG_IVL_DIR"); fi
"${compiler[@]}" -g2012 -s rtl_microcoded_tb -o "$test_dir/test.vvp" rtl/microcoded/rv32_microcoded.sv tests/rtl_microcoded_tb.sv
"${VVP:-vvp}" "$test_dir/test.vvp"
"${compiler[@]}" -g2012 -s rtl_microcoded_tb -P rtl_microcoded_tb.VERTICAL=1 -P 'rtl_microcoded_tb.ROM_FILE="rtl/microcoded/rv32_vertical.hex"' -o "$test_dir/test-vertical.vvp" rtl/microcoded/rv32_microcoded.sv tests/rtl_microcoded_tb.sv
"${VVP:-vvp}" "$test_dir/test-vertical.vvp"

"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror tests/microcoded_lockstep.cpp -o "$test_dir/lockstep"
"$test_dir/lockstep" > "$test_dir/cpp.trace"
"${compiler[@]}" -g2012 -s rtl_microcoded_lockstep_tb -o "$test_dir/lockstep.vvp" rtl/microcoded/rv32_microcoded.sv tests/rtl_microcoded_lockstep_tb.sv
"${VVP:-vvp}" "$test_dir/lockstep.vvp" | sed -n '/^TRACE /p' > "$test_dir/rtl.trace"
diff -u "$test_dir/cpp.trace" "$test_dir/rtl.trace"
printf 'PASS C++/RTL cycle lockstep (%s cycles)\n' "$(wc -l < "$test_dir/cpp.trace")"

"$test_dir/lockstep" vertical > "$test_dir/cpp-vertical.trace"
"${compiler[@]}" -g2012 -s rtl_microcoded_lockstep_tb -P rtl_microcoded_lockstep_tb.VERTICAL=1 -P 'rtl_microcoded_lockstep_tb.ROM_FILE="rtl/microcoded/rv32_vertical.hex"' -o "$test_dir/vertical.vvp" rtl/microcoded/rv32_microcoded.sv tests/rtl_microcoded_lockstep_tb.sv
"${VVP:-vvp}" "$test_dir/vertical.vvp" | sed -n '/^TRACE /p' > "$test_dir/rtl-vertical.trace"
diff -u "$test_dir/cpp-vertical.trace" "$test_dir/rtl-vertical.trace"
printf 'PASS vertical C++/RTL cycle lockstep (%s cycles)\n' "$(wc -l < "$test_dir/cpp-vertical.trace")"
