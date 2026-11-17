#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
"${YOSYS:-yosys}" -Q -T -p '
  read_verilog -sv rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv rtl/rv32im/core.sv
  hierarchy -check -top colab_rv32im_core
  proc; opt; check -assert
  select -assert-none t:$mul t:$div t:$mod
  stat
'
"${YOSYS:-yosys}" -Q -T -p '
  read_verilog -sv rtl/rv32im/sv32.sv
  hierarchy -check -top colab_sv32
  proc; opt; check -assert; stat
'
"${YOSYS:-yosys}" -Q -T -p '
  read_verilog -sv rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv rtl/rv32im/core.sv rtl/rv32im/pipeline.sv rtl/rv32im/pipeline_axi.sv rtl/rv32im/axi_cache.sv rtl/rv32im/cached_core.sv
  hierarchy -check -top colab_rv32im_cached_core
  proc; opt; check -assert
  select -assert-none t:$mul t:$div t:$mod
  stat
'
"${YOSYS:-yosys}" -Q -T -p '
  read_verilog -sv rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv rtl/rv32im/core.sv rtl/rv32im/pipeline.sv rtl/rv32im/pipeline_axi.sv rtl/rv32im/axi_cache.sv rtl/rv32im/cached_core.sv
  hierarchy -check -top colab_rv32im_cached_core -chparam PIPELINED 1
  proc; opt; check -assert
  select -assert-none t:$mul t:$div t:$mod
  stat
'
"${YOSYS:-yosys}" -Q -T -p '
  read_verilog -sv rtl/rv32im/alu32.sv rtl/rv32im/muldiv.sv rtl/rv32im/pipeline.sv
  hierarchy -check -top colab_rv32im_pipeline
  proc; opt; check -assert
  select -assert-none t:$mul t:$div t:$mod
  stat
'
