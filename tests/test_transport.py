# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
import mux
import transport


class Streams(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.pairs = []

    async def asyncTearDown(self):
        for client, server in self.pairs:
            await client.stop()
            await server.stop()

    async def pair(self, handler=None):
        left, right = socket.socketpair()
        left.setblocking(False)
        right.setblocking(False)
        lr, lw = await asyncio.open_connection(sock=left)
        rr, rw = await asyncio.open_connection(sock=right)
        async def echo(stream, request):
            stream.mux.accepted(stream)
            while block := await stream.read():
                await stream.write(block)
        client = mux.Mux(lr, lw)
        server = mux.Mux(rr, rw, handler or echo, initiator=False)
        self.pairs.append((client, server))
        await asyncio.gather(client.start(), server.start())
        return client, server

    async def exchange(self, client, payload):
        stream = await client.open({'kind': 'test'})
        send = asyncio.create_task(stream.write(payload))
        result = bytearray()
        while len(result) < len(payload):
            block = await stream.read()
            self.assertTrue(block)
            result.extend(block)
        await send
        stream.close()
        return bytes(result)

    async def test_forty_clients_share_one_transport_and_remain_independent(self):
        client, server = await self.pair()
        payloads = [bytes([n]) * (mux.WINDOW * 2 + n) for n in range(40)]
        results = await asyncio.wait_for(asyncio.gather(*(self.exchange(client, p) for p in payloads)), 15)
        self.assertEqual(results, payloads)
        self.assertFalse(client.closed)
        self.assertFalse(server.closed)
        self.assertLessEqual(client.queued_data, mux.MAX_PENDING)

    async def test_stalled_consumer_does_not_block_other_streams(self):
        client, server = await self.pair()
        stalled = await client.open({})
        write = asyncio.create_task(stalled.write(b'x' * (mux.WINDOW * 8)))
        try:
            self.assertEqual(await asyncio.wait_for(self.exchange(client, b'healthy'), 2), b'healthy')
            await asyncio.sleep(0.05)
            self.assertLessEqual(len(stalled.buffer), mux.WINDOW)
            self.assertFalse(write.done())
        finally:
            stalled.close()
            write.cancel()
            await asyncio.gather(write, return_exceptions=True)

    async def test_eof_wakes_open_streams_and_cancels_handlers(self):
        client, server = await self.pair()
        stream = await client.open({})
        waiting = asyncio.create_task(stream.read())
        server.abort('fixture disconnected')
        with self.assertRaises(mux.Closed):
            await asyncio.wait_for(waiting, 2)
        await server.stop()
        self.assertFalse(server.handlers)

    async def test_endpoint_rejection_does_not_destroy_other_clients(self):
        async def accept(stream, request):
            if request.get('bad'):
                raise ValueError('wrong container generation')
            stream.mux.accepted(stream)
            while block := await stream.read():
                await stream.write(block)
        client, _ = await self.pair(accept)
        with self.assertRaisesRegex(mux.Closed, 'wrong container'):
            await client.open({'bad': True})
        self.assertEqual(await self.exchange(client, b'ok'), b'ok')

    async def test_unsubscribing_cancels_an_idle_inventory_producer(self):
        stopped = asyncio.Event()
        async def accept(stream, request):
            stream.mux.accepted(stream)
            async def produce():
                try:
                    await asyncio.Event().wait()
                finally:
                    stopped.set()
            await mux.produce(stream, produce())
        client, _ = await self.pair(accept)
        stream = await client.open({})
        stream.close()
        await asyncio.wait_for(stopped.wait(), 2)
        self.assertFalse(client.closed)

    async def test_reused_stream_id_is_rejected(self):
        client, server = await self.pair()
        stream = await client.open({})
        stream.close()
        client.control(stream.ident, mux.OPEN, b'{}')
        await asyncio.wait_for(server.done.wait(), 2)
        self.assertIn('reused', server.error)

    async def test_cleanup_cannot_hang_on_full_child_stdout(self):
        process = await transport.containers.spawn(sys.executable, '-c',
            'import os,signal;signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            'while True: os.write(1,b"x"*65536)',
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE, limit=1024)
        try:
            await asyncio.sleep(0.05)
            await asyncio.wait_for(transport.containers.stop(process), 3)
            self.assertIsNotNone(process.returncode)
        finally:
            if process.returncode is None:
                process.kill()
                process._transport.close()
                await process.wait()

    async def test_oversized_frame_is_rejected_before_reading_payload(self):
        client, server = await self.pair()
        client.writer.write(mux.HEADER.pack(1, mux.DATA, mux.MAX_FRAME + 1))
        await client.writer.drain()
        await asyncio.wait_for(server.done.wait(), 2)
        self.assertIn('Invalid transport frame', server.error)

    async def test_credit_cannot_exceed_window(self):
        client, server = await self.pair()
        stream = await client.open({})
        server.control(stream.ident, mux.CREDIT, struct.pack('!I', mux.WINDOW))
        await asyncio.wait_for(client.done.wait(), 2)
        self.assertIn('credit', client.error)

    async def test_abrupt_socket_eof_closes_stream_without_waiting_for_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            ended = asyncio.Event()
            async def close_immediately(reader, writer):
                writer.close()
                ended.set()
            listener = await asyncio.start_unix_server(close_immediately, path=directory + '/server')
            async def accept(stream, request):
                reader, writer = await asyncio.open_unix_connection(directory + '/server')
                stream.mux.accepted(stream)
                await mux.relay(stream, mux.Socket(reader, writer))
            try:
                client, _ = await self.pair(accept)
                stream = await client.open({})
                await ended.wait()
                self.assertEqual(await asyncio.wait_for(stream.read(), 2), b'')
            finally:
                listener.close()
                await listener.wait_closed()


class Security(unittest.TestCase):
    def test_cleanup_removes_only_owned_instance_socket_names(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(transport, 'private_root', return_value=directory):
            token = 'a' * 32
            instance = Path(transport.instance_path(token))
            instance.mkdir(mode=0o700)
            listener = socket.socket(socket.AF_UNIX)
            try:
                listener.bind(str(instance / ('s' + 'b' * 24)))
                listener.close()
                transport.cleanup(token)
                self.assertFalse(instance.exists())
                self.assertTrue(Path(directory).is_dir())
                transport.cleanup(token)
                instance.symlink_to(directory, target_is_directory=True)
                with self.assertRaises(ValueError):
                    transport.cleanup(token)
                self.assertTrue(Path(directory).is_dir())
            finally:
                listener.close()

    def test_bootstrap_is_self_contained_and_below_linux_argument_limit(self):
        code = transport.bootstrap()
        self.assertLess(len(code.encode()), 128 * 1024)
        compile(code, '<bootstrap>', 'exec')


class ProcessRoute(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.token = os.urandom(16).hex()
        self.proc = None
        self.servers = []
        self.connections = set()
        self.event_fd = None
        for name in ('docker', 'ssh'):
            path = self.root / name
            path.write_text('#!' + sys.executable + '\n' +
                            (ROOT / 'tests/transport_fixture.py').read_text())
            path.chmod(0o700)
        (self.root / 'home').mkdir()
        state = {'containers': [], 'endpoints': {}}
        for number in range(2):
            cid = str(number + 1) * 64
            path = str(self.root / ('server-' + str(number)))
            async def echo(reader, writer):
                self.connections.add(writer)
                try:
                    while block := await reader.read(16384):
                        writer.write(block)
                        await writer.drain()
                except (ConnectionError, asyncio.CancelledError):
                    pass
                finally:
                    writer.close()
                    self.connections.discard(writer)
            self.servers.append(await asyncio.start_unix_server(echo, path=path))
            state['containers'].append({'Id': cid, 'Name': '/fixture-' + str(number),
                'State': {'Running': True, 'StartedAt': 'epoch-1'},
                'Config': {'Labels': {transport.containers.LABEL: 'none'}, 'WorkingDir': '/workspace',
                    'Env': [f'MYCODEX_HOST_UID={os.getuid()}',
                            'MYCODEX_CONTAINER_HOME=' + str(self.root / 'home'),
                            'CODEX_HOME=' + str(self.root / 'home/.codex')]},
                'Mounts': [{'Type': 'volume', 'Name': 'codex_state', 'Destination': str(self.root / 'home')}]})
            state['endpoints'][cid] = [{'path': path, 'pid': os.getpid(), 'executable': '/fixture/codex'}]
        (self.root / 'state.json').write_text(json.dumps(state))
        os.mkfifo(self.root / 'events')
        self.event_fd = os.open(self.root / 'events', os.O_RDWR | os.O_NONBLOCK)
        self.env = dict(os.environ, ARCHCODEX_TRANSPORT_FIXTURE=str(self.root),
                        PATH=str(self.root) + os.pathsep + os.environ.get('PATH', ''))

    async def start(self, remote=True):
        self.proc = await asyncio.create_subprocess_exec(*transport.command('desktop'), self.token,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            env=self.env, limit=mux.JSON_LIMIT + 1)
        self.proc.stdin.write(mux.encoded({'hosts': [{'id': 'host', 'sshHost': 'fixture-host' if remote else ''}]}) + b'\n')
        await self.proc.stdin.drain()
        return await self.inventory(lambda report: sum(len(c.get('endpoints', [])) for c in report.get('containers', [])) == 2)

    async def inventory(self, predicate):
        async def read():
            while True:
                line = await self.proc.stdout.readline()
                if not line:
                    raise AssertionError('Helper stopped: ' + (await self.proc.stderr.read()).decode())
                message = json.loads(line)
                report = message.get('hosts', {}).get('host', {})
                if predicate(report):
                    return report
        return await asyncio.wait_for(read(), 15)

    def commands(self):
        path = self.root / 'commands'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    async def asyncTearDown(self):
        if self.proc:
            if self.proc.returncode is None:
                self.proc.stdin.close()
                try:
                    await asyncio.wait_for(self.proc.wait(), 6)
                except asyncio.TimeoutError:
                    self.proc.kill()
                    await self.proc.wait()
            errors = (await self.proc.stderr.read()).decode()
            self.assertNotIn('Task exception was never retrieved', errors)
            self.assertNotIn('Task was destroyed', errors)
        # Equivalent to the independently supervised ExecStopPost after a crash.
        transport.cleanup(self.token)
        for writer in list(self.connections):
            writer.close()
        for server in self.servers:
            server.close()
            await server.wait_closed()
        if self.event_fd is not None:
            os.close(self.event_fd)
        self.temp.cleanup()

    async def roundtrip(self, path, payload):
        reader, writer = await asyncio.open_unix_connection(path)
        try:
            writer.write(payload)
            await writer.drain()
            return await asyncio.wait_for(reader.readexactly(len(payload)), 5)
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_remote_forty_clients_use_one_ssh_and_one_exec_per_container(self):
        report = await self.start()
        endpoints = [c['endpoints'][0]['localSocket'] for c in report['containers']]
        payloads = [b'GET / HTTP/1.1\r\nUpgrade: websocket\r\n\r\n' + bytes([n]) * 100000 for n in range(40)]
        results = await asyncio.gather(*(self.roundtrip(endpoints[n % 2], payload) for n, payload in enumerate(payloads)))
        self.assertEqual(results, payloads)
        self.assertEqual(sum(c['role'] == 'ssh' for c in self.commands()), 1)
        self.assertEqual(sum(c['role'] == 'docker:exec' for c in self.commands()), 2)
        # Inspect the actual remote-role processes: none owns a listening socket.
        if Path('/proc/net/unix').exists():
            listeners = {row.split()[6] for row in Path('/proc/net/unix').read_text().splitlines()[1:]
                         if int(row.split()[3], 16) & 0x10000}
            for entry in self.commands():
                if entry['role'] not in ('ssh', 'docker:exec'):
                    continue
                for fd in Path(f"/proc/{entry['pid']}/fd").iterdir():
                    try:
                        target = os.readlink(fd)
                    except FileNotFoundError:
                        continue  # A completed client stream may close concurrently.
                    if target.startswith('socket:['):
                        self.assertNotIn(target[8:-1], listeners)
        for path in endpoints:
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
            self.assertEqual(os.stat(Path(path).parent).st_mode & 0o777, 0o700)
        self.proc.stdin.close()
        await asyncio.wait_for(self.proc.wait(), 6)
        self.assertEqual(self.proc.returncode, 0)
        self.assertFalse(Path(transport.instance_path(self.token)).exists())

    async def test_local_route_uses_no_ssh(self):
        report = await self.start(remote=False)
        path = report['containers'][0]['endpoints'][0]['localSocket']
        self.assertEqual(await self.roundtrip(path, b'local'), b'local')
        self.assertFalse(any(c['role'] == 'ssh' for c in self.commands()))

    async def test_stalled_second_host_cannot_block_existing_host_or_shutdown(self):
        report = await self.start()
        path = report['containers'][0]['endpoints'][0]['localSocket']
        self.proc.stdin.write(mux.encoded({'hosts': [
            {'id': 'host', 'sshHost': 'fixture-host'},
            {'id': 'slow', 'sshHost': 'fixture-stalled'}]}) + b'\n')
        await self.proc.stdin.drain()
        for _ in range(100):
            if sum(c['role'] == 'ssh' for c in self.commands()) == 2:
                break
            await asyncio.sleep(0.01)
        self.assertEqual(sum(c['role'] == 'ssh' for c in self.commands()), 2)
        self.assertEqual(await self.roundtrip(path, b'still-responsive'), b'still-responsive')
        self.proc.stdin.close()
        await asyncio.wait_for(self.proc.wait(), 6)
        self.assertFalse(Path(transport.instance_path(self.token)).exists())

    async def test_container_restart_invalidates_old_socket_and_keeps_other_container(self):
        report = await self.start()
        by_id = {c['containerId']: c for c in report['containers']}
        old_path = by_id['1' * 64]['endpoints'][0]['localSocket']
        other_path = by_id['2' * 64]['endpoints'][0]['localSocket']
        state = json.loads((self.root / 'state.json').read_text())
        state['containers'][0]['State']['StartedAt'] = 'epoch-2'
        (self.root / 'state.json').write_text(json.dumps(state))
        os.write(self.event_fd, b'{"Action":"restart"}\n')
        report = await self.inventory(lambda r: any(c.get('containerEpoch') == 'epoch-2' and c.get('endpoints')
                                                   for c in r.get('containers', [])))
        new_path = next(c['endpoints'][0]['localSocket'] for c in report['containers'] if c['containerEpoch'] == 'epoch-2')
        self.assertNotEqual(new_path, old_path)
        self.assertFalse(Path(old_path).exists())
        self.assertEqual(await self.roundtrip(other_path, b'other'), b'other')
        self.assertEqual(await self.roundtrip(new_path, b'new'), b'new')

    async def test_explicit_refresh_preserves_existing_connections_and_attachments(self):
        report = await self.start()
        path = next(c['endpoints'][0]['localSocket'] for c in report['containers'] if c['containerId'] == '1' * 64)
        reader, writer = await asyncio.open_unix_connection(path)
        try:
            state = json.loads((self.root / 'state.json').read_text())
            state['endpoints']['1' * 64][0]['cwd'] = '/refreshed'
            (self.root / 'state.json').write_text(json.dumps(state))
            self.proc.stdin.write(mux.encoded({'hosts': [{'id': 'host', 'sshHost': 'fixture-host', 'revision': 1}]}) + b'\n')
            await self.proc.stdin.drain()
            report = await self.inventory(lambda r: any(ep.get('cwd') == '/refreshed'
                for c in r.get('containers', []) for ep in c.get('endpoints', [])))
            self.assertIn(path, [ep['localSocket'] for c in report['containers'] for ep in c['endpoints']])
            writer.write(b'preserved')
            await writer.drain()
            self.assertEqual(await asyncio.wait_for(reader.readexactly(9), 2), b'preserved')
            self.assertEqual(sum(c['role'] == 'ssh' for c in self.commands()), 1)
            self.assertEqual(sum(c['role'] == 'docker:exec' for c in self.commands()), 2)
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_killed_frontend_does_not_leave_remote_helpers_running(self):
        await self.start()
        pids = {c['pid'] for c in self.commands() if c['role'] in ('ssh', 'docker:exec', 'docker:events')}
        self.proc.kill()
        await self.proc.wait()
        for _ in range(60):  # Test-only bounded observation, not application polling.
            live = []
            for pid in pids:
                try:
                    state = Path(f'/proc/{pid}/stat').read_text().split(') ')[1].split()[0]
                    if state != 'Z':
                        live.append(pid)
                except FileNotFoundError:
                    pass
            if not live:
                break
            await asyncio.sleep(0.05)
        self.assertFalse(live, f'Owned helper processes survived: {live}')


if __name__ == '__main__':
    unittest.main()
