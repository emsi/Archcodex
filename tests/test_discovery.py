# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "src/discover.py"
spec = importlib.util.spec_from_file_location("discover", SCRIPT)
discovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discovery)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.proc = self.root / "proc"
        (self.proc / "net").mkdir(parents=True)
        (self.proc / "self/ns").mkdir(parents=True)
        (self.proc / "self/ns/mnt").symlink_to("mnt:[host]")
        self.rows, self.sockets = [], []
        self.executable = self.root / "bin/codex"
        self.executable.parent.mkdir()
        self.executable.write_text("fixture")
        self.executable.chmod(0o700)

    def tearDown(self):
        for s in self.sockets:
            s.close()
        self.temp.cleanup()

    def server(self, pid, *, updater=False, namespace="host", listen=True):
        path = self.root / f"socket-{pid} with spaces"
        s = socket.socket(socket.AF_UNIX)
        s.bind(str(path))
        if listen:
            s.listen()
        self.sockets.append(s)
        proc = self.proc / str(pid)
        (proc / "fd").mkdir(parents=True)
        (proc / "ns").mkdir()
        (proc / "ns/mnt").symlink_to(f"mnt:[{namespace}]")
        (proc / "exe").symlink_to(self.executable)
        (proc / "cwd").symlink_to(self.root / "workspace")
        (proc / "cmdline").write_bytes(b"codex\0app-server\0" +
                                        (b"daemon\0pid-update-loop\0" if updater else b"--listen\0unix://\0"))
        (proc / "fd/7").symlink_to(f"socket:[{pid}]")
        flags = "00010000" if listen else "00000000"
        self.rows.append(f"000: 00000002 00000000 {flags} 0001 01 {pid} {path}\n")
        (self.proc / "net/unix").write_text("Num RefCount Protocol Flags Type St Inode Path\n" + "".join(self.rows))
        return str(path)

    def test_multiple_servers_aliases_and_updater_exclusion(self):
        first = self.server(100)
        second = self.server(200)
        self.server(300, updater=True)
        self.server(400, updater=True)
        (self.proc / "400/cmdline").write_bytes(b"codex\0app-server\0-c\0a=1\0daemon\0pid-update-loop\0")
        advertised = self.root / "advertised.sock"
        advertised.symlink_to(first)
        with patch.object(discovery.pwd, "getpwuid", return_value=SimpleNamespace(
                pw_name="alice", pw_dir="/home/alice")):
            result, warnings = discovery.snapshot(self.proc, default=str(advertised))
        self.assertEqual(warnings, [])
        self.assertEqual({x["path"] for x in result}, {first, second})
        default = next(x for x in result if x["default"])
        self.assertIn(str(advertised), default["aliases"])
        self.assertTrue(default["hostEnvironment"])
        self.assertEqual(default["pid"], 100)
        self.assertEqual(default["cwd"], str(self.root / "workspace"))
        self.assertEqual(default["host"], socket.gethostname())
        self.assertEqual(default["user"], "alice")

    def test_bound_before_listen_remains_discoverable(self):
        path = self.server(100, listen=False)
        self.assertEqual(discovery.snapshot(self.proc)[0][0]["path"], path)

    def test_stale_files_foreign_users_and_private_namespaces_are_excluded(self):
        path = self.server(100)
        self.server(200, namespace="container")
        self.assertEqual(len(discovery.snapshot(self.proc)[0]), 1)
        self.assertEqual(discovery.snapshot(self.proc, uid=os.getuid() + 1)[0], [])
        (self.proc / "100/fd/7").unlink()
        self.assertTrue(Path(path).exists())
        self.assertEqual(discovery.snapshot(self.proc)[0], [])

    def test_inotify_parent_creation_socket_creation_and_deletion(self):
        target = self.root / "later"
        watcher = discovery.Watcher()
        try:
            self.assertEqual(watcher.configure([str(target)], []), [])
            target.mkdir()
            self.assertTrue(select.select([watcher.fd], [], [], 1)[0])
            self.assertTrue(watcher.changed())
            watcher.configure([str(target)], [])
            watcher.changed()  # Consume IN_IGNORED from the replaced ancestor watch.
            s = socket.socket(socket.AF_UNIX)
            path = target / "new.sock"
            try:
                s.bind(str(path))
                self.assertTrue(select.select([watcher.fd], [], [], 1)[0])
                self.assertTrue(watcher.changed())
                self.assertFalse(select.select([watcher.fd], [], [], 0.05)[0])
                path.unlink()
                self.assertTrue(select.select([watcher.fd], [], [], 1)[0])
                self.assertTrue(watcher.changed())
            finally:
                s.close()
        finally:
            watcher.close()

    def test_idle_loop_blocks_without_a_rescan_timeout(self):
        class StopLoop(Exception):
            pass
        class Selector:
            def register(self, *args):
                pass
            def select(self, *args, **kwargs):
                self_outer.assertEqual(args, ())
                self_outer.assertEqual(kwargs, {})
                raise StopLoop()
            def close(self):
                pass
        self_outer = self
        with patch.object(discovery, "snapshot", return_value=([], [])) as scan, \
             patch.object(discovery, "Watcher") as watcher, \
             patch.object(discovery.selectors, "DefaultSelector", return_value=Selector()), \
             patch("builtins.print"):
            watcher.return_value.configure.return_value = []
            with self.assertRaises(StopLoop):
                discovery.main()
            self.assertEqual(scan.call_count, 0)

    def test_process_exits_on_gui_pipe_close(self):
        with subprocess.Popen([sys.executable, "-I", "-u", str(SCRIPT)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            process.stdin.write(b'{"paths":[]}\n')
            process.stdin.flush()
            self.assertTrue(select.select([process.stdout], [], [], 5)[0])
            self.assertIn("endpoints", json.loads(process.stdout.readline()))
            process.stdin.close()
            self.assertEqual(process.wait(timeout=5), 0)

    @unittest.skipUnless(os.environ.get("ARCHCODEX_TEST_CODEX"), "Optional isolated real Codex test")
    def test_new_codex_server_is_discovered_by_event(self):
        root = Path(f"/tmp/codex-daemon-{os.getuid()}")
        root.mkdir(mode=0o700, exist_ok=True)
        path = root / f"archcodex-test-{os.getpid()}.sock"
        self.assertFalse(path.exists())
        home = self.root / "isolated-codex-home"
        home.mkdir()
        environment = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "CODEX_HOME": str(home)}
        with subprocess.Popen([sys.executable, "-I", "-u", str(SCRIPT)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as helper:
            helper.stdin.write(b'{"paths":[]}\n')
            helper.stdin.flush()
            self.assertTrue(select.select([helper.stdout], [], [], 5)[0])
            initial = json.loads(helper.stdout.readline())
            previous = {item["path"] for item in initial["endpoints"]}
            server = subprocess.Popen([os.environ["ARCHCODEX_TEST_CODEX"], "app-server",
                                       "--listen", "unix://" + str(path), "-c", "analytics.enabled=false"],
                                      env=environment, stdin=subprocess.DEVNULL,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                found = False
                while time.monotonic() < deadline and not found:
                    ready = select.select([helper.stdout], [], [], max(0, deadline - time.monotonic()))[0]
                    if not ready:
                        break
                    message = json.loads(helper.stdout.readline())
                    found = any(item["path"] not in previous and
                                item["executable"] == os.environ["ARCHCODEX_TEST_CODEX"]
                                for item in message["endpoints"])
                self.assertTrue(found, "Server creation did not produce a discovery event")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
                path.unlink(missing_ok=True)
                helper.stdin.close()
                helper.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
