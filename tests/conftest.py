# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

from typing import Any, Dict, Iterator, Optional, assert_never
from types import ModuleType

import pytest
import dbus
import dbusmock
import os
import sys
import tempfile
import subprocess
import time
import signal
import shutil

from pathlib import Path
from contextlib import chdir

import gi

gi.require_version("UMockdev", "1.0")
from gi.repository import UMockdev  # noqa E402


def pytest_configure() -> None:
    ensure_environment_set()
    ensure_umockdev_loaded()


def pytest_sessionfinish(session, exitstatus):
    # Meson and ginsttest-runner expect tests to exit with status 77 if all
    # tests were skipped
    if exitstatus == pytest.ExitCode.NO_TESTS_COLLECTED:
        session.exitstatus = 77


def ensure_environment_set() -> None:
    env_vars = [
        "XDG_DESKTOP_PORTAL_PATH",
        "XDG_PERMISSION_STORE_PATH",
        "XDG_DOCUMENT_PORTAL_PATH",
    ]

    if not os.getenv("XDP_VALIDATE_AUTO"):
        env_vars += [
            "XDP_VALIDATE_ICON",
            "XDP_VALIDATE_SOUND",
        ]
    else:
        os.environ.pop("XDP_VALIDATE_ICON", None)
        os.environ.pop("XDP_VALIDATE_SOUND", None)

    for env_var in env_vars:
        if not os.getenv(env_var):
            raise Exception(f"{env_var} must be set")


def ensure_umockdev_loaded() -> None:
    umockdev_preload = "libumockdev-preload.so"
    preload = os.environ.get("LD_PRELOAD", "")
    if umockdev_preload not in preload:
        os.environ["LD_PRELOAD"] = f"{umockdev_preload}:{preload}"
        os.execv(sys.executable, [sys.executable] + sys.argv)


def test_dir() -> Path:
    return Path(__file__).resolve().parent


@pytest.fixture
def xdg_desktop_portal_path() -> Path:
    return Path(os.environ["XDG_DESKTOP_PORTAL_PATH"])


@pytest.fixture
def xdg_permission_store_path() -> Path:
    return Path(os.environ["XDG_PERMISSION_STORE_PATH"])


@pytest.fixture
def xdg_document_portal_path() -> Path:
    return Path(os.environ["XDG_DOCUMENT_PORTAL_PATH"])


@pytest.fixture(autouse=True)
def create_test_dirs(umockdev: Optional[UMockdev.Testbed]) -> Iterator[None]:
    # The umockdev argument is to make sure the testbed
    # is created before we create the tmpdir
    env_dirs = [
        "HOME",
        "TMPDIR",
        "XDG_CACHE_HOME",
        "XDG_CONFIG_HOME",
        "XDG_DATA_HOME",
        "XDG_RUNTIME_DIR",
        "XDG_DESKTOP_PORTAL_DIR",
        "XDP_BIN",
    ]

    test_root = tempfile.TemporaryDirectory(
        prefix="xdp-testroot-", ignore_cleanup_errors=True
    )

    for env_dir in env_dirs:
        directory = Path(test_root.name) / env_dir.lower()
        directory.mkdir(mode=0o700, parents=True)
        os.environ[env_dir] = directory.absolute().as_posix()

    yield

    test_root.cleanup()


@pytest.fixture
def xdp_mocked_executables(xdp_app_info: xdp.AppInfo) -> list[xdp.ExecutableMock]:
    exe = None
    if xdp_app_info.kind == xdp.AppInfoKind.FLATPAK:
        exe = "flatpak"
    elif (
        xdp_app_info.kind == xdp.AppInfoKind.SNAP
        or xdp_app_info.kind == xdp.AppInfoKind.SNAP_SUB_APP
    ):
        exe = "snap"
    else:
        return []

    return [
        xdp.ExecutableMock(
            executable=exe,
            access_mode=xdp.FileAccessMode.HIDDEN,
        )
    ]


@pytest.fixture
def xdp_available_executables() -> list[str]:
    return [
        "true",
        "bwrap",
    ]


@pytest.fixture
def xdp_bin_path(create_test_dirs) -> Path:
    return Path(os.environ["XDP_BIN"])


@pytest.fixture(autouse=True)
def create_xdp_executables(
    xdp_bin_path, xdp_mocked_executables, xdp_available_executables
):
    for mock in xdp_mocked_executables:
        mock.create(xdp_bin_path)

    for exe_name in xdp_available_executables:
        exe_path = shutil.which(exe_name)
        assert exe_path, f"{exe_name} is not installed or not in PATH"
        (xdp_bin_path / exe_name).symlink_to(exe_path)


@pytest.fixture
def xdg_data_home_files() -> Dict[str, bytes]:
    """
    Default fixture which can be used to create files in the temporary
    XDG_DATA_HOME directory of the test.
    """
    return {}


@pytest.fixture(autouse=True)
def ensure_xdg_data_home(
    create_test_dirs: Any, xdg_data_home_files: Dict[str, bytes]
) -> None:
    files = xdg_data_home_files
    for name, content in files.items():
        file_path = Path(os.environ["XDG_DATA_HOME"]) / name
        file_path.parent.mkdir(parents=True, exist_ok=True)
        with open(file_path.absolute().as_posix(), "wb") as f:
            f.write(content)


@pytest.fixture
def xdg_desktop_portal_dir_files() -> Dict[str, bytes]:
    """
    Default fixture which can be used to create files in the temporary
    XDG_DESKTOP_PORTAL_DIR directory of the test.
    """
    return {}


@pytest.fixture
def xdg_desktop_portal_dir_default_files() -> Dict[str, bytes]:
    files = {}

    portals = [
        "org.freedesktop.impl.portal.Access",
        "org.freedesktop.impl.portal.Account",
        "org.freedesktop.impl.portal.AppChooser",
        "org.freedesktop.impl.portal.Background",
        "org.freedesktop.impl.portal.Clipboard",
        "org.freedesktop.impl.portal.DynamicLauncher",
        "org.freedesktop.impl.portal.Email",
        "org.freedesktop.impl.portal.FileChooser",
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "org.freedesktop.impl.portal.Inhibit",
        "org.freedesktop.impl.portal.InputCapture",
        "org.freedesktop.impl.portal.Lockdown",
        "org.freedesktop.impl.portal.Notification",
        "org.freedesktop.impl.portal.Print",
        "org.freedesktop.impl.portal.RemoteDesktop",
        "org.freedesktop.impl.portal.Screenshot",
        "org.freedesktop.impl.portal.Settings",
        "org.freedesktop.impl.portal.Usb",
        "org.freedesktop.impl.portal.Wallpaper",
    ]

    files["test-portals.conf"] = b"""
[preferred]
default=test;
"""

    files["test.portal"] = """
[portal]
DBusName=org.freedesktop.impl.portal.Test
Interfaces={}
""".format(";".join(portals)).encode("utf-8")

    return files


@pytest.fixture(autouse=True)
def ensure_xdg_desktop_portal_dir(
    create_test_dirs: Any,
    xdg_desktop_portal_dir_files: Dict[str, bytes],
    xdg_desktop_portal_dir_default_files: Dict[str, bytes],
) -> None:
    files = xdg_desktop_portal_dir_default_files | xdg_desktop_portal_dir_files
    for name, content in files.items():
        file_path = Path(os.environ["XDG_DESKTOP_PORTAL_DIR"]) / name
        file_path.parent.mkdir(parents=True, exist_ok=True)
        with open(file_path.absolute().as_posix(), "wb") as f:
            f.write(content)


@pytest.fixture(autouse=True)
def create_test_dbus() -> Iterator[dbusmock.DBusTestCase]:
    bus = dbusmock.DBusTestCase()
    bus.setUp()
    bus.start_session_bus()
    bus.start_system_bus()

    yield bus

    bus.tearDown()
    bus.tearDownClass()


@pytest.fixture(autouse=True)
def create_dbus_monitor(create_test_dbus) -> Iterator[Optional[subprocess.Popen]]:
    if not os.getenv("XDP_DBUS_MONITOR"):
        yield None
        return

    dbus_monitor = subprocess.Popen(["dbus-monitor", "--session"])

    yield dbus_monitor

    dbus_monitor.terminate()
    dbus_monitor.wait()


def _get_server_for_module(
    busses: dict[dbusmock.BusType, dict[str, dbusmock.SpawnedMock]],
    module: ModuleType,
    bustype: dbusmock.BusType,
) -> dbusmock.SpawnedMock:
    assert bustype in dbusmock.BusType

    try:
        return busses[bustype][module.BUS_NAME]
    except KeyError:
        server = dbusmock.SpawnedMock.spawn_for_name(
            module.BUS_NAME,
            "/dbusmock",
            dbusmock.OBJECT_MANAGER_IFACE,
            bustype,
            stdout=None,
            stderr=None,
        )

        busses[bustype][module.BUS_NAME] = server
        return server


def _get_main_obj_for_module(
    server: dbusmock.SpawnedMock, module: ModuleType, bustype: dbusmock.BusType
) -> dbusmock.DBusMockObject:
    try:
        server.obj.AddObject(
            module.MAIN_OBJ,
            "com.example.EmptyInterface",
            {},
            [],
            dbus_interface=dbusmock.MOCK_IFACE,
        )
    except Exception:
        pass

    bustype.wait_for_bus_object(module.BUS_NAME, module.MAIN_OBJ)
    bus = bustype.get_connection()
    return bus.get_object(module.BUS_NAME, module.MAIN_OBJ)


def _terminate_mock_p(process: subprocess.Popen) -> None:
    process.terminate()
    process.wait()


def _terminate_servers(
    busses: dict[dbusmock.BusType, dict[str, dbusmock.SpawnedMock]],
) -> None:
    for server in busses[dbusmock.BusType.SYSTEM].values():
        _terminate_mock_p(server.process)
    for server in busses[dbusmock.BusType.SESSION].values():
        _terminate_mock_p(server.process)


def _start_template(
    busses: dict[dbusmock.BusType, dict[str, dbusmock.SpawnedMock]],
    template: str,
    bus_name: Optional[str],
    params: Dict[str, Any] = {},
) -> None:
    """
    Start the template and potentially start a server for it
    """
    module_path_dir = (test_dir()).parent.absolute().as_posix()
    template_path = test_dir() / f"templates/{template.lower()}.py"
    template = template_path.absolute().as_posix()

    # we cd to the parent dir of the test_dir so that the module search path for
    # the templates is the same as opening the modules from here
    with chdir(module_path_dir):
        module = dbusmock.mockobject.load_module(template)
        bustype = (
            dbusmock.BusType.SYSTEM if module.SYSTEM_BUS else dbusmock.BusType.SESSION
        )

        if bus_name:
            module.BUS_NAME = bus_name

        server = _get_server_for_module(busses, module, bustype)
        main_obj = _get_main_obj_for_module(server, module, bustype)

        main_obj.AddTemplate(
            template,
            dbus.Dictionary(params, signature="sv"),
            dbus_interface=dbusmock.MOCK_IFACE,
        )


@pytest.fixture
def template_params() -> dict[str, dict[str, Any]]:
    """
    Default fixture for overriding the parameters which should be passed to the
    mocking templates. Use required_templates to specify the default parameters
    and override it for specific test cases via

        @pytest.mark.parametrize("template_params", ({"Template": {"foo": "bar"}},))

    """
    return {}


@pytest.fixture
def required_templates() -> dict[str, dict[str, Any]]:
    """
    Default fixture for enumerating the mocking templates the test case requires
    to be started. This is a map from a template spec to the parameters which
    should be passed to the template. The template spec is the name of a
    template in the templates directory and an optional dbus bus name to start
    the template at, separated by a colon.

    This starts the `settings` and `email` templates on the bus names specified
    with BUS_NAME in the templates and passes parameters to settings.

        {
            "settings": {
                "some_param": true,
            },
            "email": {},
        }

    This starts two instances of the settings template with their own parameters
    once on the bus name specified in BUS_NAME in the template, and once on the
    bus name `org.freedesktop.impl.portal.OtherImpl`.

        {
            "settings": {
                "some_param": true,
            },
            "settings:org.freedesktop.impl.portal.OtherImpl": {
                "some_param": false,
            },
        }

    """
    return {}


@pytest.fixture
def templates(
    required_templates: dict[str, dict[str, Any]],
    template_params: dict[str, dict[str, Any]],
) -> Iterator[None]:
    """
    Fixture which starts the required templates with their parameters. Usually
    the `portals` fixture is what you're looking for because it also starts
    the portal frontend and the permission store.
    """
    busses: dict[dbusmock.BusType, dict[str, dbusmock.SpawnedMock]] = {
        dbusmock.BusType.SYSTEM: {},
        dbusmock.BusType.SESSION: {},
    }
    for template_data, params in required_templates.items():
        template, bus_name = (template_data.split(":") + [None])[:2]
        assert template
        params = template_params.get(template, params)
        _start_template(busses, template, bus_name, params)
    yield
    _terminate_servers(busses)


@pytest.fixture
def xdp_overwrite_env() -> dict[str, str]:
    """
    Default fixture which can be used to override the environment that gets
    passed to xdg-desktop-portal, xdg-document-portal and xdg-permission-store.
    """
    return {}


@pytest.fixture(
    params=[
        xdp.AppInfoKind.HOST,
        xdp.AppInfoKind.FLATPAK,
        xdp.AppInfoKind.SNAP,
        xdp.AppInfoKind.SNAP_SUB_APP,
        xdp.AppInfoKind.LINYAPS,
    ]
)
def xdp_app_info(request) -> xdp.AppInfo:
    """
    Default fixture which can be used to override the XdpAppInfo the portal
    frontend will discover.
    The default fixture is parametric and will run each test with all the
    app info kinds.
    """

    app_info_kind = request.param
    app_id = "org.example.Test"

    if app_info_kind == xdp.AppInfoKind.HOST:
        return xdp.AppInfo.new_host(
            app_id=app_id,
        )

    if app_info_kind == xdp.AppInfoKind.FLATPAK:
        return xdp.AppInfo.new_flatpak(
            app_id=app_id,
        )

    if app_info_kind == xdp.AppInfoKind.SNAP:
        return xdp.AppInfo.new_snap(
            common_id=app_id,
            snap_name="test",
            app_name="test",
        )

    if app_info_kind == xdp.AppInfoKind.SNAP_SUB_APP:
        return xdp.AppInfo.new_snap(
            common_id=app_id,
            snap_name="example",
            app_name="test",
        )

    if app_info_kind == xdp.AppInfoKind.LINYAPS:
        return xdp.AppInfo.new_linyaps(
            app_id=app_id,
        )

    assert_never(app_info_kind)


@pytest.fixture(autouse=True)
def ensure_xdp_app_info_files(create_test_dirs, xdp_app_info) -> None:
    xdp_app_info.ensure_files()


@pytest.fixture
def xdp_env(
    xdp_overwrite_env: dict[str, str],
    xdp_app_info: xdp.AppInfo,
    xdp_bin_path: Path,
    umockdev: Optional[UMockdev.Testbed],
) -> dict[str, str]:
    env = os.environ.copy()
    env["G_DEBUG"] = "fatal-criticals"
    env["XDG_CURRENT_DESKTOP"] = "test"
    env["PATH"] = xdp_bin_path.as_posix()

    if xdp_app_info:
        xdp_app_info.extend_env(env)

    if umockdev:
        env["UMOCKDEV_DIR"] = umockdev.get_root_dir()

    asan_suppression = test_dir() / "asan.suppression"
    if not asan_suppression.exists():
        raise FileNotFoundError(f"{asan_suppression} does not exist")
    env["LSAN_OPTIONS"] = f"suppressions={asan_suppression}"

    for key, val in xdp_overwrite_env.items():
        env[key] = val

    return env


def _maybe_add_asan_preload(executable: Path, env: dict[str, str]) -> None:
    # ASAN really wants to be the first library to get loaded but we also
    # LD_PRELOAD umockdev and LD_PRELOAD gets loaded before any "normally"
    # linked libraries. This uses ldd to find the version of libasan.so that
    # should be loaded and puts it in front of LD_PRELOAD.
    # This way, LD_PRELOAD and ASAN can be used at the same time.
    ldd = subprocess.check_output(["ldd", executable]).decode("utf-8")
    libs = [line.split()[0] for line in ldd.splitlines()]
    try:
        libasan = next(filter(lambda lib: lib.startswith("libasan"), libs))
    except StopIteration:
        return

    preload = env.get("LD_PRELOAD", "")
    env["LD_PRELOAD"] = f"{libasan}:{preload}"


def _valgrind_glib_suppressions() -> str:
    try:
        import pkgconfig

        return pkgconfig.variables("glib-2.0")["glib_valgrind_suppressions"]
    except ImportError:
        return "/usr/share/glib-2.0/valgrind/glib.supp"


@pytest.fixture
def xdp_valgrind_args() -> list[str]:
    if not xdp.is_valgrind():
        return []

    valgrind = shutil.which("valgrind")
    assert valgrind, "Valgrind is not installed or not in PATH"

    xdp_suppression = test_dir() / "valgrind.suppression"
    glib_suppression = _valgrind_glib_suppressions()

    return [
        valgrind,
        "--tool=memcheck",
        "--error-exitcode=1",
        "--track-origins=yes",
        "--leak-check=full",
        "--leak-resolution=high",
        "--num-callers=50",
        "--show-leak-kinds=definite,possible",
        "--show-error-list=yes",
        "--show-realloc-size-zero=no",
        f"--suppressions={glib_suppression}",
        f"--suppressions={xdp_suppression}",
    ]


@pytest.fixture
def xdg_desktop_portal(
    dbus_con: dbus.Bus,
    xdg_desktop_portal_path: Path,
    xdp_env: dict[str, str],
    xdp_valgrind_args: list[str],
) -> Iterator[subprocess.Popen]:
    """
    Fixture which starts and eventually stops xdg-desktop-portal
    """
    if not xdg_desktop_portal_path.exists():
        raise FileNotFoundError(f"{xdg_desktop_portal_path} does not exist")

    env = xdp_env.copy()
    _maybe_add_asan_preload(xdg_desktop_portal_path, env)

    xdg_desktop_portal = subprocess.Popen(
        xdp_valgrind_args + [xdg_desktop_portal_path], env=env
    )

    while not dbus_con.name_has_owner("org.freedesktop.portal.Desktop"):
        returncode = xdg_desktop_portal.poll()
        if returncode is not None:
            raise subprocess.SubprocessError(
                f"xdg-desktop-portal exited with {returncode}"
            )
        time.sleep(0.1)

    yield xdg_desktop_portal

    xdg_desktop_portal.send_signal(signal.SIGHUP)
    returncode = xdg_desktop_portal.wait()
    assert returncode == 0


@pytest.fixture
def xdg_permission_store(
    dbus_con: dbus.Bus, xdg_permission_store_path: Path, xdp_env: dict[str, str]
) -> Iterator[subprocess.Popen]:
    """
    Fixture which starts and eventually stops xdg-permission-store
    """
    if not xdg_permission_store_path.exists():
        raise FileNotFoundError(f"{xdg_permission_store_path} does not exist")

    env = xdp_env.copy()
    _maybe_add_asan_preload(xdg_permission_store_path, env)

    permission_store = subprocess.Popen([xdg_permission_store_path], env=env)

    while not dbus_con.name_has_owner("org.freedesktop.impl.portal.PermissionStore"):
        returncode = permission_store.poll()
        if returncode is not None:
            raise subprocess.SubprocessError(
                f"xdg-permission-store exited with {returncode}"
            )
        time.sleep(0.1)

    yield permission_store

    permission_store.send_signal(signal.SIGHUP)
    permission_store.wait()
    # The permission store does not shut down cleanly currently
    # returncode = permission_store.wait()
    # assert returncode == 0


@pytest.fixture
def xdg_document_portal(
    dbus_con: dbus.Bus, xdg_document_portal_path: Path, xdp_env: dict[str, str]
) -> Iterator[subprocess.Popen]:
    """
    Fixture which starts and eventually stops xdg-document-portal
    """
    try:
        xdp.ensure_fuse_supported()
    except xdp.FuseNotSupportedException as e:
        pytest.skip(f"No fuse support: {e}")

    if not xdg_document_portal_path.exists():
        raise FileNotFoundError(f"{xdg_document_portal_path} does not exist")

    # FUSE and LD_PRELOAD don't like each other. Not sure what exactly is going
    # wrong but it usually just results in a weird hang that needs SIGKILL
    env = xdp_env.copy()
    env.pop("LD_PRELOAD", None)

    # The document portal also uses XdpAppInfos to figure out where a request
    # is coming from. If we don't unset this, it might think the
    # xdg-desktop-portal is a flatpak or snap app. Would be nice to find a
    # better solution here.
    env.pop("XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND", None)

    document_portal = subprocess.Popen([xdg_document_portal_path], env=env)

    while not dbus_con.name_has_owner("org.freedesktop.portal.Documents"):
        returncode = document_portal.poll()
        if returncode is not None:
            raise subprocess.SubprocessError(
                f"xdg-document-portal exited with {returncode}"
            )
        time.sleep(0.1)

    yield document_portal

    document_portal.send_signal(signal.SIGHUP)
    returncode = document_portal.wait()
    assert returncode == 0

    fuse_mount = Path(os.environ["XDG_RUNTIME_DIR"]) / "doc"

    def unmounted():
        try:
            next(fuse_mount.iterdir())
        except StopIteration:
            return True
        return False

    xdp.wait_for(unmounted)


@pytest.fixture
def portals(templates: Any, xdg_desktop_portal: Any, xdg_permission_store: Any) -> None:
    """
    Fixture which starts the required templates, xdg-desktop-portal,
    xdg-document-portal and xdg-permission-store. Most tests require this.
    """
    return None


@pytest.fixture
def umockdev() -> Optional[UMockdev.Testbed]:
    """
    Default fixture providing a umockdev testbed
    """
    return None


@pytest.fixture
def dbus_con(create_test_dbus: dbusmock.DBusTestCase) -> dbus.Bus:
    """
    Default fixture which provides the python-dbus session bus of the test.
    """
    con = create_test_dbus.get_dbus(system_bus=False)
    assert con
    return con


@pytest.fixture
def dbus_con_sys(create_test_dbus: dbusmock.DBusTestCase) -> dbus.Bus:
    """
    Default fixture which provides the python-dbus system bus of the test.
    """
    con_sys = create_test_dbus.get_dbus(system_bus=True)
    assert con_sys
    return con_sys
