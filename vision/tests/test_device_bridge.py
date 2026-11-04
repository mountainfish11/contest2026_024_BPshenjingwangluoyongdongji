import unittest

import numpy as np

from vision_assist.device_bridge import decode_rgb565


class TestRgb565Decode(unittest.TestCase):
    def test_primary_colors(self):
        raw = bytes((0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00))
        image = decode_rgb565(raw, 3, 1)
        np.testing.assert_array_equal(
            image[0],
            np.array([[0, 0, 255], [0, 255, 0], [255, 0, 0]],
                     dtype=np.uint8),
        )

    def test_rejects_wrong_size(self):
        with self.assertRaises(ValueError):
            decode_rgb565(b"\x00\x00", 2, 2)


if __name__ == "__main__":
    unittest.main()
