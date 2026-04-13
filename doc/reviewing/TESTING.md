# Testing Guidelines

All code changes should include appropriate tests. This document outlines
testing requirements and best practices for xdg-desktop-portal.

## Test Organization

XDG Desktop Portal uses two types of tests:

### 1. C Unit Tests (GLib Testing Framework)

Located in `tests/test-*.c`, these test individual functions in isolation.

**Files:**
- `test-permission-db.c` - Permission database tests
- `test-xdp-utils.c` - Utility function tests
- `test-xdp-method-info.c` - Method info tests

**Run with:**
```bash
meson test --suite unit
```

### 2. Python Integration Tests (pytest)

Located in `tests/test_*.py`, these test portal methods end-to-end in a fully
integrated environment.

**Files:**
- `test_account.py`, `test_clipboard.py`, `test_usb.py`, etc.
- One test file per portal

**Run with:**
```bash
meson test --suite integration
# or
./tests/run-test.sh
```

## Testing Requirements

### Required Tests for Changes

1. **New Portal Methods**
   - Integration test covering typical use cases
   - Tests for all supported options
   - Tests for error cases (invalid input, missing permissions)
   - Backend mock template if needed

2. **Bug Fixes**
   - Regression test demonstrating the bug
   - Test verifying the fix
   - Tests for related edge cases

3. **New Utility Functions**
   - C unit tests for new helper functions
   - Cover success and error paths
   - Test boundary conditions

4. **API Changes**
   - Tests for new options/parameters
   - Tests for backward compatibility
   - Version handling tests

## Writing Integration Tests (pytest)

### Basic Structure

```python
# tests/test_example.py
import tests.xdp_utils as xdp
import pytest

@pytest.fixture
def required_templates():
    """Specify backend templates needed for this test"""
    return {
        "Example": {},  # Uses default parameters
    }

class TestExample:
    def test_version(self, portals, dbus_con):
        """Check portal interface version"""
        xdp.check_version(dbus_con, "Example", 1)

    def test_basic_operation(self, portals, dbus_con):
        """Test basic portal functionality"""
        example_intf = xdp.get_portal_iface(dbus_con, "Example")

        # Create request
        request = xdp.Request(dbus_con, example_intf)
        response = request.call(
            "MethodName",
            arg1="value",
            options={}
        )

        # Verify response
        assert response
        assert response.response == 0  # Success
        assert response.results["key"] == "expected_value"
```

### Key Fixtures

#### Required by Most Tests

- **`portals`** - Starts portal frontend, backend templates, permission store
- **`dbus_con`** - D-Bus session bus connection
- **`required_templates`** - Dictionary of templates to load

#### Common Fixtures

- **`xdp_app_info`** - Parametric fixture testing all app info kinds (host,
  flatpak, snap, linyaps)
- **`xdg_desktop_portal_dir_files`** - Files to create in XDG_DESKTOP_PORTAL_DIR
- **`xdg_data_home_files`** - Files to create in XDG_DATA_HOME
- **`xdp_overwrite_env`** - Override environment variables
- **`template_params`** - Override template parameters for specific tests

### Common Patterns

#### Testing Portal Methods

```python
def test_portal_method(self, portals, dbus_con):
    """Test a portal method that returns via Request"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    # Call method via Request helper
    request = xdp.Request(dbus_con, portal_intf)
    response = request.call(
        "MethodName",
        arg1="value1",
        arg2="value2",
        options={
            "option_key": "option_value"
        }
    )

    # Verify response
    assert response.response == 0  # Success
    assert "result_key" in response.results
```

#### Testing Error Cases

```python
def test_invalid_input(self, portals, dbus_con):
    """Test that invalid input is rejected"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    request = xdp.Request(dbus_con, portal_intf)

    # This should fail
    with pytest.raises(Exception) as exc_info:
        request.call("MethodName", arg1="invalid")

    # Verify error details
    assert "Invalid" in str(exc_info.value)
```

#### Testing Sessions

```python
def test_session_lifecycle(self, portals, dbus_con):
    """Test creating and using a session"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    # Create session
    create_request = xdp.Request(dbus_con, portal_intf)
    create_response = create_request.call(
        "CreateSession",
        options={"session_handle_token": "test_session"}
    )

    assert create_response.response == 0
    session = xdp.Session.from_response(dbus_con, create_response)

    # Use session
    start_request = xdp.Request(dbus_con, portal_intf)
    start_response = start_request.call(
        "Start",
        session_handle=session.handle,
        parent_window="",
        options={}
    )

    assert start_response.response == 0
```

#### Waiting for Signals

```python
def test_signal_emission(self, portals, dbus_con):
    """Test that signals are emitted correctly"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    signal_received = False
    signal_data = None

    def signal_callback(arg1, arg2):
        nonlocal signal_received, signal_data
        signal_received = True
        signal_data = (arg1, arg2)

    portal_intf.connect_to_signal("SignalName", signal_callback)

    # Trigger signal
    trigger_signal()

    # Wait for signal
    xdp.wait_for(lambda: signal_received)

    # Verify signal data
    assert signal_data[0] == "expected"
```

#### Parametric Tests

```python
@pytest.mark.parametrize("option_value", [True, False])
def test_with_option(self, portals, dbus_con, option_value):
    """Test with different option values"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    request = xdp.Request(dbus_con, portal_intf)
    response = request.call(
        "MethodName",
        options={"option": option_value}
    )

    assert response.response == 0
```

#### Overriding Template Parameters

```python
@pytest.mark.parametrize(
    "template_params",
    [{"Example": {"response": 1, "delay": 500}}]
)
def test_backend_error(self, portals, dbus_con, template_params):
    """Test handling of backend errors"""
    portal_intf = xdp.get_portal_iface(dbus_con, "Example")

    request = xdp.Request(dbus_con, portal_intf)
    response = request.call("MethodName", options={})

    # Backend configured to return error (response: 1)
    assert response.response == 1
```

#### Testing Different App Info Kinds

```python
# Test automatically runs with host, flatpak, snap, linyaps app info
def test_works_for_all_app_types(self, portals, dbus_con, xdp_app_info):
    """Test that works for all app info kinds"""
    portal_intf = xdp.get_portal_iface(dbus_con, "PortalName")

    # xdp_app_info contains the current app info being tested
    assert xdp_app_info.app_id == "org.example.Test"

    # Test works regardless of app info kind
    request = xdp.Request(dbus_con, portal_intf)
    response = request.call("MethodName", options={})
    assert response.response == 0

# To test only specific app info kinds:
@pytest.mark.parametrize(
    "xdp_app_info",
    [xdp.AppInfoKind.FLATPAK],
    indirect=True
)
def test_flatpak_only(self, portals, dbus_con, xdp_app_info):
    """Test specific to flatpak apps"""
    assert xdp_app_info.kind == xdp.AppInfoKind.FLATPAK
```

## Creating Backend Mock Templates

Templates in `tests/templates/` implement backend behavior using dbusmock.

### Template Structure

```python
# tests/templates/example.py
from tests.templates.xdp_utils import Response, init_logger, ImplRequest
import dbus.service
from dataclasses import dataclass

BUS_NAME = "org.freedesktop.impl.portal.Test"
MAIN_OBJ = "/org/freedesktop/portal/desktop"
SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.impl.portal.Example"
VERSION = 1

logger = init_logger(__name__)

@dataclass
class ExampleParameters:
    delay: int
    response: int

def load(mock, parameters={}):
    """Called when template is loaded"""
    logger.debug(f"Loading parameters: {parameters}")

    mock.example_params = ExampleParameters(
        delay=parameters.get("delay", 200),
        response=parameters.get("response", 0),
    )

    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary({
            "version": dbus.UInt32(parameters.get("version", VERSION)),
        }),
    )

@dbus.service.method(
    MAIN_IFACE,
    in_signature="ossa{sv}",
    out_signature="ua{sv}",
    async_callbacks=("cb_success", "cb_error"),
)
def MethodName(self, handle, app_id, parent_window, options, cb_success, cb_error):
    """Implement backend method"""
    logger.debug(f"MethodName({handle}, {app_id}, {parent_window}, {options})")
    params = self.example_params

    request = ImplRequest(
        self, BUS_NAME, handle, logger, cb_success, cb_error
    )

    # Build response
    results = {
        "key": "value",
    }

    # Respond after delay
    request.respond(Response(params.response, results), delay=params.delay)
```

### Adding New Templates

1. Create `tests/templates/example.py`
2. Add to `tests/meson.build`:
   ```meson
   templates += files('templates/example.py')
   ```
3. Add to `xdg_desktop_portal_dir_default_files` in `conftest.py`:
   ```python
   portals = [
       ...
       "org.freedesktop.impl.portal.Example",
   ]
   ```

## Writing C Unit Tests

### Basic Structure

```c
#include <glib.h>
#include "xdp-utils.h"

static void
test_function_name (void)
{
  g_autoptr(GError) error = NULL;

  /* Test setup */
  int result = function_under_test (params, &error);

  /* Assertions */
  g_assert_true (result);
  g_assert_no_error (error);
}

static void
test_error_case (void)
{
  g_autoptr(GError) error = NULL;

  int result = function_under_test (invalid_params, &error);

  g_assert_false (result);
  g_assert_error (error, EXPECTED_DOMAIN, EXPECTED_CODE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/path/test-name", test_function_name);
  g_test_add_func ("/path/error-case", test_error_case);

  return g_test_run ();
}
```

### Add to meson.build

```meson
test_example = executable(
  'test-example',
  'test-example.c',
  dependencies: [common_deps],
  include_directories: [common_includes],
)
test(
  'unit/example',
  test_example,
  suite: ['unit'],
  env: env_tests,
)
```

## Running Tests

### Run All Tests
```bash
meson test -C builddir
```

### Run Specific Suite
```bash
meson test -C builddir --suite unit
meson test -C builddir --suite integration
```

### Run Single Test
```bash
meson test -C builddir unit/xdp-utils
meson test -C builddir integration/clipboard
```

### Run with Verbose Output
```bash
meson test -C builddir --verbose
```

### Run Under Valgrind
```bash
XDP_TEST_VALGRIND=1 meson test -C builddir
```

### Run with Debug Output
```bash
G_MESSAGES_DEBUG=all meson test -C builddir --verbose
```

### Run Specific pytest Test
```bash
./tests/run-test.sh ./tests/test_clipboard.py::TestClipboard::test_version -v
```

## Environment Variables

### Required (Set automatically by meson)
- `XDG_DESKTOP_PORTAL_PATH` - Path to xdg-desktop-portal binary
- `XDG_PERMISSION_STORE_PATH` - Path to xdg-permission-store binary
- `XDG_DOCUMENT_PORTAL_PATH` - Path to xdg-document-portal binary
- `XDP_VALIDATE_ICON` - Path to icon validator
- `XDP_VALIDATE_SOUND` - Path to sound validator

### Optional Testing Control
- `XDP_TEST_IN_CI` - Skip unreliable tests, run less thoroughly
- `XDP_TEST_RUN_LONG` - Run more iterations, test more thoroughly
- `XDP_TEST_VALGRIND` - Run tests under Valgrind
- `FLATPAK_BWRAP` - Path to bwrap executable

### Optional Debugging
- `G_MESSAGES_DEBUG=all` - Enable debug output
- `XDP_DBUS_MONITOR` - Start dbus-monitor on test bus
- `XDP_DBUS_TIMEOUT` - D-Bus call timeout in ms (default: 5000)
- `XDG_DESKTOP_PORTAL_WAIT_FOR_DEBUGGER` - Wait for debugger (raises SIGSTOP)

## Test Guidelines

### Do's
- Test both success and error paths
- Use descriptive test names
- Keep tests focused on one thing
- Use fixtures appropriately
- Test edge cases and boundary conditions
- Make tests deterministic (no race conditions)
- Clean up resources properly

### Don'ts
- Don't skip writing tests
- Don't use hardcoded timeouts (use `xdp.wait_for`)
- Don't test implementation details
- Don't share state between tests
- Don't assume test execution order
- Don't leave debug output in tests

## Review Checklist

### For New Tests
- [ ] Test file added to `tests/meson.build`
- [ ] Backend template created if needed
- [ ] Required templates listed in `conftest.py`
- [ ] Tests cover success cases
- [ ] Tests cover error cases
- [ ] Tests cover edge cases
- [ ] No hardcoded timeouts
- [ ] Tests are deterministic
- [ ] Test names are descriptive

### For All Changes
- [ ] Existing tests still pass
- [ ] New functionality has tests
- [ ] Bug fixes have regression tests
- [ ] No resource leaks
- [ ] Works with all app info kinds (if relevant)

## Common Utilities

### From `tests.xdp_utils`

```python
# Portal interaction
xdp.get_portal_iface(dbus_con, "PortalName")
xdp.Request(dbus_con, interface)
xdp.Session.from_response(dbus_con, response)

# Waiting
xdp.wait(ms)  # Wait milliseconds
xdp.wait_for(lambda: condition)  # Wait until condition true

# Checking
xdp.check_version(dbus_con, "PortalName", expected_version)

# Environment
xdp.is_in_ci()
xdp.is_valgrind()
xdp.run_long_tests()
```

## Debugging Test Failures

### Get More Information
```bash
# Verbose output
meson test -C builddir --verbose test_name

# Debug messages
G_MESSAGES_DEBUG=all meson test -C builddir test_name

# D-Bus monitor
XDP_DBUS_MONITOR=1 meson test -C builddir test_name

# Single test with pytest directly
cd builddir
pytest ../tests/test_example.py::TestClass::test_method -v -s
```

### Attach Debugger
```bash
# Make portal wait for debugger
XDG_DESKTOP_PORTAL_WAIT_FOR_DEBUGGER=1 pytest ../tests/test_example.py -v -s

# In another terminal, attach:
gdb -p $(pidof xdg-desktop-portal)
```

### Common Issues
- **Timeout**: Increase `XDP_DBUS_TIMEOUT` or fix async handling
- **Race condition**: Use `xdp.wait_for()` instead of `xdp.wait()`
- **App info issues**: Check fixture parametrization
- **Backend errors**: Check template implementation
- **Permission errors**: Check app info and permission store setup

## Further Reading

- [pytest documentation](https://docs.pytest.org/)
- [dbusmock documentation](https://github.com/martinpitt/python-dbusmock)
- [GLib Testing](https://docs.gtk.org/glib/testing.html)
- `tests/README.md` - Test suite overview
