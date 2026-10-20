import hashlib
import importlib.util
import json
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
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

    def test_release_json_names_the_whole_file(self):
        data = bytes(pack({"index.json": b"{}"}))
        self.assertEqual(publish_assets.release_json(data),
                         {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)})


if __name__ == "__main__":
    unittest.main()
