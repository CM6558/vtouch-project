#!/usr/bin/env python3
"""Fail before APK packaging if a core/launcher does not match its pairing manifest."""
import hashlib
from pathlib import Path
import re
import sys


def check(directory):
    root = Path(directory)
    props = dict(line.split("=", 1) for line in (root / "core.properties").read_text().splitlines() if "=" in line)
    if props.get("protocol") != "1" or props.get("shm") != "18" or props.get("abi") != "arm64-v8a":
        raise ValueError("unsupported paired core protocol / ABI")
    if not re.fullmatch(r"[a-f0-9]{24}", props.get("build_id", "")):
        raise ValueError("unpaired or invalid core build ID")
    for key, name in [("core", "vtouchd_ui"), ("launcher", "vtouch.sh")]:
        data = (root / name).read_bytes()
        if len(data) != int(props[key + "_size"]) or hashlib.sha256(data).hexdigest() != props[key + "_sha256"]:
            raise ValueError("core bundle mismatch: " + name)
    core = (root / "vtouchd_ui").read_bytes()
    if props["build_id"].encode() not in core:
        raise ValueError("core binary does not contain paired identity")
    print("Paired core verified:", props["build_id"])


if __name__ == "__main__":
    check(sys.argv[1])
