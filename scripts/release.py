#!/usr/bin/env python3
"""Local package identity checks and receipts. This tool never publishes anything."""
import argparse
import configparser
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
APP_ID = "io.github.emsi.Archcodex"


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def version_key(version):
    match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-rc\.([1-9][0-9]*))?", version)
    if not match:
        raise ValueError("VERSION must be X.Y.Z or X.Y.Z-rc.N (N starts at 1)")
    major, minor, patch, rc = match.groups()
    return int(major), int(minor), int(patch), rc is None, int(rc or 0)


def source_state(root):
    def git(*args):
        return run("git", "-C", str(root), *args)

    if git("status", "--porcelain", "--untracked-files=normal"):
        raise ValueError("Commit the intended source changes before packaging; the worktree must be clean.")
    version = (root / "VERSION").read_text().strip()
    key = version_key(version)
    catalog = json.loads((root / "docs/releases/catalog.json").read_text())
    if any(key <= version_key(item["version"]) for item in catalog["releases"]):
        raise ValueError("VERSION must be newer than every published version in the release catalog.")
    if git("tag", "--list", f"v{version}", f"test/v{version}"):
        raise ValueError(f"{version} already exists; assign a new candidate or release number.")
    return {"version": version, "source_commit": git("rev-parse", "HEAD"),
            "source_tree": git("rev-parse", "HEAD^{tree}"),
            "source_date_epoch": int(git("show", "-s", "--format=%ct", "HEAD")),
            "manifest_sha256": digest(root / "packaging" / f"{APP_ID}.json")}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(root):
    state = source_state(root)
    output = root / "dist"
    output.mkdir(parents=True, exist_ok=True)
    artifact = re.compile(rf'Archcodex-{re.escape(state["version"])}-[A-Za-z0-9_]+\.'
                          r'(?:flatpak(?:\.sha256)?|BUILD-INFO\.(?:json|txt)|TESTING\.md)')
    if any(artifact.fullmatch(path.name) for path in output.iterdir()):
        raise FileExistsError(f'Artifacts for {state["version"]} already exist in {output}')
    # Reserve only this version; other versions keep their existing files in dist/.
    with (output / f'.Archcodex-{state["version"]}.building.json').open("x") as f:
        json.dump(state, f, indent=2)
        f.write("\n")
    return output


def verify_appstream(build, version):
    metainfo = build / "files/share/metainfo" / f"{APP_ID}.metainfo.xml"
    if ET.parse(metainfo).find("releases/release").get("version") != version:
        raise ValueError("Staged AppStream version differs from VERSION.")
    # Flatpak records the installed version from the composed catalog, not the
    # source metainfo file. The retained-SDK fallback must compose it too.
    catalog = build / "files/share/app-info/xmls" / f"{APP_ID}.xml.gz"
    try:
        with gzip.open(catalog) as stream:
            components = ET.parse(stream).getroot().findall("component")
    except (OSError, EOFError, ET.ParseError) as error:
        raise ValueError("Missing or invalid compiled AppStream catalog. Compose it before export.") from error
    matches = [c for c in components if c.findtext("id") == APP_ID]
    if (len(matches) != 1 or matches[0].find("releases/release") is None
            or matches[0].find("releases/release").get("version") != version):
        raise ValueError("Compiled AppStream component/version differs from VERSION.")


def finish(root, build, repository, sdk_root):
    state = source_state(root)
    output = root / "dist"
    marker = output / f'.Archcodex-{state["version"]}.building.json'
    if json.loads(marker.read_text()) != state:
        raise ValueError("Source identity changed during the build; this package cannot be recorded.")
    arch = run("flatpak", "--default-arch")
    ref = f"app/{APP_ID}/{arch}/stable"
    metadata = configparser.ConfigParser()
    metadata.read(build / "metadata")
    manifest = json.loads((root / "packaging" / f"{APP_ID}.json").read_text())
    runtime = f'{manifest["runtime"]}/{arch}/{manifest["runtime-version"]}'
    if metadata["Application"]["name"] != APP_ID or metadata["Application"]["runtime"] != runtime:
        raise ValueError("Staged application ID or runtime differs from the manifest.")
    verify_appstream(build, state["version"])
    with tempfile.TemporaryDirectory(prefix="archcodex-package-check-") as temp:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=temp,
                   XDG_DATA_HOME=temp, XDG_CACHE_HOME=temp, XDG_RUNTIME_DIR=temp)
        if sdk_root:
            libraries = [build / "files/lib", sdk_root / "usr/lib/x86_64-linux-gnu",
                         sdk_root / "usr/lib/x86_64-linux-gnu/pulseaudio", sdk_root / "usr/lib"]
            env["QT_PLUGIN_PATH"] = str(sdk_root / "usr/lib/plugins")
            command = [str(sdk_root / "usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
                       "--library-path", ":".join(map(str, libraries)), str(build / "files/bin/archcodex")]
        else:
            command = ["flatpak", "build", "--env=QT_QPA_PLATFORM=offscreen", str(build),
                       "/app/bin/archcodex"]
        reported = run(*command, "--version", env=env)
    if reported != f'archcodex {state["version"]}':
        raise ValueError(f"Packaged executable reports unexpected version: {reported}")
    commit = run("ostree", f"--repo={repository}", "rev-parse", ref)
    bundle = output / f'Archcodex-{state["version"]}-{arch}.flatpak'
    # Check the actual bundle against the exported payload, not just its filename.
    with tempfile.TemporaryDirectory(prefix="archcodex-bundle-check-") as temp:
        repo = Path(temp) / "repo"
        subprocess.run(["ostree", f"--repo={repo}", "init", "--mode=archive"], check=True)
        subprocess.run(["flatpak", "build-import-bundle", str(repo), str(bundle)], check=True)
        if run("ostree", f"--repo={repo}", "rev-parse", ref) != commit:
            raise ValueError("Bundle commit differs from the exported commit.")
        packaged = subprocess.check_output(["ostree", f"--repo={repo}", "cat", ref, "/files/bin/archcodex"])
        if hashlib.sha256(packaged).hexdigest() != digest(build / "files/bin/archcodex"):
            raise ValueError("Bundle executable differs from the verified executable.")
        catalog = f"files/share/app-info/xmls/{APP_ID}.xml.gz"
        if subprocess.check_output(["ostree", f"--repo={repo}", "cat", ref, "/" + catalog]) != (build / catalog).read_bytes():
            raise ValueError("Bundle AppStream catalog differs from the verified catalog.")
    receipt = dict(state, schema=1, bundle=bundle.name, sha256=digest(bundle), bytes=bundle.stat().st_size,
                   flatpak_ref=ref, flatpak_commit=commit, runtime=runtime,
                   build_method="retained-sdk-stage" if sdk_root else "flatpak-builder")
    if source_state(root) != state:
        raise ValueError("Source identity changed during verification.")
    with (output / f"{bundle.stem}.BUILD-INFO.json").open("x") as f:
        json.dump(receipt, f, indent=2)
        f.write("\n")
    with (output / f"{bundle.name}.sha256").open("x") as f:
        f.write(f'{receipt["sha256"]}  {bundle.name}\n')
    run("git", "-C", str(root), "tag", f'test/v{state["version"]}', state["source_commit"])
    marker.unlink()
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("prepare")
    record = sub.add_parser("finish")
    record.add_argument("--build-dir", required=True, type=Path)
    record.add_argument("--repository", required=True, type=Path)
    record.add_argument("--sdk-root", type=Path)
    args = parser.parse_args()
    try:
        result = prepare(ROOT) if args.command == "prepare" else finish(
            ROOT, args.build_dir.resolve(), args.repository.resolve(),
            args.sdk_root.resolve() if args.sdk_root else None)
        print(result)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release check failed: {error}\n")


if __name__ == "__main__":
    main()
