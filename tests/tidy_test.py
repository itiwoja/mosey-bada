import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location("tidy", Path(__file__).resolve().parents[1] / "tools/fold5-airdrop/tidy.py")
tidy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tidy)


class TidyTest(unittest.TestCase):
    def test_paused_upload_keeps_archive_path_and_partial_members(self):
        with tempfile.TemporaryDirectory() as temp:
            staging, dest = Path(temp) / "staging", Path(temp) / "dest"
            staging.mkdir()
            raw = staging / "airdrop-received-123.dvzip"
            raw.write_bytes(b"unfinished upload")
            cpio = staging / "airdrop-received-124.cpio"
            cpio.write_bytes(b"unfinished cpio")
            wrapper = staging / "NSIRD_AirDrop_test"
            wrapper.mkdir()
            partial = wrapper / "photo.jpg.partial"
            partial.write_bytes(b"incomplete")
            photo = wrapper / "finished.jpg"
            photo.write_bytes(b"finished photo")
            for path in [raw, cpio, partial, photo, wrapper]:
                old = time.time() - 60
                os.utime(path, (old, old))
            with raw.open("ab") as upload:
                moved = tidy.tidy_once(str(staging), str(dest))
                self.assertEqual([Path(p).name for p in moved], ["finished.jpg"])
                self.assertTrue(raw.exists())
                upload.write(b" resumed")
            self.assertTrue(raw.read_bytes().endswith(b" resumed"))
            self.assertTrue(cpio.exists())
            self.assertTrue(partial.exists())
            self.assertEqual((dest / "finished.jpg").read_bytes(), b"finished photo")
            self.assertFalse((dest / raw.name).exists())

    def test_heic_and_jpeg_photos_are_both_delivered(self):
        # "High Efficiency" cameras send HEIC, "Most Compatible" ones JPEG
        with tempfile.TemporaryDirectory() as temp:
            staging, dest = Path(temp) / "staging", Path(temp) / "dest"
            wrapper = staging / "NSIRD_AirDrop_test"
            wrapper.mkdir(parents=True)
            names = ["IMG_0001.HEIC", "IMG_0002.heic", "IMG_0003.JPG"]
            for name in names:
                (wrapper / name).write_bytes(name.encode())
            for path in [*wrapper.iterdir(), wrapper]:
                old = time.time() - 60
                os.utime(path, (old, old))
            moved = tidy.tidy_once(str(staging), str(dest))
            self.assertEqual(sorted(Path(p).name for p in moved), sorted(names))
            for name in names:
                self.assertEqual((dest / name).read_bytes(), name.encode())


if __name__ == "__main__":
    unittest.main()
