# SPDX-License-Identifier: GPL-3.0-or-later
"""Local OpenSSH askpass bridge. Answers are transient and never inventoried."""
import asyncio
import contextlib
import ctypes
import json
import os
import re
import resource
import secrets
import socket
import struct

LIMIT = 16384
USER = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9_.@+\-]{0,127}$')


def no_core_dumps():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if ctypes.CDLL(None, use_errno=True).prctl(4, 0, 0, 0, 0) != 0:  # PR_SET_DUMPABLE
        raise OSError('Cannot protect SSH authentication memory from core dumps')


def options(value):
    if not isinstance(value, dict) or set(value) - {'user', 'port', 'identityFile'}:
        raise ValueError('Invalid SSH connection options')
    user, port, identity = value.get('user', ''), value.get('port', 0), value.get('identityFile', '')
    if not isinstance(user, str) or (user and not USER.fullmatch(user)):
        raise ValueError('Invalid SSH user')
    if type(port) is not int or not 0 <= port <= 65535:
        raise ValueError('SSH port must be between 1 and 65535, or inherited')
    if not isinstance(identity, str) or len(identity) > 4096 or any(ord(c) < 32 or ord(c) == 127 for c in identity):
        raise ValueError('Invalid SSH identity file')
    if identity and not (identity.startswith('/') or identity.startswith('~/')):
        raise ValueError('SSH identity file must be an absolute or ~/ path on the local computer')
    return {k: v for k, v in [('user', user), ('port', port), ('identityFile', identity)] if v}


def arguments(value):
    value = options(value)
    result = []
    for key, flag in [('user', '-l'), ('port', '-p'), ('identityFile', '-i')]:
        if key in value:
            result += [flag, str(value[key])]
    return result


# OpenSSH executes a pathname, not an argument vector. This credential-free
# adapter is staged on the host beside this instance's private sockets.
ADAPTER = '''#!/usr/bin/python3 -I
import ctypes,json,os,resource,socket,sys
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
if ctypes.CDLL(None).prctl(4,0,0,0,0):sys.exit(1)
try:
 s=socket.socket(socket.AF_UNIX);s.connect(os.environ['ARCHCODEX_ASKPASS_SOCKET'])
 message={'token':os.environ['ARCHCODEX_ASKPASS_TOKEN'],
          'text':sys.argv[1] if len(sys.argv)>1 else 'SSH authentication',
          'kind':os.environ.get('SSH_ASKPASS_PROMPT','entry')}
 data=json.dumps(message).encode()+b'\\n'
 if len(data)>16384:sys.exit(1)
 s.sendall(data);reply=bytearray()
 while b'\\n' not in reply:
  block=s.recv(4096)
  if not block or len(reply)+len(block)>16384:sys.exit(1)
  reply.extend(block)
 answer=json.loads(reply)
 if 'answer' not in answer:sys.exit(1)
 sys.stdout.write(answer['answer']+'\\n');sys.stdout.flush()
except (OSError,ValueError,KeyError):sys.exit(1)
'''


class Stopped(ConnectionError):
    def __init__(self, state, message):
        self.state = state
        super().__init__(message)


class Attempt:
    def __init__(self, interactive, publish):
        self.token = secrets.token_hex(16)
        self.interactive, self.publish = interactive, publish
        self.pending = {}
        self.changed = asyncio.Event()
        self.stopped = asyncio.get_running_loop().create_future()

    def notify(self):
        self.changed.set()
        self.publish()

    async def wait(self, operation, timeout=120):
        """Bound startup work, excluding time awaiting a human (shown or queued)."""
        task = asyncio.ensure_future(operation)
        loop = asyncio.get_running_loop()
        remaining = timeout
        try:
            while True:
                self.changed.clear()
                waiting = bool(self.pending)
                changed = asyncio.create_task(self.changed.wait())
                start = loop.time()
                try:
                    done, _ = await asyncio.wait((task, self.stopped, changed),
                        timeout=None if waiting else max(0, remaining),
                        return_when=asyncio.FIRST_COMPLETED)
                finally:
                    changed.cancel()
                    await asyncio.gather(changed, return_exceptions=True)
                if self.stopped.done():
                    raise self.stopped.result()
                if task.done():
                    return await task
                if not done:
                    raise asyncio.TimeoutError('SSH startup timed out while no authentication prompt was pending.')
                if not waiting:
                    remaining -= loop.time() - start
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    def snapshot(self):
        return [{'id': ident, 'attempt': self.token, 'text': item[0], 'kind': item[1]}
                for ident, item in self.pending.items()]

    def stop(self, state='cancelled', message='SSH authentication cancelled. Use Connect to try again.'):
        if not self.stopped.done():
            self.stopped.set_result(Stopped(state, message))
        for _, _, future in self.pending.values():
            if not future.done():
                future.set_result(None)

    def answer(self, message):
        if message.get('attempt') != self.token or self.stopped.done():
            return
        ident = message.get('id')
        if not isinstance(ident, str):
            return
        if message.get('cancel') is True:
            # Closing the dialog also cancels between successive challenges.
            # The attempt token prevents a late close affecting its replacement.
            self.stop()
            return
        entry = self.pending.get(ident)
        if not entry or entry[2].done():
            return
        value = message.get('answer')
        if entry[1] == 'none' or not isinstance(value, str) or len(value.encode()) > 1023 or any(c in value for c in '\r\n\0'):
            self.stop('failed', 'Invalid SSH authentication response.')
            return
        entry[2].set_result(value)


class Broker:
    def __init__(self, directory):
        self.path = os.path.join(directory, 'askpass.sock')
        self.adapter = os.path.join(directory, 'askpass')
        self.attempts, self.tasks = {}, set()
        self.server = None

    async def start(self):
        no_core_dumps()
        fd = os.open(self.adapter, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o700)
        with os.fdopen(fd, 'w') as file:
            file.write(ADAPTER)
        self.server = await asyncio.start_unix_server(self.accept, path=self.path, limit=LIMIT)
        os.chmod(self.path, 0o600)

    def begin(self, interactive, publish):
        attempt = Attempt(interactive, publish)
        self.attempts[attempt.token] = attempt
        return attempt

    def end(self, attempt):
        self.attempts.pop(attempt.token, None)
        attempt.stop()

    def environment(self, attempt):
        return dict(os.environ, SSH_ASKPASS=self.adapter, SSH_ASKPASS_REQUIRE='force',
                    ARCHCODEX_ASKPASS_SOCKET=self.path, ARCHCODEX_ASKPASS_TOKEN=attempt.token)

    async def accept(self, reader, writer):
        task = asyncio.current_task()
        self.tasks.add(task)
        attempt = ident = None
        gone = None
        try:
            peer = writer.get_extra_info('socket').getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
            if struct.unpack('3i', peer)[1] != os.getuid() or len(self.tasks) > 64:
                return
            data = json.loads(await asyncio.wait_for(reader.readline(), 5))
            if not isinstance(data, dict) or not isinstance(data.get('token'), str):
                return
            attempt = self.attempts.get(data['token'])
            if not attempt or attempt.stopped.done():
                return
            text, kind = data.get('text'), data.get('kind', 'entry')
            if not isinstance(text, str) or len(text) > 8192 or kind not in ('entry', 'confirm', 'none') or len(attempt.pending) >= 8:
                return
            if not attempt.interactive:
                attempt.stop('authentication-required', 'SSH authentication requires your input. Select Connect.')
                return
            ident = secrets.token_hex(16)
            answer = asyncio.get_running_loop().create_future()
            attempt.pending[ident] = (text, kind, answer)
            attempt.notify()
            gone = asyncio.create_task(reader.read(1))
            done, _ = await asyncio.wait([answer, gone], return_when=asyncio.FIRST_COMPLETED)
            if gone in done:  # OpenSSH withdraws e.g. its security-key touch notice.
                return
            value = answer.result()
            if value is not None and not attempt.stopped.done():
                writer.write(json.dumps({'answer': value}).encode() + b'\n')
                await writer.drain()
        except (OSError, ValueError, asyncio.TimeoutError):
            pass  # No prompt or answer is ever copied into diagnostics.
        finally:
            if gone:
                gone.cancel()
                await asyncio.gather(gone, return_exceptions=True)
            if attempt and ident:
                attempt.pending.pop(ident, None)
                attempt.notify()
            writer.close()
            self.tasks.discard(task)

    async def close(self):
        for attempt in list(self.attempts.values()):
            self.end(attempt)
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        for path in (self.path, self.adapter):
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)
