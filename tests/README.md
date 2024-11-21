xdg-desktop-portal test suite
=============================

## Environment

Some relevant environment variables that can be set during testing,
but should not normally be set on production systems:

* `FLATPAK_BWRAP`: Path to the **bwrap**(1) executable
    (default: discovered at build-time)

* `LIBEXECDIR`: If set, look for the x-d-p executable in this directory

* `TEST_IN_CI`: If set (to any value), some tests that are not always
    reliable are skipped.
    Set this for automated QA testing, leave it unset during development.

* `XDP_VALIDATE_ICON_INSECURE`: If set (to any value), x-d-p doesn't
    sandbox the icon validator using **bwrap**(1), even if sandboxed
    validation was enabled at compile time.
    This can be used to run build-time tests in a chroot or unprivileged
    container environment, where **bwrap**(1) normally can't work.
    It should never be set on a production system that will be validating
    untrusted icons!

* `XDP_VALIDATE_SOUND_INSECURE`: Same as `XDP_VALIDATE_ICON_INSECURE`,
    but for sounds

### Used automatically

These environment variables are set automatically and shouldn't need to be
changed, but developers improving the test suite might need to be aware
of them:

* `XDG_DESKTOP_PORTAL_DIR`: If set, it will be used instead of the
    compile-time path (normally `/usr/share/xdg-desktop-portal/portals`)

* `XDP_UNINSTALLED`: Set to 1 when running build-time tests on a version
    of x-d-p that has not yet been installed. Leave unset when running
    "as-installed" tests on the system copy of x-d-p.

* `XDP_VALIDATE_ICON`: Path to `x-d-p-validate-icon` executable in the
    build directory

* `XDP_VALIDATE_SOUND`: Path to `x-d-p-validate-sound` executable in the
    build directory
