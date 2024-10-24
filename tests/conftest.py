# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from typing import Any, Iterator

import pytest
import dbusmock

from tests import PortalMock


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
def portal_mock(
    dbus_test_case, portal_name, required_templates, template_params, app_id
) -> PortalMock:
    """
    Fixture yielding a PortalMock object with the impl started, if applicable.
    """
    pmock = PortalMock(dbus_test_case, portal_name, app_id)
    for template, params in required_templates.items():
        params = template_params.get(template, params)
        pmock.start_template(template, params)
    pmock.start_xdp()
    return pmock
