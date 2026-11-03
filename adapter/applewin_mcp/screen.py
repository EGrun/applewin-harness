# SPDX-License-Identifier: MIT
"""Screen helpers: RGB out of a harness PNG frame, region crop and zoom, PNG encoding (stdlib only)."""

import struct
import zlib

APPLE_W, APPLE_H = 280, 192
FRAME_W, FRAME_H = 560, 384
MAX_OUT_W, MAX_OUT_H = 1120, 768
MAX_SHEET_W, MAX_SHEET_H = 1280, 1024   # a frame strip's contact sheet (four full-screen tiles at zoom 1 are 1126 wide)


class ScreenError(ValueError):
    """The message is the user-facing tool error text."""


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


def crop_zoom(frame_rgb, region, zoom):
    """Crop a region (in Apple pixels, each 2x2 frame pixels) from a 560x384 RGB frame and scale it to
    (w*zoom, h*zoom) with nearest neighbour: output pixel (ox, oy) is frame pixel
    (2*x + (2*ox)//zoom, 2*y + (2*oy)//zoom), so zoom 2 or more shows every dot. Returns (width, height, rgb)."""
    check_zoom(zoom)
    x, y, w, h = region
    out_w, out_h = w * zoom, h * zoom
    cols = [(2 * x + (2 * ox) // zoom) * 3 for ox in range(out_w)]
    out = bytearray()
    for oy in range(out_h):
        base = (2 * y + (2 * oy) // zoom) * FRAME_W * 3
        out += b"".join([frame_rgb[base + c:base + c + 3] for c in cols])
    return out_w, out_h, bytes(out)


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


def decode_png(data):
    """(width, height, rgb) from an 8-bit RGB, uninterlaced PNG whose rows all use filter 0, as the harness writes
    them; any other PNG is a ScreenError. The IDAT data may be split over any number of chunks."""
    bad = ScreenError("unexpected PNG from the harness")
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise bad
    pos, header, idat = 8, None, []
    while pos + 8 <= len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            header = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    if header is None or len(header) != 13:
        raise bad
    width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", header)
    if (depth, ctype, interlace) != (8, 2, 0):
        raise bad
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error:
        raise bad from None
    stride = width * 3
    if len(raw) != height * (stride + 1):
        raise bad
    rows = []
    for y in range(height):
        row = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        if row[0] != 0:
            raise bad
        rows.append(row[1:])
    return width, height, b"".join(rows)


def region_differs(ref_rgb, cur_rgb, region):
    """True when any frame pixel inside region (Apple pixels, scaled x2) differs between two 560x384 RGB buffers."""
    x, y, w, h = region
    for row in range(2 * y, 2 * (y + h)):
        o = (row * FRAME_W + 2 * x) * 3
        if ref_rgb[o:o + 2 * w * 3] != cur_rgb[o:o + 2 * w * 3]:
            return True
    return False
