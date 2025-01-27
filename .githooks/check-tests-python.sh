#!/usr/bin/env bash
set -euo pipefail

PYTHON=${PYTHON:-python3}

HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="${HERE}/.venv"

[ -d $VENV ] || $PYTHON -m venv $VENV
source $VENV/bin/activate


$PYTHON -m ruff --version >/dev/null || $PYTHON -m pip install ruff
$PYTHON -m mypy --version >/dev/null || $PYTHON -m pip install mypy
$PYTHON -m pytest --version >/dev/null || $PYTHON -m pip install pytest

cd tests

python3 -m ruff check .  || CHECK_FAILED=1
python3 -m ruff format --check . || CHECK_FAILED=1
mypy .  || CHECK_FAILED=1

[ -z ${CHECK_FAILED+x} ] || exit 1
