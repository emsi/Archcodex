#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One-shot host updater for the existing GitHub Release Flatpak bundles."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit

APP = "io.github.emsi.Archcodex"
REPOSITORY = "emsi/Archcodex"
API = f"https://api.github.com/repos/{REPOSITORY}/releases"
MAX_BUNDLE = 512 * 1024 * 1024
NETWORK_TIMEOUT = 30


def emit(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def version_key(version):
    match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-rc\.([1-9][0-9]*))?", version)
    if not match:
        raise ValueError("Unrecognized application version: " + str(version)[:100])
    major, minor, patch, candidate = match.groups()
    return int(major), int(minor), int(patch), candidate is None, int(candidate or 0)


def command(*args, timeout=30):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=timeout, env=dict(os.environ, LC_ALL="C"))
    if result.returncode:
        raise ValueError((result.stderr.strip() or result.stdout.strip() or
                          f"{args[0]} exited with code {result.returncode}")[-4000:])
    return result.stdout.strip()


def trusted_url(url):
    parsed = urlsplit(url)
    if (parsed.scheme != "https" or parsed.hostname not in {
            "api.github.com", "github.com", "release-assets.githubusercontent.com",
            "objects.githubusercontent.com"} or parsed.username or parsed.password
            or parsed.port not in (None, 443)):
        raise ValueError("GitHub download redirected to an unexpected address")
    return url


class GitHubRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        trusted_url(newurl)
        return super().redirect_request(request, fp, code, message, headers, newurl)


def response(url):
    request = urllib.request.Request(trusted_url(url), headers={
        "User-Agent": "Archcodex-updater", "Accept": "application/vnd.github+json"})
    try:
        return urllib.request.build_opener(GitHubRedirect()).open(request, timeout=NETWORK_TIMEOUT)
    except urllib.error.HTTPError as error:
        if error.code in (403, 429):
            raise ValueError("GitHub refused the request or its rate limit was reached. Try again later.") from error
        raise ValueError(f"GitHub returned HTTP {error.code}; try checking again later.") from error


def fetch(url, limit):
    with response(url) as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("GitHub response exceeded its size limit")
    return data


def release(url, arch):
    data = json.loads(fetch(url, 2 * 1024 * 1024))
    if not isinstance(data, dict) or not isinstance(data.get("tag_name"), str):
        raise ValueError("GitHub returned invalid release metadata")
    tag = data.get("tag_name", "")
    if not tag.startswith("v") or not version_key(tag[1:])[3] or data.get("draft") or data.get("prerelease"):
        raise ValueError("GitHub did not return a published stable release")
    version = tag[1:]
    name = f"Archcodex-{version}-{arch}.flatpak"
    assets = data.get("assets")
    if not isinstance(assets, list) or any(not isinstance(asset, dict) for asset in assets):
        raise ValueError("GitHub returned invalid release assets")
    matches = [asset for asset in assets if asset.get("name") == name]
    if len(matches) != 1:
        raise ValueError(f"Release {version} has no unique Flatpak bundle for {arch}")
    asset = matches[0]
    size = asset.get("size")
    if (type(size) is not int or not 0 < size <= MAX_BUNDLE or asset.get("state") != "uploaded"
            or type(asset.get("id")) is not int or asset["id"] <= 0):
        raise ValueError("Release bundle has an invalid size or is not fully uploaded")
    download = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
    if asset.get("browser_download_url") != download:
        raise ValueError("Release bundle has an unexpected download URL")
    digest = asset.get("digest") or ""
    if re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        digest = digest[7:]
    else:
        # Older GitHub assets may have no API digest; use the existing release checksum file.
        sums = fetch(f"https://github.com/{REPOSITORY}/releases/download/{tag}/SHA256SUMS", 128 * 1024).decode()
        matches = re.findall(r"^([0-9a-fA-F]{64}) [ *]" + re.escape(name) + r"$", sums, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError("Release has no unique SHA-256 checksum for its Flatpak bundle")
        digest = matches[0].lower()
    return dict(version=version, tag=tag, name=name, size=size, sha256=digest,
                asset_id=asset["id"], url=download,
                notes=str(data.get("body") or "")[:65536],
                page=f"https://github.com/{REPOSITORY}/releases/tag/{tag}")


def installation(context):
    """Match the running deployment to --user; never create a second installation."""
    running = context.get("app_path", "")
    if not running:
        raise ValueError("Automatic installation is available in the per-user Flatpak. This is a native build.")
    path = Path(running)
    if (not path.is_absolute() or len(path.parts) < 7 or path.parts[-6:-4] != ("app", APP)
            or path.name != "files" or path.parts[-3] != "stable"
            or not re.fullmatch(r"[0-9a-f]{64}", path.parts[-2])):
        raise ValueError("Cannot identify the running stable Flatpak deployment")
    arch = path.parts[-4]
    if not re.fullmatch(r"[a-zA-Z0-9_]+", arch):
        raise ValueError("Invalid Flatpak architecture")
    ref = f"app/{APP}/{arch}/stable"
    try:
        location = Path(command("/usr/bin/flatpak", "info", "--user", "--show-location", ref))
    except ValueError as error:
        raise ValueError("This is not an installed per-user Flatpak. Update this system/named installation "
                         "with your host's Flatpak installer; Archcodex will not create a second copy.") from error
    if location.parent.resolve() != path.parent.parent.resolve():
        raise ValueError("Archcodex is running from a different installation. Automatic updates support "
                         "the per-user installation only; update this copy with your host's Flatpak installer.")
    commit = command("/usr/bin/flatpak", "info", "--user", "--show-commit", ref)
    listing = command("/usr/bin/flatpak", "list", "--user", "--app",
                      "--columns=application,arch,branch,version")
    installed = None
    for line in listing.splitlines():
        fields = line.split("\t")
        if len(fields) == 4 and fields[:3] == [APP, arch, "stable"]:
            installed = fields[3]
    if not installed and commit == path.parts[-2]:
        installed = context["version"]
    if not installed:
        raise ValueError("The installed deployment changed. Restart Archcodex before updating.")
    version_key(installed)
    return dict(ref=ref, arch=arch, commit=commit, version=installed)


def check(context):
    current = version_key(context["version"])
    supported, installed = "", None
    try:
        installed = installation(context)
        current = max(current, version_key(installed["version"]))
    except ValueError as error:
        supported = str(error)
    arch = installed["arch"] if installed else command("/usr/bin/flatpak", "--default-arch")
    available = release(API + "/latest", arch)
    result = dict(release=available, install_reason=supported)
    if version_key(available["version"]) > current:
        emit("available", **result)
    elif installed and version_key(installed["version"]) > version_key(context["version"]):
        emit("installed", version=installed["version"])
    else:
        emit("current", **result)


def download(selected, path):
    digest = hashlib.sha256()
    size, last = 0, -1
    deadline = time.monotonic() + 600
    with response(selected["url"]) as stream, path.open("xb") as output:
        while True:
            data = stream.read(256 * 1024)
            if not data:
                break
            size += len(data)
            if size > selected["size"] or time.monotonic() > deadline:
                raise ValueError("Download exceeded its size or time limit")
            output.write(data)
            digest.update(data)
            percent = size * 100 // selected["size"]
            if percent != last:
                emit("downloading", percent=percent)
                last = percent
    if size != selected["size"] or digest.hexdigest() != selected["sha256"]:
        raise ValueError("Downloaded Flatpak failed size/SHA-256 verification. Nothing was installed.")


def install(context, selected):
    before = installation(context)
    if version_key(selected["version"]) <= max(version_key(before["version"]), version_key(context["version"])):
        raise ValueError("This release is no longer newer than the installed/running application")
    checked = release(API + "/tags/" + selected["tag"], before["arch"])
    for key in ("version", "tag", "name", "size", "sha256", "asset_id", "url"):
        if checked[key] != selected.get(key):
            raise ValueError("The release changed after confirmation. Check for updates again.")
    cache = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "archcodex-updates"
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    if cache.is_symlink() or cache.stat().st_uid != os.getuid() or cache.stat().st_mode & 0o077:
        raise ValueError("Update cache must be a private directory owned by your user")
    fd = os.open(cache / "lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("Another Archcodex update is already in progress") from error
        # Only our private download directories, after holding the updater lock.
        import shutil
        for stale in cache.glob("download-*"):
            if stale.is_dir() and not stale.is_symlink():
                shutil.rmtree(stale)
        with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
            path = Path(temporary) / selected["name"]
            download(selected, path)
            if installation(context) != before:
                raise ValueError("The installed application changed during download. Check again before installing.")
            # The GUI enters its non-cancellable phase before authorizing the
            # transaction. A cancellation racing download completion cannot install.
            emit("ready_to_install")
            if (not select.select([sys.stdin], [], [], 30)[0]
                    or sys.stdin.readline(32) != "install\n"):
                raise ValueError("Installation was not authorized; nothing was installed")
            command("/usr/bin/flatpak", "install", "--user", "--or-update", "--noninteractive",
                    "--bundle", str(path), timeout=900)
            after = installation(context)
            if after["version"] != selected["version"]:
                raise ValueError("Flatpak finished but the expected version is not installed. Check the host installation.")
            emit("installed", version=after["version"])
    finally:
        os.close(fd)


# A bounded handoff, created only on explicit Restart. It has no network access
# and exits if the old process does not end. pidfd avoids PID reuse and polling.
RESTART = r'''
import os,select,sys
pidfd, ready, arch, data = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
os.write(ready,b"ready")
os.close(ready)
if not select.select([pidfd],[],[],30)[0]:
    sys.exit(1)
os.close(pidfd)
os.execv("/usr/bin/flatpak", ["flatpak","run","--user","--arch="+arch,
    "--branch=stable","io.github.emsi.Archcodex","--data-dir",data])
'''


def restart(context):
    installed = installation(context)
    instance = context.get("instance_id", "")
    if not re.fullmatch(r"[0-9]+", instance):
        raise ValueError("Cannot identify this Flatpak instance for restart; quit and reopen Archcodex.")
    entries = command("/usr/bin/flatpak", "ps", "--columns=instance,child-pid").splitlines()
    pids = [int(fields[1]) for line in entries if len(fields := line.split()) == 2
            and fields[0] == instance and fields[1].isdigit()]
    if len(pids) != 1:
        raise ValueError("Cannot locate this running Flatpak instance; quit and reopen Archcodex.")
    data_dir = context.get("data_dir", "")
    if not Path(data_dir).is_absolute() or "\x00" in data_dir:
        raise ValueError("Invalid workspace directory for restart")
    pidfd = os.pidfd_open(pids[0])
    read_fd, write_fd = os.pipe()
    try:
        child = subprocess.Popen(["/usr/bin/python3", "-I", "-c", RESTART,
                                  str(pidfd), str(write_fd), installed["arch"], data_dir],
                                 pass_fds=(pidfd, write_fd), start_new_session=True,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        os.close(write_fd)
        write_fd = -1
        if not select.select([read_fd], [], [], 5)[0] or os.read(read_fd, 5) != b"ready":
            child.kill()
            child.wait()
            raise ValueError("Restart handoff failed; Archcodex remains open.")
        emit("restart_ready")
    finally:
        for fd in (pidfd, read_fd, write_fd):
            if fd >= 0:
                os.close(fd)


def main():
    os.umask(0o077)
    def interrupted(_signal, _frame):
        raise InterruptedError("Update operation cancelled")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        raw = sys.stdin.readline(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError("Update request is too large")
        request = json.loads(raw)
        context = request["context"]
        version_key(context["version"])
        if request["operation"] == "check":
            check(context)
        elif request["operation"] == "install":
            install(context, request["release"])
        elif request["operation"] == "restart":
            restart(context)
        else:
            raise ValueError("Unknown update operation")
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError, urllib.error.URLError) as error:
        emit("error", message=str(error)[:4000])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
