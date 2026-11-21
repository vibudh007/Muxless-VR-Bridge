#!/usr/bin/env python3
"""Verify and optionally stage the pinned portable build inputs."""
import argparse
import hashlib
import io
import json
import tarfile
import urllib.request
from pathlib import Path


def verify(data, expected, label):
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f"Build input checksum mismatch: {label}")


def fetch(spec):
    with urllib.request.urlopen(spec["url"], timeout=30) as response:
        data = response.read()
    verify(data, spec["sha256"], spec["root"])
    return tarfile.open(fileobj=io.BytesIO(data), mode="r:*")


def read(archive, root, relative):
    member = archive.getmember(f"{root}/{relative}")
    if not member.isfile():
        raise SystemExit(f"Expected a regular file in archive: {member.name}")
    with archive.extractfile(member) as source:
        return source.read()


def stage(directory, lock):
    licenses = directory / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)

    spec = lock["vulkan_headers"]
    with fetch(spec) as archive:
        root = spec["root"]
        for member in archive.getmembers():
            if not member.name.startswith((f"{root}/include/vulkan/", f"{root}/include/vk_video/")):
                continue
            if not member.name.endswith(".h") or not member.isfile():
                continue
            relative = member.name[len(root) + 1:]
            if ".." in Path(relative).parts:
                raise SystemExit(f"Invalid header path in archive: {member.name}")
            path = directory / "vulkan-headers" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(read(archive, root, relative))
        for relative, name in (("LICENSE.md", "COPYING.vulkan-headers"),
                               ("LICENSES/MIT.txt", "COPYING.vulkan-headers.MIT"),
                               ("LICENSES/Apache-2.0.txt", "COPYING.vulkan-headers.Apache-2.0")):
            (licenses / name).write_bytes(read(archive, root, relative))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, help="download and verify pinned archive inputs here")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root / "shim/appimage/toolchain-lock.json").read_text())
    for relative, expected in lock["third_party"].items():
        verify((root / relative).read_bytes(), expected, relative)
    container = (root / "shim/appimage/Containerfile").read_text().splitlines()
    if "FROM " + lock["base_image"] not in container:
        raise SystemExit("Container base does not match toolchain-lock.json")
    if args.stage:
        stage(args.stage, lock)
    print("PASS: portable build inputs match the lock file.")


if __name__ == "__main__":
    main()
