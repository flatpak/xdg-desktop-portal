# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from typing import Any, Iterator

import pytest
import dbus
import dbusmock
import os
import sys
import tempfile

from tests import PortalMock


def pytest_configure():
    ensure_umockdev_loaded()
    create_test_dirs()


def ensure_umockdev_loaded():
    umockdev_preload = "libumockdev-preload.so"
    preload = os.environ.get("LD_PRELOAD", "")
    if umockdev_preload not in preload:
        os.environ["LD_PRELOAD"] = f"{umockdev_preload}:{preload}"
        os.execv(sys.executable, [sys.executable] + sys.argv)


def create_test_dirs():
    env_dirs = [
        "HOME",
        "TMPDIR",
        "XDG_CACHE_HOME",
        "XDG_CONFIG_HOME",
        "XDG_DATA_HOME",
        "XDG_RUNTIME_DIR",
    ]

    test_root = tempfile.TemporaryDirectory(
        prefix="xdp-testroot-", ignore_cleanup_errors=True
    )

    for env_dir in env_dirs:
        directory = test_root / env_dir.lower()
        directory.mkdir(mode=0o700, parents=True)
        os.environ[env_dir] = directory.absolute()

    yield

    test_root.cleanup()


@pytest.fixture()
def dbus_test_case() -> Iterator[dbusmock.DBusTestCase]:
    """
    Fixture to yield a DBusTestCase with a started session bus.
    """
    bus = dbusmock.DBusTestCase()
    bus.setUp()
    bus.start_session_bus()
    bus.start_system_bus()
    con = bus.get_dbus(system_bus=False)
    con_sys = bus.get_dbus(system_bus=True)
    assert con
    assert con_sys
    setattr(bus, "dbus_con", con)
    setattr(bus, "dbus_con_sys", con_sys)
    yield bus
    bus.tearDown()
    bus.tearDownClass()


@pytest.fixture
def portal_name() -> str:
    raise NotImplementedError("All test files need to define the portal_name fixture")


@pytest.fixture
def portal_has_impl() -> bool:
    """
    Default fixture for signaling that a portal has an impl.portal as well.

    For tests of portals that do not have an impl, override this fixture to
    return False in the respective test_foo.py.
    """
    return True


@pytest.fixture
def params() -> dict[str, Any]:
    """
    Default fixture providing empty parameters that get passed to the impl.portal.
    To use this in test cases, pass the parameters via

        @pytest.mark.parametrize("params", ({"foo": "bar"}, ))

    Note that this must be a tuple as pytest will iterate over the value.
    """
    return {}


@pytest.fixture
def template_params(portal_name, params) -> dict[str, dict[str, Any]]:
    """
    Default fixture for overriding the parameters which should be passed to the
    mocking templates. Use required_templates to specify the default parameters
    and override it for specific test cases via

        @pytest.mark.parametrize("template_params", ({"Template": {"foo": "bar"}},))

    """
    return {portal_name: params}


@pytest.fixture
def required_templates(portal_name, portal_has_impl) -> dict[str, dict[str, Any]]:
    """
    Default fixture for enumerating the mocking templates the test case requires
    to be started. This is a map from a name of a template in the templates
    directory to the parameters which should be passed to the template.
    """
    if portal_has_impl:
        return {portal_name: {}}

    return {}


@pytest.fixture
def app_id():
    """
    Default fixture providing the app id of the connecting process
    """
    return "org.example.App"


@pytest.fixture
def usb_queries():
    """
    Default fixture providing the usb queries the connecting process can
    enumerate
    """
    return None


@pytest.fixture
def umockdev():
    """
    Default fixture providing a umockdev testbed
    """
    return None


@pytest.fixture
def portal_mock(
    dbus_test_case,
    portal_name,
    required_templates,
    template_params,
    app_id,
    usb_queries,
    umockdev,
) -> PortalMock:
    """
    Fixture yielding a PortalMock object with the impl started, if applicable.
    """
    pmock = PortalMock(dbus_test_case, portal_name, app_id, usb_queries, umockdev)

    for template, params in required_templates.items():
        params = template_params.get(template, params)
        pmock.start_template(template, params)

    pmock.start_xdp()
    yield pmock

    pmock.tear_down()


@pytest.fixture
def portals(portal_mock) -> None:
    # will be used to bring up xdg-desktop-portal and dependencies
    pass


@pytest.fixture
def dbus_con(portal_mock) -> dbus.Bus:
    return portal_mock.dbus_con


@pytest.fixture
def dbus_con_sys(portal_mock) -> dbus.Bus:
    return portal_mock.dbus_con_sys
