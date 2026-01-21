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
- If this is the first release of a new stable release series, update
  `SECURITY.md`. It should list the development branch, new stable branch, and
  old stable branch as currently maintained.
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
 - The tip of the branch is a release commit (e.g. `1.19.4`)
 - The version in `meson.build` is correct

Then create the tag:

```sh
$ git evtag sign ${version}
$ git push -u origin ${version}
```

Copy paste the release notes from NEWS.md into the tag message when running
`git evtag`.

### Post-Release

- Update version number in `meson.build` to the next release version
- Start a new section in `NEWS.md`
```md
Changes in ${nextVersion}
=================
Released: Not yet

...
```

- Commit the changes as "Post-release version bump" and push to the
  corresponding branch

### Post-Branching

After creating a stable branch:
 
- Update version number in `meson.build` to the next unstable release version.
  For example if the created stable branch name is xdg-desktop-portal-1.N, bump
  the version to 1.N+1.0. Commit as "Post-branching version bump".
- Update `.github/ISSUE_TEMPLATE/bug-report.yml`. It should list the
  development version, and the currently maintained stable versions, under "XDG
  Desktop Portal version".
- Update the pending entry in NEWS.md to refer to the next unstable version.
- Push changes to the `main` branch.
