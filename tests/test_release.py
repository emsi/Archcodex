#!/usr/bin/env python3
"""Exercise version reservations against real, isolated Git repositories."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release", Path(__file__).parents[1] / "scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.name", "Release test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        (self.root / "VERSION").write_text("0.9.0-rc.1\n")
        (self.root / ".gitignore").write_text("dist/\n")
        (self.root / "docs/releases").mkdir(parents=True)
        (self.root / "docs/releases/catalog.json").write_text(json.dumps({"releases": [{"version": "0.8.1"}]}))
        (self.root / "packaging").mkdir()
        (self.root / "packaging/io.github.emsi.Archcodex.json").write_text("{}")
        self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.DEVNULL, text=True)

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def test_candidate_order_and_invalid_versions(self):
        versions = ["0.8.1", "0.9.0-rc.1", "0.9.0-rc.2", "0.9.0-rc.10", "0.9.0", "0.10.0"]
        self.assertEqual(versions, sorted(versions, key=release.version_key))
        for version in ["0.09.0", "0.9.0-rc.0", "0.9.0-rc.01", "0.9.0-dirty", "v0.9.0", "0.9.0\nextra"]:
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.version_key(version)

    def test_reservation_records_source_and_refuses_overwrite(self):
        dist = self.root / "dist"
        dist.mkdir()
        previous = dist / "Archcodex-0.8.1-x86_64.flatpak"
        previous.write_bytes(b"previous bundle")
        output = release.prepare(self.root)
        self.assertEqual(output, dist)
        self.assertEqual(previous.read_bytes(), b"previous bundle")
        recorded = json.loads((output / ".Archcodex-0.9.0-rc.1.building.json").read_text())
        self.assertEqual(recorded["source_commit"], self.git("rev-parse", "HEAD").strip())
        with self.assertRaises(FileExistsError):
            release.prepare(self.root)

    def test_existing_bundle_is_not_overwritten(self):
        dist = self.root / "dist"
        dist.mkdir()
        bundle = dist / "Archcodex-0.9.0-rc.1-x86_64.flatpak"
        bundle.write_bytes(b"existing candidate")
        with self.assertRaises(FileExistsError):
            release.prepare(self.root)
        self.assertEqual(bundle.read_bytes(), b"existing candidate")
        self.assertFalse((dist / ".Archcodex-0.9.0-rc.1.building.json").exists())

    def test_candidate_bundle_does_not_block_final_version(self):
        dist = self.root / "dist"
        dist.mkdir()
        candidate = dist / "Archcodex-0.9.0-rc.1-x86_64.flatpak"
        candidate.write_bytes(b"accepted candidate")
        (self.root / "VERSION").write_text("0.9.0\n")
        self.commit()
        self.assertEqual(release.prepare(self.root), dist)
        self.assertEqual(candidate.read_bytes(), b"accepted candidate")

    def test_dirty_source_is_rejected(self):
        (self.root / "VERSION").write_text("0.9.0-rc.2\n")
        with self.assertRaisesRegex(ValueError, "worktree must be clean"):
            release.prepare(self.root)

    def test_untracked_source_is_rejected(self):
        (self.root / "untracked.cpp").write_text("// uncommitted source")
        with self.assertRaisesRegex(ValueError, "worktree must be clean"):
            release.prepare(self.root)

    def test_existing_tag_is_rejected(self):
        self.git("tag", "v0.9.0-rc.1")
        with self.assertRaisesRegex(ValueError, "already exists"):
            release.prepare(self.root)

    def test_local_candidate_tag_prevents_reuse_after_artifact_cleanup(self):
        self.git("tag", "test/v0.9.0-rc.1")
        with self.assertRaisesRegex(ValueError, "already exists"):
            release.prepare(self.root)

    def test_source_change_during_build_is_rejected(self):
        release.prepare(self.root)
        (self.root / "another-source").write_text("changed")
        self.commit()
        with self.assertRaisesRegex(ValueError, "Source identity changed"):
            release.finish(self.root, self.root / "stage", self.root / "repo", None)

    def test_published_version_cannot_be_reused(self):
        (self.root / "VERSION").write_text("0.8.1\n")
        self.commit()
        with self.assertRaisesRegex(ValueError, "newer than"):
            release.prepare(self.root)


if __name__ == "__main__":
    unittest.main()
