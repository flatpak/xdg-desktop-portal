# Steps for doing a xdg-desktop-portal release

 - git clean -fxd
 - meson setup . _build && meson compile -C _build/ xdg-desktop-portal-update-po
 - git add po/*po &&  git commit -m "Update po files"
 - git clean -fxd
 - add content to NEWS
 - git commit -m <version>
 - git push origin main
 - meson setup . _build -Ddocbook-docs=enabled 
 - meson dist -C _build
 - git tag <version>
 - git push origin refs/tags/<version>
 - upload tarball to github as release
 - edit release, copy NEWS section in
 - update portal api docs in the gh-pages branch
 - bump version in meson.build
 - git commit -m "Post-release version bump"
 - git push origin main
 - Update SECURITY.md if this is a new stable release
