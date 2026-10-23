import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import pack_assets  # noqa: E402
from build_default_assets import pack_assets_entries, read_assets_entries  # noqa: E402


GIF = b"GIF89a" + struct.pack("<HH", 128, 64) + b"rest"
INDEX = {
    "text_font": "font.bin",
    "srmodels": "srmodels.bin",
    "multinet_model": {"threshold": 0.4, "commands": [
        {"command": "xiao si tong xue", "text": "小司同学", "action": "wake"}]},
    "emoji_collection": [{"name": "happy", "file": "happy.gif"}],
}


class PackAssetsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def generator_pack(self):
        index = json.dumps(INDEX, ensure_ascii=False, indent=2).encode()
        files = {"font.bin": (b"font", 0, 0), "srmodels.bin": (b"models", 0, 0),
                 "angry.gif": (GIF, 128, 64), "happy.gif": (GIF, 128, 64),
                 "index.json": (index, 0, 0)}
        path = self.root / "assets.bin"
        path.write_bytes(pack_assets_entries(files))
        return path

    def test_unchanged_folder_packs_to_same_bytes(self):
        pack = self.generator_pack()
        pack_assets.unpack_pack(pack, self.root / "folder")
        self.assertEqual(pack_assets.pack_folder(self.root / "folder"), pack.read_bytes())

    def test_edited_index_is_packed_with_new_length(self):
        pack_assets.unpack_pack(self.generator_pack(), self.root / "folder")
        index = dict(INDEX, multinet_model=dict(INDEX["multinet_model"], threshold=0.35))
        (self.root / "folder/index.json").write_text(json.dumps(index, ensure_ascii=False),
                                                     encoding="utf-8")
        (self.root / "folder/.DS_Store").write_bytes(b"junk")
        files = read_assets_entries(self.write(pack_assets.pack_folder(self.root / "folder")))
        self.assertEqual(json.loads(files["index.json"][0])["multinet_model"]["threshold"], 0.35)
        self.assertNotIn(".DS_Store", files)
        self.assertEqual(files["angry.gif"][1:], (128, 64))

    def test_rejects_missing_referenced_file(self):
        pack_assets.unpack_pack(self.generator_pack(), self.root / "folder")
        (self.root / "folder/happy.gif").unlink()
        with self.assertRaisesRegex(ValueError, "happy.gif"):
            pack_assets.pack_folder(self.root / "folder")

    def test_rejects_broken_index(self):
        pack_assets.unpack_pack(self.generator_pack(), self.root / "folder")
        (self.root / "folder/index.json").write_text('{"threshold": 0.4,}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "JSON"):
            pack_assets.pack_folder(self.root / "folder")

    def test_refuses_to_unpack_over_files(self):
        (self.root / "folder").mkdir()
        (self.root / "folder/old.gif").write_bytes(GIF)
        with self.assertRaisesRegex(ValueError, "空目录"):
            pack_assets.unpack_pack(self.generator_pack(), self.root / "folder")

    def write(self, data):
        path = self.root / "packed.bin"
        path.write_bytes(data)
        return path


if __name__ == "__main__":
    unittest.main()
