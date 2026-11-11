#!/usr/bin/env python3
"""Publication contracts and real signed Flatpak repositories, without app builds."""
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import flatpak_repository as publishing


class ContractTests(unittest.TestCase):
    def test_rejects_candidates_bad_urls_and_short_keys(self):
        for value in ("0.9.0-rc.1", "v0.9.0", "0.09.0"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                publishing.asset_name(value)
        for value in ("http://example.com/flatpak/", "https://user@example.com/flatpak/",
                      "https://example.com/flatpak/?secret", "https://example.com/flatpak/\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                publishing.repository_url(value)
        with self.assertRaises(ValueError):
            publishing.fingerprint("12345678")

    def test_archive_rejects_traversal_links_duplicates_and_oversize(self):
        cases = [("../escape", tarfile.REGTYPE, 0), ("/absolute", tarfile.REGTYPE, 0),
                 ("link", tarfile.SYMTYPE, 0), ("hardlink", tarfile.LNKTYPE, 0),
                 ("device", tarfile.CHRTYPE, 0), ("large", tarfile.REGTYPE, publishing.MAX_BYTES + 1)]
        for name, kind, size in cases:
            entry = tarfile.TarInfo(name)
            entry.type, entry.size = kind, size
            with self.subTest(name=name), self.assertRaises(ValueError):
                publishing.members([entry])
        entry = tarfile.TarInfo("duplicate")
        with self.assertRaises(ValueError):
            publishing.members([entry, entry])

    def test_successor_and_idempotence_reject_rollback_or_lost_history(self):
        old = dict(build={"version": "1.0.0"}, previous_manifest_sha256=None)
        raw = json.dumps(old).encode()
        new = dict(build={"version": "1.1.0"}, previous_manifest_sha256=publishing.sha256_bytes(raw))
        publishing.check_successor(old, None, bootstrap=True)
        publishing.check_successor(old, raw)
        publishing.check_successor(new, raw)
        for candidate, current in ((old, None), (new, None), (old, json.dumps(new).encode()),
                                   (dict(new, previous_manifest_sha256=None), raw)):
            with self.subTest(candidate=candidate), self.assertRaises(ValueError):
                publishing.check_successor(candidate, current)

    def test_release_requires_latest_non_draft_final_and_peels_tag(self):
        release = dict(id=42, tag_name="v1.0.0", draft=False, prerelease=False)
        responses = [release, release, {"object": {"type": "tag", "sha": "a" * 40}},
                     {"object": {"type": "commit", "sha": "b" * 40}}]
        with patch.object(publishing, "gh_json", side_effect=responses):
            self.assertEqual(publishing.check_published_release("v1.0.0")[1], "b" * 40)
        for changes in (dict(draft=True), dict(prerelease=True), dict(tag_name="v2.0.0")):
            with patch.object(publishing, "gh_json", side_effect=[dict(release, **changes), release]):
                with self.assertRaises(ValueError):
                    publishing.check_published_release("v1.0.0")
        with patch.object(publishing, "gh_json", side_effect=[release, dict(release, id=43)]):
            with self.assertRaises(ValueError):
                publishing.check_published_release("v1.0.0")


@unittest.skipUnless(all(shutil.which(tool) for tool in ("flatpak", "ostree", "gpg", "gpgconf")),
                     "Flatpak, OSTree and GnuPG are required for repository integration tests")
class RepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="archcodex-publication-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.home = cls.root / "gnupg"
        cls.home.mkdir(mode=0o700)
        cls.addClassCleanup(lambda: subprocess.run(["gpgconf", "--homedir", str(cls.home), "--kill", "gpg-agent"], check=True))
        cls.command("gpg", "--homedir", cls.home, "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                    "--quick-gen-key", "Archcodex test only <test@example.invalid>", "ed25519", "sign", "1d")
        keys = cls.command("gpg", "--homedir", cls.home, "--with-colons", "--list-keys").decode()
        cls.key = next(line.split(":")[9] for line in keys.splitlines() if line.startswith("fpr:"))
        cls.bundles = cls.root / "bundles"
        cls.bundles.mkdir()
        cls.first_bundle, cls.first_receipt = cls.fixture("1.0.0")
        cls.second_bundle, cls.second_receipt = cls.fixture("1.1.0")
        cls.output = cls.root / "dist"
        cls.first = publishing.prepare(cls.first_bundle, cls.first_receipt, cls.key, cls.home,
                                       publishing.URL, cls.output)
        cls.second = publishing.prepare(cls.second_bundle, cls.second_receipt, cls.key, cls.home,
                                        publishing.URL, cls.output, cls.first)

    @staticmethod
    def command(*args, **kwargs):
        return subprocess.check_output([str(arg) for arg in args], stderr=subprocess.PIPE, **kwargs)

    @classmethod
    def fixture(cls, version):
        stage = cls.root / f"stage-{version}"
        meta = stage / "files/share/metainfo"
        meta.mkdir(parents=True)
        (stage / "files/bin").mkdir()
        (stage / "files/bin/archcodex").write_text(f"#!/bin/sh\necho fixture {version}\n")
        (stage / "files/bin/archcodex").chmod(0o755)
        (stage / "metadata").write_text(f"[Application]\nname={publishing.APP_ID}\n"
                                       "runtime=org.kde.Platform/x86_64/6.11\ncommand=archcodex\n")
        (meta / f"{publishing.APP_ID}.metainfo.xml").write_text(
            f'<component type="desktop-application"><id>{publishing.APP_ID}</id><name>Archcodex</name>'
            '<summary>Publication test fixture</summary><metadata_license>CC0-1.0</metadata_license>'
            '<project_license>GPL-3.0-or-later</project_license><releases>'
            f'<release version="{version}" date="2026-11-11"/></releases></component>')
        repo = cls.root / f"export-{version}"
        cls.command("flatpak", "build-finish", stage)
        cls.command("flatpak", "build-export", "--arch=x86_64", "--disable-sandbox", repo, stage, "stable")
        bundle = cls.bundles / f"Archcodex-{version}-x86_64.flatpak"
        cls.command("flatpak", "build-bundle", repo, bundle, publishing.APP_ID, "stable")
        commit = cls.command("ostree", f"--repo={repo}", "rev-parse", publishing.REF).decode().strip()
        receipt = dict(schema=1, version=version, bundle=bundle.name, sha256=publishing.sha256(bundle),
                       bytes=bundle.stat().st_size, flatpak_ref=publishing.REF, flatpak_commit=commit,
                       runtime="org.kde.Platform/x86_64/6.11", source_commit="a" * 40,
                       source_tree="b" * 40, manifest_sha256="c" * 64, source_date_epoch=1)
        receipt_path = bundle.with_suffix(".BUILD-INFO.json")
        receipt_path.write_text(json.dumps(receipt))
        return bundle, receipt_path

    def setUp(self):
        temp = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)

    def unpack(self, archive, destination):
        return publishing.verify(archive, Path(str(archive) + ".asc"), self.key, publishing.URL, destination)

    def test_preparation_preserves_bundle_and_previous_objects(self):
        old_site, new_site = self.work / "old", self.work / "new"
        old = self.unpack(self.first, old_site)
        new = self.unpack(self.second, new_site)
        self.assertEqual(new["previous_manifest_sha256"], publishing.sha256(old_site / "publication.json"))
        self.assertEqual(publishing.sha256(self.first_bundle), old["build"]["sha256"])
        self.assertEqual(publishing.sha256(self.second_bundle), new["build"]["sha256"])
        for path in (old_site / "flatpak/objects").rglob("*"):
            if path.is_file():
                self.assertTrue((new_site / path.relative_to(old_site)).is_file())
        publishing.verify_repository(new_site / "flatpak", new["build"])
        self.assertEqual(sorted(path.name for path in self.output.iterdir()),
                         sorted(path.name for archive in (self.first, self.second)
                                for path in (archive, Path(str(archive) + ".asc"), Path(str(archive) + ".sha256"))))

    def test_native_pull_rejects_invalid_commit_signature(self):
        site = self.work / "site"
        publication = self.unpack(self.first, site)
        commit = publication["build"]["flatpak_commit"]
        (site / "flatpak/objects" / commit[:2] / (commit[2:] + ".commitmeta")).unlink()
        with self.assertRaises(subprocess.CalledProcessError):
            publishing.verify_repository(site / "flatpak", publication["build"])

    def test_wrong_key_url_signature_and_bundle_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            publishing.verify(self.first, Path(str(self.first) + ".asc"), "F" * 40, publishing.URL, self.work / "key")
        with self.assertRaisesRegex(ValueError, "identity"):
            publishing.verify(self.first, Path(str(self.first) + ".asc"), self.key,
                              "https://example.invalid/flatpak/", self.work / "url")
        with self.assertRaises(subprocess.CalledProcessError):
            publishing.verify(self.first, Path(str(self.second) + ".asc"), self.key, publishing.URL, self.work / "signature")
        tampered = self.work / self.first.name
        tampered.write_bytes(self.first.read_bytes() + b"tampered")
        with self.assertRaises(subprocess.CalledProcessError):
            publishing.verify(tampered, Path(str(self.first) + ".asc"), self.key, publishing.URL, self.work / "tampered")
        receipt = json.loads(self.first_receipt.read_text())
        receipt["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "Bundle differs"):
            publishing.check_bundle(self.first_bundle, receipt)
        self.assertFalse((self.work / "tampered").exists())

    def test_overwrite_bad_commit_and_old_predecessor_are_rejected(self):
        with self.assertRaises(FileExistsError):
            publishing.prepare(self.first_bundle, self.first_receipt, self.key, self.home, publishing.URL, self.output)
        with self.assertRaisesRegex(ValueError, "older"):
            publishing.prepare(self.first_bundle, self.first_receipt, self.key, self.home, publishing.URL,
                               self.work / "older", self.second)
        receipt = json.loads(self.first_receipt.read_text())
        receipt["flatpak_commit"] = "0" * 64
        broken = self.work / "receipt.json"
        broken.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "commit differs"):
            publishing.prepare(self.first_bundle, broken, self.key, self.home, publishing.URL, self.work / "bad")
        self.assertEqual(list((self.work / "bad").iterdir()), [])

    def test_stage_public_release_verifies_assets_tag_and_predecessor(self):
        site = self.work / "stage"
        assets = [self.first, Path(str(self.first) + ".asc"), self.first_bundle]
        info = dict(assets=[dict(name=path.name, size=path.stat().st_size) for path in assets])
        real_run = publishing.run

        def download(*args, **kwargs):
            if args[:3] == ("gh", "release", "download"):
                name = args[args.index("--pattern") + 1]
                destination = args[args.index("--dir") + 1]
                shutil.copyfile(next(path for path in assets if path.name == name), destination / name)
                return b""
            return real_run(*args, **kwargs)

        with patch.object(publishing, "run", side_effect=download), \
                patch.object(publishing, "check_published_release", return_value=(info, "a" * 40)), \
                patch.object(publishing.urllib.request, "urlopen", side_effect=publishing.urllib.error.HTTPError(
                    publishing.URL, 404, "not deployed", {}, None)):
            publishing.stage_release("v1.0.0", publishing.sha256(self.first), self.key, publishing.URL, site, bootstrap=True)
        self.assertTrue((site / "flatpak/summary.sig").is_file())
        with patch.object(publishing, "check_published_release", return_value=(info, "a" * 40)), \
                patch.object(publishing, "run", side_effect=download):
            with self.assertRaisesRegex(ValueError, "approved checksum"):
                publishing.stage_release("v1.0.0", "0" * 64, self.key, publishing.URL, self.work / "bad")
        with patch.object(publishing, "check_published_release", return_value=(info, "b" * 40)), \
                patch.object(publishing, "run", side_effect=download):
            with self.assertRaisesRegex(ValueError, "public release tag/source"):
                publishing.stage_release("v1.0.0", publishing.sha256(self.first), self.key,
                                         publishing.URL, self.work / "wrong-source")
        with patch.object(publishing, "run", side_effect=download), \
                patch.object(publishing, "check_published_release", return_value=(info, "a" * 40)), \
                patch.object(publishing.urllib.request, "urlopen", side_effect=publishing.urllib.error.HTTPError(
                    publishing.URL, 503, "unavailable", {}, None)):
            with self.assertRaises(publishing.urllib.error.HTTPError):
                publishing.stage_release("v1.0.0", publishing.sha256(self.first), self.key,
                                         publishing.URL, self.work / "network-failure", bootstrap=True)

    def test_native_flatpak_install_update_and_bundle_origin_migration(self):
        if not shutil.which("dbus-run-session"):
            self.skipTest("dbus-run-session is required for an isolated Flatpak installation")
        site = self.work / "site"
        first = self.unpack(self.first, site)

        class QuietHandler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(QuietHandler, directory=site))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(lambda: thread.join(timeout=5))
        self.addCleanup(server.shutdown)
        env = dict(os.environ, FLATPAK_USER_DIR=str(self.work / "installation"),
                   XDG_CACHE_HOME=str(self.work / "cache"), XDG_DATA_HOME=str(self.work / "data"),
                   XDG_CONFIG_HOME=str(self.work / "config"))

        def flatpak(*args):
            # This container has no system bus; the isolated private bus is sufficient.
            return self.command("dbus-run-session", "--", "bash", "-c",
                                'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec flatpak "$@"',
                                "flatpak-test", *args, env=env).decode().strip()

        flatpak("install", "--user", "--noninteractive", "--no-deps", "--bundle", self.first_bundle)
        original_origin = flatpak("info", "--user", "--show-origin", publishing.APP_ID)
        self.assertNotEqual(original_origin, "archcodex")
        flatpak("remote-add", "--user", f"--gpg-import={site / publishing.KEY_PATH}", "archcodex",
                f"http://127.0.0.1:{server.server_port}/flatpak/")
        flatpak("install", "--user", "--noninteractive", "--no-deps", "--reinstall", "archcodex", publishing.REF)
        self.assertEqual(flatpak("info", "--user", "--show-origin", publishing.APP_ID), "archcodex")
        self.assertEqual(flatpak("info", "--user", "--show-commit", publishing.APP_ID), first["build"]["flatpak_commit"])
        shutil.rmtree(site)
        second = self.unpack(self.second, site)
        flatpak("update", "--user", "--noninteractive", "--no-deps", publishing.APP_ID)
        self.assertEqual(flatpak("info", "--user", "--show-commit", publishing.APP_ID), second["build"]["flatpak_commit"])
        # Use a fresh installation to exclude cached summaries/HTTP 304 responses.
        # Flatpak may use either the summary or its newer summary index.
        for path in (site / "flatpak").rglob("*.sig"):
            path.write_bytes(b"invalid signature")
        env["FLATPAK_USER_DIR"] = str(self.work / "untrusted-installation")
        env["XDG_CACHE_HOME"] = str(self.work / "untrusted-cache")
        with self.assertRaises(subprocess.CalledProcessError) as rejected:
            flatpak("remote-add", "--user", f"--gpg-import={site / publishing.KEY_PATH}", "archcodex",
                    f"http://127.0.0.1:{server.server_port}/flatpak/")
            flatpak("remote-ls", "--user", "archcodex")
        self.assertIn(b"GPG", rejected.exception.stderr)


if __name__ == "__main__":
    unittest.main()
