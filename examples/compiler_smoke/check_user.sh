#!/usr/bin/env bash
set -euo pipefail
user_smoke_dir=$(cd -- "$(dirname -- "$0")" && pwd)
user_smoke_machine=${1:?Usage: check_user.sh path/to/systemc_machine [output-directory]}
user_smoke_output=${2:-/tmp/colab-user-smoke}
bash "$user_smoke_dir/build.sh" "$user_smoke_output" user
"${PYTHON:-python3}" "$user_smoke_dir/verify_user.py" --machine "$user_smoke_machine" --elf "$user_smoke_output/program.elf"
