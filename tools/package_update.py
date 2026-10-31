#!/usr/bin/env python3
"""Build deterministic, data-only PenMods update archives and catalog metadata."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from urllib.parse import urlsplit
import zipfile

MAX_ARCHIVE = 256 * 1024 * 1024
MAX_UNPACKED = 512 * 1024 * 1024
MAX_FILE = 192 * 1024 * 1024


def fail(message):
    raise ValueError(message)


def version(value):
    if not re.fullmatch(r"[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}", value):
        raise argparse.ArgumentTypeError("version must be X.Y.Z, each component 0..999")
    return ".".join(str(int(part)) for part in value.split("."))


def regular_files(source):
    if source.is_symlink() or not source.is_dir():
        fail("source must be a directory, not a symlink")
    result = {}
    for path in source.rglob("*"):
        if path.is_symlink():
            fail(f"symlink input is forbidden: {path}")
        if path.is_dir():
            continue
        if not stat.S_ISREG(path.stat().st_mode):
            fail(f"non-regular input is forbidden: {path}")
        name = path.relative_to(source).as_posix()
        if "\\" in name or any(p in ("", ".", "..") or ":" in p for p in name.split("/")):
            fail(f"unsafe archive path: {name}")
        result[name] = path
    return result


def package_files(source, component):
    files = regular_files(source)
    if component == "core":
        required = {"libPenMods.so", "libPenModsResources.so"}
        allowed = required
    elif component == "player":
        required = {"FFmpegPlayer/libffmpegplayerplugin.so", "FFmpegPlayer/qmldir"}
        allowed = required
    elif component == "mathjax":
        required = {"mathjax-server"}
        allowed = required
    else:
        required = {"libyocr.so"}
        required |= {"models/PP_OCRv5_mobile_rec.ncnn.param", "models/PP_OCRv5_mobile_rec.ncnn.bin"}
        models = {name for name in files if re.fullmatch(r"models/[A-Za-z0-9_.-]+\.ncnn\.(param|bin)", name)}
        params = {name[:-6] for name in models if name.endswith(".param")}
        bins = {name[:-4] for name in models if name.endswith(".bin")}
        if not params or params != bins:
            fail("ppocr requires matching models/*.ncnn.param and models/*.ncnn.bin pairs")
        allowed = required | models
    if not required <= files.keys():
        fail("missing required files: " + ", ".join(sorted(required - files.keys())))
    # Only the defined payload is copied; build directories may contain unrelated files.
    return [(name, files[name]) for name in sorted(allowed)]


def validate_requirements(path):
    if path is None:
        return {}
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("requires must be a JSON object")
    for component, bounds in value.items():
        if component not in {"core", "player", "mathjax", "ppocr"} or not isinstance(bounds, dict):
            fail("invalid component requirement")
        if not bounds or set(bounds) - {"min", "max_exclusive"}:
            fail("requirement supports min and max_exclusive only")
        for bound in bounds.values():
            version(bound)
    return value


def validate_elf(path, library):
    with path.open("rb") as stream:
        header = stream.read(64)
    if (len(header) < 64 or header[:4] != b"\x7fELF" or header[4:6] != b"\x02\x01"
            or header[18:20] != b"\xb7\x00" or header[17] != 0
            or header[16] not in ((3,) if library else (2, 3))):
        fail(f"payload must be little-endian aarch64 ELF: {path}")


def create_package(args):
    url = urlsplit(args.download)
    if (url.scheme != "https" or not url.hostname or url.username or url.password
            or url.fragment or len(args.download) > 2048):
        fail("download must be an HTTPS URL without credentials or fragment")
    entries = package_files(args.source, args.component)
    for name, path in entries:
        if name.endswith(".so") or name == "mathjax-server":
            validate_elf(path, name.endswith(".so"))
    if any(path.stat().st_size > MAX_FILE for _, path in entries):
        fail("each payload file must not exceed 192 MiB")
    unpacked = sum(path.stat().st_size for _, path in entries)
    if not 0 < unpacked <= MAX_UNPACKED:
        fail("unpacked payload must be 1 byte through 512 MiB")
    requires = validate_requirements(args.requires)
    if args.output.resolve() in {path.resolve() for _, path in entries}:
        fail("output cannot overwrite a package input")
    protected = {path.resolve() for _, path in entries} | {args.output.resolve()}
    if args.descriptor and args.descriptor.resolve() in protected:
        fail("descriptor cannot overwrite archive or package input")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".penmods-package-", dir=args.output.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, path in entries:
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                mode = 0o755 if name == "mathjax-server" else 0o644
                info.external_attr = (stat.S_IFREG | mode) << 16
                with path.open("rb") as source, archive.open(info, "w") as target:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        target.write(chunk)
        size = Path(temporary).stat().st_size
        if not 0 < size <= MAX_ARCHIVE:
            fail("archive must not exceed 256 MiB")
        digest = hashlib.sha256()
        md5 = hashlib.md5()
        with open(temporary, "rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
                md5.update(chunk)
        metadata = {
            "id": args.component, "version": [int(v) for v in args.version.split(".")],
            "size": size, "unpacked_size": unpacked, "sha256": digest.hexdigest(),
            "download": args.download, "model": args.model, "arch": "aarch64",
            "channel": args.channel, "note": args.note,
        }
        if args.component == "core":
            metadata["health_protocol"] = 1
            metadata["md5"] = md5.hexdigest()  # The legacy versions list still validates this field.
        if requires:
            metadata["requires"] = requires
        os.replace(temporary, args.output)
        if args.descriptor:
            args.descriptor.parent.mkdir(parents=True, exist_ok=True)
            native = dict(metadata, version=args.version)
            args.descriptor.write_text(json.dumps(native, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return metadata
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


# The outer archive is exclusively for old clients' first migration. New clients
# consume the normal inner ZIP and never execute this compatibility script.
BOOTSTRAP_SCRIPT = """#!/bin/sh
set -eu
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$package_dir"
rm -f INSTALL_SUCCESSFULLY
prefix=${PENMODS_BOOTSTRAP_PREFIX:-}
case "$prefix" in ''|/*) ;; *) echo "bootstrap prefix must be absolute" >&2; exit 1 ;; esac
module_dir=$prefix/userdata/PenMods
root=$prefix/userdisk/PenMods/update-system
helper=$module_dir/penmods-update
snapshot_mode=--snapshot-only
reenabling=0
if [ -e "$root/disabled" ]; then
    snapshot_mode=--reinstall-snapshot
    reenabling=1
fi
mkdir -p "$module_dir"
temporary=$helper.new.$$
[ ! -e "$temporary" ] && [ ! -L "$temporary" ] || { echo "bootstrap scratch file already exists" >&2; exit 1; }
trap 'rm -f "$temporary"' EXIT HUP INT TERM
cp penmods-update "$temporary"
chmod 755 "$temporary"
sync
mv -f "$temporary" "$helper"
sync
# Snapshot first. Neither imports nor stage may replace old app aliases.
if [ -n "$prefix" ]; then
    "$helper" --root "$root" --prefix "$prefix" --core-version 0.0.0 init "$snapshot_mode"
    "$helper" --root "$root" --prefix "$prefix" stage "$package_dir/core-descriptor.json" "$package_dir/core-package.zip"
    sh "$package_dir/install-launcher.sh" --rootfs-prefix "$prefix"
else
    "$helper" --root "$root" --core-version 0.0.0 init "$snapshot_mode"
    "$helper" --root "$root" stage "$package_dir/core-descriptor.json" "$package_dir/core-package.zip"
    mount -o remount,rw /
    sh "$package_dir/install-launcher.sh"
fi
if [ "$reenabling" = 1 ]; then
    if [ -n "$prefix" ]; then
        "$helper" --root "$root" --prefix "$prefix" enable
    else
        "$helper" --root "$root" enable
    fi
fi
sync
touch INSTALL_SUCCESSFULLY
trap - EXIT HUP INT TERM
"""


def create_bootstrap(args, inner):
    values = (args.bootstrap_helper, args.bootstrap_installer, args.bootstrap_output, args.bootstrap_download)
    if not any(values) and not args.bootstrap_descriptor:
        return None
    if not all(values) or args.component != "core":
        fail("bootstrap requires core plus --bootstrap-helper/installer/output/download")
    paths = (args.bootstrap_helper, args.bootstrap_installer)
    for path in paths:
        if path.is_symlink() or not path.is_file() or not stat.S_ISREG(path.stat().st_mode):
            fail(f"bootstrap input must be a regular file: {path}")
    if args.bootstrap_output.resolve() in {args.output.resolve(), *(p.resolve() for p in paths)}:
        fail("bootstrap output must be distinct from normal archive and inputs")
    protected = {args.output.resolve(), args.bootstrap_output.resolve(), *(p.resolve() for p in paths)}
    if args.bootstrap_descriptor and args.bootstrap_descriptor.resolve() in protected:
        fail("bootstrap descriptor cannot overwrite an archive or input")
    url = urlsplit(args.bootstrap_download)
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
        fail("bootstrap download must be HTTPS without credentials or fragment")
    native = dict(inner)
    native["version"] = args.version
    payloads = {
        "_do_update.sh": BOOTSTRAP_SCRIPT.encode(),
        "penmods-update": args.bootstrap_helper.read_bytes(),
        "install-launcher.sh": args.bootstrap_installer.read_bytes(),
        "core-package.zip": args.output.read_bytes(),
        "core-descriptor.json": (json.dumps(native, sort_keys=True) + "\n").encode(),
    }
    args.bootstrap_output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".penmods-bootstrap-", dir=args.bootstrap_output.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, payload in sorted(payloads.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (stat.S_IFREG | (0o755 if name.endswith(".sh") or name == "penmods-update" else 0o644)) << 16
                archive.writestr(info, payload)
        payload = Path(temporary).read_bytes()
        if len(payload) > MAX_ARCHIVE:
            fail("bootstrap archive exceeds 256 MiB")
        result = {"version": inner["version"], "size": len(payload), "md5": hashlib.md5(payload).hexdigest(),
                  "sha256": hashlib.sha256(payload).hexdigest(), "download": args.bootstrap_download,
                  "note": args.note, "bootstrap": True}
        os.replace(temporary, args.bootstrap_output)
        if args.bootstrap_descriptor:
            args.bootstrap_descriptor.parent.mkdir(parents=True, exist_ok=True)
            args.bootstrap_descriptor.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", required=True, choices=("core", "player", "mathjax", "ppocr"))
    parser.add_argument("--version", required=True, type=version)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--descriptor", type=Path, help="save native stage descriptor JSON; stdout uses catalog version arrays")
    parser.add_argument("--download", required=True)
    parser.add_argument("--model", required=True, choices=("YDP02X", "YDPG3", "YDP03X"))
    parser.add_argument("--arch", default="aarch64", choices=("aarch64",))
    parser.add_argument("--channel", required=True, choices=("dev", "canary", "beta", "stable"))
    parser.add_argument("--note", default="")
    parser.add_argument("--requires", type=Path, help="JSON object with component min/max_exclusive bounds")
    parser.add_argument("--bootstrap-helper", type=Path)
    parser.add_argument("--bootstrap-installer", type=Path)
    parser.add_argument("--bootstrap-output", type=Path)
    parser.add_argument("--bootstrap-download")
    parser.add_argument("--bootstrap-descriptor", type=Path)
    args = parser.parse_args()
    try:
        normal = create_package(args)
        bootstrap = create_bootstrap(args, normal)
        result = {"package": normal, "bootstrap": bootstrap} if bootstrap else normal
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    except (OSError, ValueError, argparse.ArgumentTypeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
