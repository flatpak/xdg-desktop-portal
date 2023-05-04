# XDG Desktop Portal - Contributing Guide

Before developing features or fixing bugs, please make sure you are have done
the following:

- Your code is not on the *main* branch of your fork
- The code has been tested
- All commit messages are properly formatted and commits squashed where
  appropriate
- You have included updates to all appropriate documentation

We use GitHub pull requests to review contributions. Please be kind and patient
as code reviews can be long and minutious.

## Development

xdg-desktop-portal usually runs as a user session service, initialized on
demand through D-Bus activation. It usually starts with the session though,
as many desktop environments try to talk to xdg-desktop-portal on startup.
xdg-desktop-portal initializes specific backends through D-Bus activation
as well.

### Building

To build xdg-desktop-portal, first make sure you have the build dependencies
installed through your distribution's package manager. With them installed,
run:

```
$ meson setup . _build
$ meson compile -C _build
```

### Running

xdg-desktop-portal needs to own the D-Bus name and replace the user session
service that might already be running. To do so, run:

```
$ _build/src/xdg-desktop-portal --replace
```

You may need to restart backends after replacing xdg-desktop-portal (please
replace `[name]` with the backend name, e.g. `gnome` or `kde` or `wlr`):

```
$ systemctl --user restart xdg-desktop-portal-[name].service
```

### Testing

To execute the test suite present in xdg-desktop-portal, make sure you built it
with `-Dlibportal=enabled`, and run:

```
$ meson test -C _build
```

