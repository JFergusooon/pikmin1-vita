import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
import import_iso


def fake_disc(game_id=b"GPIE01", revision=1):
    image = bytearray(0x604)
    image[:6] = game_id
    image[7] = revision
    image[0x1C:0x20] = import_iso.GAMECUBE_MAGIC
    image[0x20:0x26] = b"Pikmin"

    names = b"hello.bin\0"
    fst = (
        struct.pack(">III", 0x01000000, 0, 2)
        + struct.pack(">III", 0, 0x600, 4)
        + names
    )
    struct.pack_into(">II", image, 0x424, 0x500, len(fst))
    image[0x500 : 0x500 + len(fst)] = fst
    image[0x600:0x604] = b"piki"
    return io.BytesIO(image)


class ImportIsoTest(unittest.TestCase):
    def test_inspects_rev_one_disc(self):
        info = import_iso.inspect_disc(fake_disc())
        self.assertEqual(info.game_id, "GPIE01")
        self.assertEqual(info.revision, 1)
        self.assertEqual(info.title, "Pikmin")

    def test_rejects_non_us_disc(self):
        with self.assertRaises(import_iso.ImportError):
            import_iso.inspect_disc(fake_disc(b"GPIJ01"))

    def test_extracts_files_and_manifest(self):
        image = fake_disc()
        info = import_iso.inspect_disc(image)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            count = import_iso.extract_files(image, info, output)
            import_iso.write_manifest(output, info, count)
            self.assertEqual((output / "hello.bin").read_bytes(), b"piki")
            self.assertTrue((output / ".pikmin-vita.json").is_file())


if __name__ == "__main__":
    unittest.main()
