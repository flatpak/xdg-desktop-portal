#!/bin/bash
#
# - Sets up the required environment to run tests based on the build dir
# - If another build directory is used, it can be set in the BUILDDIR env
#   variable
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

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

BUILDDIR="${BUILDDIR:-$SCRIPT_DIR/../build}"

[ ! -d $BUILDDIR ] && echo "Env BUILDDIR must be set to the build dir" && exit 1

export XDP_VALIDATE_SOUND=$BUILDDIR/src/xdg-desktop-portal-validate-sound
export XDP_VALIDATE_ICON=$BUILDDIR/src/xdg-desktop-portal-validate-icon
export XDG_DESKTOP_PORTAL_PATH=$BUILDDIR/src/xdg-desktop-portal
export XDG_DOCUMENT_PORTAL_PATH=$BUILDDIR/document-portal/xdg-document-portal
export XDG_PERMISSION_STORE_PATH=$BUILDDIR/document-portal/xdg-permission-store
exec pytest-3 $@

