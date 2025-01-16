#!/bin/sh
# Copyright 2025 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu
e=0
"$@" || e="$?"

if [ "$e" = 5 ]; then
    # pytest exits with status 5 if all tests were skipped.
    # Meson and ginsttest-runner expect tests to exit with status 77 in
    # this situation, like Automake
    exit 77
fi

exit "$e"
