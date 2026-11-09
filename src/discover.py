# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-side socket discovery. No server lifecycle actions or session-state queries.

Executed by the GUI with the host's isolated Python 3 interpreter. Startup and
explicit refresh take a procfs snapshot; inotify invalidations trigger subsequent
snapshots. The idle loop waits indefinitely for descriptors, with no scan timer.
"""

import ctypes
import errno
import json
import os
import pwd
import socket
from pathlib import Path
import selectors
import stat
import struct
import sys

LIMIT = 4096


def valid_path(value):
    return (isinstance(value, str) and value.startswith("/") and len(value) <= 4096
            and all(ord(c) >= 32 and ord(c) != 127 and not 0xD800 <= ord(c) <= 0xDFFF
                    for c in value))


def issue(code, summary, explanation, action, details=()):
    return {"code": code, "summary": summary, "explanation": explanation,
            "action": action, "details": list(details)[:12],
            "omitted": max(0, len(details) - 12)}


def error_detail(path, operation, error):
    code = errno.errorcode.get(error.errno, str(error.errno))
    return f"{path} — {operation}: {error.strerror or str(error)} ({code})"


def socket_path(path, uid, on_denied=None):
    try:
        resolved = os.path.realpath(path)
        info = os.stat(resolved)
        if valid_path(resolved) and info.st_uid == uid and stat.S_ISSOCK(info.st_mode):
            return resolved
    except PermissionError as error:
        if on_denied:
            on_denied(path, error)
    except OSError:
        pass
    return None


def snapshot(proc=Path("/proc"), uid=None, default=None, aliases=()):
    """Associate named Unix streams with same-user Codex process descriptors.

    A bound socket can precede listen(): the protocol client's finite connection
    retry handles that race. Never treat socket-file existence as live status.
    """
    uid = os.getuid() if uid is None else uid
    listeners, found, issues = {}, {}, []
    try:
        owner = pwd.getpwuid(uid)
        identity = {"user": owner.pw_name, "ownerHome": owner.pw_dir}
    except KeyError:
        identity = {}
    identity["host"] = socket.gethostname()
    probe, operation = proc / "net/unix", "read Unix socket table"
    try:
        for line in (proc / "net/unix").read_text(errors="surrogateescape").splitlines()[1:]:
            fields = line.split(maxsplit=7)
            if len(fields) == 8 and fields[4] == "0001" and fields[5] == "01":
                if valid_path(fields[7]):
                    listeners[fields[6]] = fields[7]
        probe, operation = proc, "list processes"
        processes = list(proc.iterdir())
    except OSError as error:
        return [], [issue("procfs", "Host process information is unavailable",
                          "Automatic discovery could not take a host snapshot. Existing connections are retained.",
                          "Retry discovery. If you know the app-server socket, add it as a connection.",
                          [error_detail(probe, operation, error)])]
    denied = {"ownership": {}, "unidentified": {}, "codex": {}}
    for process in processes:
        if not process.name.isdecimal():
            continue
        category = "ownership"
        probe, operation = process, "check process owner"

        def denied_path(path, error):
            denied[category].setdefault(process.name, error_detail(path, operation, error))

        try:
            if process.stat().st_uid != uid:
                continue
            category = "unidentified"
            probe, operation = process / "exe", "read executable link"
            exe = os.readlink(probe)
            name = os.path.basename(exe).removesuffix(" (deleted)")
            if name != "codex" and not name.startswith("codex-"):
                continue
            category = "codex"
            probe, operation = process / "cmdline", "read command metadata"
            args = probe.read_bytes()[:65536].split(b"\0")
            if b"app-server" in args:
                remaining = args[args.index(b"app-server") + 1:]
                if b"pid-update-loop" in remaining or (remaining and remaining[0] in (
                        b"daemon", b"proxy", b"generate-ts", b"generate-json-schema", b"help")):
                    continue  # Updater/control tooling is not a conversation server.
            # A container-private path with the same spelling is not a host socket.
            probe, operation = process / "ns/mnt", "read mount namespace link"
            namespace = os.readlink(probe)
            probe = proc / "self/ns/mnt"
            host_environment = namespace == os.readlink(probe)
            try:
                cwd = os.readlink(process / "cwd")
            except OSError:
                cwd = ""
            probe, operation = process / "fd", "list socket descriptors"
            descriptors = list(probe.iterdir())
            for fd in descriptors:
                try:
                    operation = "read descriptor link"
                    link = os.readlink(fd)
                except PermissionError as error:
                    denied_path(fd, error)
                    continue
                except OSError:
                    continue
                if not link.startswith("socket:["):
                    continue
                path = listeners.get(link[8:-1])
                if not path:
                    continue
                operation = "inspect socket node"
                canonical = socket_path(path, uid, denied_path)
                if canonical:
                    if not host_environment:
                        try:
                            probe = process / "root" / path.lstrip("/")
                            operation = "verify socket across mount namespaces"
                            there = os.stat(probe)
                            probe = path
                            here = os.stat(probe)
                            if (there.st_dev, there.st_ino) != (here.st_dev, here.st_ino):
                                continue
                        except PermissionError as error:
                            denied_path(probe, error)
                            continue
                        except OSError:
                            continue
                    executable = exe if valid_path(exe) and os.access(exe, os.X_OK) else "codex"
                    found[canonical] = {"path": canonical, "aliases": [path],
                                        "executable": executable, "default": False,
                                        "hostEnvironment": host_environment, **identity,
                                        "pid": int(process.name),
                                        "cwd": cwd if valid_path(cwd) else ""}
                    if len(found) >= LIMIT:
                        issues.append(issue("endpoint-limit", "Discovery reached its socket limit",
                                            "The scan stopped after 4,096 endpoints; remaining processes were not checked.",
                                            "Add a known socket manually if its server is missing."))
                        break
        except PermissionError as error:
            denied_path(probe, error)
        except OSError:
            continue  # Process exit during a snapshot is ordinary.
        if len(found) >= LIMIT:
            break
    for category, failures in denied.items():
        if not failures:
            continue
        count = len(failures)
        noun = "process" if count == 1 else "processes"
        summary, explanation = {
            "ownership": (f"Could not check the owner of {count} {noun}",
                          "Linux denied access to process ownership information. These entries were skipped."),
            "unidentified": (f"{count} {noun} could not be identified",
                             "Linux denied access to executable links for processes owned by your user. "
                             "Archcodex cannot tell whether they are Codex; this does not prove any sessions are missing."),
            "codex": (f"{count} Codex {noun} could not be fully inspected",
                      "Linux denied access while matching known Codex processes to their sockets. "
                      "Those servers may be absent from the explorer; existing connections are retained."),
        }[category]
        issues.append(issue(category + "-access", summary, explanation,
                            "If all expected servers are listed, no action is needed. Otherwise, add the missing "
                            "server's Unix socket in Connections. The details identify the blocked operation; "
                            "retry after resolving access to that process.",
                            [failures[pid] for pid in sorted(failures, key=int)]))
    for alias in [default, *aliases]:
        if not alias or not valid_path(alias):
            continue
        canonical = socket_path(alias, uid)
        if canonical in found:
            found[canonical]["aliases"].append(alias)
            found[canonical]["default"] |= alias == default
    for item in found.values():
        item["aliases"] = sorted(set(item["aliases"]))
    return sorted(found.values(), key=lambda item: item["path"]), issues


class Watcher:
    # Only namespace-changing events; never IN_ACCESS, which our scan itself causes.
    MASK = 0x100 | 0x200 | 0x40 | 0x80 | 0x400 | 0x800

    def __init__(self):
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.inotify_init1.argtypes = [ctypes.c_int]
        self.libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self.fd = self.libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), "Cannot initialize inotify")
        self.directories = {}
        self.watches = {}

    def close(self):
        os.close(self.fd)

    def configure(self, roots, paths):
        """Watch target parents, or the nearest existing ancestor until created."""
        desired = {}
        targets = [(root, "*") for root in roots]
        targets += [(os.path.dirname(path), os.path.basename(path)) for path in paths
                    if valid_path(path)]
        for directory, name in targets:
            while not os.path.isdir(directory):
                parent, name = os.path.split(directory)
                if not parent or parent == directory:
                    break
                directory = parent
            desired.setdefault(directory, set()).add(name)
        errors = []
        for directory in set(self.directories) - set(desired):
            wd = self.directories.pop(directory)
            self.watches.pop(wd, None)
            self.libc.inotify_rm_watch(self.fd, wd)
        for directory, names in desired.items():
            wd = self.directories.get(directory)
            if wd is None:
                wd = self.libc.inotify_add_watch(self.fd, os.fsencode(directory), self.MASK)
                if wd < 0:
                    code = ctypes.get_errno()
                    errors.append(error_detail(directory, "watch socket directory",
                                               OSError(code, os.strerror(code))))
                    continue
                self.directories[directory] = wd
            self.watches[wd] = (directory, names)
        if not errors:
            return []
        return [issue("directory-watches", f"Automatic discovery updates unavailable for {len(errors)} socket directories",
                      "New or removed servers in these locations may not appear automatically. "
                      "Connected sessions still receive their normal server events.",
                      "Use Discover to refresh after starting or stopping a server. Check the directory error below.",
                      sorted(errors))]

    def changed(self):
        changed = False
        while True:
            try:
                data = os.read(self.fd, 65536)
            except BlockingIOError:
                break
            offset = 0
            while offset < len(data):
                wd, mask, _, length = struct.unpack_from("iIII", data, offset)
                name = os.fsdecode(data[offset + 16:offset + 16 + length].rstrip(b"\0"))
                offset += 16 + length
                if mask & 0x4000:  # Queue overflow: one reconciliation snapshot.
                    changed = True
                watch = self.watches.get(wd)
                if watch:
                    directory, names = watch
                    changed |= not name or name in names or "*" in names
                    if mask & 0x8000:  # IN_IGNORED: the watched directory was removed.
                        self.watches.pop(wd, None)
                        self.directories.pop(directory, None)
                        changed = True
        return changed


def main():
    uid = os.getuid()
    home = os.environ.get("CODEX_HOME") or os.path.join(os.path.expanduser("~"), ".codex")
    default = os.path.join(home, "app-server-control", "app-server-control.sock")
    roots = [f"/tmp/codex-daemon-{uid}"]
    paths, known, buffer, last = [default], [], b"", None
    watcher = Watcher()
    selector = selectors.DefaultSelector()
    selector.register(watcher.fd, selectors.EVENT_READ)
    selector.register(sys.stdin.fileno(), selectors.EVENT_READ)
    try:
        # The GUI's initial configuration must arrive before the first snapshot:
        # otherwise an existing custom alias can be duplicated as an auto-profile.
        dirty = False
        while True:
            if dirty:
                watcher.configure(roots, paths + known)
                endpoints, issues = snapshot(default=default, aliases=paths)
                known = sorted(set(known + [item["path"] for item in endpoints]))[-LIMIT:]
                issues += watcher.configure(roots, paths + known)
                message = {"endpoints": endpoints, "issues": issues}
                if message != last:
                    print(json.dumps(message, ensure_ascii=True, separators=(",", ":")), flush=True)
                    last = message
                dirty = False
            for key, _ in selector.select():  # No timeout, background rescan or heartbeat.
                if key.fd == watcher.fd:
                    dirty |= watcher.changed()
                else:
                    block = os.read(sys.stdin.fileno(), 65536)
                    if not block:
                        return
                    buffer += block
                    if len(buffer) > 1024 * 1024:
                        raise ValueError("Discovery request exceeds limit")
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        request = json.loads(line)
                        if not isinstance(request, dict):
                            raise ValueError("Invalid discovery request")
                        requested = request.get("paths", [])
                        if not isinstance(requested, list) or len(requested) > LIMIT:
                            raise ValueError("Invalid discovery request")
                        paths = [default] + [p for p in requested if valid_path(p)]
                        last, dirty = None, True
    finally:
        selector.close()
        watcher.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error)[:512]}), flush=True)
        sys.exit(1)
