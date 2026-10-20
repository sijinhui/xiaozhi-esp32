#!/usr/bin/env python3
"""Upload an assets pack to the "assets" GitHub pre-release for boards to download.

Boards built with CONFIG_GITHUB_OTA_ENABLE read <board>-assets.json from that release
on boot and install <board>-assets.bin once for every new pack (main/ota.cc
CheckGithubAssets). Needs a logged-in gh.

    python scripts/publish_assets.py assets.bin
    python scripts/publish_assets.py assets.bin --board lichuang-dev
"""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


TAG = "assets"
DEFAULT_BOARD = "esp32-s3-touch-amoled-1.75c-32m"
DEFAULT_REPOSITORY = "sijinhui/xiaozhi-esp32"
HEADER_SIZE = 12
TABLE_ENTRY_SIZE = 44


def describe_pack(data):
    """Check the pack the way the firmware does and return its index.json."""
    if len(data) < HEADER_SIZE:
        raise ValueError("文件太小，不是资源包")
    files, checksum, length = struct.unpack_from("<III", data)
    if length != len(data) - HEADER_SIZE:
        raise ValueError(f"头部记录的长度 {length} 和文件大小 {len(data)} - 12 对不上")
    if sum(data[HEADER_SIZE:]) & 0xFFFF != checksum:
        raise ValueError("校验和不对，板子会拒绝这个资源包（改过内容要重算偏移 4 的校验）")
    base = HEADER_SIZE + files * TABLE_ENTRY_SIZE
    for i in range(files):
        entry = HEADER_SIZE + i * TABLE_ENTRY_SIZE
        name = data[entry:entry + 32].split(b"\0")[0].decode()
        size, offset = struct.unpack_from("<II", data, entry + 32)
        if name == "index.json":
            start = base + offset
            if data[start:start + 2] != b"ZZ":
                raise ValueError("index.json 的数据头不是 ZZ")
            return json.loads(data[start + 2:start + 2 + size])
    raise ValueError("资源包里没有 index.json")


def release_json(data):
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}


def gh(*args, check=True):
    return subprocess.run(["gh", *args], check=check, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, help="资源包 .bin")
    parser.add_argument("--board", default=DEFAULT_BOARD, help="板子名（BOARD_NAME），默认 1.75C")
    parser.add_argument("--repository", default=DEFAULT_REPOSITORY)
    parser.add_argument("--yes", action="store_true", help="不再询问，直接上传")
    args = parser.parse_args()

    data = args.pack.read_bytes()
    index = describe_pack(data)
    meta = release_json(data)
    print(f"资源包 {args.pack.name}: {meta['size']} 字节, sha256 {meta['sha256']}")
    for command in index.get("multinet_model", {}).get("commands", []):
        print(f"  唤醒词 {command.get('command')} -> 显示 {command.get('text')}")
    threshold = index.get("multinet_model", {}).get("threshold")
    if threshold is not None:
        print(f"  阈值 {threshold}")
    print(f"将上传到 {args.repository} 的 {TAG} 预发布，文件名 {args.board}-assets.bin/.json")
    if not args.yes and input("确认上传？[y/N] ").strip().lower() != "y":
        return

    if gh("release", "view", TAG, "-R", args.repository, check=False).returncode:
        # Pre-releases never become "latest", which is where firmware updates come from
        gh("release", "create", TAG, "-R", args.repository, "--prerelease",
           "--title", "资源包", "--notes", "板子开机自动下载的资源包，用 scripts/publish_assets.py 上传。")

    with tempfile.TemporaryDirectory() as directory:
        pack = Path(directory) / f"{args.board}-assets.bin"
        info = Path(directory) / f"{args.board}-assets.json"
        shutil.copyfile(args.pack, pack)
        info.write_text(json.dumps(meta) + "\n", encoding="utf-8")
        # The json goes last, so a board never sees it before its pack is there
        gh("release", "upload", TAG, str(pack), "--clobber", "-R", args.repository)
        gh("release", "upload", TAG, str(info), "--clobber", "-R", args.repository)
    print("已上传。板子下次开机会下载这个资源包（约 10MB，需要一两分钟）。")


if __name__ == "__main__":
    main()
