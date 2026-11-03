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
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import nacl.encoding
import nacl.signing

# Only these trees of the system partition are serviced; everything else is user or machine state.
INCLUDE_ROOTS = ("Windows/", "Program Files/", "Program Files (x86)/")
# Boot partition of the image (FAT, the EFI system partition) unpacked under this name. Only the loaders are
# serviced: BCD is the machine's boot configuration and the rest are Raspberry Pi firmware files.
BOOT_DIR = "BootPartition/"
BOOT_FILES = ("freeldr.sys", "rosload.exe", "EFI/BOOT/bootx64.efi", "EFI/LiberNT/Boot/bootmgfw.efi")
# The update client itself ships in every release (publish --client).
CLIENT_DIR = "Program Files/LiberNT Update"
CLIENT_FILES = ("LiberNTUpdate.exe", "luupboot.exe")
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
    if low.startswith(BOOT_DIR.lower()):
        return low[len(BOOT_DIR):] in {f.lower() for f in BOOT_FILES}
    if not any(low.startswith(r.lower()) for r in INCLUDE_ROOTS):
        return False
    if any(low.startswith(p.lower()) for p in EXCLUDE_PREFIXES):
        return False
    if low.endswith(EXCLUDE_SUFFIXES):
        return False
    return not any(part.startswith("$") for part in rel.split("/"))


# Strings every build writes into its files: the build tag (20261103-nt10.0-044-1374-g320a592) and commit hashes.
BUILD_TAG = re.compile(rb"20\d{6}-nt10\.0-\d{3}-\d{4}-g[0-9a-f]{7,12}")
BUILD_TAG_UTF16 = re.compile(rb"2\x000\x00(?:\d\x00){6}-\x00n\x00t\x001\x000\x00\.\x000\x00-\x00(?:\d\x00){3}-\x00(?:\d\x00){4}-\x00g\x00(?:[0-9a-f]\x00){7,12}")
COMMIT_HASH = re.compile(rb"(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])")


def _pe_offset(b: bytes) -> int | None:
    """Offset of the MZ header of a PE image: 0, or a sector boundary when a loader stub comes first (freeldr.sys)."""
    for base in range(0, min(len(b), 0x10000), 0x200):
        if b[base:base + 2] == b"MZ" and base + 0x40 <= len(b):
            pe = base + struct.unpack_from("<I", b, base + 0x3C)[0]
            if pe + 24 <= len(b) and b[pe:pe + 4] == b"PE\0\0":
                return base
    return None


def build_neutral(b: bytes) -> bytes:
    """The bytes with what changes on every build blanked out: PE time stamps, checksum, debug directory contents
    (CodeView GUID, PDB path), build tags and, in PE images, commit hashes. Two files with equal results run the same."""
    base = _pe_offset(b)
    d = bytearray(b)
    if base is not None:
        pe = base + struct.unpack_from("<I", d, base + 0x3C)[0]
        opt = pe + 24
        d[pe + 8:pe + 12] = bytes(4)
        d[opt + 64:opt + 68] = bytes(4)
        ddir = opt + (96 if struct.unpack_from("<H", d, opt)[0] == 0x10B else 112)
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        secs = opt + struct.unpack_from("<H", d, pe + 20)[0]
        dbg_rva, dbg_size = struct.unpack_from("<II", d, ddir + 6 * 8) if ddir + 56 <= len(d) else (0, 0)
        for i in range(nsec if dbg_rva else 0):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, secs + 40 * i + 8)
            if va <= dbg_rva < va + max(vsize, rsize):
                off = base + raw + dbg_rva - va
                for e in range(dbg_size // 28):
                    ent = off + 28 * e
                    if ent + 28 > len(d):
                        break
                    d[ent + 4:ent + 8] = bytes(4)
                    size, ptr = struct.unpack_from("<I", d, ent + 16)[0], struct.unpack_from("<I", d, ent + 24)[0]
                    if ptr and base + ptr + size <= len(d):
                        d[base + ptr:base + ptr + size] = bytes(size)
                break
    out = bytes(d)
    patterns = (BUILD_TAG, BUILD_TAG_UTF16, COMMIT_HASH) if base is not None else (BUILD_TAG, BUILD_TAG_UTF16)
    for rx in patterns:
        out = rx.sub(lambda m: b"#" * len(m.group(0)), out)
    return out


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
    # The FAT boot partition comes out as 0.img (or *.fat, depending on the 7-Zip version).
    boot = sorted(p for p in parts.iterdir() if p.suffix.lower() in (".img", ".fat"))
    if boot:
        subprocess.run(["7z", "x", "-y", f"-o{tree / BOOT_DIR.rstrip('/')}", str(boot[0])], check=True, stdout=subprocess.DEVNULL)
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


def add_client(tree: Path, client_dir: Path) -> str:
    """Puts the client (LiberNTUpdate.exe, luupboot.exe) into the tree, so updates also update the updater.
    client_dir is an unpacked client release; its VERSION file names the version."""
    dest = tree / CLIENT_DIR
    dest.mkdir(parents=True, exist_ok=True)
    for name in CLIENT_FILES:
        shutil.copyfile(client_dir / name, dest / name)
    return (client_dir / "VERSION").read_text().strip()


def publish_tree(tree: Path, feed_dir: Path, key: nacl.signing.SigningKey, commit: str,
                 repo: Path | None = None, notes: str | None = None, now: dt.datetime | None = None,
                 client: str | None = None) -> dict:
    now = now or dt.datetime.now(dt.timezone.utc)
    files = scan_tree(tree)
    if not files:
        raise SystemExit(f"{tree}: no serviced files")
    feed_path = feed_dir / "feed.json"
    feed = json.loads(feed_path.read_text("utf-8")) if feed_path.exists() else {"product": "LiberNT", "format": 1, "releases": []}
    previous = feed["releases"][-1] if feed["releases"] else None

    # A file rebuilt from unchanged sources differs only in build metadata; it keeps the previous release's
    # contents, so clients neither download nor replace it.
    kept = 0
    if previous:
        prev_files = {e["path"]: e for e in json.loads((feed_dir / previous["manifest"]).read_text("utf-8"))["files"]}
        for f in files:
            old = prev_files.get(f["path"])
            if not old or old["sha256"] == f["sha256"] or old["size"] != f["size"]:
                continue
            old_obj = feed_dir / "objects" / old["sha256"][:2] / old["sha256"]
            if old_obj.exists() and build_neutral(old_obj.read_bytes()) == build_neutral(f["_src"].read_bytes()):
                f["sha256"] = old["sha256"]
                f["_src"] = old_obj
                kept += 1
    if kept:
        print(f"{kept} files differ from {previous['id']} only in build metadata and keep its contents")

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
        "client": client,
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
    assert serviced("BootPartition/freeldr.sys") and serviced("BootPartition/EFI/BOOT/bootx64.efi")
    assert not serviced("BootPartition/EFI/LiberNT/Boot/BCD") and not serviced("BootPartition/config.txt")
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
        cdir = tmp / "client"
        cdir.mkdir()
        for name in CLIENT_FILES:
            (cdir / name).write_bytes(b"client " + name.encode())
        (cdir / "VERSION").write_text("v9.9.9\n")
        e4 = publish_tree(tree, feed, key, "c" * 40, now=t0 + dt.timedelta(hours=3), client=add_client(tree, cdir))
        assert e4["client"] == "v9.9.9" and e4["changedFromPrevious"] == 2, e4
        m4 = json.loads((feed / e4["manifest"]).read_text("utf-8"))
        assert any(f["path"] == "Program Files/LiberNT Update/LiberNTUpdate.exe" for f in m4["files"])
        # 5 contents from the first two releases plus the two client files
        assert len(list((feed / "objects").rglob("*"))) - len(list((feed / "objects").iterdir())) == 7
        assert verify_feed(feed, pub) == 0

        def fake_pe(stamp: int, checksum: int, tag: bytes, code: bytes) -> bytes:
            mz = b"MZ" + bytes(0x3A) + struct.pack("<I", 0x40)
            coff = struct.pack("<HHIIIHH", 0x8664, 0, stamp, 0, 0, 0xF0, 0x22)
            opt = bytearray(0xF0)
            struct.pack_into("<H", opt, 0, 0x20B)
            struct.pack_into("<I", opt, 64, checksum)
            return mz + b"PE\0\0" + coff + bytes(opt) + code + b"LiberNT (Build " + tag + b")"

        pe_path = tree / "Windows/system32/tool.exe"
        pe_path.write_bytes(fake_pe(1, 0x1111, b"20261103-nt10.0-044-1374-g320a592", b"\x90" * 64))
        e5 = publish_tree(tree, feed, key, "d" * 40, now=t0 + dt.timedelta(hours=4), client="v9.9.9")
        sha5 = {f["path"]: f["sha256"] for f in json.loads((feed / e5["manifest"]).read_text("utf-8"))["files"]}
        pe_path.write_bytes(fake_pe(2, 0x2222, b"20261104-nt10.0-044-1380-gcd66369", b"\x90" * 64))
        e6 = publish_tree(tree, feed, key, "e" * 40, now=t0 + dt.timedelta(hours=5), client="v9.9.9")
        assert e6 is e5 or e6["id"] == e5["id"], "a rebuild that changes only build metadata must not become a release"
        pe_path.write_bytes(fake_pe(3, 0x3333, b"20261105-nt10.0-044-1390-g0a41bd8", b"\x90" * 63 + b"\xc3"))
        e7 = publish_tree(tree, feed, key, "f" * 40, now=t0 + dt.timedelta(hours=6), client="v9.9.9")
        sha7 = {f["path"]: f["sha256"] for f in json.loads((feed / e7["manifest"]).read_text("utf-8"))["files"]}
        assert e7["changedFromPrevious"] == 1 and sha7["Windows/system32/tool.exe"] != sha5["Windows/system32/tool.exe"], e7
        assert build_neutral(b"caroots: " + b"a" * 40) != build_neutral(b"caroots: " + b"b" * 40), "hashes in text files count"
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
    p.add_argument("--client", type=Path, help="unpacked client release (LiberNTUpdate.exe, luupboot.exe, VERSION)")
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
            client = add_client(a.tree, a.client) if a.client else None
            publish_tree(a.tree, a.feed, key, a.commit, a.repo, a.notes, client=client)
        else:
            with tempfile.TemporaryDirectory(dir=a.feed.parent) as work:
                tree = extract_image(a.img, Path(work))
                client = add_client(tree, a.client) if a.client else None
                publish_tree(tree, a.feed, key, a.commit, a.repo, a.notes, client=client)
        return 0
    if a.cmd == "verify":
        return verify_feed(a.feed, a.pub)
    return self_test()


if __name__ == "__main__":
    sys.exit(main())
