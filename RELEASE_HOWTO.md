<!--
SPDX-License-Identifier: LGPL-2.1-or-later
SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
-->

# Steps for doing a xdg-desktop-portal release

### Prepare the release

- Make sure the version in `meson.build` is correct
- Create a branch
```sh
$ git checkout -b release-${version}
```
- Update translations
```sh
$ ninja -C ${builddir} xdg-desktop-portal-update-po
$ git add po/
$ git commit -m "Update translations"
```
- Update `SECURITY.md`
- Update `.github/ISSUE_TEMPLATE/bug-report.yml`
- Add your changelog to the `NEWS.md` file
```sh
$ # Find PRs for the release
$ gh pr list \
    --search "$(echo $(git log "${prev_version}..origin/main" --format=%H) | tr ' ' ',')" \
    --state merged --limit 999 \
    --json number,mergedAt,title,url
$ # Adjust NEWS.md
$ git add NEWS.md
$ git commit -m ${version}
```
- Push your branch
```sh
$ git push -u ${fork} release-${version}
```
- Open a pull request
- Merge the pull request and wait for it to reach the `main` branch or the
  stable branch

### Create a Signed Tag

**NOTE**: Only project maintainers can create a tag.

Make sure that:
 - You're on the `main` branch, or a stable branch;
 - The tip of the branch is a release commit (e.g. `23.0`)
 - The version in `meson.build` is correct

Then create the tag:
```sh
$ git evtag sign ${version} # Use ${version} as the tag message
$ git push -u origin ${version}
```

### Post-Release

Create the version branch: `git branch xdg-desktop-portal-${version}`

On main and on the version branch:
- Update version number in `meson.build` to the next release version
- Start a new section in `NEWS.md`
```md
Changes in ${nextVersion}
=================
Released: Not yet

...
```
- Commit the changes as "Post-release version bump" and push to the
  corresponding branches
