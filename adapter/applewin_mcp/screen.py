# SPDX-License-Identifier: MIT
"""Screen helpers: Apple pixels out of a harness frame, region crop and zoom, PNG encoding (stdlib only)."""

import struct
import zlib

APPLE_W, APPLE_H = 280, 192
FRAME_W, FRAME_H = 560, 384
MAX_OUT_W, MAX_OUT_H = 1120, 768
MAX_SHEET_W, MAX_SHEET_H = 1280, 1024   # a frame strip's contact sheet (four full-screen tiles at zoom 1 are 1126 wide)


class ScreenError(ValueError):
    """The message is the user-facing tool error text."""


def apple_pixels(bgra, width, height):
    """280x192 RGB from a 560x384 BGRA frame (each Apple pixel is drawn 2x2; sample the top-left of each block)."""
    if (width, height) != (FRAME_W, FRAME_H) or len(bgra) != FRAME_W * FRAME_H * 4:
        raise ScreenError("unsupported video mode for region/zoom/strips")
    out = bytearray(APPLE_W * APPLE_H * 3)
    stride = FRAME_W * 4
    for y in range(APPLE_H):
        row = bgra[2 * y * stride:][:stride]
        o = y * APPLE_W * 3
        out[o + 0:o + APPLE_W * 3:3] = row[2::8]
        out[o + 1:o + APPLE_W * 3:3] = row[1::8]
        out[o + 2:o + APPLE_W * 3:3] = row[0::8]
    return bytes(out)


def check_region(region):
    if region is None:
        return (0, 0, APPLE_W, APPLE_H)
    bad = ScreenError("region must be [x, y, w, h] inside 280x192 Apple pixels")
    try:
        valid = len(region) == 4 and all(isinstance(v, int) and not isinstance(v, bool) for v in region)
    except TypeError:
        valid = False
    if not valid:
        raise bad
    x, y, w, h = region
    if x < 0 or y < 0 or w < 1 or h < 1 or x + w > APPLE_W or y + h > APPLE_H:
        raise bad
    return (x, y, w, h)


def check_zoom(zoom):
    if not isinstance(zoom, int) or isinstance(zoom, bool) or not 1 <= zoom <= 8:
        raise ScreenError("zoom must be 1 to 8")


def crop_zoom(apple_rgb, region, zoom):
    """Crop to region, then scale by an integer factor with nearest neighbour. Returns (width, height, rgb)."""
    check_zoom(zoom)
    x, y, w, h = region
    out = bytearray()
    for row in range(y, y + h):
        src = apple_rgb[(row * APPLE_W + x) * 3:][:w * 3]
        line = b"".join(src[i:i + 3] * zoom for i in range(0, w * 3, 3))
        out += line * zoom
    return w * zoom, h * zoom, bytes(out)


def check_size(width, height, max_w=MAX_OUT_W, max_h=MAX_OUT_H):
    if width > max_w or height > max_h:
        raise ScreenError(f"the image would be {width}x{height}; the limit is {max_w}x{max_h}")


def sheet_size(tile_w, tile_h, count, per_row=4, gap=2):
    """(width, height) of the contact sheet for count tiles of tile_w x tile_h."""
    cols = min(count, per_row)
    rows = -(-count // per_row)
    return cols * tile_w + (cols - 1) * gap, rows * tile_h + (rows - 1) * gap


def contact_sheet(tiles, per_row=4, gap=2, gap_rgb=(128, 128, 128)):
    """Lays equal-sized (w, h, rgb) tiles out left to right, top to bottom, per_row to a row, with gap pixels of
    gap_rgb between them (and in the unused cells of the last row). Returns (width, height, rgb)."""
    tile_w, tile_h, _ = tiles[0]
    if any((w, h) != (tile_w, tile_h) for w, h, _ in tiles):
        raise ScreenError("contact sheet tiles must all be the same size")
    width, height = sheet_size(tile_w, tile_h, len(tiles), per_row, gap)
    grey = bytes(gap_rgb)
    canvas = [bytearray(grey * width) for _ in range(height)]
    for n, (_, _, rgb) in enumerate(tiles):
        x0 = (n % per_row) * (tile_w + gap)
        y0 = (n // per_row) * (tile_h + gap)
        for row in range(tile_h):
            canvas[y0 + row][x0 * 3:(x0 + tile_w) * 3] = rgb[row * tile_w * 3:(row + 1) * tile_w * 3]
    return width, height, b"".join(canvas)


def _chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def encode_png(width, height, rgb):
    """8-bit RGB PNG, filter 0 on every row, a single IDAT."""
    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + _chunk(b"IDAT", zlib.compress(raw))
            + _chunk(b"IEND", b""))


def region_differs(ref_rgb, cur_rgb, region):
    """True when any pixel inside region differs between two 280x192 RGB buffers."""
    x, y, w, h = region
    for row in range(y, y + h):
        o = (row * APPLE_W + x) * 3
        if ref_rgb[o:o + w * 3] != cur_rgb[o:o + w * 3]:
            return True
    return False
