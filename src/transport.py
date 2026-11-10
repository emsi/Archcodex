# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared desktop/host/container transport. Remote roles have no listeners."""
import asyncio
import base64
import contextlib
import json
import os
import pwd
import re
import secrets
import shlex
import shutil
import signal
import socket
import stat
import struct
import sys
import zlib

import containers
import discover
import mux

BUNDLE = ""  # Set by the packaged, isolated bootstrap, not the environment.
HOST = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.@:%+\[\]-]{0,254}$")
TOKEN = re.compile(r"^[a-f0-9]{32}$")


def bootstrap():
    bundle = BUNDLE
    if not bundle:  # Source-tree tests; packaged roles never read external code.
        from pathlib import Path
        root = Path(__file__).parent
        sources = {n: (root / (n + '.py')).read_text() for n in ('mux', 'containers', 'discover', 'transport')}
        bundle = base64.b64encode(zlib.compress(mux.encoded(sources))).decode()
    return ("import base64,json,sys,types,zlib\n"
            "S=json.loads(zlib.decompress(base64.b64decode('" + bundle + "')))\n"
            "for n in ('mux','containers','discover','transport'):\n"
            " m=types.ModuleType(n);m.__file__='<packaged-'+n+'>';sys.modules[n]=m;exec(S[n],m.__dict__)\n"
            "sys.modules['transport'].BUNDLE='" + bundle + "'\n"
            "sys.modules['transport'].main()\n")


def command(role):
    return ['/usr/bin/python3', '-I', '-u', '-c', bootstrap(), role]


async def stdio():
    # Beast's observer bridge supplies one full-duplex socket as both stdin
    # and stdout. A write-pipe transport would mistake readable data for EOF.
    incoming, outgoing = os.fstat(0), os.fstat(1)
    if stat.S_ISSOCK(incoming.st_mode) and (incoming.st_dev, incoming.st_ino) == (outgoing.st_dev, outgoing.st_ino):
        sock = socket.socket(fileno=os.dup(0))
        sock.setblocking(False)
        return await asyncio.open_connection(sock=sock)
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader(limit=mux.MAX_FRAME * 2)
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader),
                                 os.fdopen(os.dup(0), 'rb', buffering=0))
    protocol = asyncio.streams.FlowControlMixin(loop=loop)
    transport, _ = await loop.connect_write_pipe(lambda: protocol,
                                                os.fdopen(os.dup(1), 'wb', buffering=0))
    writer = asyncio.StreamWriter(transport, protocol, None, loop)
    return reader, writer


async def cancel(*tasks):
    tasks = [t for t in tasks if t and t is not asyncio.current_task()]
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)


async def wait_connection(connection):
    await connection.done.wait()
    raise mux.Closed(connection.error)


async def consume_errors(process, tail):
    while block := await process.stderr.read(4096):
        tail[:] = (tail + block)[-4096:]


class ChildConnection:
    def __init__(self, args):
        self.args, self.process, self.connection, self.error_task = args, None, None, None
        self.tail = bytearray()

    async def start(self):
        self.process = await containers.spawn(*self.args, stdin=asyncio.subprocess.PIPE,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                    limit=mux.MAX_FRAME * 2)
        self.error_task = asyncio.create_task(consume_errors(self.process, self.tail))
        self.connection = mux.Mux(self.process.stdout, self.process.stdin)
        return await self.connection.start()

    async def stop(self):
        if self.connection:
            await self.connection.stop()
        await containers.stop(self.process)
        await cancel(self.error_task)


class ContainerAgent:
    def __init__(self):
        self.endpoints = {}
        self.snapshot = {}
        self.changed = asyncio.Event()
        self.dirty = asyncio.Event()
        self.inventory_open = False
        self.connection = None

    async def scan(self):
        uid = os.getuid()
        home = os.environ.get('CODEX_HOME') or os.path.expanduser('~/.codex')
        default = os.path.join(home, 'app-server-control', 'app-server-control.sock')
        roots = [f'/tmp/codex-daemon-{uid}']
        watcher = discover.Watcher()
        dirty = self.dirty
        dirty.set()
        loop = asyncio.get_running_loop()
        def notified():
            if watcher.changed():
                dirty.set()
        loop.add_reader(watcher.fd, notified)
        try:
            while True:
                await dirty.wait()
                dirty.clear()
                paths = [default] + [v['path'] for v in self.endpoints.values()]
                watcher.configure(roots, paths)
                endpoints, issues = discover.snapshot(default=default, aliases=paths)
                registered = {}
                for ep in endpoints:
                    try:
                        info = os.stat(ep['path'], follow_symlinks=False)
                        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != uid:
                            continue
                        signature = (ep['path'], info.st_dev, info.st_ino, ep.get('pid'))
                        token = next((k for k, v in self.endpoints.items()
                                      if v['_signature'] == signature), None) or secrets.token_hex(16)
                        registered[token] = dict(ep, endpointToken=token, _signature=signature)
                    except OSError:
                        continue
                self.endpoints = registered
                public = [{k: v for k, v in ep.items() if not k.startswith('_')}
                          for ep in registered.values()]
                issues += watcher.configure(roots, [default] + [ep['path'] for ep in public])
                self.snapshot = {'runtimeUser': pwd.getpwuid(uid).pw_name,
                                 'endpoints': public, 'issues': issues}
                self.changed.set()
        finally:
            loop.remove_reader(watcher.fd)
            watcher.close()

    async def accept(self, stream, request):
        if request.get('kind') == 'refresh':
            self.dirty.set()
            stream.mux.accepted(stream)
        elif request.get('kind') == 'inventory':
            if self.inventory_open:
                raise ValueError('Inventory already attached')
            self.inventory_open = True
            stream.mux.accepted(stream)
            async def output():
                while not stream.closed:
                    await self.changed.wait()
                    self.changed.clear()
                    await mux.report(stream, self.snapshot)
            try:
                await mux.produce(stream, output())
            finally:
                self.inventory_open = False
        elif request.get('kind') == 'endpoint':
            ep = self.endpoints.get(request.get('token', ''))
            if not ep:
                raise ValueError('Endpoint changed; rediscover before reconnecting')
            info = os.stat(ep['path'], follow_symlinks=False)
            if (ep['path'], info.st_dev, info.st_ino, ep.get('pid')) != ep['_signature'] or \
                    not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise ValueError('Socket identity changed; rediscover before reconnecting')
            reader, writer = await asyncio.wait_for(asyncio.open_unix_connection(ep['path']), 10)
            try:
                peer = writer.get_extra_info('socket').getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                pid, uid, _ = struct.unpack('3i', peer)
                if uid != os.getuid() or (ep.get('pid') and pid != ep['pid']):
                    raise ValueError('App-server socket peer changed')
                stream.mux.accepted(stream)
                await mux.relay(stream, mux.Socket(reader, writer))
            finally:
                writer.close()
        else:
            raise ValueError('Unknown container transport operation')

    async def run(self, reader, writer):
        connection = self.connection = mux.Mux(reader, writer, self.accept, initiator=False)
        scan = ended = None
        try:
            await connection.start()
            scan = asyncio.create_task(self.scan())
            ended = asyncio.create_task(connection.done.wait())
            done, _ = await asyncio.wait([scan, ended], return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            await cancel(scan, ended)
            await connection.stop()


class SharedInventory(containers.Inventory):
    def __init__(self):
        super().__init__('')
        self.attachments = {}
        self.consumer = None
        self.contexts_changed = asyncio.Event()

    async def refresh(self):
        self.contexts_changed.set()
        for dirty in self.engine_dirty.values():
            dirty.set()
        async def refresh_container(connection):
            try:
                stream = await connection.open({'kind': 'refresh'})
                stream.close()
            except (OSError, ValueError, asyncio.TimeoutError):
                pass  # Failed attachments already have their own recovery path.
        await asyncio.gather(*(refresh_container(connection)
                               for _, connection, _ in list(self.attachments.values())))

    async def output(self):
        while True:
            await self.changed.wait()
            self.changed.clear()
            await mux.report(self.consumer, {'host': self.host, 'user': self.user,
                'containers': list(self.records.values()), 'issues': list(self.errors.values()),
                'complete': bool(self.engines) and self.ready == self.engines and not self.errors and not self.pending})

    async def container(self, context, engine, desc):
        key = engine + ':' + desc['containerId']
        desc.update(dockerContext=context, engine=engine, endpoints=[], issues=[])
        self.records[key] = desc
        self.publish()
        if desc['paused']:
            desc['issues'] = [containers.issue('Container is paused')]
            self.publish()
            return
        child = ChildConnection(containers.exec_args(context, desc, command('container')))
        try:
            async with self.semaphore:
                # Recheck the actual engine/container before opening the single
                # attachment. Subsequent streams stay pinned to that process.
                endpoint = json.loads(await containers.run(['docker', 'context', 'inspect', context,
                                         '--format', '{{json .Endpoints.docker.Host}}']))
                if not isinstance(endpoint, str) or not endpoint.startswith('unix://'):
                    raise ValueError('Only a local Unix Docker engine is supported on the selected host')
                current_engine = (await containers.run(containers.docker_prefix(context) +
                                                       ['info', '--format', '{{.ID}}'])).decode().strip()
                info = json.loads(await containers.run(containers.docker_prefix(context) +
                            ['inspect', '--type', 'container', desc['containerId']]))[0]
                current = containers.descriptor(info)
                if current_engine != engine or current['environment'] != desc['environment'] or \
                        current['containerEpoch'] != desc['containerEpoch'] or not current['running'] or current['paused']:
                    raise ValueError('Container execution context changed; rediscover before reconnecting')
                connection = await child.start()
                stream = await connection.open({'kind': 'inventory'})
                messages = mux.messages(stream)
                first = await asyncio.wait_for(anext(messages), 20)
            generation = secrets.token_hex(16)
            self.attachments[key] = (generation, connection, desc)
            while True:
                if not isinstance(first.get('endpoints'), list) or not isinstance(first.get('issues'), list):
                    raise ValueError('Invalid container endpoint inventory')
                user = first.get('runtimeUser', '')
                if not containers.text(user, 128) or not user or user.startswith('-'):
                    raise ValueError('Invalid container runtime account')
                desc['runtimeUser'] = user
                desc['endpoints'] = [dict(ep, attachment=generation) for ep in first['endpoints']]
                desc['issues'] = first['issues']
                self.publish()
                first = await anext(messages)
        except (OSError, ValueError, asyncio.TimeoutError, StopAsyncIteration) as error:
            desc['endpoints'] = []
            desc['issues'] = [containers.issue(str(error) or 'Container helper disconnected',
                                               child.tail.decode(errors='replace'))]
            self.publish()
        finally:
            self.attachments.pop(key, None)
            await child.stop()

    async def accept(self, stream, request):
        if request.get('kind') != 'endpoint':
            raise ValueError('Unknown host transport operation')
        entry = self.attachments.get(request.get('container', ''))
        if not entry or request.get('attachment') != entry[0]:
            raise ValueError('Container attachment changed; rediscover before reconnecting')
        _, connection, desc = entry
        if request.get('epoch') != desc['containerEpoch'] or request.get('environment') != desc['environment']:
            raise ValueError('Container execution context changed')
        token = request.get('token')
        if not any(ep.get('endpointToken') == token for ep in desc['endpoints']):
            raise ValueError('Endpoint is not registered in this container')
        upstream = await connection.open({'kind': 'endpoint', 'token': token})
        try:
            stream.mux.accepted(stream)
            await mux.relay(stream, upstream)
        finally:
            upstream.close()

    async def run(self, consumer):
        self.consumer = consumer
        tasks, names, seen = [], set(), set()
        self.contexts_changed.set()
        async def contexts():
            while True:
                await self.contexts_changed.wait()
                self.contexts_changed.clear()
                lines = (await containers.run(['docker', 'context', 'ls', '--format', '{{json .}}'])).decode().splitlines()
                for line in lines[:64]:
                    context = json.loads(line)
                    if context.get('DockerEndpoint', '').startswith('unix://'):
                        name = context.get('Name', '')
                        if not containers.text(name, 256) or not name or name.startswith('-') or name in names:
                            continue
                        names.add(name)
                        self.pending.add(name)
                        tasks.append(asyncio.create_task(self.connect_context(name, seen)))
                if not names:
                    self.errors['docker'] = containers.issue('No accessible local Unix Docker engines')
                else:
                    self.errors.pop('docker', None)
                self.publish()
        context_task = asyncio.create_task(contexts())
        output = asyncio.create_task(self.output())
        try:
            done, _ = await asyncio.wait([context_task, output], return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            await cancel(context_task, output, *tasks)


class HostAgent:
    def __init__(self):
        self.inventory = SharedInventory()
        self.inventory_open = False

    async def accept(self, stream, request):
        if request.get('kind') == 'refresh':
            await self.inventory.refresh()
            stream.mux.accepted(stream)
        elif request.get('kind') == 'inventory':
            if self.inventory_open:
                raise ValueError('Inventory already attached')
            self.inventory_open = True
            stream.mux.accepted(stream)
            try:
                await mux.produce(stream, self.inventory.run(stream))
            finally:
                self.inventory_open = False
        else:
            await self.inventory.accept(stream, request)

    async def run(self, reader, writer):
        connection = mux.Mux(reader, writer, self.accept, initiator=False)
        try:
            await connection.start()
            await connection.done.wait()
        finally:
            await connection.stop()


def private_root():
    root = os.path.join(os.path.realpath('/tmp'), f'codex-daemon-{os.getuid()}')
    try:
        os.mkdir(root, 0o700)
    except FileExistsError:
        pass
    info = os.lstat(root)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('Codex protected socket directory must be owned by this user with mode 0700')
    return root


def instance_path(token):
    if not TOKEN.fullmatch(token):
        raise ValueError('Invalid transport instance')
    return os.path.join(private_root(), 'archcodex-' + token)


def cleanup(token):
    # systemd ExecStopPost also runs after SIGKILL. Only remove our exact private
    # instance; never traverse symlinks or remove Codex's sockets/root directory.
    path = instance_path(token)
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('Refusing to clean an unowned transport directory')
    fd = os.open(path, os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        for name in os.listdir(fd):
            info = os.stat(name, dir_fd=fd, follow_symlinks=False)
            if re.fullmatch(r's[a-f0-9]{24}', name) and stat.S_ISSOCK(info.st_mode) and info.st_uid == os.getuid():
                os.unlink(name, dir_fd=fd)
        os.rmdir(path)
    finally:
        os.close(fd)


class LocalEndpoint:
    def __init__(self, path, open_stream):
        self.path, self.open_stream = path, open_stream
        self.server = None
        self.tasks = set()

    async def start(self):
        self.server = await asyncio.start_unix_server(self.accept, path=self.path, limit=mux.MAX_FRAME)
        os.chmod(self.path, 0o600)

    async def accept(self, reader, writer):
        if len(self.tasks) >= mux.MAX_STREAMS:
            writer.close()
            return
        task = asyncio.current_task()
        self.tasks.add(task)
        upstream = None
        try:
            peer = writer.get_extra_info('socket').getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
            if struct.unpack('3i', peer)[1] != os.getuid():
                return
            upstream = await self.open_stream()
            await mux.relay(mux.Socket(reader, writer), upstream)
        except (OSError, ValueError, asyncio.TimeoutError):
            # Never write a diagnostic into a native client's WebSocket stream.
            pass
        finally:
            if upstream:
                upstream.close()
            writer.close()
            self.tasks.discard(task)

    async def stop(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        await cancel(*self.tasks)
        with contextlib.suppress(FileNotFoundError):
            os.unlink(self.path)


class DesktopHost:
    def __init__(self, desktop, ident, host):
        self.desktop, self.ident, self.host = desktop, ident, host
        self.endpoints = {}
        self.delay = 1
        self.active = None
        self.refresh_task = None
        self.task = asyncio.create_task(self.run())

    async def apply(self, report, open_stream):
        present = set()
        for container in report.get('containers', []):
            engine, cid = container.get('engine', ''), container.get('containerId', '')
            if not containers.CID.fullmatch(cid) or not containers.text(engine, 256):
                raise ValueError('Invalid container identity')
            for ep in container.get('endpoints', []):
                token, attachment = ep.get('endpointToken', ''), ep.get('attachment', '')
                if not TOKEN.fullmatch(token) or not TOKEN.fullmatch(attachment):
                    raise ValueError('Invalid registered endpoint')
                key = (engine, cid, container.get('containerEpoch'), attachment, token)
                present.add(key)
                endpoint = self.endpoints.get(key)
                if not endpoint:
                    request = {'kind': 'endpoint', 'container': engine + ':' + cid,
                               'attachment': attachment, 'token': token,
                               'epoch': container.get('containerEpoch'),
                               'environment': container.get('environment')}
                    endpoint = LocalEndpoint(os.path.join(self.desktop.directory, 's' + secrets.token_hex(12)),
                                             lambda request=request: open_stream(request))
                    await endpoint.start()
                    self.endpoints[key] = endpoint
                ep['localSocket'] = endpoint.path
        for key in set(self.endpoints) - present:
            await self.endpoints.pop(key).stop()
        self.desktop.reports[self.ident] = report
        self.desktop.changed.set()
        self.delay = 1

    async def clear(self):
        endpoints, self.endpoints = self.endpoints, {}
        await asyncio.gather(*(e.stop() for e in endpoints.values()))

    async def remote(self):
        # All command arguments are quoted once for the remote shell; no PTY,
        # background master, agent forwarding, or per-terminal SSH process.
        args = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
                '-o', 'ControlMaster=no', '-o', 'ControlPath=none', '-o', 'ControlPersist=no',
                '-o', 'ForwardAgent=no', '-o', 'ForwardX11=no', '-o', 'ClearAllForwardings=yes',
                '-o', 'ForkAfterAuthentication=no', '-o', 'StdinNull=no', '-o', 'SessionType=default',
                '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3',
                '--', self.host, 'exec ' + shlex.join(command('host'))]
        child = ChildConnection(args)
        try:
            connection = await child.start()
            self.active = connection
            inventory = await connection.open({'kind': 'inventory'})
            async for report in mux.messages(inventory):
                await self.apply(report, connection.open)
            raise mux.Closed('Host inventory disconnected')
        except Exception as error:
            detail = child.tail.decode(errors='replace').strip()
            raise mux.Closed(str(error) + ('\n' + detail if detail else '')) from error
        finally:
            self.active = None
            await self.clear()
            await child.stop()

    async def local(self):
        # Reuse the exact host agent in this process without a second local
        # helper process. socketpair is unnamed and never published.
        left, right = socket.socketpair()
        left.setblocking(False)
        right.setblocking(False)
        lr, lw = await asyncio.open_connection(sock=left)
        rr, rw = await asyncio.open_connection(sock=right)
        agent = asyncio.create_task(HostAgent().run(rr, rw))
        connection = mux.Mux(lr, lw)
        try:
            await connection.start()
            self.active = connection
            inventory = await connection.open({'kind': 'inventory'})
            async for report in mux.messages(inventory):
                await self.apply(report, connection.open)
            raise mux.Closed('Local container inventory disconnected')
        finally:
            self.active = None
            await self.clear()
            await connection.stop()
            await cancel(agent)
            rw.close()

    async def run(self):
        while True:
            try:
                await (self.remote() if self.host else self.local())
            except (OSError, ValueError, asyncio.TimeoutError) as error:
                self.desktop.reports[self.ident] = {'containers': [], 'complete': False,
                    'issues': [containers.issue('Container transport disconnected', str(error)[:4096])]}
                self.desktop.changed.set()
            await asyncio.sleep(self.delay)  # Failed-connection retry, never session polling.
            self.delay = min(30, self.delay * 2)

    async def stop(self):
        await cancel(self.refresh_task, self.task)
        await self.clear()

    def request_refresh(self):
        if not self.refresh_task or self.refresh_task.done():
            self.refresh_task = asyncio.create_task(self.refresh())

    async def refresh(self):
        if self.active and not self.active.closed:
            try:
                stream = await self.active.open({'kind': 'refresh'})
                stream.close()
                return
            except (OSError, ValueError, asyncio.TimeoutError):
                pass
        await self.stop()
        self.delay = 1
        self.task = asyncio.create_task(self.run())


class Desktop:
    def __init__(self, token):
        self.directory = instance_path(token)
        os.mkdir(self.directory, 0o700)
        self.token = token
        self.hosts, self.reports = {}, {}
        self.changed = asyncio.Event()

    async def configure(self, message):
        entries = message.get('hosts')
        if not isinstance(entries, list) or len(entries) > 64:
            raise ValueError('Invalid host configuration')
        desired = {}
        for entry in entries:
            ident, host = entry.get('id', ''), entry.get('sshHost', '')
            if not ident or not containers.text(ident, 128) or (host and not HOST.fullmatch(host)) or ident in desired:
                raise ValueError('Invalid SSH host identity')
            desired[ident] = (host, entry.get('revision', 0))
        for ident in list(self.hosts):
            config, host = self.hosts[ident]
            replacement = desired.get(ident)
            if replacement and replacement[0] == config[0]:
                if replacement != config:
                    self.hosts[ident] = (replacement, host)
                    host.request_refresh()
            else:
                del self.hosts[ident]
                await host.stop()
                self.reports.pop(ident, None)
        for ident, config in desired.items():
            if ident not in self.hosts:
                self.hosts[ident] = (config, DesktopHost(self, ident, config[0]))
        self.changed.set()

    async def output(self, writer):
        while True:
            await self.changed.wait()
            self.changed.clear()
            data = mux.encoded({'hosts': self.reports})
            if len(data) > mux.JSON_LIMIT:
                raise ValueError('Combined host inventory exceeded 8 MiB')
            writer.write(data + b'\n')
            await writer.drain()

    async def run(self, reader, writer):
        output = asyncio.create_task(self.output(writer))
        async def inputs():
            while line := await reader.readline():
                if len(line) > 256 * 1024:
                    raise ValueError('Host configuration exceeded its limit')
                await self.configure(json.loads(line))
        input_task = asyncio.create_task(inputs())
        try:
            done, _ = await asyncio.wait([input_task, output], return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            await cancel(input_task, output)
            await asyncio.gather(*(host.stop() for _, host in self.hosts.values()))
            cleanup(self.token)


async def run(role, token=''):
    reader, writer = await stdio()
    if role == 'desktop':
        operation = Desktop(token).run(reader, writer)
    elif role == 'host':
        operation = HostAgent().run(reader, writer)
    elif role == 'container':
        operation = ContainerAgent().run(reader, writer)
    elif role == 'proxy':
        # Flatpak observer boundary only; connects to the shared local endpoint.
        if not token.startswith('/') or not containers.text(token):
            raise ValueError('Missing explicit desktop relay socket')
        remote_reader, remote_writer = await asyncio.wait_for(asyncio.open_unix_connection(token), 10)
        operation = mux.relay(mux.Socket(reader, writer), mux.Socket(remote_reader, remote_writer))
    else:
        raise ValueError('Unknown helper role')
    task = asyncio.create_task(operation)
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        loop.add_signal_handler(sig, task.cancel)
    try:
        await task
    except asyncio.CancelledError:
        pass
    finally:
        writer.close()


def main():
    try:
        if sys.argv[1] == 'cleanup':
            cleanup(sys.argv[2])
        elif sys.argv[1] == 'client':
            request = json.loads(base64.b64decode(sys.argv[2], validate=True))
            path, thread, cwd = (request.get(key, '') for key in ('socket', 'thread', 'cwd'))
            if not path.startswith('/') or not containers.text(path) or \
                    not containers.text(thread, 128) or not containers.text(cwd) or (cwd and not cwd.startswith('/')):
                raise ValueError('Invalid desktop client endpoint or remote working directory')
            local = 'local computer (' + socket.gethostname() + ')'
            executable = request.get('executable', '')
            if executable and (not containers.text(executable) or not os.path.isabs(executable)):
                raise ValueError('The local Codex executable must be an absolute path')
            executable = executable or shutil.which('codex')
            if not executable:
                raise ValueError('Could not locate Codex on this ' + local + '. '
                    'Choose its absolute path in Settings > Application > Local Codex executable. '
                    'The selected remote host is not searched for the TUI.')
            if not os.path.isfile(executable) or not os.access(executable, os.X_OK):
                raise ValueError('Cannot execute ' + executable + ' on this ' + local +
                                 '. Check Settings > Application > Local Codex executable.')
            version = asyncio.run(containers.run([executable, '--version'])).decode().strip()
            match = re.search(r'codex-cli (\d+)\.(\d+)\.(\d+)', version)
            if not match or tuple(map(int, match.groups())) < (0, 166, 0):
                raise ValueError('Codex 0.166.0 or newer is required on this ' + local +
                    ' for the local TUI and image input. Found ' + version[:256] + ' at ' + executable +
                    '. This is not a version check on the remote SSH host.')
            args = [executable, '--remote', 'unix://' + path]
            args += ['resume', '--', thread] if thread else ['--cd', cwd]
            os.execv(executable, args)
        else:
            asyncio.run(run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else ''))
    except (OSError, ValueError, IndexError) as error:
        print('Archcodex transport: ' + str(error)[:4096], file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
