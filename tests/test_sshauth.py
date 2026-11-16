#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Broker tests; optional real OpenSSH tests: uv run --with paramiko this-file."""
import asyncio
import contextlib
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import sshauth

try:
    import paramiko
except ImportError:
    paramiko = None


class Options(unittest.TestCase):
    def test_structured_arguments_and_validation(self):
        self.assertEqual(sshauth.arguments({'user': 'alice', 'port': 2222,
            'identityFile': '~/key with spaces'}), ['-l', 'alice', '-p', '2222', '-i', '~/key with spaces'])
        for value in ({'password': 'rejected'}, {'port': True}, {'port': 65536},
                      {'identityFile': 'relative'}, {'user': '-option'}, {'identityFile': '/bad\npath'}):
            with self.assertRaises(ValueError):
                sshauth.options(value)


class Attempts(unittest.IsolatedAsyncioTestCase):
    async def test_stale_reply_and_cancel_between_challenges(self):
        attempt = sshauth.Attempt(True, lambda: None)
        answer = asyncio.get_running_loop().create_future()
        attempt.pending['prompt'] = ('Password:', 'entry', answer)
        attempt.answer({'attempt': 'previous', 'id': 'prompt', 'answer': 'ignored'})
        self.assertFalse(answer.done())
        attempt.answer({'attempt': attempt.token, 'id': 'prompt', 'answer': 'fixture-secret'})
        self.assertEqual(answer.result(), 'fixture-secret')
        self.assertNotIn('fixture-secret', str(attempt.snapshot()))
        attempt.pending.clear()
        attempt.answer({'attempt': attempt.token, 'id': 'prompt', 'cancel': True})
        self.assertEqual(attempt.stopped.result().state, 'cancelled')


@unittest.skipUnless(paramiko, 'Optional Paramiko fixture not installed; run with uv --with paramiko')
class OpenSsh(unittest.IsolatedAsyncioTestCase):
    async def authenticate(self, method, *, known=True, background=False):
        # Only this ephemeral loopback server is involved; no user SSH config,
        # agent, accounts, keys or known_hosts files are touched.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host_key = paramiko.RSAKey.generate(2048)
            user_key = paramiko.RSAKey.generate(2048)
            user_key.write_private_key_file(str(root / 'key'), password='fixture-passphrase')
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]
            errors = []
            executed = threading.Event()

            class Server(paramiko.ServerInterface):
                def get_allowed_auths(self, username):
                    return method
                def check_auth_password(self, username, password):
                    return paramiko.AUTH_SUCCESSFUL if username == 'fixture' and password == 'fixture-secret' else paramiko.AUTH_FAILED
                def check_auth_publickey(self, username, key):
                    return paramiko.AUTH_SUCCESSFUL if username == 'fixture' and key == user_key else paramiko.AUTH_FAILED
                def check_auth_interactive(self, username, submethods):
                    return paramiko.InteractiveQuery('Fixture', 'Two challenges', ('Password:', False), ('Code:', False))
                def check_auth_interactive_response(self, responses):
                    return paramiko.AUTH_SUCCESSFUL if responses == ['fixture-secret', 'fixture-code'] else paramiko.AUTH_FAILED
                def check_channel_request(self, kind, chanid):
                    return paramiko.OPEN_SUCCEEDED if kind == 'session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
                def check_channel_exec_request(self, channel, command):
                    executed.set()
                    return command == b'fixture'

            def serve():
                try:
                    with listener.accept()[0] as sock, paramiko.Transport(sock) as remote:
                        remote.add_server_key(host_key)
                        remote.start_server(server=Server())
                        channel = remote.accept(10)
                        if channel:
                            with channel:
                                if not executed.wait(5):
                                    raise TimeoutError('SSH client did not request fixture command')
                                channel.sendall(b'connected\n')
                                channel.send_exit_status(0)
                                channel.shutdown_write()
                                channel.settimeout(3)
                                channel.recv(1)
                except (EOFError, ConnectionError):
                    pass
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            known_hosts = root / 'known_hosts'
            known_hosts.write_text(f'[127.0.0.1]:{port} {host_key.get_name()} {host_key.get_base64()}\n' if known else '')
            broker = sshauth.Broker(directory)
            await broker.start()
            changed = asyncio.Event()
            attempt = broker.begin(not background, changed.set)
            command = ['ssh', '-F', '/dev/null', '-T', '-p', str(port), '-l', 'fixture',
                '-o', f'UserKnownHostsFile={known_hosts}', '-o', 'GlobalKnownHostsFile=/dev/null',
                '-o', 'StrictHostKeyChecking=ask', '-o', 'IdentityAgent=none', '-o', 'AddKeysToAgent=no',
                '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=no', '-o', 'ConnectTimeout=5',
                '-o', 'PreferredAuthentications=' + method, '-i', str(root / 'key'), '127.0.0.1', 'fixture']
            proc = await asyncio.create_subprocess_exec(*command, env=broker.environment(attempt),
                stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            communication = asyncio.create_task(proc.communicate())
            prompts = []
            async def answer_prompts():
                while not communication.done():
                    await changed.wait()
                    changed.clear()
                    for prompt in attempt.snapshot():
                        if prompt['id'] in prompts:
                            continue
                        prompts.append(prompt['id'])
                        text = prompt['text'].lower()
                        value = ('yes' if 'continue connecting' in text else 'fixture-passphrase' if 'passphrase' in text
                                 else 'fixture-code' if 'code:' in text else 'fixture-secret')
                        attempt.answer(dict(prompt, answer=value))
            answers = asyncio.create_task(answer_prompts())
            try:
                if background:
                    stopped = await asyncio.wait_for(attempt.stopped, 10)
                    self.assertEqual(stopped.state, 'authentication-required')
                    proc.terminate()
                stdout, stderr = await asyncio.wait_for(communication, 15)
                self.assertNotIn(b'fixture-secret', stderr)
                self.assertNotIn(b'fixture-passphrase', stderr)
                if not background:
                    self.assertEqual(proc.returncode, 0, stderr.decode())
                    self.assertEqual(stdout, b'connected\n')
                    self.assertGreaterEqual(len(prompts), 1)
                    if method == 'keyboard-interactive' or not known:
                        self.assertGreaterEqual(len(prompts), 2)
                self.assertFalse(errors, errors)
            finally:
                answers.cancel()
                await asyncio.gather(answers, return_exceptions=True)
                if proc.returncode is None:
                    proc.kill()
                await proc.wait()
                await broker.close()
                listener.close()
                await asyncio.to_thread(thread.join, 3)
                self.assertFalse(thread.is_alive())
                self.assertFalse((root / 'askpass.sock').exists())
                self.assertFalse((root / 'askpass').exists())

    async def test_password(self):
        await self.authenticate('password')

    async def test_encrypted_private_key(self):
        await self.authenticate('publickey')

    async def test_keyboard_interactive(self):
        await self.authenticate('keyboard-interactive')

    async def test_unknown_host_requires_confirmation(self):
        await self.authenticate('password', known=False)

    async def test_background_stops_without_answer(self):
        await self.authenticate('password', background=True)


if __name__ == '__main__':
    unittest.main()
