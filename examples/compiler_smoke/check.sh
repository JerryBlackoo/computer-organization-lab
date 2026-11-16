#!/usr/bin/env bash
set -euo pipefail
smoke_check_dir=$(cd -- "$(dirname -- "$0")" && pwd)
smoke_machine=${1:?Usage: check.sh path/to/systemc_machine [output-directory]}
smoke_check_output=${2:-/tmp/colab-compiler-smoke}
bash "$smoke_check_dir/build.sh" "$smoke_check_output"
"${PYTHON:-python3}" "$smoke_check_dir/verify.py" --machine "$smoke_machine" --elf "$smoke_check_output/program.elf"
