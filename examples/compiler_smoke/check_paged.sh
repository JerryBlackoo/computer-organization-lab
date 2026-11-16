#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
machine=${1:?Usage: check_paged.sh machine [output-dir]}
output=${2:-/tmp/colab-paged-smoke}
bash "$source_dir/build.sh" "$output" paged
"${PYTHON:-python3}" "$source_dir/verify_paged.py" --machine "$machine" --elf "$output/program.elf"
