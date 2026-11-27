import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
image = bytearray(path.read_bytes())
if image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
    raise SystemExit("VCC did not produce an ELF64 little-endian executable")

os_exit = b"\xb8\x09\x00\x00\x00\xcd\x80"
if image.count(os_exit) != 1:
    raise SystemExit("VCC executable has an unexpected OS exit stub")
offset = image.index(os_exit) + 1
image[offset] = 1
path.write_bytes(image)
path.chmod(0o755)
