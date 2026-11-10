#!/usr/bin/env python3
"""Exercise the installed launcher's display policy without launching the viewer.

Only the socket directory is substituted; the actual shell function is executed
with synthetic sockets and a fake user-manager command. No desktop is queried.
"""

from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]
WRAPPER = ROOT / "indra/newview/linux_tools/wrapper.sh"


class DisplayInheritance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cef-display-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.runtime = self.base / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.xdir = self.base / "x11"
        self.xdir.mkdir()
        self.bin = self.base / "bin"
        self.bin.mkdir()
        self.auth = self.base / "auth"
        self.auth.write_text("display=:7\n")
        self.auth.chmod(0o600)
        self.session_auth = self.runtime / "xauth_oSryWk"
        self.session_auth.write_text("display=:7\n")
        self.session_auth.chmod(0o600)
        self.calls = self.base / "calls"
        self.manager = self.base / "manager"
        fake = self.bin / "systemctl"
        fake.write_text('#!/bin/sh\nprintf x >> "$CALLS"\ncat "$MANAGER"\nexit "${MANAGER_STATUS:-0}"\n')
        fake.chmod(0o700)
        xauth = self.bin / "xauth"
        xauth.write_text(
            '#!/bin/sh\n'
            '[ "$1" = -f ] && [ "$3" = nlist ] || exit 1\n'
            'grep -Fqx "display=$4" "$2" || exit 0\n'
            'printf "0000 0000 0000 3037 4d49542d4d414749432d434f4f4b49452d31 636f6f6b6965\\n"\n'
        )
        xauth.chmod(0o700)
        for path in (self.runtime / "wayland-0", self.xdir / "X7"):
            sock = socket.socket(socket.AF_UNIX)
            self.addCleanup(sock.close)
            sock.bind(str(path))
        source = WRAPPER.read_text()
        begin = source.index("slipstream_inherit_xwayland_display()\n{")
        end = source.index("\n}\n", begin) + 3
        self.function = source[begin:end].replace("/tmp/.X11-unix", str(self.xdir))
        ozone_begin = source.index("slipstream_select_cef_display_backend()\n{")
        ozone_end = source.index("\n}\n", ozone_begin) + 3
        self.ozone_function = source[ozone_begin:ozone_end].replace("/tmp/.X11-unix", str(self.xdir))
        self.env = {
            "PATH": f"{self.bin}:/usr/bin:/bin", "LC_ALL": "C",
            "WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": str(self.runtime),
            "CALLS": str(self.calls), "MANAGER": str(self.manager),
        }
        self.values = {
            "DISPLAY": ":7.0", "WAYLAND_DISPLAY": "wayland-0",
            "XDG_RUNTIME_DIR": str(self.runtime), "XAUTHORITY": str(self.auth),
        }

    def run_policy(self, values=None):
        values = self.values if values is None else values
        self.manager.write_text("".join(f"{key}={value}\n" for key, value in values.items()))
        result = subprocess.run(
            ["/bin/bash", "-c", self.function + '\nslipstream_inherit_xwayland_display\nprintf "%s\\0%s\\0" "${DISPLAY-}" "${XAUTHORITY-}"'],
            env=self.env, capture_output=True, check=True, timeout=3,
        )
        self.assertEqual(result.stderr, b"")
        fields = result.stdout.decode().split("\0")
        self.assertEqual(len(fields), 3)
        return tuple(fields[:2])

    def test_inherited_display_never_queries_manager(self):
        for display in (":19", "remote.example:2"):
            self.env.update(DISPLAY=display, XAUTHORITY="explicit-auth")
            self.assertEqual(self.run_policy(), (display, "explicit-auth"))
            self.assertFalse(self.calls.exists())

    def test_nonempty_display_recovers_matching_xauth_from_wayland_runtime(self):
        self.env.update(DISPLAY=":7.0", WAYLAND_DISPLAY="wayland-0", XDG_RUNTIME_DIR=str(self.runtime), XDG_SESSION_TYPE="tty")
        self.env.pop("XAUTHORITY", None)
        self.assertEqual(self.run_policy(dict(self.values, XAUTHORITY="")), (":7.0", str(self.session_auth)))
        self.assertTrue(self.calls.exists())

    def test_inherited_display_uses_live_socket_when_manager_has_stale_wayland(self):
        self.env.update(DISPLAY=":7.0", WAYLAND_DISPLAY="wayland-0", XDG_RUNTIME_DIR=str(self.runtime), XDG_SESSION_TYPE="tty")
        self.env.pop("XAUTHORITY", None)
        stale = dict(self.values, WAYLAND_DISPLAY="wayland-stale", XAUTHORITY="/stale/auth")
        self.assertEqual(self.run_policy(stale), (":7.0", str(self.session_auth)))

    def test_missing_or_empty_display_recovers_matching_session(self):
        for display in (None, ""):
            if display is not None:
                self.env["DISPLAY"] = display
            self.assertEqual(self.run_policy(), (":7.0", str(self.auth)))

    def test_existing_matching_auth_is_preserved(self):
        self.env["XAUTHORITY"] = str(self.auth)
        self.assertEqual(self.run_policy(), (":7.0", str(self.auth)))

    def test_conflicting_auth_refuses_without_mutation(self):
        self.env["XAUTHORITY"] = "explicit-other-session"
        self.assertEqual(self.run_policy(), ("", "explicit-other-session"))

    def test_no_manager_auth_preserves_inherited_auth(self):
        self.values.pop("XAUTHORITY")
        self.env["XAUTHORITY"] = str(self.auth)
        self.assertEqual(self.run_policy(), (":7.0", str(self.auth)))

    def test_other_session_refuses(self):
        for key in ("WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"):
            values = dict(self.values, **{key: "other-session"})
            self.assertEqual(self.run_policy(values), ("", ""))

    def test_missing_x_socket_refuses(self):
        (self.xdir / "X7").unlink()
        self.assertEqual(self.run_policy(), ("", ""))

    def test_missing_wayland_socket_does_not_query(self):
        (self.runtime / "wayland-0").unlink()
        self.assertEqual(self.run_policy(), ("", ""))
        self.assertFalse(self.calls.exists())

    def test_no_wayland_does_not_guess_display(self):
        self.env.pop("WAYLAND_DISPLAY")
        self.assertEqual(self.run_policy(), ("", ""))
        self.assertFalse(self.calls.exists())

    def test_missing_or_relative_auth_refuses(self):
        self.session_auth.unlink()
        for auth in (str(self.base / "missing"), "relative-auth"):
            self.assertEqual(self.run_policy(dict(self.values, XAUTHORITY=auth)), ("", ""))

    def test_runtime_auth_must_be_private(self):
        self.session_auth.chmod(0o644)
        self.env.update(DISPLAY=":7.0", WAYLAND_DISPLAY="wayland-0", XDG_RUNTIME_DIR=str(self.runtime))
        self.env.pop("XAUTHORITY", None)
        self.assertEqual(self.run_policy(dict(self.values, XAUTHORITY="")), (":7.0", ""))

    def test_failed_manager_even_with_output_refuses(self):
        self.env["MANAGER_STATUS"] = "1"
        self.assertEqual(self.run_policy(), ("", ""))

    def test_nonlocal_quoted_or_shell_display_is_data(self):
        marker = self.base / "must-not-exist"
        for display in ("localhost:7", "':7'", f"$(touch {marker})", f":7;touch {marker}"):
            self.assertEqual(self.run_policy(dict(self.values, DISPLAY=display)), ("", ""))
            self.assertFalse(marker.exists())

    def test_oversized_manager_value_refuses(self):
        self.assertEqual(self.run_policy(dict(self.values, DISPLAY=":" + "7" * 4096)), ("", ""))

    def test_excess_manager_fields_refuse(self):
        # A second display record is malformed user-manager output.
        self.values["DISPLAY_EXTRA"] = "ignored\nDISPLAY=:7"
        self.assertEqual(self.run_policy(), ("", ""))

    def test_flatpak_exposes_both_display_backends(self):
        manifest = (ROOT / "scripts/packaging/flatpak/org.slipstreamviewer.Slipstream.yml").read_text()
        flags = [line.strip() for line in manifest.splitlines() if line.strip().startswith("- --socket=")]
        self.assertIn("- --socket=wayland", flags)
        self.assertIn("- --socket=x11", flags)
        self.assertNotIn("- --socket=fallback-x11", flags)

    def run_ozone_policy(self, extra_env=None):
        env = dict(self.env, DISPLAY=":7", XDG_RUNTIME_DIR=str(self.runtime), WAYLAND_DISPLAY="wayland-0")
        if extra_env:
            env.update(extra_env)
        result = subprocess.run(
            ["/bin/bash", "-c", self.ozone_function + '\nslipstream_select_cef_display_backend\nprintf "%s|%s" "${SLIPSTREAM_CEF_OZONE_PLATFORM-}" "${SDL_VIDEODRIVER-}"'],
            env=env, capture_output=True, check=True, timeout=2,
        )
        self.assertEqual(result.stderr, b"")
        return tuple(result.stdout.decode().split("|", 1))

    def test_live_wayland_socket_prefers_cef_ozone_wayland_with_display_set(self):
        self.assertEqual(self.run_ozone_policy(), ("wayland", ""))

    def test_explicit_cef_x11_environment_is_preserved(self):
        self.assertEqual(self.run_ozone_policy({"SDL_VIDEODRIVER": "x11"}), ("", "x11"))

    def test_viewer_x11_override_maps_to_cef_x11(self):
        self.assertEqual(self.run_ozone_policy({"SDL_VIDEO_DRIVER": "x11"}), ("x11", ""))

    def test_viewer_wayland_override_maps_to_cef_wayland(self):
        self.assertEqual(self.run_ozone_policy({"SDL_VIDEO_DRIVER": "wayland"}), ("wayland", ""))

    def test_missing_wayland_socket_does_not_force_ozone_backend(self):
        (self.runtime / "wayland-0").unlink()
        self.assertEqual(self.run_ozone_policy(), ("", ""))

    def test_missing_xauth_does_not_block_native_wayland_selection(self):
        env = {
            "PATH": str(self.base / "empty-bin"), "DISPLAY": ":7",
            "WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": str(self.runtime),
        }
        result = subprocess.run(
            ["/bin/bash", "-c", self.function + '\nslipstream_inherit_xwayland_display\n'
             + self.ozone_function + '\nslipstream_select_cef_display_backend\n'
             + 'printf "%s|%s" "${XAUTHORITY-}" "${SLIPSTREAM_CEF_OZONE_PLATFORM-}"'],
            env=env, capture_output=True, check=True, timeout=2,
        )
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.stdout.decode(), "|wayland")


if __name__ == "__main__":
    unittest.main()
