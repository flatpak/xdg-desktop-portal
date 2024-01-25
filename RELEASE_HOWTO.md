# Steps for doing a xdg-desktop-portal release

- Make sure the version in `meson.build` is correct
- Add your changelog to the `NEWS` file
- Run the "Release new version" GitHub Action
  - The options are taken from the last `NEWS` entry by default, you may override them if needed
- Bump version in `meson.build`
- Update `SECURITY.md` if this is a new stable release
- Update `.github/ISSUE_TEMPLATE/bug-report.yml` if this is a new stable release
