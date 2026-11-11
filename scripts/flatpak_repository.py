#!/usr/bin/env python3
"""Prepare signed Flatpak Pages assets locally; verify/stage them without building."""
import argparse
import base64
import configparser
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.error
import urllib.request
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET

from release import APP_ID, ROOT, version_key

REPOSITORY = "emsi/Archcodex"
URL = "https://emsi.github.io/Archcodex/flatpak/"
REF = f"app/{APP_ID}/x86_64/stable"
# Leave space below GitHub Pages' 1 GB published-site limit.
MAX_BYTES = 900 * 1024 * 1024
MAX_FILES = 100_000
KEY_PATH = "flatpak/archcodex.gpg"


def run(*args, **kwargs):
    return subprocess.check_output([str(arg) for arg in args], **kwargs)


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def final_version(value):
    if not version_key(value)[3]:
        raise ValueError("Only final versions can be published to the stable repository")
    return value


def fingerprint(value):
    if not re.fullmatch(r"[A-Fa-f0-9]{40}", value):
        raise ValueError("Supply the complete 40-character primary GPG fingerprint")
    return value.upper()


def repository_url(value):
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password
            or parsed.query or parsed.fragment or not value.endswith("/flatpak/")
            or re.search(r"[\s\x00-\x1f]", value)):
        raise ValueError("Repository URL must be HTTPS and end in /flatpak/")
    return value


def asset_name(version):
    return f"Archcodex-{final_version(version)}-x86_64.pages.tar.gz"


def read_receipt(value):
    version = final_version(value["version"])
    if (value["schema"] != 1 or value["flatpak_ref"] != REF
            or value["bundle"] != f"Archcodex-{version}-x86_64.flatpak"
            or not isinstance(value["bytes"], int) or not 0 < value["bytes"] <= MAX_BYTES
            or not re.fullmatch(r"org\.kde\.Platform/x86_64/[0-9.]+", value["runtime"])):
        raise ValueError("Unsupported build receipt identity")
    for key, length in [("sha256", 64), ("flatpak_commit", 64), ("source_commit", 40),
                        ("source_tree", 40), ("manifest_sha256", 64)]:
        if not re.fullmatch(f"[0-9a-f]{{{length}}}", value[key]):
            raise ValueError(f"Invalid receipt {key}")
    return value


def check_bundle(bundle, receipt):
    if bundle.name != receipt["bundle"] or bundle.stat().st_size != receipt["bytes"] or sha256(bundle) != receipt["sha256"]:
        raise ValueError("Bundle differs from its accepted build receipt")


def check_public_key(key, expected, home):
    # Import into an isolated public-only keyring, never the operator's keyring.
    run("gpg", "--homedir", home, "--batch", "--no-autostart", "--import", input=key,
        stderr=subprocess.PIPE)
    listing = run("gpg", "--homedir", home, "--batch", "--with-colons", "--list-keys").decode()
    primaries = []
    primary = False
    for line in listing.splitlines():
        fields = line.split(":")
        if fields[0] in ("pub", "sub"):
            primary = fields[0] == "pub"
        elif fields[0] == "fpr" and primary:
            primaries.append(fields[9])
            primary = False
    if primaries != [fingerprint(expected)]:
        raise ValueError("Publication key does not match the independently trusted fingerprint")


def members(archive):
    """Validate before extraction; Pages accepts neither symlinks nor hardlinks."""
    seen = set()
    size = 0
    result = []
    for member in archive:
        path = PurePosixPath(member.name)
        if (not member.name or path.is_absolute() or ".." in path.parts
                or str(path) != member.name.rstrip("/") or member.name in seen
                or not re.fullmatch(r"[A-Za-z0-9_./+-]+", member.name)
                or not (member.isdir() or member.isfile()) or member.size < 0):
            raise ValueError(f"Unsafe publication archive entry: {member.name!r}")
        seen.add(member.name)
        size += member.size
        result.append(member)
        if len(result) > MAX_FILES or size > MAX_BYTES:
            raise ValueError("Publication exceeds Pages size/file limits")
    return result


def read_member(archive, name, limit):
    member = archive.getmember(name)
    if not member.isfile() or member.size > limit:
        raise ValueError(f"Invalid {name} in publication")
    return archive.extractfile(member).read()


def verify(archive_path, signature, expected_fingerprint, expected_url, output):
    """Authenticate the entire payload before extracting into a new directory."""
    if output.exists():
        raise FileExistsError(f"Refusing to replace {output}")
    if archive_path.stat().st_size > MAX_BYTES or signature.stat().st_size > 65536:
        raise ValueError("Publication archive/signature is too large")
    with tarfile.open(archive_path, "r:gz") as archive, tempfile.TemporaryDirectory(prefix="archcodex-verify-") as temp:
        entries = members(archive)
        key = read_member(archive, KEY_PATH, 1024 * 1024)
        check_public_key(key, expected_fingerprint, temp)
        run("gpg", "--homedir", temp, "--batch", "--no-auto-key-retrieve", "--no-auto-key-import",
            "--verify", signature, archive_path, stderr=subprocess.PIPE)
        publication = json.loads(read_member(archive, "publication.json", 65536))
        receipt = read_receipt(publication["build"])
        previous = publication["previous_manifest_sha256"]
        if (publication["schema"] != 1 or publication["signing_fingerprint"] != fingerprint(expected_fingerprint)
                or publication["repository_url"] != repository_url(expected_url)
                or (previous is not None and not re.fullmatch(r"[0-9a-f]{64}", previous))
                or archive_path.name != asset_name(receipt["version"])):
            raise ValueError("Publication identity differs from the requested repository/release")
        output.mkdir(parents=True)
        try:
            for entry in entries:
                destination = output / entry.name
                if entry.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with destination.open("xb") as stream:
                        shutil.copyfileobj(archive.extractfile(entry), stream)
                    destination.chmod(0o644)
        except BaseException:
            shutil.rmtree(output)
            raise
    return publication


def verify_repository(repo, receipt):
    commit = run("ostree", f"--repo={repo}", "rev-parse", REF).decode().strip()
    if commit != receipt["flatpak_commit"]:
        raise ValueError("Imported repository commit differs from the accepted bundle")
    metadata = configparser.ConfigParser(interpolation=None)
    metadata.read_string(run("ostree", f"--repo={repo}", "cat", REF, "/metadata").decode())
    appstream = run("ostree", f"--repo={repo}", "cat", REF,
                    f"/files/share/metainfo/{APP_ID}.metainfo.xml")
    if (metadata["Application"]["name"] != APP_ID or metadata["Application"]["runtime"] != receipt["runtime"]
            or ET.fromstring(appstream).find("releases/release").get("version") != receipt["version"]):
        raise ValueError("Repository runtime or embedded version differs from the build receipt")
    run("ostree", f"--repo={repo}", "fsck", stderr=subprocess.PIPE)
    # Exercise the same native signature checks used by Flatpak clients.
    with tempfile.TemporaryDirectory(prefix="archcodex-pull-") as temp:
        consumer = Path(temp) / "repo"
        run("ostree", f"--repo={consumer}", "init", "--mode=archive")
        run("ostree", f"--repo={consumer}", "remote", "add", "origin", repo.as_uri(),
            f"--gpg-import={repo / 'archcodex.gpg'}", "--set=gpg-verify-summary=true")
        refs = run("ostree", f"--repo={repo}", "refs").decode().splitlines()
        if set(refs) != {REF, "appstream/x86_64", "appstream2/x86_64"}:
            raise ValueError("Repository must contain only the stable app and its AppStream refs")
        run("ostree", f"--repo={consumer}", "pull", "origin", *refs, stderr=subprocess.PIPE)


def prepare(bundle, receipt_path, key, gpg_home, url, output, previous=None):
    key, url = fingerprint(key), repository_url(url)
    receipt = read_receipt(json.loads(receipt_path.read_text()))
    check_bundle(bundle, receipt)
    output.mkdir(parents=True, exist_ok=True)
    name = asset_name(receipt["version"])
    destinations = [output / name, output / (name + ".asc"), output / (name + ".sha256")]
    if any(path.exists() for path in destinations):
        raise FileExistsError("Publication artifacts already exist; refusing to overwrite them")
    signing = [f"--gpg-sign={key}", f"--gpg-homedir={gpg_home}"]
    with tempfile.TemporaryDirectory(prefix=".archcodex-pages-", dir=output) as temp:
        temp = Path(temp)
        site = temp / "site"
        previous_hash = None
        if previous:
            old = verify(previous, Path(str(previous) + ".asc"), key, url, site)
            if version_key(old["build"]["version"]) >= version_key(receipt["version"]):
                raise ValueError("The previous publication must be older than this release")
            previous_hash = sha256(site / "publication.json")
        else:
            site.mkdir()
            run("ostree", f"--repo={site / 'flatpak'}", "init", "--mode=archive")
        repo = site / "flatpak"
        public_key = run("gpg", "--homedir", gpg_home, "--batch", "--export-options", "export-minimal", "--export", key)
        with tempfile.TemporaryDirectory(prefix="archcodex-key-") as home:
            check_public_key(public_key, key, home)
        (repo / "archcodex.gpg").write_bytes(public_key)
        run("flatpak", "build-import-bundle", *signing, repo, bundle, stderr=subprocess.PIPE)
        run("flatpak", "build-update-repo", *signing, "--generate-static-deltas", "--static-delta-jobs=2",
            "--title=Archcodex", "--default-branch=stable", repo, stderr=subprocess.PIPE)
        verify_repository(repo.resolve(), receipt)
        encoded = base64.b64encode(public_key).decode()
        (site / "archcodex.flatpakrepo").write_text(
            f"[Flatpak Repo]\nTitle=Archcodex\nUrl={url}\nDefaultBranch=stable\n"
            f"Homepage=https://github.com/{REPOSITORY}\nGPGKey={encoded}\n")
        (site / "archcodex.flatpakref").write_text(
            f"[Flatpak Ref]\nName={APP_ID}\nBranch=stable\nTitle=Archcodex\n"
            f"Url={url}\nSuggestRemoteName=archcodex\nIsRuntime=false\n"
            f"RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo\nGPGKey={encoded}\n")
        (site / "index.html").write_text(
            '<!doctype html><html lang="en"><meta charset="utf-8"><title>Archcodex Flatpak</title>'
            '<h1>Archcodex Flatpak</h1><p><a href="archcodex.flatpakref">Install Archcodex</a> · '
            '<a href="archcodex.flatpakrepo">Add the update repository</a></p>'
            '<p><a href="https://github.com/emsi/Archcodex">Documentation and releases</a></p></html>\n')
        publication = dict(schema=1, build=receipt, signing_fingerprint=key, repository_url=url,
                           previous_manifest_sha256=previous_hash)
        (site / "publication.json").write_text(json.dumps(publication, indent=2) + "\n")
        archive_path = temp / name
        # Archive only regular files/directories, excluding OSTree's transient lock/tmp files.
        with tarfile.open(archive_path, "w:gz", dereference=False) as archive:
            for path in sorted(site.rglob("*")):
                relative = path.relative_to(site)
                if relative.parts[:2] in [("flatpak", "tmp"), ("flatpak", "extensions")]:
                    continue
                if relative.as_posix() == "flatpak/.lock":
                    continue
                archive.add(path, arcname=str(relative), recursive=False)
        signature = temp / (name + ".asc")
        run("gpg", "--homedir", gpg_home, "--local-user", key, "--armor", "--detach-sign", "--output", signature, archive_path)
        verify(archive_path, signature, key, url, temp / "verified")
        if sha256(bundle) != receipt["sha256"]:
            raise ValueError("Bundle changed during repository preparation")
        (temp / (name + ".sha256")).write_text(f"{sha256(archive_path)}  {name}\n")
        # Exclusive creation preserves already delivered artifacts even with concurrent operators.
        for target in destinations:
            with target.open("xb") as stream, (temp / target.name).open("rb") as source:
                shutil.copyfileobj(source, stream)
    return destinations[0]


def gh_json(endpoint):
    return json.loads(run("gh", "api", f"repos/{REPOSITORY}/{endpoint}"))


def check_published_release(tag):
    final_version(tag.removeprefix("v"))
    info = gh_json(f"releases/tags/{tag}")
    latest = gh_json("releases/latest")
    if info["tag_name"] != tag or info["draft"] or info["prerelease"] or latest["id"] != info["id"]:
        raise ValueError("Select the published final release currently marked Latest")
    obj = gh_json(f"git/ref/tags/{tag}")["object"]
    for _ in range(4):
        if obj["type"] == "commit":
            return info, obj["sha"]
        if obj["type"] != "tag":
            break
        obj = gh_json(f"git/tags/{obj['sha']}")["object"]
    raise ValueError("Release tag does not resolve to a commit")


def check_successor(publication, current, bootstrap=False):
    if current is None:
        if not bootstrap or publication["previous_manifest_sha256"] is not None:
            raise ValueError("Previous deployment is missing; only explicit first-publication bootstrap is allowed")
        return
    deployed = json.loads(current)
    if deployed == publication:
        return  # Idempotent retry with the same signed publication identity.
    if (version_key(deployed["build"]["version"]) >= version_key(publication["build"]["version"])
            or sha256_bytes(current) != publication["previous_manifest_sha256"]):
        raise ValueError("Publication is stale or does not extend the deployed repository")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def stage_release(tag, expected_sha256, key, url, output, bootstrap=False):
    """Actions entrypoint: download/verify existing release assets, never rebuild."""
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) or not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise ValueError("Supply a final vX.Y.Z tag and the locally verified archive SHA-256")
    key, url = fingerprint(key), repository_url(url)
    info, commit = check_published_release(tag)
    version = tag[1:]
    name = asset_name(version)
    required = [name, name + ".asc", f"Archcodex-{version}-x86_64.flatpak"]
    for name_ in required:
        assets = [asset for asset in info["assets"] if asset["name"] == name_]
        if len(assets) != 1 or not 0 < assets[0]["size"] <= MAX_BYTES:
            raise ValueError(f"Missing or invalid release asset: {name_}")
    with tempfile.TemporaryDirectory(prefix="archcodex-download-") as temp:
        temp = Path(temp)
        for name_ in required:
            run("gh", "release", "download", tag, "--repo", REPOSITORY, "--pattern", name_, "--dir", temp)
        archive = temp / name
        if sha256(archive) != expected_sha256:
            raise ValueError("Release archive differs from the explicitly approved checksum")
        site = temp / "site"
        publication = verify(archive, Path(str(archive) + ".asc"), key, url, site)
        if publication["build"]["version"] != version or publication["build"]["source_commit"] != commit:
            raise ValueError("Publication does not match the public release tag/source")
        check_bundle(temp / publication["build"]["bundle"], publication["build"])
        try:
            request = urllib.request.Request(url.removesuffix("flatpak/") + "publication.json",
                                             headers={"Cache-Control": "no-cache"})
            with urllib.request.urlopen(request, timeout=30) as response:
                current = response.read(65537)
                if len(current) > 65536:
                    raise ValueError("Unexpected deployed publication metadata")
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
            current = None
        check_successor(publication, current, bootstrap)
        # Check Latest again after downloads. The workflow serializes the full operation.
        if check_published_release(tag)[1] != commit:
            raise ValueError("Release changed during verification")
        if output.exists():
            raise FileExistsError(f"Refusing to replace {output}")
        shutil.copytree(site, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    local = sub.add_parser("prepare", help="import/sign an accepted final bundle locally")
    local.add_argument("--bundle", type=Path, required=True)
    local.add_argument("--receipt", type=Path, required=True)
    local.add_argument("--gpg-homedir", type=Path, required=True)
    local.add_argument("--previous", type=Path, help="previous Pages archive; adjacent .asc is required")
    local.add_argument("--output", type=Path, default=ROOT / "dist")
    verification = sub.add_parser("verify", help="verify a signed archive and extract a new staging directory")
    verification.add_argument("--archive", type=Path, required=True)
    verification.add_argument("--signature", type=Path, required=True)
    verification.add_argument("--output", type=Path, required=True)
    deployment = sub.add_parser("stage-release", help="verify and stage the Latest public release for Pages")
    deployment.add_argument("--tag", required=True)
    deployment.add_argument("--sha256", required=True)
    deployment.add_argument("--output", type=Path, required=True)
    deployment.add_argument("--bootstrap", action="store_true", help="allow an explicitly authorized first deployment")
    for command in (local, verification, deployment):
        command.add_argument("--fingerprint", required=True, help="trusted full primary signing-key fingerprint")
        command.add_argument("--url", default=URL)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            result = prepare(args.bundle.resolve(), args.receipt.resolve(), args.fingerprint,
                             args.gpg_homedir.resolve(), args.url, args.output.resolve(), args.previous)
        elif args.command == "verify":
            verify(args.archive, args.signature, args.fingerprint, args.url, args.output)
            result = args.output
        else:
            result = stage_release(args.tag, args.sha256, args.fingerprint, args.url, args.output, args.bootstrap)
        print(result)
    except (ValueError, OSError, KeyError, TypeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        detail = ""
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            detail = "\n" + error.stderr.decode(errors="replace")[-4000:]
        parser.exit(1, f"Flatpak publication failed: {error}{detail}\n")


if __name__ == "__main__":
    main()
