#!/usr/bin/env python3
"""Fetch the large test datasets listed in datasets.json.

Data too large for git lives here. Nothing downloads automatically: tests skip
when a dataset is absent (see tests/testdatasets.h), so a fresh clone still runs
a green ctest without fetching anything.

    python3 tests/datasets/fetch.py                 # everything
    python3 tests/datasets/fetch.py redwood_rgbd_sample
    python3 tests/datasets/fetch.py --list
    python3 tests/datasets/fetch.py --verify        # re-check what is present
    python3 tests/datasets/fetch.py --ci-only       # only ciEligible entries

The sha256 is checked on every run, not only after downloading. An upstream file
that changes under a stable URL has to fail loudly here rather than quietly
altering a behavioural test's numbers.
"""

import argparse
import hashlib
import json
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "datasets.json"
ARCHIVES = HERE / ".archives"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load(selected, ci_only):
    entries = json.loads(MANIFEST.read_text(encoding="utf-8"))["datasets"]
    if selected:
        names = {e["name"] for e in entries}
        unknown = sorted(set(selected) - names)
        if unknown:
            sys.exit(f"unknown dataset(s): {', '.join(unknown)}\nknown: {', '.join(sorted(names))}")
        entries = [e for e in entries if e["name"] in selected]
    if ci_only:
        skipped = [e["name"] for e in entries if not e.get("ciEligible")]
        for name in skipped:
            print(f"  skipping {name}: not marked ciEligible")
        entries = [e for e in entries if e.get("ciEligible")]
    return entries


def download(entry, archive: Path) -> None:
    last = None
    for url in entry["urls"]:
        try:
            print(f"  downloading {url}")
            with urllib.request.urlopen(url) as response, \
                 tempfile.NamedTemporaryFile(dir=ARCHIVES, delete=False) as tmp:
                shutil.copyfileobj(response, tmp)
                staged = Path(tmp.name)
            staged.replace(archive)
            return
        except Exception as exc:  # noqa: BLE001 - any failure moves to the next mirror
            last = exc
            print(f"    failed: {exc}")
    raise SystemExit(f"could not download {entry['name']}: {last}")


def extract(entry, archive: Path, target: Path) -> None:
    if entry.get("archive") == "zip":
        staging = target.with_name(target.name + ".incoming")
        shutil.rmtree(staging, ignore_errors=True)
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(staging)
        shutil.rmtree(target, ignore_errors=True)
        staging.replace(target)
    else:
        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(archive, target / archive.name)


def process(entry, verify_only: bool) -> bool:
    name = entry["name"]
    target = HERE / name
    archive = ARCHIVES / f"{name}.{entry.get('archive', 'bin')}"
    print(f"{name}:")

    if entry["license"] == "UNRESOLVED":
        print("  ! licence unresolved - see licenseNote in datasets.json")

    if archive.exists():
        digest = sha256(archive)
        if digest != entry["sha256"]:
            print(f"  ! checksum mismatch\n      expected {entry['sha256']}\n      got      {digest}")
            if verify_only:
                return False
            print("  re-downloading")
            archive.unlink()
        elif target.is_dir():
            print(f"  ok ({entry['bytes']} bytes, verified)")
            return True
    elif verify_only:
        print("  absent")
        return True

    if verify_only:
        return True

    ARCHIVES.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        download(entry, archive)
        digest = sha256(archive)
        if digest != entry["sha256"]:
            archive.unlink(missing_ok=True)
            print(f"  ! checksum mismatch after download\n      expected {entry['sha256']}\n      got      {digest}")
            return False

    extract(entry, archive, target)
    print(f"  extracted to {target.relative_to(HERE.parent.parent)}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="datasets to fetch (default: all)")
    parser.add_argument("--list", action="store_true", help="list the manifest and exit")
    parser.add_argument("--verify", action="store_true", help="check what is present, download nothing")
    parser.add_argument("--ci-only", action="store_true", help="restrict to ciEligible entries")
    args = parser.parse_args()

    entries = load(args.names, args.ci_only)

    if args.list:
        for entry in entries:
            present = "present" if (HERE / entry["name"]).is_dir() else "absent"
            ci = "ci" if entry.get("ciEligible") else "local-only"
            print(f"{entry['name']:28s} {entry['bytes'] / 1e6:8.2f} MB  {present:8s} {ci:10s} {entry['license']}")
        return 0

    ok = all([process(entry, args.verify) for entry in entries])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
