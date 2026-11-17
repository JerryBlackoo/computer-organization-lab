#!/usr/bin/env bash
set -euo pipefail
rtl_source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
rtl_output_dir=${1:-/tmp/colab-rtl-smoke}
rtl_compiler=${CLANG:-clang}
rtl_linker=${LD_LLD:-}
if [[ -z "$rtl_linker" ]]; then
    for candidate in ld.lld ld.lld-18 ld.lld-17 rust-lld; do
        if command -v "$candidate" >/dev/null 2>&1; then rtl_linker=$(command -v "$candidate"); break; fi
    done
fi
if [[ -z "$rtl_linker" ]]; then
    for candidate in "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld; do
        if [[ -x "$candidate" ]]; then rtl_linker=$candidate; break; fi
    done
fi
if [[ -z "$rtl_linker" ]]; then printf 'LLVM lld is required; set LD_LLD.\n' >&2; exit 1; fi
mkdir -p -- "$rtl_output_dir"
rtl_flags=(--target=riscv32 -march=rv32im -mabi=ilp32 -mno-relax -msmall-data-limit=0
    -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -O1)
"$rtl_compiler" "${rtl_flags[@]}" -c "$rtl_source_dir/start.S" -o "$rtl_output_dir/start.o"
"$rtl_compiler" "${rtl_flags[@]}" -c "$rtl_source_dir/main.c" -o "$rtl_output_dir/main.o"
rtl_link_flags=(-m elf32lriscv)
if [[ ${rtl_linker##*/} == rust-lld ]]; then rtl_link_flags=(-flavor gnu "${rtl_link_flags[@]}"); fi
"$rtl_linker" "${rtl_link_flags[@]}" -T "$rtl_source_dir/link.ld" "$rtl_output_dir/start.o" "$rtl_output_dir/main.o" -o "$rtl_output_dir/program.elf"
printf '%s\n' "$rtl_output_dir/program.elf"
