#!/bin/bash
#
# - Runs pytest with the required environment to run tests on an x-d-p build
# - By default, the tests run on the first x-d-p build directory that is found
#   inside the source tree
# - The BUILDDIR environment variable can be set to a specific x-d-p build
#   directory
# - All arguments are passed along to pytest
# - Check tests/README.md for useful environment variables
#
# Examples:
#
#   ./run-test.sh ./test_camera.py -k test_version -v -s
#
#   BUILDDIR=../_build ./run-test.sh ./test_usb.py
#

set -euo pipefail

function fail()
{
  sed -n '/^#$/,/^$/p' "${BASH_SOURCE[0]}"
  echo "$1"
  exit 1
}

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

PYTEST=$(command -v "pytest-3" || command -v "pytest") || fail "pytest is missing"

BUILDDIR=${BUILDDIR:-$(find "${SCRIPT_DIR}/.." -maxdepth 2 -name "build.ninja" -printf "%h\n" -quit)}

[ ! -f "${BUILDDIR}/build.ninja" ] && fail "Path '${BUILDDIR}' does not appear to be a build dir"

echo "Running tests on build dir: $(readlink -f "${BUILDDIR}")"
echo ""

export XDP_VALIDATE_SOUND="$BUILDDIR/src/xdg-desktop-portal-validate-sound"
export XDP_VALIDATE_ICON="$BUILDDIR/src/xdg-desktop-portal-validate-icon"
export XDG_DESKTOP_PORTAL_PATH="$BUILDDIR/src/xdg-desktop-portal"
export XDG_DOCUMENT_PORTAL_PATH="$BUILDDIR/document-portal/xdg-document-portal"
export XDG_PERMISSION_STORE_PATH="$BUILDDIR/document-portal/xdg-permission-store"

if [ ! -v UNDER_MESON ]; then
  meson compile -C "${BUILDDIR}"
fi

exec "$PYTEST" "$@"
