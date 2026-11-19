#!/usr/bin/env python3
"""
OTA-Forever build step (runs in GitHub Actions, also works locally).

Reads ota.yml, compiles the sketch for each board with arduino-cli and writes
dist/ (firmware-<chip>-b<build>.bin + manifest.json).

Signing is optional:
  - no OTA_SIGNING_KEY secret: devices trust the build because it comes over
    HTTPS from this GitHub repo (SHA-256 still checked).
  - with OTA_SIGNING_KEY: each binary is signed (ECDSA P-256 over
    "otaf1|<chip>|<build>|<sha256 hex>") and devices that pinned the key
    refuse anything else. See loader/main/updater.c.
"""
import base64
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import yaml
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / "dist"
BUILD_DIR = ROOT / "build"
ESP32_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
USER_SLOT_SIZE = 0x160000  # app0/app1 size in partitions.csv

# chip name (= ESP-IDF target) -> chip_id in the ESP image header
CHIP_IDS = {"esp32": 0x0000, "esp32s2": 0x0002, "esp32s3": 0x0009, "esp32c3": 0x0005, "esp32c6": 0x000D}

# `boards: auto` builds every supported chip with its generic Arduino board
AUTO_BOARDS = [{"chip": chip, "fqbn": f"esp32:esp32:{chip}"} for chip in CHIP_IDS]


class BoardError(Exception):
    pass


def fail(msg):
    print(f"::error::{msg}", flush=True)
    sys.exit(1)


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def set_output(**kv):
    path = os.environ.get("GITHUB_OUTPUT")
    lines = "".join(f"{k}={v}\n" for k, v in kv.items())
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(lines)
    print(lines, end="")


def channel_for(branch):
    return re.sub(r"[^A-Za-z0-9._-]", "-", branch)


def load_signing_key():
    pem = os.environ.get("OTA_SIGNING_KEY", "").strip()
    if not pem:
        print("::notice::Unsigned build (no OTA_SIGNING_KEY secret). Devices trust it because it comes "
              "from this repo over HTTPS. To sign builds, see README > Optional: signed builds.")
        return None
    try:
        key = serialization.load_pem_private_key(pem.encode(), password=None)
    except ValueError as e:
        fail(f"OTA_SIGNING_KEY is not a valid PEM private key: {e}")
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        fail("OTA_SIGNING_KEY must be an EC P-256 (prime256v1) key; create one with keygen.py")
    return key


def check_image(data, chip):
    if len(data) < 16 or data[0] != 0xE9:
        raise BoardError("output is not an ESP application image")
    chip_id = data[12] | (data[13] << 8)
    if chip_id != CHIP_IDS[chip]:
        name = next((k for k, v in CHIP_IDS.items() if v == chip_id), hex(chip_id))
        raise BoardError(f"the fqbn builds for {name}, not {chip}. Fix 'chip' or 'fqbn' in ota.yml")
    if len(data) > USER_SLOT_SIZE:
        raise BoardError(f"firmware is {len(data)} bytes, the OTA slot holds {USER_SLOT_SIZE}")


def read_boards(cfg):
    boards = cfg.get("boards", "auto")
    if boards in (None, "auto", ["auto"]):
        return AUTO_BOARDS, True
    if not isinstance(boards, list) or not boards:
        fail("ota.yml 'boards' must be 'auto' or a list of {chip, fqbn}")
    for b in boards:
        if b.get("chip") not in CHIP_IDS:
            fail(f"Unsupported chip '{b.get('chip')}'. Use one of: {', '.join(CHIP_IDS)}")
        if not b.get("fqbn"):
            fail(f"Board '{b['chip']}' needs an fqbn")
    if len({b["chip"] for b in boards}) != len(boards):
        fail("Each chip may appear only once in ota.yml boards")
    return boards, False


def compile_board(board, sketch):
    chip, fqbn = board["chip"], board["fqbn"]
    out = BUILD_DIR / chip
    cmd = ["arduino-cli", "compile", "--fqbn", fqbn, "--output-dir", str(out)]
    if (ROOT / "libraries").is_dir():
        cmd += ["--libraries", str(ROOT / "libraries")]
    for prop in board.get("build_properties") or []:
        cmd += ["--build-property", str(prop)]
    cmd.append(str(sketch))
    try:
        run(cmd)
    except subprocess.CalledProcessError:
        raise BoardError("compile failed (see the log above)")
    data = (out / f"{sketch.name}.ino.bin").read_bytes()
    check_image(data, chip)
    return data


def main():
    cfg = yaml.safe_load((ROOT / "ota.yml").read_text(encoding="utf-8")) or {}
    branch = os.environ.get("GITHUB_REF_NAME", "main")
    allowed = cfg.get("branches") or ["*"]
    if "*" not in allowed and branch not in allowed:
        print(f"Branch '{branch}' is not listed in ota.yml 'branches'; nothing to publish.")
        set_output(publish="false")
        return

    channel = channel_for(branch)
    build = int(os.environ.get("GITHUB_RUN_NUMBER", "1"))
    repo = os.environ.get("GITHUB_REPOSITORY", "local/test")
    key = load_signing_key()

    sketch = ROOT / cfg.get("sketch", "firmware")
    if not (sketch / f"{sketch.name}.ino").exists():
        fail(f"Sketch not found: {sketch.name}/{sketch.name}.ino")
    if not (sketch / "partitions.csv").exists():
        fail(f"{sketch.name}/partitions.csv is missing (copy it from the OTA-Forever template)")
    boards, auto = read_boards(cfg)

    # Toolchain
    core = "esp32:esp32" + (f"@{cfg['core_version']}" if cfg.get("core_version") else "")
    run(["arduino-cli", "core", "update-index", "--additional-urls", ESP32_INDEX])
    run(["arduino-cli", "core", "install", core, "--additional-urls", ESP32_INDEX])
    for lib in cfg.get("libraries") or []:
        run(["arduino-cli", "lib", "install", str(lib)])

    shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir(parents=True)

    targets, skipped = {}, []
    for board in boards:
        chip = board["chip"]
        print(f"::group::Build for {chip}", flush=True)
        try:
            data = compile_board(board, sketch)
        except BoardError as e:
            print("::endgroup::", flush=True)
            if not auto:
                fail(f"{chip}: {e}")
            print(f"::warning::Skipping {chip}: {e}", flush=True)
            skipped.append(chip)
            continue
        print("::endgroup::", flush=True)

        sha = hashlib.sha256(data).hexdigest()
        name = f"firmware-{chip}-b{build}.bin"
        (DIST / name).write_bytes(data)
        entry = {
            "url": f"https://github.com/{repo}/releases/download/ota-{channel}/{name}",
            "size": len(data),
            "sha256": sha,
            "fqbn": board["fqbn"],
        }
        if key:
            message = f"otaf1|{chip}|{build}|{sha}".encode()
            entry["sig"] = base64.b64encode(key.sign(message, ec.ECDSA(hashes.SHA256()))).decode()
        targets[chip] = entry
        print(f"{chip}: {len(data)} bytes ({len(data) * 100 // USER_SLOT_SIZE}% of slot)", flush=True)

    if not targets:
        fail("No board compiled successfully")

    pubkey = None
    if key:
        pubkey = key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    manifest = {
        "schema": 1,
        "repo": repo,
        "channel": channel,
        "version": str(cfg.get("version", "")),
        "build": build,
        "commit": os.environ.get("GITHUB_SHA", ""),
        "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "pubkey": pubkey,
        "targets": targets,
    }
    (DIST / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Build #{build} for channel '{channel}': {', '.join(targets)}"
          + (f" (skipped: {', '.join(skipped)})" if skipped else "")
          + (" [signed]" if key else " [unsigned]"))
    set_output(publish="true", channel=channel, build=build,
               chips=" ".join(targets), skipped=" ".join(skipped), signed=str(bool(key)).lower())


if __name__ == "__main__":
    main()
