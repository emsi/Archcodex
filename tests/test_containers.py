# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from unittest.mock import AsyncMock

SCRIPT = Path(__file__).resolve().parents[1] / "src/containers.py"
spec = importlib.util.spec_from_file_location("containers", SCRIPT)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
CID = "a" * 64


def container(cid=CID):
    return {"Id": cid, "Name": "/example-codex", "State": {"Running": True, "StartedAt": "epoch-1"},
            "Config": {"Labels": {adapter.LABEL: "none"}, "WorkingDir": "/work/project",
                       "Env": ["MYCODEX_HOST_UID=1000", "MYCODEX_CONTAINER_HOME=/home/alice",
                               "CODEX_HOME=/home/alice/.codex", "SECRET_MUST_NOT_ESCAPE=secret"]},
            "Mounts": [{"Type": "volume", "Destination": "/home/alice", "Name": "codex_state"}]}


class ContainerTests(unittest.TestCase):
    def test_initial_engine_failure_reconnects_without_polling_healthy_engine(self):
        async def check():
            inventory = adapter.Inventory("fixture")
            inventory.pending.add("default")
            with patch.object(adapter, "run", new=AsyncMock(side_effect=[OSError("offline"), b"engine"])) as run:
                with patch.object(inventory, "connect_engine", new=AsyncMock()) as connected:
                    await asyncio.wait_for(inventory.connect_context("default", set()), 3)
                    self.assertEqual(run.await_count, 2)
                    connected.assert_awaited_once_with("default", "engine")
                    self.assertFalse(inventory.errors)
                    self.assertFalse(inventory.pending)
        asyncio.run(check())

    def test_shared_history_is_not_execution_identity(self):
        a = adapter.descriptor(container())
        b = adapter.descriptor(container("b" * 64))
        self.assertEqual(a["stateVolume"], b["stateVolume"])
        self.assertNotEqual(a["environment"], b["environment"])
        self.assertNotIn("secret", json.dumps(a))
        self.assertNotIn("SECRET", json.dumps(a))

    def test_metadata_does_not_guess_private_mode_or_root(self):
        info = container()
        info["Mounts"][0]["Name"] = "example_codex_state"
        self.assertEqual(adapter.descriptor(info)["stateMode"], "Custom storage")
        info["Config"]["Env"] = [x for x in info["Config"]["Env"] if not x.startswith("MYCODEX_HOST_UID=")]
        with self.assertRaises(ValueError): adapter.descriptor(info)

    def test_launch_revalidates_lifetime_and_preserves_argument_boundaries(self):
        info = container()
        desc = adapter.descriptor(info)
        data = {"containerId": CID, "containerEpoch": "epoch-1", "dockerContext": "default",
                "runtimeUser": "alice", "identity": {"containerEnvironment": desc["environment"], "dockerEngine": "engine-fixture"},
                "socket": "/tmp/socket with spaces;$(false)", "executable": "/bin/codex",
                "thread": "thread;$(false)", "cwd": "/work/project"}
        def run(args):
            if "info" in args: return b"engine-fixture"
            return json.dumps("unix:///run/docker.sock" if args[1] == "context" else [info]).encode()
        with patch.object(adapter, "sync_run", side_effect=run), patch.object(adapter.os, "execvp") as execute:
            adapter.launch("proxy", data)
            args = execute.call_args.args[1]
            self.assertIn("-i", args)
            self.assertNotIn("-it", args)
            self.assertEqual(args[-2:], ["--sock", data["socket"]])
            self.assertIn("alice", args)
            data["identity"]["serverCodexHome"] = "/home/alice/.codex-project"
            adapter.launch("tui", data)
            args = execute.call_args.args[1]
            self.assertIn("-it", args)
            self.assertIn("CODEX_HOME=/home/alice/.codex-project", args)
            self.assertEqual(args[-3:], ["resume", "--", data["thread"]])
            data["containerEpoch"] = "old-epoch"
            with self.assertRaisesRegex(ValueError, "lifetime"): adapter.launch("proxy", data)
            self.assertEqual(execute.call_count, 2)

    def test_launch_rejects_remote_docker_context_and_changed_mounts(self):
        info = container()
        data = {"containerId": CID, "dockerContext": "default", "containerEpoch": "epoch-1",
                "identity": {"containerEnvironment": "old", "dockerEngine": "engine-fixture"}}
        with patch.object(adapter, "sync_run", return_value=b'"ssh://unexpected"'):
            with self.assertRaisesRegex(ValueError, "Unix Docker"): adapter.launch("proxy", data)
        with patch.object(adapter, "sync_run", side_effect=[b'"unix:///run/docker.sock"', b"engine-fixture", json.dumps([info]).encode()]):
            with self.assertRaisesRegex(ValueError, "context changed"): adapter.launch("proxy", data)
        with patch.object(adapter, "sync_run", side_effect=[b'"unix:///run/docker.sock"', b"different-engine"]):
            with self.assertRaisesRegex(ValueError, "engine identity changed"): adapter.launch("proxy", data)

    def test_chunked_command_output_is_not_truncated(self):
        async def check():
            reader = asyncio.StreamReader()
            async def feed():
                reader.feed_data(b"one")
                await asyncio.sleep(0)
                reader.feed_data(b"two")
                reader.feed_eof()
            task = asyncio.create_task(feed())
            result = await adapter.bounded_read(reader)
            await task
            self.assertEqual(result, b"onetwo")
        asyncio.run(check())

    def test_event_driven_inventory_and_owned_watcher_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state = root / "state.json"
            state.write_text(json.dumps([container()]))
            log = root / "commands"
            fifo = root / "events"
            os.mkfifo(fifo)
            docker = root / "docker"
            docker.write_text("#!" + sys.executable + "\n" + r'''
import json,os,sys
from pathlib import Path
root=Path(os.environ['ARCHCODEX_DOCKER_FIXTURE'])
args=sys.argv[1:]
with (root/'commands').open('a') as f: f.write(json.dumps(args)+'\n')
if args[:2]==['context','ls']:
 print(json.dumps({'Name':'default','DockerEndpoint':'unix:///run/docker.sock'}))
 print(json.dumps({'Name':'remote','DockerEndpoint':'ssh://must-not-contact'}))
 sys.exit()
assert args[:2]==['--context','default'],args
args=args[2:]
state=json.loads((root/'state.json').read_text())
if args[0]=='info': print('engine-fixture')
elif args[0]=='ps':
 for c in state: print(c['Id'])
elif args[0]=='inspect': print(json.dumps([c for c in state if c['Id']==args[-1]]))
elif args[0]=='events':
 with (root/'events').open() as f:
  for line in f: print(line,end='',flush=True)
elif args[0]=='exec':
 sys.stdin.readline()
 print(json.dumps({'runtimeUser':'alice'}),flush=True)
 print(json.dumps({'endpoints':[{'path':'/tmp/codex.sock','executable':'/bin/codex'}],'issues':[]}),flush=True)
 sys.stdin.read()
else: raise RuntimeError(args)
''')
            docker.chmod(0o700)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ.get("PATH", ""),
                       ARCHCODEX_DOCKER_FIXTURE=str(root))
            proc = subprocess.Popen([sys.executable, "-B", str(SCRIPT), "watch"], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                                    start_new_session=True)
            event_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            buffer = bytearray()
            def until(predicate):
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if b"\n" in buffer:
                        end = buffer.index(b"\n")
                        message = json.loads(buffer[:end])
                        del buffer[:end + 1]
                        if predicate(message): return message
                    elif select.select([proc.stdout], [], [], 0.1)[0]:
                        block = os.read(proc.stdout.fileno(), 65536)
                        if not block: self.fail(proc.stderr.read().decode())
                        buffer.extend(block)
                self.fail("No matching inventory event")
            try:
                proc.stdin.write(b'{"script":"fixture"}\n'); proc.stdin.flush()
                until(lambda m: m["complete"] and m["containers"] and m["containers"][0]["endpoints"])
                previous = log.read_text()
                time.sleep(0.15)
                self.assertEqual(previous, log.read_text(), "Idle discovery must not poll")
                self.assertNotIn('"remote"', previous)
                state.write_text("[]")
                os.write(event_fd, b'{"Action":"die"}\n')
                until(lambda m: m["complete"] and not m["containers"])
                proc.stdin.close()
                self.assertEqual(proc.wait(timeout=5), 0, proc.stderr.read().decode())
            finally:
                os.close(event_fd)
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                proc.stdout.close(); proc.stderr.close()


if __name__ == "__main__": unittest.main()
