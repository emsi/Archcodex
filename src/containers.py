# SPDX-License-Identifier: GPL-3.0-or-later
"""Archcodex-owned Docker adapter. No installation, lifecycle actions or polling.

Executed on the Docker host, locally or through SSH. Only proxy/TUI clients and
event watchers are children of this helper. All protocol execs have no PTY.
"""
import asyncio
import base64
import contextlib
import ctypes
import hashlib
import json
import os
import pwd
import re
import signal
import socket
import subprocess
import sys
import time

LIMIT = 8 * 1024 * 1024
MAX_CONTAINERS = 512
LABEL = "io.infrasecture.mycodex.gui"
CID = re.compile(r"^[0-9a-f]{64}$")
_LIBC = ctypes.CDLL(None, use_errno=True)


def child_lifetime(parent):
    # Linux child-side only, before exec. A killed helper must not leave SSH or
    # Docker clients behind. Container exec programs still own their EOF cleanup.
    if _LIBC.prctl(1, signal.SIGTERM, 0, 0, 0) != 0 or os.getppid() != parent:
        os._exit(125)


async def spawn(*args, **kwargs):
    parent = os.getpid()
    return await asyncio.create_subprocess_exec(*args, start_new_session=True,
                preexec_fn=lambda: child_lifetime(parent), **kwargs)


def text(value, maximum=4096):
    return isinstance(value, str) and len(value) <= maximum and all(ord(c) >= 32 and ord(c) != 127 for c in value)


def issue(message, details=""):
    return {"code": "containers", "summary": message[:512],
            "explanation": "Container discovery is incomplete. Existing conversations are not modified.",
            "action": "Check host Docker/Python access and SSH authentication, then retry discovery.",
            "details": [details[:2048]] if details else []}


def descriptor(info):
    """Copy only explicitly allowed non-secret runtime metadata from Docker."""
    cid = info.get("Id", "")
    config, state = info.get("Config") or {}, info.get("State") or {}
    labels = config.get("Labels") or {}
    if not CID.fullmatch(cid) or LABEL not in labels:
        raise ValueError("Not an identified myCodex container")
    env = dict(entry.split("=", 1) for entry in config.get("Env", []) if "=" in entry)
    uid = env.get("MYCODEX_HOST_UID", "")
    if not uid.isdecimal() or not 0 <= int(uid) < 2**32:
        raise ValueError("myCodex runtime UID is missing; automatic root execution is disabled")
    home = env.get("MYCODEX_CONTAINER_HOME", "")
    codex_home = env.get("CODEX_HOME", home + "/.codex")
    if not home.startswith("/") or not text(home) or not codex_home.startswith("/") or not text(codex_home):
        raise ValueError("Invalid myCodex runtime home")
    # Docker does not guarantee Mounts order. Canonicalize the list as well as
    # object keys, retaining every field so real mount changes still invalidate
    # the execution context. Do not mutate the discovery response.
    mounts = sorted(info.get("Mounts") or [], key=lambda mount: json.dumps(mount, sort_keys=True))
    volume = next((m.get("Name", "") for m in mounts if m.get("Destination") == home and m.get("Type") == "volume"), "")
    # A project-looking volume name is not proof of private mode on legacy images.
    mode = labels.get("io.infrasecture.mycodex.state-mode")
    if mode not in ("Private", "Shared", "Custom"):
        mode = "Shared" if volume == "codex_state" else "Custom storage"
    fingerprint = json.dumps([cid, uid, home, codex_home, mounts], sort_keys=True)
    return {"containerId": cid, "containerEpoch": state.get("StartedAt", ""),
            "containerName": info.get("Name", cid[:12]).lstrip("/"), "runtimeUid": uid,
            "runtimeHome": home, "codexHome": codex_home,
            "cwd": config.get("WorkingDir") or "/", "stateMode": mode,
            "stateVolume": volume, "running": bool(state.get("Running")),
            "paused": bool(state.get("Paused")),
            "environment": hashlib.sha256(fingerprint.encode()).hexdigest()}


def docker_prefix(context):
    if not text(context, 256) or not context or context.startswith("-"):
        raise ValueError("Invalid Docker context")
    return ["docker", "--context", context]


def exec_args(context, desc, command, terminal=False):
    user = desc.get("runtimeUser") or desc["runtimeUid"]
    if not text(user, 128) or not user or user.startswith("-"):
        raise ValueError("Invalid runtime user")
    return docker_prefix(context) + ["exec", "-it" if terminal else "-i", "--user", user,
            "--env", "HOME=" + desc["runtimeHome"], "--env", "CODEX_HOME=" + desc["codexHome"],
            "--env", "USER=" + user, "--env", "LOGNAME=" + user,
            desc["containerId"]] + command


def sync_run(args):
    return asyncio.run(run(args))


def launch(mode, data):
    cid, context = data.get("containerId", ""), data.get("dockerContext", "")
    if not text(cid, 64) or not CID.fullmatch(cid):
        raise ValueError("Invalid container ID")
    docker_prefix(context)
    endpoint = json.loads(sync_run(["docker", "context", "inspect", context, "--format", "{{json .Endpoints.docker.Host}}"] ))
    if not isinstance(endpoint, str) or not endpoint.startswith("unix://"):
        raise ValueError("This adapter requires a local Unix Docker engine on the selected host")
    engine = sync_run(docker_prefix(context) + ["info", "--format", "{{.ID}}"] ).decode().strip()
    if not engine or engine != data.get("identity", {}).get("dockerEngine"):
        raise ValueError("Docker engine identity changed. Rediscover before reconnecting.")
    info = json.loads(sync_run(docker_prefix(context) + ["inspect", "--type", "container", cid]))[0]
    desc = descriptor(info)
    if not desc["running"] or desc["paused"] or desc["containerEpoch"] != data.get("containerEpoch"):
        raise ValueError("Container lifetime changed or is unavailable. Rediscover before reconnecting.")
    if desc["environment"] != data.get("identity", {}).get("containerEnvironment"):
        raise ValueError("Container execution context changed. Rediscover before reconnecting.")
    user = data.get("runtimeUser", "")
    if not text(user, 128) or not user:
        raise ValueError("Container runtime user was not discovered")
    desc["runtimeUser"] = user
    server_home = data.get("identity", {}).get("serverCodexHome", desc["codexHome"])
    if not text(server_home) or not server_home.startswith("/"):
        raise ValueError("Invalid server configuration home")
    desc["codexHome"] = server_home
    path, executable = data.get("socket", ""), data.get("executable", "codex")
    if not text(path) or not path.startswith("/") or not text(executable) or not (executable == "codex" or executable.startswith("/")):
        raise ValueError("Invalid explicit server endpoint")
    if mode == "proxy":
        command = [executable, "app-server", "proxy", "--sock", path]
    else:
        thread, cwd = data.get("thread", ""), data.get("cwd", "")
        if not text(thread, 128) or not text(cwd) or (cwd and not cwd.startswith("/")):
            raise ValueError("Invalid conversation or working directory")
        command = [executable, "--remote", "unix://" + path]
        command += ["resume", "--", thread] if thread else ["--cd", cwd]
        # Check/chdir as the actual runtime user; Docker --workdir may create a directory.
        command = ["/usr/bin/python3", "-I", "-c",
                   "import os,sys; os.chdir(sys.argv[1]); os.execvp(sys.argv[2],sys.argv[2:])",
                   cwd or desc["runtimeHome"]] + command
    args = exec_args(context, desc, command, mode == "tui")
    os.execvp(args[0], args)


async def stop(process):
    if process is None:
        return
    try:
        if process.returncode is not None:
            return
        if process.stdin:
            process.stdin.close()
            try:
                await asyncio.wait_for(process.wait(), 0.5)
                return
            except asyncio.TimeoutError:
                pass
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            await asyncio.wait_for(process.wait(), 1)
        except asyncio.TimeoutError:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
    finally:
        # asyncio Process has no public close API. Closing its transport releases
        # paused/full stdout pipes too; otherwise wait() can hang even after KILL.
        for descriptor in (0, 1, 2):
            pipe = process._transport.get_pipe_transport(descriptor)
            if pipe:
                pipe.close()
        if process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
        await process.wait()
        process._transport.close()


async def bounded_read(stream):
    data = bytearray()
    while block := await stream.read(65536):
        data.extend(block)
        if len(data) > LIMIT:
            raise ValueError("Docker output exceeded its limit")
    return bytes(data)


async def run(args):
    proc = await spawn(*args, stdin=asyncio.subprocess.DEVNULL,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        out, err = await asyncio.wait_for(asyncio.gather(bounded_read(proc.stdout), bounded_read(proc.stderr)), 15)
        if await proc.wait():
            raise ValueError(err.decode(errors="replace")[-2048:] or "Docker command failed")
        return out
    finally:
        await stop(proc)


async def drain_errors(process, tail):
    while block := await process.stderr.read(4096):
        tail[:] = (tail + block)[-4096:]


class Inventory:
    def __init__(self, script):
        self.script, self.records, self.errors, self.ready = script, {}, {}, set()
        self.engines = set()
        self.engine_dirty = {}
        self.pending = set()
        self.changed = asyncio.Event()
        self.semaphore = asyncio.Semaphore(8)
        self.host = socket.gethostname()
        try:
            self.user = pwd.getpwuid(os.getuid()).pw_name
        except KeyError:
            self.user = str(os.getuid())

    def publish(self):
        self.changed.set()

    async def output(self):
        while True:
            await self.changed.wait()
            self.changed.clear()
            message = {"host": self.host, "user": self.user,
                       "containers": list(self.records.values()), "issues": list(self.errors.values()),
                       "complete": bool(self.engines) and self.ready == self.engines and not self.errors and not self.pending}
            encoded = json.dumps(message, ensure_ascii=True, separators=(",", ":"))
            if len(encoded) > LIMIT:
                raise ValueError("Container inventory exceeded its limit")
            # stdout is a pipe to the UI worker. A slow reader cannot block Docker event consumption.
            await asyncio.to_thread(print, encoded, flush=True)

    async def container(self, context, engine, desc):
        key = engine + ":" + desc["containerId"]
        desc.update(dockerContext=context, engine=engine, endpoints=[], issues=[])
        self.records[key] = desc
        self.publish()
        if desc["paused"]:
            desc["issues"] = [issue("Container is paused")]
            self.publish()
            return
        proc, stderr_task = None, None
        tail = bytearray()
        try:
            # Resolve the entrypoint's actual account by UID; it need not equal the requested name.
            preamble = "import json,os,pwd; print(json.dumps({'runtimeUser':pwd.getpwuid(os.getuid()).pw_name}),flush=True)\n"
            async with self.semaphore:
                proc = await spawn(*exec_args(context, desc,
                    ["/usr/bin/python3", "-I", "-u", "-c", preamble + self.script]),
                    stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE, limit=LIMIT)
                stderr_task = asyncio.create_task(drain_errors(proc, tail))
                proc.stdin.write(b'{"paths":[]}\n')
                await proc.stdin.drain()
                first = await asyncio.wait_for(proc.stdout.readline(), 15)
                account = json.loads(first)
                user = account.get("runtimeUser", "")
                if not text(user, 128) or not user:
                    raise ValueError("Cannot resolve myCodex runtime user")
                desc["runtimeUser"] = user
                first = await asyncio.wait_for(proc.stdout.readline(), 15)
            while first:
                report = json.loads(first)
                if not isinstance(report.get("endpoints"), list) or not isinstance(report.get("issues"), list):
                    raise ValueError("Invalid container discovery response")
                desc["endpoints"], desc["issues"] = report["endpoints"], report["issues"]
                self.publish()
                first = await proc.stdout.readline()  # No timeout or recurring state query.
            raise ValueError("Container socket watcher stopped")
        except (OSError, ValueError, asyncio.TimeoutError) as error:
            desc["endpoints"] = []
            desc["issues"] = [issue(str(error) or "Container discovery timed out", tail.decode(errors="replace"))]
            self.publish()
        finally:
            await stop(proc)
            if stderr_task:
                stderr_task.cancel()
                await asyncio.gather(stderr_task, return_exceptions=True)

    async def engine(self, context, engine):
        children, epochs = {}, {}
        proc, events_task, stderr_task = None, None, None
        tail = bytearray()
        dirty = asyncio.Event()
        dirty.set()
        self.engine_dirty[engine] = dirty
        async def events():
            while line := await proc.stdout.readline():
                event = json.loads(line)
                if event.get("Action", event.get("status")) in {"start", "die", "destroy", "pause", "unpause", "rename", "restart"}:
                    dirty.set()
            raise ValueError("Docker event stream disconnected")
        try:
            prefix = docker_prefix(context)
            # Replay events overlapping subscription startup; the inventory is authoritative on recovery.
            proc = await spawn(*prefix, "events", "--since", str(int(time.time())),
                        "--filter", "type=container", "--filter", "label=" + LABEL,
                        "--format", "{{json .}}", stdin=asyncio.subprocess.DEVNULL,
                        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=LIMIT)
            stderr_task = asyncio.create_task(drain_errors(proc, tail))
            events_task = asyncio.create_task(events())
            while True:
                waiter = asyncio.create_task(dirty.wait())
                done, _ = await asyncio.wait([waiter, events_task], return_when=asyncio.FIRST_COMPLETED)
                if events_task in done:
                    waiter.cancel()
                    await asyncio.gather(waiter, return_exceptions=True)
                    await events_task
                dirty.clear()
                ids = (await run(prefix + ["ps", "--no-trunc", "--filter", "label=" + LABEL, "--format", "{{.ID}}"])).decode().splitlines()
                if len(ids) > MAX_CONTAINERS or any(not CID.fullmatch(cid) for cid in ids):
                    raise ValueError("Invalid or excessive container inventory")
                for cid in set(children) - set(ids):
                    child = children.pop(cid)
                    child.cancel()
                    await asyncio.gather(child, return_exceptions=True)
                    epochs.pop(cid, None)
                    self.records.pop(engine + ":" + cid, None)
                for cid in ids:
                    info = {}
                    try:
                        async with self.semaphore:
                            info = json.loads(await run(prefix + ["inspect", "--type", "container", cid]))[0]
                        desc = descriptor(info)
                        if not desc["running"]:
                            continue
                        epoch = (desc["containerEpoch"], desc["paused"], desc["environment"])
                        if epochs.get(cid) != epoch or children[cid].done():
                            if cid in children:
                                children[cid].cancel()
                                await asyncio.gather(children[cid], return_exceptions=True)
                            epochs[cid] = epoch
                            children[cid] = asyncio.create_task(self.connect_container(context, engine, desc))
                        elif engine + ":" + cid in self.records:
                            self.records[engine + ":" + cid]["containerName"] = desc["containerName"]
                    except (OSError, ValueError, asyncio.TimeoutError) as error:
                        self.records[engine + ":" + cid] = {"containerId": cid, "containerName": info.get("Name", cid[:12]).lstrip("/"),
                            "engine": engine, "dockerContext": context, "endpoints": [], "issues": [issue(str(error))]}
                self.ready.add(engine)
                self.errors.pop(engine, None)
                self.publish()
        finally:
            self.engine_dirty.pop(engine, None)
            self.ready.discard(engine)
            for task in [events_task, stderr_task, *children.values()]:
                if task:
                    task.cancel()
            await asyncio.gather(*(task for task in [events_task, stderr_task, *children.values()] if task), return_exceptions=True)
            await stop(proc)

    async def connect_container(self, context, engine, desc):
        delay = 1
        while True:
            await self.container(context, engine, desc)
            if desc["paused"]:
                return
            # Only restart a failed watcher. Healthy watchers wait on kernel events.
            await asyncio.sleep(delay)
            delay = min(30, delay * 2)

    async def connect_engine(self, context, engine):
        delay = 1
        while True:
            try:
                await self.engine(context, engine)
            except (OSError, ValueError, asyncio.TimeoutError) as error:
                self.errors[engine] = issue(context + ": " + (str(error) or "Docker timed out"))
                self.publish()
            # Disconnected transport retry, never a running-container/status poll.
            await asyncio.sleep(delay)
            delay = min(30, delay * 2)

    async def connect_context(self, name, seen):
        delay = 1
        while True:
            try:
                engine = (await run(docker_prefix(name) + ["info", "--format", "{{.ID}}"])).decode().strip()
                if not text(engine, 256) or not engine:
                    raise ValueError("Docker returned no engine identity")
                self.errors.pop(name, None)
                self.pending.discard(name)
                if engine in seen:
                    self.publish()
                    return
                seen.add(engine)
                self.engines.add(engine)
                await self.connect_engine(name, engine)
                return
            except (OSError, ValueError, asyncio.TimeoutError) as error:
                self.errors[name] = issue(name + ": " + (str(error) or "Docker timed out"))
                self.publish()
            await asyncio.sleep(delay)  # Retry a failed engine connection only.
            delay = min(30, delay * 2)


async def watch():
    request = json.loads(await asyncio.to_thread(sys.stdin.readline))
    script = request.get("script", "")
    if not isinstance(script, str) or not 0 < len(script) < 100000:
        raise ValueError("Missing packaged endpoint discovery code")
    inventory = Inventory(script)
    tasks = [asyncio.create_task(inventory.output())]
    try:
        contexts = (await run(["docker", "context", "ls", "--format", "{{json .}}"])).decode().splitlines()
        seen = set()
        for line in contexts[:64]:
            ctx = json.loads(line)
            if not ctx.get("DockerEndpoint", "").startswith("unix://"):
                continue  # Never follow remote Docker contexts implicitly.
            name = ctx.get("Name", "")
            inventory.pending.add(name)
            tasks.append(asyncio.create_task(inventory.connect_context(name, seen)))
        if len(contexts) > 64:
            inventory.errors["limit"] = issue("Docker context inventory exceeds the 64-context limit")
        if not inventory.pending and not inventory.errors:
            inventory.errors["docker"] = issue("No accessible local Unix Docker engines")
        inventory.publish()
        # EOF/signals end only this connection's watchers. No permanently blocked
        # executor thread can prevent cleanup after SSH or the GUI disconnects.
        closed = asyncio.Event()
        loop = asyncio.get_running_loop()
        def input_ready():
            if not os.read(sys.stdin.fileno(), 65536):
                closed.set()
                loop.remove_reader(sys.stdin.fileno())
        loop.add_reader(sys.stdin.fileno(), input_ready)
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            loop.add_signal_handler(sig, closed.set)
        ended = asyncio.create_task(closed.wait())
        done, _ = await asyncio.wait([ended, tasks[0]], return_when=asyncio.FIRST_COMPLETED)
        loop.remove_reader(sys.stdin.fileno())
        ended.cancel()
        await asyncio.gather(ended, return_exceptions=True)
        if tasks[0] in done:
            await tasks[0]
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def main():
    mode = sys.argv[1]
    if mode == "watch":
        asyncio.run(watch())
    elif mode in ("proxy", "tui"):
        launch(mode, json.loads(base64.b64decode(sys.argv[2], validate=True)))
    else:
        raise ValueError("Unknown adapter operation")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError, IndexError) as error:
        print("Archcodex container connection: " + str(error)[:2048], file=sys.stderr, flush=True)
        sys.exit(1)
