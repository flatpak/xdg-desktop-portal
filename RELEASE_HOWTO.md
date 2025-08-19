# Steps for doing a xdg-desktop-portal release

### Prepare the release

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
- Update `.github/ISSUE_TEMPLATE/bug-report.yml`. It should list `main`, and the
  new currently maintained major releases, under "XDG Desktop Portal version".
- Update version number in `meson.build` to the next version
- If this is a major release, update `SECURITY.md`. It should list the
  development version, the current version, and the previous, still maintained
  version.
- Add your changelog to a new section in the `NEWS.md` file
```md
Changes in ${version}
=================
Released: ${date}

...
```
- Commit changes and push your branch
```sh
$ git add ...
$ git commit -m ${version}
$ git push -u ${fork} release-${version}
```
- Open a pull request against the `main` branch for a major release, or the
  major release branch (e.g. `xdg-desktop-portal-4`) for a minor release
- Merge the pull request and wait for it to get merged and CI to succeed

### Create a Signed Tag

**NOTE**: Only project maintainers can create a tag.

Make sure that:
 - You're on the correct branch (`main` or a major release branch)
 - The tip of the branch is a release commit (e.g. `4` on the `main` branch,
   or `5.1` on a major release branch)
 - The version in `meson.build` is correct

Then create the tag:

```sh
$ git evtag sign ${version}
$ git push -u origin ${version}
```

Copy paste the release notes from NEWS.md into the tag message when running
`git evtag`.

### Branching

When appropriate (usually a bit after a major release), create a release branch
(e.g. `xdg-desktop-portal-4` for the major version 4 series) and resume feature
development on the `main` branch.
