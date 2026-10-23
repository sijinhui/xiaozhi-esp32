#!/usr/bin/env python3
"""Turn an assets pack into a folder and back, so its index.json can live in the repo.

    python scripts/pack_assets.py unpack assets.bin <folder>
    python scripts/pack_assets.py pack <folder> assets.bin

Every file in the folder goes into the pack. Packing uses the order of the
xiaozhi.me generator (text font, speech models, the rest by name, index.json
last), so an unchanged folder packs back to the same bytes.
"""

import argparse
import json
import struct
from pathlib import Path

from build_default_assets import pack_assets_entries, read_assets_entries


INDEX = "index.json"


def unpack_pack(pack, folder):
    folder = Path(folder)
    if folder.exists() and any(folder.iterdir()):
        raise ValueError(f"{folder} 不是空目录，先删掉再解包")
    folder.mkdir(parents=True, exist_ok=True)
    files = read_assets_entries(pack)
    for name, (data, _, _) in files.items():
        (folder / name).write_bytes(data)
    return list(files)


def image_size(data):
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return struct.unpack_from("<HH", data, 6)
    return 0, 0


def pack_folder(folder):
    folder = Path(folder)
    index_data = (folder / INDEX).read_bytes()
    try:
        index = json.loads(index_data)
    except ValueError as error:
        raise ValueError(f"{folder / INDEX} 不是合法的 JSON：{error}") from error

    names = sorted(path.name for path in folder.iterdir()
                   if path.is_file() and path.name != INDEX and not path.name.startswith("."))
    referenced = [index.get("text_font"), index.get("srmodels")]
    referenced += [emoji.get("file") for emoji in index.get("emoji_collection", [])]
    missing = [name for name in referenced if name and name not in names]
    if missing:
        raise ValueError(f"index.json 引用的文件不在 {folder} 里：{', '.join(missing)}")

    first = [name for name in referenced[:2] if name]
    files = {}
    for name in first + [name for name in names if name not in first]:
        data = (folder / name).read_bytes()
        files[name] = (data, *image_size(data))
    files[INDEX] = (index_data, 0, 0)
    return pack_assets_entries(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    unpack = commands.add_parser("unpack", help="把资源包 .bin 解到目录")
    unpack.add_argument("pack", type=Path)
    unpack.add_argument("folder", type=Path)
    pack = commands.add_parser("pack", help="把目录打成资源包 .bin")
    pack.add_argument("folder", type=Path)
    pack.add_argument("pack", type=Path)
    args = parser.parse_args()

    try:
        if args.command == "unpack":
            names = unpack_pack(args.pack, args.folder)
            print(f"已解出 {len(names)} 个文件到 {args.folder}")
        else:
            data = pack_folder(args.folder)
            args.pack.parent.mkdir(parents=True, exist_ok=True)
            args.pack.write_bytes(data)
            print(f"已打包 {args.pack}：{len(data)} 字节")
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
