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
    def test_frame_pixels_keeps_every_pixel(self):
        buf = bytearray(560 * 384 * 4)
        points = [(0, 0), (559, 383), (1, 1), (301, 7)]
        for x, y in points:
            off = (y * 560 + x) * 4
            buf[off:off + 4] = bytes([x % 256, y % 256, 7, 0])
        rgb = screen.frame_pixels(bytes(buf), 560, 384)
        self.assertEqual(len(rgb), 560 * 384 * 3)
        for x, y in points:
            self.assertEqual(rgb[3 * (y * 560 + x):][:3], bytes([7, y % 256, x % 256]))

    def test_frame_pixels_rejects_other_sizes(self):
        with self.assertRaisesRegex(screen.ScreenError, "unsupported video mode"):
            screen.frame_pixels(bytes(600 * 420 * 4), 600, 420)

    def test_region_outside_screen_is_rejected(self):
        for bad in ([270, 0, 20, 10], [0, 0, 0, 5], [0, 0, 5], [-1, 0, 5, 5]):
            with self.assertRaises(screen.ScreenError):
                screen.check_region(bad)
        self.assertEqual(screen.check_region(None), (0, 0, 280, 192))

    def test_region_that_is_not_a_list_is_rejected(self):
        for bad in (5, "abcd"):
            with self.assertRaises(screen.ScreenError):
                screen.check_region(bad)

    def test_crop_zoom_nearest_neighbour(self):
        buf = bytearray(560 * 384 * 3)
        for fy in (40, 41):   # apple pixel (10, 20) is frame pixels (20..21, 40..41); (11, 20) is (22..23, 40..41)
            for fx, colour in ((20, (255, 0, 0)), (21, (255, 0, 0)), (22, (1, 2, 3)), (23, (1, 2, 3))):
                buf[3 * (fy * 560 + fx):3 * (fy * 560 + fx + 1)] = bytes(colour)
        w, h, rgb = screen.crop_zoom(bytes(buf), (10, 20, 2, 1), 3)
        self.assertEqual((w, h), (6, 3))
        for y in range(3):
            for x in range(6):
                expect = bytes([255, 0, 0]) if x < 3 else bytes([1, 2, 3])
                self.assertEqual(rgb[3 * (y * 6 + x):][:3], expect)
        with self.assertRaisesRegex(screen.ScreenError, "zoom must be 1 to 8"):
            screen.crop_zoom(bytes(buf), (0, 0, 1, 1), 9)

    def test_crop_zoom_keeps_every_dot_at_zoom_2_and_up(self):
        white, black = bytes([255, 255, 255]), bytes(3)
        row = b"".join(white if x % 2 else black for x in range(560))
        frame = row * 384
        w, h, rgb = screen.crop_zoom(frame, (0, 0, 4, 1), 2)
        self.assertEqual((w, h), (8, 2))
        for y in range(2):
            for x in range(8):
                self.assertEqual(rgb[3 * (y * 8 + x):][:3], white if x % 2 else black)
        # zoom 3: label each frame column with its own colour to see which source columns are hit
        labelled = b"".join(bytes([x % 256, 0, 0]) for x in range(560)) * 384
        w, h, rgb = screen.crop_zoom(labelled, (0, 0, 4, 1), 3)
        self.assertEqual((w, h), (12, 3))
        self.assertEqual({rgb[3 * x] for x in range(12)}, set(range(8)))
        w, h, rgb = screen.crop_zoom(frame, (0, 0, 4, 1), 1)
        self.assertEqual((w, h), (4, 1))
        self.assertEqual({rgb[3 * x:][:3] for x in range(4)}, {black})

    def test_crop_zoom_zoom_2_whole_screen_is_the_frame(self):
        frame = os.urandom(560 * 384 * 3)
        self.assertEqual(screen.crop_zoom(frame, (0, 0, 280, 192), 2), (560, 384, frame))

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
        ref = bytes(560 * 384 * 3)
        cur = bytearray(ref)
        region = (10, 10, 5, 5)
        self.assertFalse(screen.region_differs(ref, bytes(cur), region))
        cur[3 * (24 * 560 + 24)] = 1
        self.assertTrue(screen.region_differs(ref, bytes(cur), region))
        cur = bytearray(ref)
        cur[3 * (24 * 560 + 40)] = 1
        self.assertFalse(screen.region_differs(ref, bytes(cur), region))

    def test_region_differs_sees_a_single_odd_column(self):
        ref = bytes(560 * 384 * 3)
        cur = bytearray(ref)
        cur[3 * ((2 * 5 + 1) * 560 + 2 * 10 + 1)] = 1
        self.assertTrue(screen.region_differs(ref, bytes(cur), (10, 5, 1, 1)))
        self.assertFalse(screen.region_differs(ref, bytes(cur), (11, 5, 1, 1)))

    def test_contact_sheet_layout(self):
        tiles = [(2, 1, bytes([i, i, i, i, i, i])) for i in range(1, 6)]
        w, h, rgb = screen.contact_sheet(tiles, per_row=4, gap=2)
        self.assertEqual((w, h), (14, 4))
        grey = bytes((128, 128, 128))

        def px(x, y):
            return rgb[(y * w + x) * 3:][:3]
        self.assertEqual(px(0, 0), bytes((1, 1, 1)))
        self.assertEqual(px(4, 0), bytes((2, 2, 2)))   # tile 2 starts at x = 2 + 2
        self.assertEqual(px(2, 0), grey)                       # the gap between tile 1 and 2
        self.assertEqual(px(0, 1), grey)                       # the gap between rows
        self.assertEqual(px(0, 3), bytes((5, 5, 5)))           # tile 5: row 2, column 1
        self.assertEqual(px(1, 3), bytes((5, 5, 5)))
        for x in range(2, 14):                                 # the three empty cells and gaps in row 2
            self.assertEqual(px(x, 3), grey, x)

    def test_sheet_size(self):
        self.assertEqual(screen.sheet_size(2, 1, 5, per_row=4, gap=2), (14, 4))
        self.assertEqual(screen.sheet_size(280, 192, 4, per_row=4, gap=2), (1126, 192))


if __name__ == "__main__":
    unittest.main()
