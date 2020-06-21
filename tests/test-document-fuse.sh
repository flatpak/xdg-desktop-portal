#!/bin/bash


skip() {
    echo "1..0 # SKIP" "$@"
    exit 0
}

skip_without_fuse () {
    fusermount --version >/dev/null 2>&1 || skip "no fusermount"

    capsh --print | grep -q 'Bounding set.*[^a-z]cap_sys_admin' || \
        skip "No cap_sys_admin in bounding set, can't use FUSE"

    [ -w /dev/fuse ] || skip "no write access to /dev/fuse"
    [ -e /etc/mtab ] || skip "no /etc/mtab"
}

skip_without_fuse

echo "1..2"

set -e

if [ -n "${G_TEST_SRCDIR:-}" ]; then
    test_srcdir="${G_TEST_SRCDIR}"
else
    test_srcdir=$(realpath $(dirname $0))
fi

if [ -n "${G_TEST_BUILDDIR:-}" ]; then
    test_builddir="${G_TEST_BUILDDIR}"
else
    test_builddir=$(realpath $(dirname $0))
fi

export TEST_DATA_DIR=`mktemp -d /tmp/xdp-XXXXXX`
mkdir -p ${TEST_DATA_DIR}/home
mkdir -p ${TEST_DATA_DIR}/runtime
mkdir -p ${TEST_DATA_DIR}/system
mkdir -p ${TEST_DATA_DIR}/config

export HOME=${TEST_DATA_DIR}/home
export XDG_CACHE_HOME=${TEST_DATA_DIR}/home/cache
export XDG_CONFIG_HOME=${TEST_DATA_DIR}/home/config
export XDG_DATA_HOME=${TEST_DATA_DIR}/home/share
export XDG_RUNTIME_DIR=${TEST_DATA_DIR}/runtime

cleanup () {
    fusermount -u $XDG_RUNTIME_DIR/doc || :
    sleep 0.1
    kill -9 $DBUS_SESSION_BUS_PID
    kill $(jobs -p) &> /dev/null || true
    rm -rf $TEST_DATA_DIR
}
trap cleanup EXIT

sed s#@testdir@#${test_builddir}# ${test_srcdir}/session.conf.in > session.conf

dbus-daemon --fork --config-file=session.conf --print-address=3 --print-pid=4 \
            3> dbus-session-bus-address 4> dbus-session-bus-pid
export DBUS_SESSION_BUS_ADDRESS="$(cat dbus-session-bus-address)"
DBUS_SESSION_BUS_PID="$(cat dbus-session-bus-pid)"

if ! kill -0 "$DBUS_SESSION_BUS_PID"; then
    assert_not_reached "Failed to start dbus-daemon"
fi

# Run portal manually so that we get any segfault our assert output
# Add -v here to get debug output from fuse
./xdg-document-portal -r &

# First run a basic single-thread test
echo Testing single-threaded
python3 ${test_srcdir}/test-document-fuse.py --iterations 3 -v
echo "ok single-threaded"

# Then a bunch of copies in parallel to stress-test
echo Testing in parallel
PIDS=()
for i in $(seq 20); do
    python3 ${test_srcdir}/test-document-fuse.py --iterations 10 --prefix $i &
    PID="$!"
    PIDS+=( "$PID" )
done

for PID in ${PIDS[@]}; do
    echo waiting for pid ${PID}
    wait ${PID}
done
echo "ok load-test"
