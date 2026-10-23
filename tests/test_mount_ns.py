# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import shutil
import subprocess
from pathlib import Path
from urllib.parse import unquote

import pytest

import tests.xdp_utils as xdp

HELPERS_DIR = Path(__file__).resolve().parent / "helpers"
PORTAL_CALL_HELPER = HELPERS_DIR / "bwrap-portal-call.py"


def check_bwrap_available():
    """Check that bwrap is installed and functional, skip otherwise."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        pytest.skip("bwrap not available")

    result = subprocess.run(
        [bwrap, "--bind", "/", "/", "--", "true"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        pytest.skip(f"bwrap not functional: {result.stderr.decode().strip()}")

    return bwrap


def run_in_bwrap(bwrap_args, portal_args):
    """Run the portal call helper inside bwrap."""
    bwrap = check_bwrap_available()

    cmd = (
        [bwrap, "--bind", "/", "/"]
        + bwrap_args
        + ["--", "python3", str(PORTAL_CALL_HELPER)]
        + portal_args
    )

    result = subprocess.run(cmd, timeout=15, capture_output=True, check=False)

    assert result.returncode == 0, f"bwrap helper failed: {result.stderr.decode()}"


class TestMountNsOpenURI:
    defaults_list = b"""[Default Applications]
text/plain=furrfix.desktop
"""

    furrfix_desktop = b"""[Desktop Entry]
Version=1.0
Name=Furrfix
Exec=true %u
Type=Application
MimeType=text/plain;
"""

    mimeinfo_cache = b"""[MIME Cache]
text/plain=furrfix.desktop;
"""

    @pytest.fixture
    def xdp_app_info(self):
        return xdp.AppInfoHost()

    @pytest.fixture
    def xdg_data_home_files(self):
        return {
            "applications/defaults.list": self.defaults_list,
            "applications/furrfix.desktop": self.furrfix_desktop,
            "applications/mimeinfo.cache": self.mimeinfo_cache,
        }

    @pytest.fixture
    def required_templates(self):
        return {
            "appchooser": {},
            "lockdown": {},
        }

    def enable_paranoid_mode(self, dbus_con):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        from dbus import Dictionary

        perm_store_intf.SetValue(
            "desktop-used-apps",
            True,
            "text/plain",
            Dictionary({"always-ask": True}, signature="sv"),
        )

    def call_openfile_in_bwrap(self, sandbox_path, bwrap_args):
        run_in_bwrap(
            bwrap_args,
            [
                "--interface",
                "org.freedesktop.portal.OpenURI",
                "--method",
                "OpenFile",
                "--fd",
                sandbox_path,
            ],
        )

    def assert_resolved_uri(self, dbus_con, expected_host_path):
        """Check that the mock backend received the correct host path in the URI."""
        mock_intf = xdp.get_mock_iface(dbus_con)
        method_calls = mock_intf.GetMethodCalls("ChooseApplication")
        assert len(method_calls) > 0

        _, args = method_calls[-1]
        uri = args[4]["uri"]
        assert uri.startswith("file:///")
        resolved_path = unquote(uri[len("file://") :])
        assert resolved_path == expected_host_path

    def test_bind_mount_remaps_path(self, portals, dbus_con):
        """A file opened via a bind mount in a different mount namespace
        is resolved to the host path."""
        self.enable_paranoid_mode(dbus_con)

        host_dir = Path.home() / "mount-ns-host-data"
        host_dir.mkdir()
        host_file = host_dir / "testfile.txt"
        host_file.write_text("mount ns test content")

        sandbox_dir = "/tmp/xdp-sandbox-data"

        self.call_openfile_in_bwrap(
            f"{sandbox_dir}/testfile.txt",
            ["--bind", str(host_dir), sandbox_dir],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_nested_bind_mount(self, portals, dbus_con):
        """A file in a nested directory under a bind mount is resolved correctly."""
        self.enable_paranoid_mode(dbus_con)

        host_dir = Path.home() / "mount-ns-nested"
        nested = host_dir / "sub" / "dir"
        nested.mkdir(parents=True)
        host_file = nested / "deep.txt"
        host_file.write_text("nested content")

        sandbox_dir = "/tmp/xdp-sandbox-nested"

        self.call_openfile_in_bwrap(
            f"{sandbox_dir}/sub/dir/deep.txt",
            ["--bind", str(host_dir), sandbox_dir],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_same_path_no_remap(self, portals, dbus_con):
        """When sandbox and host paths are identical (like --filesystem=home),
        the fast path returns the correct path without mountinfo lookup."""
        self.enable_paranoid_mode(dbus_con)

        host_file = Path.home() / "mount-ns-same-path.txt"
        host_file.write_text("same path content")

        self.call_openfile_in_bwrap(
            str(host_file),
            [],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_path_with_spaces(self, portals, dbus_con):
        """Paths containing spaces are handled correctly through
        mountinfo octal escaping."""
        self.enable_paranoid_mode(dbus_con)

        host_dir = Path.home() / "mount ns spaces"
        host_dir.mkdir()
        host_file = host_dir / "space file.txt"
        host_file.write_text("spaces content")

        sandbox_dir = "/tmp/xdp-sandbox-spaces"

        self.call_openfile_in_bwrap(
            f"{sandbox_dir}/space file.txt",
            ["--bind", str(host_dir), sandbox_dir],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_persist_style_mount(self, portals, dbus_con):
        """Flatpak --persist style: host ~/.var/app/<id>/config is bound
        to ~/.config inside the sandbox."""
        self.enable_paranoid_mode(dbus_con)

        host_config = Path.home() / ".var" / "app" / "org.example.Test" / "config"
        host_config.mkdir(parents=True)
        host_file = host_config / "settings.ini"
        host_file.write_text("persist config")

        sandbox_config = str(Path.home() / ".config")

        self.call_openfile_in_bwrap(
            f"{sandbox_config}/settings.ini",
            ["--bind", str(host_config), sandbox_config],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_filesystem_style_mount(self, portals, dbus_con):
        """Flatpak --filesystem style: a host directory is exposed
        at a different location inside the sandbox."""
        self.enable_paranoid_mode(dbus_con)

        host_dir = Path.home() / "Documents"
        host_dir.mkdir()
        host_file = host_dir / "report.txt"
        host_file.write_text("document content")

        sandbox_dir = "/tmp/xdp-sandbox-docs"

        self.call_openfile_in_bwrap(
            f"{sandbox_dir}/report.txt",
            ["--bind", str(host_dir), sandbox_dir],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))

    def test_multiple_bind_mounts(self, portals, dbus_con):
        """Multiple bind mounts: the correct one is used for resolution."""
        self.enable_paranoid_mode(dbus_con)

        host_dir_a = Path.home() / "mount-ns-multi-a"
        host_dir_a.mkdir()
        (host_dir_a / "decoy.txt").write_text("wrong")

        host_dir_b = Path.home() / "mount-ns-multi-b"
        host_dir_b.mkdir()
        host_file = host_dir_b / "target.txt"
        host_file.write_text("correct")

        self.call_openfile_in_bwrap(
            "/tmp/xdp-sandbox-b/target.txt",
            [
                "--bind",
                str(host_dir_a),
                "/tmp/xdp-sandbox-a",
                "--bind",
                str(host_dir_b),
                "/tmp/xdp-sandbox-b",
            ],
        )

        self.assert_resolved_uri(dbus_con, str(host_file))
