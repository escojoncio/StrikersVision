#!/usr/bin/env python3
# Rewrites the platform of every Mach-O object inside a static archive (ar, BSD/Darwin format), in
# place: LC_BUILD_VERSION's platform, minos and sdk fields. Used to turn Aurora's prebuilt iOS Dawn
# (dawn-ios-arm64) into a visionOS one: there is no prebuilt for xros, the code is the same arm64
# with the same Metal API, and ld refuses to mix objects "built for iOS" into a visionOS link.
#
# Only fixed-size fields change, so every member keeps its size and the archive's symbol table
# (__.SYMDEF, offsets into the file) stays valid. Objects carrying the older LC_VERSION_MIN_* form
# cannot be rewritten this way and make the script fail rather than be left behind.
#
#   macho-retarget.py <archive.a> <platform number> <minos X.Y> <sdk X.Y>
#   (visionOS device = 11, see <mach-o/loader.h> PLATFORM_VISIONOS)
import struct
import sys

LC_BUILD_VERSION = 0x32
LC_VERSION_MIN = (0x24, 0x25, 0x2F, 0x30)
MH_MAGIC_64 = 0xFEEDFACF


def encode_version(text):
    parts = [int(p) for p in text.split(".")] + [0, 0]
    return (parts[0] << 16) | (parts[1] << 8) | parts[2]


def patch_object(buf, start, end, platform, minos, sdk, name):
    if end - start < 32 or struct.unpack_from("<I", buf, start)[0] != MH_MAGIC_64:
        return 0
    ncmds = struct.unpack_from("<I", buf, start + 16)[0]
    q = start + 32
    patched = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, q)
        if cmd == LC_BUILD_VERSION:
            struct.pack_into("<III", buf, q + 8, platform, minos, sdk)
            patched += 1
        elif cmd in LC_VERSION_MIN:
            sys.exit(f"{name}: LC_VERSION_MIN_* load command, cannot retarget in place")
        q += size
    if patched == 0:
        sys.exit(f"{name}: no LC_BUILD_VERSION")
    return patched


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__ or "usage: macho-retarget.py <archive.a> <platform> <minos> <sdk>")
    path, platform = sys.argv[1], int(sys.argv[2])
    minos, sdk = encode_version(sys.argv[3]), encode_version(sys.argv[4])
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    if buf[:8] != b"!<arch>\n":
        sys.exit(f"{path}: not an ar archive")
    p = 8
    objects = 0
    while p + 60 <= len(buf):
        header = buf[p:p + 60]
        name = header[:16].decode("ascii", "replace").strip()
        size = int(header[48:58].decode("ascii").strip())
        body = p + 60
        offset = 0
        if name.startswith("#1/"):
            offset = int(name[3:])
            name = buf[body:body + offset].rstrip(b"\0").decode("utf-8", "replace")
        if name not in ("__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"):
            if patch_object(buf, body + offset, body + size, platform, minos, sdk, name):
                objects += 1
        p = body + size + (size & 1)
    with open(path, "wb") as f:
        f.write(buf)
    print(f"{path}: {objects} objects retargeted to platform {platform}")


if __name__ == "__main__":
    main()
