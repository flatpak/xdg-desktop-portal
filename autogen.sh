#!/bin/sh
# Run this to generate all the initial makefiles, etc.

REQUIRED_FLATPAK_VERSION=1.5.0

check_flatpak() {
        # This check is only needed when building from git, as
        # the tarball carries a copy of a new enough file
        if ! pkg-config --exists flatpak; then
                echo "*** flatpak $REQUIRED_FLATPAK_VERSION required ***"
                exit 1
        fi
        if ! pkg-config --atleast-version $REQUIRED_FLATPAK_VERSION flatpak; then
                FLATPAK_VERSION=$(pkg-config --modversion flatpak)
                echo "*** flatpak $REQUIRED_FLATPAK_VERSION required, $FLATPAK_VERSION found ***"
                exit 1
        fi
}

# Check whether docs are enabled, and if so, require the newest
# Flatpak so that docs can be built
for i in "$@"; do
        if [ x"$i" = x"--enable-docbook-docs" ]; then
                check_flatpak
        fi
done

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

olddir=`pwd`
cd "$srcdir"

AUTORECONF=`which autoreconf`
if test -z $AUTORECONF; then
        echo "*** No autoreconf found, please install it ***"
        exit 1
fi

# INSTALL are required by automake, but may be deleted by clean
# up rules. to get automake to work, simply touch these here, they will be
# regenerated from their corresponding *.in files by ./configure anyway.
touch INSTALL

autoreconf --force --install --verbose || exit $?

cd "$olddir"
test -n "$NOCONFIGURE" || "$srcdir/configure" "$@"
