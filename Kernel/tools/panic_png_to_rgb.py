#!/usr/bin/env python3

import struct
import sys
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_PIXELS = 16_777_216
BACKGROUND = (0x17, 0x0A, 0x0A)


def paeth(left, above, upper_left):
    estimate = left + above - upper_left
    distances = (
        abs(estimate - left),
        abs(estimate - above),
        abs(estimate - upper_left),
    )
    return (left, above, upper_left)[distances.index(min(distances))]


def decode_png(path):
    data = open(path, "rb").read()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("input is not a PNG")

    position = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = None
    palette = None
    transparency = b""
    compressed = bytearray()
    saw_end = False

    while position + 12 <= len(data):
        length = struct.unpack_from(">I", data, position)[0]
        position += 4
        kind = data[position:position + 4]
        position += 4
        if length > len(data) - position - 4:
            raise ValueError("truncated PNG chunk")

        payload = data[position:position + length]
        position += length
        expected_crc = struct.unpack_from(">I", data, position)[0]
        position += 4
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != expected_crc:
            raise ValueError("PNG chunk CRC mismatch")

        if kind == b"IHDR":
            if width is not None or len(payload) != 13:
                raise ValueError("invalid PNG header")
            (
                width,
                height,
                bit_depth,
                color_type,
                compression,
                filtering,
                interlace,
            ) = struct.unpack(">IIBBBBB", payload)
            if compression != 0 or filtering != 0 or interlace != 0:
                raise ValueError("unsupported PNG compression or interlacing")
            if bit_depth != 8 or color_type not in (0, 2, 3, 4, 6):
                raise ValueError("PNG must use 8-bit grayscale, RGB, or indexed color")
            if not width or not height or width * height > MAX_PIXELS:
                raise ValueError("PNG dimensions are empty or too large")
        elif kind == b"PLTE":
            palette = payload
        elif kind == b"tRNS":
            transparency = payload
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            saw_end = True
            break

    if width is None or not saw_end:
        raise ValueError("PNG is missing required chunks")

    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    channels = channels_by_type[color_type]
    row_bytes = width * channels
    expected_size = height * (row_bytes + 1)
    decoder = zlib.decompressobj()
    filtered = decoder.decompress(bytes(compressed), expected_size + 1)
    if len(filtered) > expected_size or decoder.unconsumed_tail:
        raise ValueError("PNG image data expands beyond its dimensions")
    filtered += decoder.flush()
    if not decoder.eof or len(filtered) != expected_size:
        raise ValueError("PNG image data has an invalid size")
    if color_type == 3 and (not palette or len(palette) % 3):
        raise ValueError("indexed PNG has an invalid palette")

    rgb = bytearray(width * height * 3)
    previous = bytearray(row_bytes)
    source_position = 0
    output_position = 0

    for _ in range(height):
        filter_type = filtered[source_position]
        source_position += 1
        row = bytearray(filtered[source_position:source_position + row_bytes])
        source_position += row_bytes

        if filter_type > 4:
            raise ValueError("PNG uses an unknown row filter")
        for index in range(row_bytes):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                predictor = paeth(left, above, upper_left)
            else:
                predictor = 0
            row[index] = (row[index] + predictor) & 0xFF

        for index in range(0, row_bytes, channels):
            if color_type == 0:
                red = green = blue = row[index]
                alpha = 0 if transparency and struct.unpack(">H", transparency[:2])[0] == red else 255
            elif color_type == 2:
                red, green, blue = row[index:index + 3]
                alpha = 255
                if len(transparency) == 6:
                    transparent_rgb = struct.unpack(">HHH", transparency)
                    if (red, green, blue) == transparent_rgb:
                        alpha = 0
            elif color_type == 3:
                palette_index = row[index]
                palette_position = palette_index * 3
                if palette_position + 2 >= len(palette):
                    raise ValueError("indexed PNG refers outside its palette")
                red, green, blue = palette[palette_position:palette_position + 3]
                alpha = transparency[palette_index] if palette_index < len(transparency) else 255
            elif color_type == 4:
                red = green = blue = row[index]
                alpha = row[index + 1]
            else:
                red, green, blue, alpha = row[index:index + 4]

            if alpha != 255:
                red, green, blue = (
                    (red * alpha + BACKGROUND[0] * (255 - alpha) + 127) // 255,
                    (green * alpha + BACKGROUND[1] * (255 - alpha) + 127) // 255,
                    (blue * alpha + BACKGROUND[2] * (255 - alpha) + 127) // 255,
                )
            rgb[output_position:output_position + 3] = bytes((red, green, blue))
            output_position += 3

        previous = row

    return width, height, rgb


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: panic_png_to_rgb.py INPUT.png OUTPUT.rgb")
    try:
        width, height, pixels = decode_png(sys.argv[1])
        with open(sys.argv[2], "wb") as output:
            output.write(struct.pack("<II", width, height))
            output.write(pixels)
    except (OSError, ValueError, zlib.error, struct.error) as error:
        raise SystemExit(f"panic PNG conversion failed: {error}") from error


if __name__ == "__main__":
    main()
