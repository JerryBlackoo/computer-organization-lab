#!/usr/bin/env bash
set -euo pipefail
smoke_supervisor_dir=$(cd -- "$(dirname -- "$0")" && pwd)
smoke_supervisor_machine=${1:?Usage: check_supervisor.sh path/to/systemc_machine [output-directory]}
smoke_supervisor_output=${2:-/tmp/colab-supervisor-smoke}
bash "$smoke_supervisor_dir/build.sh" "$smoke_supervisor_output" supervisor
"${PYTHON:-python3}" "$smoke_supervisor_dir/verify_supervisor.py" \
    --machine "$smoke_supervisor_machine" --elf "$smoke_supervisor_output/program.elf"
