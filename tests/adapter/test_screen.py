# SPDX-License-Identifier: MIT
"""Unit tests for the adapter's screen helpers (crop, zoom, PNG encoding); no harness needed."""

import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "adapter"))

from applewin_mcp import screen  # noqa: E402


def _decode_png(png):
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, {}
    while pos < len(png):
        (length,) = struct.unpack(">I", png[pos:pos + 4])
        chunks.setdefault(png[pos + 4:pos + 8], b"")
        chunks[png[pos + 4:pos + 8]] += png[pos + 8:pos + 8 + length]
        pos += 12 + length
    w, h, depth, ctype = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    raw = zlib.decompress(chunks[b"IDAT"])
    stride = w * 3
    rows = []
    for y in range(h):
        row = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        assert row[0] == 0
        rows.append(row[1:])
    return w, h, depth, ctype, b"".join(rows)


class ScreenTests(unittest.TestCase):
    def test_apple_pixels_samples_top_left_of_each_block(self):
        buf = bytearray(560 * 384 * 4)
        points = [(0, 0), (279, 191), (100, 50)]
        for i, j in points:
            off = ((2 * j) * 560 + 2 * i) * 4
            buf[off:off + 4] = bytes([i % 256, j, 7, 0])
        rgb = screen.apple_pixels(bytes(buf), 560, 384)
        for i, j in points:
            self.assertEqual(rgb[3 * (j * 280 + i):][:3], bytes([7, j, i % 256]))

    def test_apple_pixels_rejects_other_sizes(self):
        with self.assertRaisesRegex(screen.ScreenError, "unsupported video mode"):
            screen.apple_pixels(bytes(600 * 420 * 4), 600, 420)

    def test_region_outside_screen_is_rejected(self):
        for bad in ([270, 0, 20, 10], [0, 0, 0, 5], [0, 0, 5], [-1, 0, 5, 5]):
            with self.assertRaises(screen.ScreenError):
                screen.check_region(bad)
        self.assertEqual(screen.check_region(None), (0, 0, 280, 192))

    def test_crop_zoom_nearest_neighbour(self):
        buf = bytearray(280 * 192 * 3)
        buf[3 * (20 * 280 + 10):3 * (20 * 280 + 11)] = bytes([255, 0, 0])
        buf[3 * (20 * 280 + 11):3 * (20 * 280 + 12)] = bytes([1, 2, 3])
        w, h, rgb = screen.crop_zoom(bytes(buf), (10, 20, 2, 1), 3)
        self.assertEqual((w, h), (6, 3))
        for y in range(3):
            for x in range(6):
                expect = bytes([255, 0, 0]) if x < 3 else bytes([1, 2, 3])
                self.assertEqual(rgb[3 * (y * 6 + x):][:3], expect)
        with self.assertRaisesRegex(screen.ScreenError, "zoom must be 1 to 8"):
            screen.crop_zoom(bytes(buf), (0, 0, 1, 1), 9)

    def test_size_limit(self):
        screen.check_size(1120, 768)
        with self.assertRaises(screen.ScreenError) as cm:
            screen.check_size(1121, 768)
        self.assertIn("1121x768", str(cm.exception))
        self.assertIn("1120x768", str(cm.exception))

    def test_encode_png_round_trip(self):
        rgb = bytes(range(18))
        w, h, depth, ctype, pixels = _decode_png(screen.encode_png(3, 2, rgb))
        self.assertEqual((w, h, depth, ctype), (3, 2, 8, 2))
        self.assertEqual(pixels, rgb)

    def test_region_differs(self):
        ref = bytes(280 * 192 * 3)
        cur = bytearray(ref)
        region = (10, 10, 5, 5)
        self.assertFalse(screen.region_differs(ref, bytes(cur), region))
        cur[3 * (12 * 280 + 12)] = 1
        self.assertTrue(screen.region_differs(ref, bytes(cur), region))
        cur = bytearray(ref)
        cur[3 * (12 * 280 + 20)] = 1
        self.assertFalse(screen.region_differs(ref, bytes(cur), region))


if __name__ == "__main__":
    unittest.main()
