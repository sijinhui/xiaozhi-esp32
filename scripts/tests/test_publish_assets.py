import hashlib
import importlib.util
import json
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("publish_assets", ROOT / "scripts/publish_assets.py")
publish_assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish_assets)


def pack(files):
    table, data = bytearray(), bytearray()
    for name, contents in files.items():
        table.extend(struct.pack("<32sIIHH", name.encode(), len(contents), len(data), 0, 0))
        data.extend(b"ZZ" + contents)
    payload = table + data
    return bytearray(struct.pack("<III", len(files), sum(payload) & 0xFFFF, len(payload)) + payload)


class PublishAssetsTests(unittest.TestCase):
    INDEX = {"multinet_model": {"commands": [
        {"command": "xiao si tong xue", "text": "小司同学", "action": "wake"}]}}

    def test_reads_index_of_valid_pack(self):
        data = pack({"font.bin": b"font", "index.json": json.dumps(self.INDEX).encode()})
        self.assertEqual(publish_assets.describe_pack(bytes(data)), self.INDEX)

    def test_rejects_stale_checksum(self):
        data = pack({"index.json": json.dumps(self.INDEX).encode()})
        data[-2] ^= 1
        with self.assertRaisesRegex(ValueError, "校验和"):
            publish_assets.describe_pack(bytes(data))

    def test_rejects_truncated_pack(self):
        data = pack({"index.json": json.dumps(self.INDEX).encode()})
        with self.assertRaisesRegex(ValueError, "长度"):
            publish_assets.describe_pack(bytes(data[:-1]))

    def test_repo_packs_are_valid_and_fit_their_partition(self):
        # assets partition of each board's partition table (partitions/v2)
        limits = {publish_assets.DEFAULT_BOARD: 16 * 1024 * 1024, "lichuang-dev": 8 * 1024 * 1024}
        self.assertEqual(set(publish_assets.BOARD_PACKS), set(limits))
        for board, folder in publish_assets.BOARD_PACKS.items():
            with self.subTest(board=board):
                data = publish_assets.read_pack(folder)
                index = publish_assets.describe_pack(data)
                self.assertTrue(index["multinet_model"]["commands"])
                self.assertLessEqual(len(data), limits[board])

    def test_release_json_names_the_whole_file(self):
        data = bytes(pack({"index.json": b"{}"}))
        self.assertEqual(publish_assets.release_json(data),
                         {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)})


if __name__ == "__main__":
    unittest.main()
