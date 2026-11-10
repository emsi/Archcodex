#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Docker/SSH executables for process-boundary tests. Never contacts a daemon."""
import json
import os
from pathlib import Path
import shlex
import sys

root = Path(os.environ['ARCHCODEX_TRANSPORT_FIXTURE'])
args = sys.argv[1:]
role = Path(sys.argv[0]).name
if role == 'ssh':
    with (root / 'commands').open('a') as log:
        log.write(json.dumps({'role': 'ssh', 'pid': os.getpid()}) + '\n')
    assert '-T' in args and '-tt' not in args
    assert 'ControlPath=none' in args
    assert 'ServerAliveInterval=15' in args and 'ServerAliveCountMax=3' in args
    assert 'ForwardAgent=no' in args and 'ClearAllForwardings=yes' in args
    if args[-2] == 'fixture-stalled':
        import time
        time.sleep(60)
        sys.exit()
    command = shlex.split(args[-1])
    assert command.pop(0) == 'exec'
    os.execvp(command[0], command)

if args[:2] == ['context', 'ls']:
    print(json.dumps({'Name': 'fixture', 'DockerEndpoint': 'unix:///fixture/docker.sock'}))
    sys.exit()
if args[:2] == ['context', 'inspect']:
    print(json.dumps('unix:///fixture/docker.sock'))
    sys.exit()
assert args[:2] == ['--context', 'fixture'], args[:3]
args = args[2:]
with (root / 'commands').open('a') as log:
    log.write(json.dumps({'role': 'docker:' + args[0], 'pid': os.getpid()}) + '\n')
state = json.loads((root / 'state.json').read_text())
if args[0] == 'info':
    print('fixture-engine')
elif args[0] == 'ps':
    for container in state['containers']:
        print(container['Id'])
elif args[0] == 'inspect':
    print(json.dumps([c for c in state['containers'] if c['Id'] == args[-1]]))
elif args[0] == 'events':
    with (root / 'events').open() as events:
        for line in events:
            print(line, end='', flush=True)
elif args[0] == 'exec':
    assert '-i' in args and '-it' not in args
    for i, value in enumerate(args):
        if value == '--env':
            key, value = args[i + 1].split('=', 1)
            os.environ[key] = value
    cid_index = next(i for i, value in enumerate(args) if value in {c['Id'] for c in state['containers']})
    os.environ['ARCHCODEX_FIXTURE_CONTAINER'] = args[cid_index]
    command = args[cid_index + 1:]
    index = command.index('-c') + 1
    # Only discovery is substituted. The actual packaged container, host and
    # desktop transport implementations communicate over real process pipes.
    fixture = '''
import os,json
def fixture_snapshot(default=None,aliases=()):
 data=json.load(open(os.environ['ARCHCODEX_TRANSPORT_FIXTURE']+'/state.json'))
 return data['endpoints'][os.environ['ARCHCODEX_FIXTURE_CONTAINER']],[]
sys.modules['discover'].snapshot=fixture_snapshot
sys.modules['transport'].main()
'''
    command[index] = command[index].replace("sys.modules['transport'].main()", fixture)
    os.execvp(command[0], command)
else:
    raise RuntimeError(args[:3])
