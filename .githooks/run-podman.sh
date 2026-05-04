#!/usr/bin/env bash

set -eo pipefail
set -x

RUN="${TOOLBOX_PATH:+flatpak-spawn --host}"

if $RUN command -v podman &>/dev/null; then
  exec $RUN podman run --rm --workdir "/src" -v "$PWD:/src:Z" $@
elif $RUN command -v docker &>/dev/null; then
  exec $RUN docker run --rm --workdir "/src" -v "$PWD:/src:Z" $@
else
  echo '`podman` and `docker` not found.'
  exit 1
fi
