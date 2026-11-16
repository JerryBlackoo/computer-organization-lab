#!/usr/bin/env bash
set -euo pipefail
systemc_example_dir=$(cd -- "$(dirname -- "$0")" && pwd)
systemc_build_dir=${COLAB_SYSTEMC_BUILD_DIR:-/tmp/colab-systemc-build}
cmake -S "$systemc_example_dir" -B "$systemc_build_dir" -DCOLAB_FETCH_SYSTEMC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$systemc_build_dir" --parallel 2
exec "$systemc_build_dir/systemc_machine" "$@"
