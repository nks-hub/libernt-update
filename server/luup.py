#!/usr/bin/env python3
"""LiberNT Update publisher.

Builds a signed update feed from LiberNT build images, so installed systems can update
without a reinstall. Every release is a full manifest of system files with SHA-256 hashes;
file contents are stored once per hash under objects/. Clients compare the manifest with
their own disk and download only what differs.

Feed layout (serve the directory over HTTP):
    feed.json, feed.json.sig        list of releases, Ed25519 signature over feed.json
    releases/<id>/manifest.json     files of one release (path, sha256, size)
    objects/<aa>/<sha256>           file contents

Commands:
    luup.py keygen  --key KEY                      create a signing key, print the public key
    luup.py publish --key KEY --feed DIR --img LiberNT.img --commit SHA [--repo DIR] [--notes TEXT]
    luup.py verify  --feed DIR --pub HEX           check signatures and every object
    luup.py test                                   self-test on a synthetic tree
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import nacl.encoding
import nacl.signing

# Only these trees of the system partition are serviced; everything else is user or machine state.
INCLUDE_ROOTS = ("Windows/", "Program Files/", "Program Files (x86)/")
# Machine state that must never be overwritten by an update (registry hives, caches, logs, user-edited files).
EXCLUDE_PREFIXES = (
    "Windows/system32/config/",
    "Windows/SysWOW64/config/",
    "Windows/repair/",
    "Windows/TEMP/",
    "Windows/SystemTemp/",
    "Windows/Prefetch/",
    "Windows/system32/drivers/etc/",
    "Windows/system32/LogFiles/",
    "Windows/security/logs/",
    "Windows/Debug/",
    "Windows/unattend.inf",  # drives the first boot of the preinstalled image only
)
# Files Setup picks a variant for at install time (UP or MP kernel, one of several HALs). The client
# keeps the variant the machine has: it finds which candidate the installed file matches and takes
# that candidate's new version.
VARIANTS = {
    "Windows/system32/ntoskrnl.exe": ("Windows/system32/ntkrnlmp.exe",),
    "Windows/system32/hal.dll": ("Windows/system32/hal*.dll",),
}
EXCLUDE_SUFFIXES = (".log", ".etl", ".pnf", ".tmp")


def serviced(rel: str) -> bool:
    """Whether a path (relative to the system drive, forward slashes) belongs to an update."""
    low = rel.lower()
    if not any(low.startswith(r.lower()) for r in INCLUDE_ROOTS):
        return False
    if any(low.startswith(p.lower()) for p in EXCLUDE_PREFIXES):
        return False
    if low.endswith(EXCLUDE_SUFFIXES):
        return False
    return not any(part.startswith("$") for part in rel.split("/"))


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def scan_tree(root: Path):
    """Manifest entries for the serviced files below root, sorted by path."""
    files = []
    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            full = Path(dirpath) / name
            if full.is_symlink() or not full.is_file():
                continue
            rel = full.relative_to(root).as_posix()
            if serviced(rel):
                files.append({"path": rel, "sha256": sha256_file(full), "size": full.stat().st_size, "_src": full})
    files.sort(key=lambda f: f["path"].lower())
    return files


def extract_image(img: Path, work: Path) -> Path:
    """Unpack the NTFS system partition of a LiberNT disk image with 7-Zip; returns the tree root."""
    parts = work / "parts"
    tree = work / "tree"
    parts.mkdir()
    tree.mkdir()
    subprocess.run(["7z", "x", "-y", f"-o{parts}", str(img)], check=True, stdout=subprocess.DEVNULL)
    ntfs = sorted(p for p in parts.iterdir() if p.suffix.lower() == ".ntfs")
    if not ntfs:
        raise SystemExit(f"{img}: no NTFS partition found")
    subprocess.run(["7z", "x", "-y", f"-o{tree}", str(ntfs[0])], check=True, stdout=subprocess.DEVNULL)
    return tree


def load_key(path: Path) -> nacl.signing.SigningKey:
    return nacl.signing.SigningKey(bytes.fromhex(path.read_text().strip()))


def write_json(path: Path, data) -> bytes:
    raw = (json.dumps(data, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(raw)
    os.replace(tmp, path)
    return raw


def sign_feed(feed_dir: Path, key: nacl.signing.SigningKey, feed: dict) -> None:
    raw = write_json(feed_dir / "feed.json", feed)
    sig = key.sign(raw).signature
    tmp = feed_dir / "feed.json.sig.tmp"
    tmp.write_text(sig.hex() + "\n")
    os.replace(tmp, feed_dir / "feed.json.sig")


def variant_aliases(files) -> list[dict]:
    """For each variant file in the manifest: the files whose contents Setup may have put in its place."""
    import fnmatch
    paths = [f["path"] for f in files]
    out = []
    for target, patterns in VARIANTS.items():
        if target not in paths:
            continue
        cands = [target]
        for pat in patterns:
            cands += [p for p in paths if fnmatch.fnmatch(p.lower(), pat.lower()) and p not in cands]
        if len(cands) > 1:
            out.append({"path": target, "candidates": cands})
    return out


def git_notes(repo: Path, old_commit: str | None, new_commit: str) -> list[str]:
    if not repo or not old_commit or old_commit == new_commit:
        return []
    r = subprocess.run(["git", "-C", str(repo), "log", "--format=%h %s", f"{old_commit}..{new_commit}"],
                       capture_output=True, text=True)
    return r.stdout.splitlines()[:200] if r.returncode == 0 else []


def publish_tree(tree: Path, feed_dir: Path, key: nacl.signing.SigningKey, commit: str,
                 repo: Path | None = None, notes: str | None = None, now: dt.datetime | None = None) -> dict:
    now = now or dt.datetime.now(dt.timezone.utc)
    files = scan_tree(tree)
    if not files:
        raise SystemExit(f"{tree}: no serviced files")
    feed_path = feed_dir / "feed.json"
    feed = json.loads(feed_path.read_text("utf-8")) if feed_path.exists() else {"product": "LiberNT", "format": 1, "releases": []}
    previous = feed["releases"][-1] if feed["releases"] else None

    stored = 0
    for f in files:
        obj = feed_dir / "objects" / f["sha256"][:2] / f["sha256"]
        if not obj.exists():
            obj.parent.mkdir(parents=True, exist_ok=True)
            tmp = obj.with_suffix(".tmp")
            shutil.copyfile(f["_src"], tmp)
            if sha256_file(tmp) != f["sha256"]:
                raise SystemExit(f"{f['path']}: changed while copying")
            os.replace(tmp, obj)
            stored += 1

    changed = None
    if previous:
        old = json.loads((feed_dir / previous["manifest"]).read_text("utf-8"))
        old_map = {e["path"].lower(): e["sha256"] for e in old["files"]}
        new_paths = {f["path"].lower() for f in files}
        changed = sum(1 for f in files if old_map.get(f["path"].lower()) != f["sha256"])
        if not changed and new_paths == set(old_map):
            print(f"nothing to publish: commit {commit[:10]} builds the same files as {previous['id']}")
            return previous

    release_id = now.strftime("%Y%m%d-%H%M%S") + "-" + commit[:10]
    manifest = {
        "id": release_id,
        "commit": commit,
        "date": now.isoformat(timespec="seconds"),
        "files": [{k: v for k, v in f.items() if not k.startswith("_")} for f in files],
        "aliases": variant_aliases(files),
    }
    manifest_raw = write_json(feed_dir / "releases" / release_id / "manifest.json", manifest)

    entry = {
        "id": release_id,
        "commit": commit,
        "date": manifest["date"],
        "manifest": f"releases/{release_id}/manifest.json",
        "manifestSha256": hashlib.sha256(manifest_raw).hexdigest(),
        "files": len(files),
        "bytes": sum(f["size"] for f in files),
        "changedFromPrevious": changed,
        "notes": ([notes] if notes else []) + git_notes(repo, previous and previous["commit"], commit),
    }
    feed["releases"].append(entry)
    sign_feed(feed_dir, key, feed)
    print(f"published {release_id}: {len(files)} files, {stored} new objects, "
          f"{'first release' if changed is None else f'{changed} changed since {previous['id']}'}")
    return entry


def verify_feed(feed_dir: Path, pub_hex: str) -> int:
    raw = (feed_dir / "feed.json").read_bytes()
    sig = bytes.fromhex((feed_dir / "feed.json.sig").read_text().strip())
    nacl.signing.VerifyKey(bytes.fromhex(pub_hex)).verify(raw, sig)
    feed = json.loads(raw)
    bad = 0
    for rel in feed["releases"]:
        mraw = (feed_dir / rel["manifest"]).read_bytes()
        if hashlib.sha256(mraw).hexdigest() != rel["manifestSha256"]:
            print(f"{rel['id']}: manifest hash mismatch")
            bad += 1
            continue
        for f in json.loads(mraw)["files"]:
            obj = feed_dir / "objects" / f["sha256"][:2] / f["sha256"]
            if not obj.exists() or sha256_file(obj) != f["sha256"]:
                print(f"{rel['id']}: object missing or corrupt for {f['path']}")
                bad += 1
    print(f"feed {'OK' if not bad else 'BROKEN'}: {len(feed['releases'])} releases, {bad} problems")
    return 1 if bad else 0


def self_test() -> int:
    """Publish two synthetic releases and check diff, dedup, exclusions and signatures."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        key = nacl.signing.SigningKey.generate()
        pub = key.verify_key.encode(nacl.encoding.HexEncoder).decode()
        tree, feed = tmp / "tree", tmp / "feed"
        (tree / "Windows/system32/config").mkdir(parents=True)
        (tree / "Windows/system32/ntoskrnl.exe").write_bytes(b"kernel-v1")
        (tree / "Windows/system32/ntkrnlmp.exe").write_bytes(b"kernel-mp-v1")
        (tree / "Windows/system32/kernel32.dll").write_bytes(b"k32")
        (tree / "Windows/unattend.inf").write_bytes(b"[Unattend]")
        (tree / "Windows/system32/config/SYSTEM").write_bytes(b"hive")
        (tree / "Windows/setupapi.log").write_bytes(b"log")
        (tree / "Users").mkdir()
        (tree / "Users/readme.txt").write_bytes(b"user data")
        t0 = dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc)
        e1 = publish_tree(tree, feed, key, "a" * 40, now=t0)
        assert e1["files"] == 3 and e1["changedFromPrevious"] is None, e1
        m1 = json.loads((feed / e1["manifest"]).read_text("utf-8"))
        assert m1["aliases"] == [{"path": "Windows/system32/ntoskrnl.exe",
                                  "candidates": ["Windows/system32/ntoskrnl.exe", "Windows/system32/ntkrnlmp.exe"]}], m1["aliases"]
        (tree / "Windows/system32/ntoskrnl.exe").write_bytes(b"kernel-v2")
        (tree / "Windows/system32/new.dll").write_bytes(b"new")
        e2 = publish_tree(tree, feed, key, "b" * 40, now=t0 + dt.timedelta(hours=1))
        assert e2["files"] == 4 and e2["changedFromPrevious"] == 2, e2
        e3 = publish_tree(tree, feed, key, "c" * 40, now=t0 + dt.timedelta(hours=2))
        assert e3 is e2 or e3["id"] == e2["id"], "a commit with identical files must not become a release"
        assert len(json.loads((feed / "feed.json").read_text("utf-8"))["releases"]) == 2
        assert len(list((feed / "objects").rglob("*"))) - len(list((feed / "objects").iterdir())) == 5
        assert verify_feed(feed, pub) == 0
        (feed / "feed.json").write_bytes((feed / "feed.json").read_bytes().replace(b"LiberNT", b"Evil"))
        try:
            verify_feed(feed, pub)
            raise AssertionError("tampered feed accepted")
        except nacl.exceptions.BadSignatureError:
            pass
    print("self-test PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--key", type=Path, required=True)
    p = sub.add_parser("publish")
    p.add_argument("--key", type=Path, required=True)
    p.add_argument("--feed", type=Path, required=True)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--img", type=Path, help="LiberNT.img from `ninja liberntimg`")
    src.add_argument("--tree", type=Path, help="an already extracted system drive")
    p.add_argument("--commit", required=True)
    p.add_argument("--repo", type=Path, help="source checkout, for release notes")
    p.add_argument("--notes")
    v = sub.add_parser("verify")
    v.add_argument("--feed", type=Path, required=True)
    v.add_argument("--pub", required=True)
    sub.add_parser("test")
    a = ap.parse_args()

    if a.cmd == "keygen":
        if a.key.exists():
            raise SystemExit(f"{a.key} exists; refusing to overwrite a signing key")
        key = nacl.signing.SigningKey.generate()
        a.key.write_text(key.encode(nacl.encoding.HexEncoder).decode() + "\n")
        os.chmod(a.key, 0o600)
        print(key.verify_key.encode(nacl.encoding.HexEncoder).decode())
        return 0
    if a.cmd == "publish":
        key = load_key(a.key)
        a.feed.mkdir(parents=True, exist_ok=True)
        if a.tree:
            publish_tree(a.tree, a.feed, key, a.commit, a.repo, a.notes)
        else:
            with tempfile.TemporaryDirectory(dir=a.feed.parent) as work:
                publish_tree(extract_image(a.img, Path(work)), a.feed, key, a.commit, a.repo, a.notes)
        return 0
    if a.cmd == "verify":
        return verify_feed(a.feed, a.pub)
    return self_test()


if __name__ == "__main__":
    sys.exit(main())
