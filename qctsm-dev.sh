#!/usr/bin/env sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python_command=${QCOPILOTS_TOOL_SEMANTIC_MATCHING_PYTHON:-python3}
exec "$python_command" "$script_dir/tools/qctsm-dev.py" "$@"
