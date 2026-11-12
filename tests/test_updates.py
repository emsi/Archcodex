#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Updater boundaries, without touching the user's installation or the network."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("updates", Path(__file__).parents[1] / "src/updates.py")
updates = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updates)
APP = updates.APP
OLD = "a" * 64
NEW = "b" * 64
PAYLOAD = b"controlled bundle fixture"
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


def metadata(version="0.10.0"):
    name = f"Archcodex-{version}-x86_64.flatpak"
    return dict(tag_name="v" + version, draft=False, prerelease=False, body="Release notes",
                assets=[dict(name=name, id=123, size=len(PAYLOAD), state="uploaded",
                             digest="sha256:" + DIGEST,
                             browser_download_url=f"https://github.com/emsi/Archcodex/releases/download/v{version}/{name}")])


def candidate():
    with patch.object(updates, "fetch", return_value=json.dumps(metadata()).encode()):
        return updates.release(updates.API + "/latest", "x86_64")


def context(root="/home/test/.local/share/flatpak"):
    return dict(version="0.9.0", app_path=f"{root}/app/{APP}/x86_64/stable/{OLD}/files")


def installed(version="0.9.0", commit=OLD):
    return dict(ref=f"app/{APP}/x86_64/stable", arch="x86_64", commit=commit, version=version)


class UpdateTests(unittest.TestCase):
    def test_version_order_and_invalid_versions(self):
        versions = ["0.9.0", "0.10.0-rc.1", "0.10.0-rc.12", "0.10.0", "0.10.1"]
        self.assertEqual(sorted(reversed(versions), key=updates.version_key), versions)
        for value in ["v0.9.0", "01.0.0", "0.9", "0.9.0-rc.0", "0.9.0;exit", "0.9.0-beta.1"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                updates.version_key(value)

    def test_release_requires_stable_matching_asset(self):
        for mutate in [lambda d: d.update(draft=True), lambda d: d.update(prerelease=True),
                       lambda d: d.update(tag_name="v0.10.0-rc.1"),
                       lambda d: d.update(assets=[]),
                       lambda d: d["assets"].append(d["assets"][0]),
                       lambda d: d["assets"][0].update(size=updates.MAX_BUNDLE + 1),
                       lambda d: d["assets"][0].update(state="new"),
                       lambda d: d["assets"][0].update(browser_download_url="https://example.com/a")]:
            data = metadata()
            mutate(data)
            with patch.object(updates, "fetch", return_value=json.dumps(data).encode()), self.assertRaises(ValueError):
                updates.release(updates.API + "/latest", "x86_64")
        self.assertEqual(candidate()["sha256"], DIGEST)

    def test_checksum_fallback_is_exact_and_unique(self):
        data = metadata()
        data["assets"][0].pop("digest")
        name = data["assets"][0]["name"]
        for sums, valid in [(f"{DIGEST}  {name}\n", True),
                            (f"{DIGEST}  other.flatpak\n", False),
                            (f"{DIGEST}  {name}\n" * 2, False)]:
            with patch.object(updates, "fetch", side_effect=[json.dumps(data).encode(), sums.encode()]):
                if valid:
                    self.assertEqual(updates.release(updates.API + "/latest", "x86_64")["sha256"], DIGEST)
                else:
                    with self.assertRaises(ValueError):
                        updates.release(updates.API + "/latest", "x86_64")

    def test_malformed_release_metadata_is_rejected(self):
        for value in [[], None, dict(tag_name="v0.10.0", assets=None),
                      dict(tag_name="v0.10.0", assets=[None])]:
            with patch.object(updates, "fetch", return_value=json.dumps(value).encode()), self.assertRaises(ValueError):
                updates.release(updates.API + "/latest", "x86_64")

    def test_only_github_https_redirects(self):
        for url in ["http://github.com/a", "https://github.com.evil/a", "file:///tmp/a",
                    "https://a@github.com/a", "https://github.com:8080/a"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                updates.trusted_url(url)
        self.assertEqual(updates.trusted_url("https://release-assets.githubusercontent.com/a?signature=abc"),
                         "https://release-assets.githubusercontent.com/a?signature=abc")

    def test_running_installation_must_match_user_location(self):
        location = context()["app_path"].removesuffix("/files")
        listing = f"{APP}\tx86_64\tstable\t0.9.0"
        with patch.object(updates, "command", side_effect=[location, OLD, listing]):
            self.assertEqual(updates.installation(context()), installed())
        with patch.object(updates, "command", return_value=location) as command, self.assertRaisesRegex(ValueError, "different installation"):
            updates.installation(context("/var/lib/flatpak"))
        command.assert_called_once()
        with patch.object(updates, "command", side_effect=ValueError("not installed")), self.assertRaisesRegex(ValueError, "second copy"):
            updates.installation(context())
        with self.assertRaisesRegex(ValueError, "native build"):
            updates.installation(dict(version="0.9.0"))

    def test_installed_metadata_fallback_requires_same_commit(self):
        location = context()["app_path"].removesuffix("/files")
        with patch.object(updates, "command", side_effect=[location, OLD, ""]):
            self.assertEqual(updates.installation(context())["version"], "0.9.0")
        with patch.object(updates, "command", side_effect=[location, NEW, ""]), self.assertRaisesRegex(ValueError, "deployment changed"):
            updates.installation(context())

    def test_newer_rc_is_not_downgraded_to_latest_public(self):
        for current, deployed, event in [("0.10.0-rc.1", "0.10.0-rc.1", "available"),
                                         ("0.11.0-rc.1", "0.11.0-rc.1", "current"),
                                         ("0.9.0", "0.10.0", "installed")]:
            with (patch.object(updates, "installation", return_value=installed(deployed)),
                  patch.object(updates, "release", return_value=candidate()),
                  patch.object(updates, "emit") as emit):
                updates.check(dict(version=current))
                self.assertEqual(emit.call_args.args[0], event)

    def test_unsupported_installation_can_check_but_not_install(self):
        with (patch.object(updates, "installation", side_effect=ValueError("system installation")),
              patch.object(updates, "command", return_value="x86_64"),
              patch.object(updates, "release", return_value=candidate()),
              patch.object(updates, "emit") as emit):
            updates.check(dict(version="0.9.0"))
            self.assertEqual(emit.call_args.kwargs["install_reason"], "system installation")

    def test_download_verifies_bytes_and_limits(self):
        selected = candidate()
        with tempfile.TemporaryDirectory() as tmp:
            for i, payload in enumerate([PAYLOAD, PAYLOAD[:-1], PAYLOAD + b"extra", b"x" * len(PAYLOAD)]):
                with (patch.object(updates, "response", return_value=io.BytesIO(payload)),
                      patch.object(updates, "emit")):
                    path = Path(tmp) / str(i)
                    if payload == PAYLOAD:
                        updates.download(selected, path)
                        self.assertEqual(path.read_bytes(), PAYLOAD)
                    else:
                        with self.assertRaises(ValueError):
                            updates.download(selected, path)

    def install_fixture(self, acknowledgement="install\n", change=None):
        selected = candidate()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        temporary = stack.enter_context(tempfile.TemporaryDirectory())
        cache = Path(temporary)
        stack.enter_context(patch.dict(os.environ, XDG_CACHE_HOME=str(cache)))
        states = [installed(), change or installed(), installed("0.10.0", NEW)]
        stack.enter_context(patch.object(updates, "installation", side_effect=states))
        stack.enter_context(patch.object(updates, "release", return_value=selected))
        stack.enter_context(patch.object(updates, "response", return_value=io.BytesIO(PAYLOAD)))
        stack.enter_context(patch.object(updates.sys, "stdin", io.StringIO(acknowledgement)))
        stack.enter_context(patch.object(updates.select, "select", return_value=([1], [], [])))
        emit = stack.enter_context(patch.object(updates, "emit"))
        command = stack.enter_context(patch.object(updates, "command", return_value=""))
        return selected, cache, emit, command

    def test_install_pins_asset_and_uses_only_existing_user_install(self):
        selected, cache, emit, command = self.install_fixture()
        updates.install(context(), selected)
        self.assertEqual(command.call_args.args[:6], ("/usr/bin/flatpak", "install", "--user", "--or-update", "--noninteractive", "--bundle"))
        self.assertEqual(emit.call_args.kwargs["version"], "0.10.0")
        self.assertEqual(list((cache / "archcodex-updates").glob("download-*")), [])

    def test_install_rejects_changed_asset(self):
        selected, cache, emit, command = self.install_fixture()
        with self.assertRaisesRegex(ValueError, "release changed"):
            updates.install(context(), dict(selected, asset_id=456))
        command.assert_not_called()

    def test_install_needs_final_acknowledgement(self):
        selected, cache, emit, command = self.install_fixture(acknowledgement="")
        with self.assertRaisesRegex(ValueError, "not authorized"):
            updates.install(context(), selected)
        command.assert_not_called()
        self.assertEqual(list((cache / "archcodex-updates").glob("download-*")), [])

    def test_install_rejects_concurrent_deployment_change(self):
        selected, cache, emit, command = self.install_fixture(change=installed("0.10.0", NEW))
        with self.assertRaisesRegex(ValueError, "changed during download"):
            updates.install(context(), selected)
        command.assert_not_called()

    def test_install_refuses_insecure_cache(self):
        selected, cache, emit, command = self.install_fixture()
        (cache / "archcodex-updates").mkdir(mode=0o755)
        with self.assertRaisesRegex(ValueError, "private directory"):
            updates.install(context(), selected)
        command.assert_not_called()

    def test_install_failure_cleans_download(self):
        selected, cache, emit, command = self.install_fixture()
        command.side_effect = ValueError("disk full")
        with self.assertRaisesRegex(ValueError, "disk full"):
            updates.install(context(), selected)
        self.assertEqual(list((cache / "archcodex-updates").glob("download-*")), [])
        self.assertNotIn("installed", [call.args[0] for call in emit.call_args_list])

    def test_restart_handoff_waits_for_old_process_without_polling(self):
        # Exercise the actual pidfd wait; replace only the final exec so no user
        # application can be launched by this test.
        old = subprocess.Popen(["/usr/bin/python3", "-c", "import time; time.sleep(60)"])
        pidfd = os.pidfd_open(old.pid)
        ready_read, ready_write = os.pipe()
        prefix = "import os,json\nos.execv=lambda path,args: print(json.dumps(args))\n"
        child = subprocess.Popen(["/usr/bin/python3", "-I", "-c", prefix + updates.RESTART,
                                  str(pidfd), str(ready_write), "x86_64", "/tmp/isolated workspace"],
                                 pass_fds=(pidfd, ready_write), stdout=subprocess.PIPE)
        os.close(ready_write)
        try:
            self.assertTrue(updates.select.select([ready_read], [], [], 3)[0])
            self.assertEqual(os.read(ready_read, 5), b"ready")
            self.assertIsNone(child.poll())
            old.terminate()
            old.wait(timeout=3)
            output, _ = child.communicate(timeout=3)
            self.assertEqual(child.returncode, 0)
            self.assertEqual(json.loads(output), ["flatpak", "run", "--user", "--arch=x86_64",
                             "--branch=stable", APP, "--data-dir", "/tmp/isolated workspace"])
        finally:
            old.kill()
            old.wait()
            child.kill()
            child.wait()
            os.close(ready_read)
            os.close(pidfd)

    def test_busy_update_lock_rejects_second_installer(self):
        selected, cache, emit, command = self.install_fixture()
        directory = cache / "archcodex-updates"
        directory.mkdir(mode=0o700)
        with (directory / "lock").open("w") as lock:
            updates.fcntl.flock(lock, updates.fcntl.LOCK_EX)
            with self.assertRaisesRegex(ValueError, "already in progress"):
                updates.install(context(), selected)
        command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
