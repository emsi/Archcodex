#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the notification activation regression on a private Xvfb, bus and KWin.

Example: python3 tests/run-x11-activation.py -- build/test-x11activation
The command after -- may include a runtime loader, followed by the test binary.
KWin/Xvfb must already be available; this script does not install dependencies.
"""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile


def line(stream):
    if not select.select([stream], [], [], 10)[0]:
        raise RuntimeError("Timed out starting private display/session bus")
    value = stream.readline().decode().strip()
    if not value:
        raise RuntimeError("Private display/session bus exited during startup")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kwin", default="kwin_x11")
    parser.add_argument("--kwin-library-path")
    parser.add_argument("--focus-policy", type=int, choices=(2, 4), default=2)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Supply the test executable after --")
    children = []
    with tempfile.TemporaryDirectory(prefix="archcodex-x11-activation-") as directory:
        base = Path(directory)
        env = os.environ.copy()
        for name in ("CONFIG", "CACHE", "DATA", "RUNTIME"):
            path = base / name.lower()
            path.mkdir(mode=0o700)
            env[f"XDG_{name}_HOME" if name != "RUNTIME" else "XDG_RUNTIME_DIR"] = str(path)
        env.update(QT_QPA_PLATFORM="xcb", QT_ACCESSIBILITY="0", KWIN_COMPOSE="N",
                   XDG_CONFIG_DIRS=str(base / "empty-config"),
                   ARCHCODEX_TEST_REAL_KWIN="1",
                   ARCHCODEX_TEST_FOCUS_POLICY=str(args.focus_policy))
        (base / "config/kwinrc").write_text(
            "[Desktops]\nNumber=2\nRows=1\n[Windows]\n"
            f"FocusStealingPreventionLevel={args.focus_policy}\nFocusPolicy=ClickToFocus\n"
            "[Compositing]\nEnabled=false\n[org.kde.kdecoration2]\nNoPlugin=true\n"
        )

        def launch(argv, **kwargs):
            process = subprocess.Popen(argv, start_new_session=True, **kwargs)
            children.append(process)
            return process

        try:
            bus = launch(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            env["DBUS_SESSION_BUS_ADDRESS"] = line(bus.stdout)
            read_fd, write_fd = os.pipe()
            try:
                launch(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                        "1024x768x24", "-nolisten", "tcp"],
                       pass_fds=(write_fd,), env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            finally:
                os.close(write_fd)
            with os.fdopen(read_fd, "rb") as display:
                env["DISPLAY"] = ":" + line(display)
            wm_env = env.copy()
            for key in ("QT_PLUGIN_PATH", "LD_LIBRARY_PATH", "QT_LOGGING_RULES"):
                wm_env.pop(key, None)
            if args.kwin_library_path:
                wm_env["LD_LIBRARY_PATH"] = args.kwin_library_path
            with (base / "kwin.log").open("w+") as log:
                wm = launch([args.kwin, "--replace"], env=wm_env,
                            stdout=log, stderr=subprocess.STDOUT)
                env["ARCHCODEX_X11_TEST_COMMAND"] = json.dumps(command)
                test = launch(command + args.case, env=env)
                result = test.wait(timeout=180)
                if result or wm.poll() is not None:
                    log.seek(0)
                    print(log.read()[-6000:])
                return result
        finally:
            for child in reversed(children):
                # Session groups also contain helpers started by the test binary.
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()


if __name__ == "__main__":
    raise SystemExit(main())
