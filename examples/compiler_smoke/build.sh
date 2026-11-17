#!/usr/bin/env bash
set -euo pipefail
smoke_source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
smoke_output_dir=${1:-/tmp/colab-compiler-smoke}
smoke_clang=${CLANG:-clang}
smoke_variant=${2:-machine}
case "$smoke_variant" in
    machine) smoke_start=start.S; smoke_main=main.c; smoke_link=link.ld ;;
    user) smoke_start=user_start.S; smoke_main=user_main.c; smoke_link=user.ld ;;
    supervisor) smoke_start=supervisor_start.S; smoke_main=supervisor_main.c; smoke_link=supervisor.ld ;;
    paged) smoke_start=paged_start.S; smoke_main=paged_main.c; smoke_link=paged.ld ;;
    *) printf 'Unknown smoke variant: %s\n' "$smoke_variant" >&2; exit 1 ;;
esac
smoke_linker=${LD_LLD:-}
if [[ -z "$smoke_linker" ]]; then
    for smoke_candidate in ld.lld ld.lld-18 ld.lld-17 rust-lld; do
        if command -v "$smoke_candidate" >/dev/null 2>&1; then
            smoke_linker=$(command -v "$smoke_candidate")
            break
        fi
    done
fi
if [[ -z "$smoke_linker" ]]; then
    for smoke_candidate in "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld; do
        if [[ -x "$smoke_candidate" ]]; then smoke_linker=$smoke_candidate; break; fi
    done
fi
if [[ -z "$smoke_linker" ]]; then
    printf 'Set LD_LLD to an executable LLVM lld or rust-lld.\n' >&2
    exit 1
fi
mkdir -p -- "$smoke_output_dir"
smoke_flags=(--target=riscv32 -march=rv32im_zicsr_zifencei -mabi=ilp32 -mno-relax -msmall-data-limit=0
    -ffreestanding -fno-builtin -fno-pic -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -O1)
"$smoke_clang" "${smoke_flags[@]}" -c "$smoke_source_dir/$smoke_start" -o "$smoke_output_dir/start.o"
"$smoke_clang" "${smoke_flags[@]}" -c "$smoke_source_dir/$smoke_main" -o "$smoke_output_dir/main.o"
smoke_link_flags=(-m elf32lriscv)
if [[ ${smoke_linker##*/} == rust-lld ]]; then smoke_link_flags=(-flavor gnu "${smoke_link_flags[@]}"); fi
"$smoke_linker" "${smoke_link_flags[@]}" -T "$smoke_source_dir/$smoke_link" \
    "$smoke_output_dir/start.o" "$smoke_output_dir/main.o" -o "$smoke_output_dir/program.elf"
printf '%s\n' "$smoke_output_dir/program.elf"
