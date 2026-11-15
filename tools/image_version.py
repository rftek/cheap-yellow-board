"""Prints the version stored in an ESP-IDF app image (esp_app_desc_t.version).

The app description follows the 24-byte image header and the first 8-byte
segment header; its version field starts 16 bytes in and holds up to 32
characters. The launcher reads the same field from each slot.

Usage: python tools/image_version.py firmware.bin
"""

import struct
import sys

APP_DESC_OFFSET = 24 + 8
APP_DESC_MAGIC = 0xABCD5432


def image_version(path):
    with open(path, "rb") as image:
        header = image.read(APP_DESC_OFFSET + 48)
    if len(header) < APP_DESC_OFFSET + 48 or header[0] != 0xE9:
        raise ValueError(f"{path} is not an ESP-IDF app image")
    (magic,) = struct.unpack_from("<I", header, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise ValueError(f"{path} has no app description")
    field = header[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48]
    return field.split(b"\0", 1)[0].decode("ascii", "replace")


if __name__ == "__main__":
    print(image_version(sys.argv[1]))
