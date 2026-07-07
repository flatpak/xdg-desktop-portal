<!--
SPDX-License-Identifier: LGPL-2.1-or-later
SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
-->

# XDP Git Hooks

Enable the git hooks:

  git config --local core.hooksPath githooks/

Run `githooks/run` with `--files path/to files` or `--all-files` to run
pre-commit on the specific or all files.

Run `githooks/gitlint <commit-range>` to check commit messages, e.g.
`githooks/gitlint main..HEAD`.

Run `githooks/build` to rebuild the image.

The hooks will build an OCI image which contains all the tools and programs we
need to run the hooks, and then runs a podman/docker container to run the
checks. We use the same Containerfile in the CI to minimize differences.
