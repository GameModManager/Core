#!/usr/bin/env python3
"""Hand-craft the RAR5 fixtures the 7-Zip backend tests use.

7-Zip cannot create RAR (that is the unRAR restriction), and there is no `rar`
binary on this box, so the fixtures are built byte-by-byte from the layout that
7-Zip's own reader accepts (CPP/7zip/Archive/Rar/Rar5Handler.cpp).

Layout, per CInArchive::ReadBlockHeader + CInArchive::Open + ReadFileHeader:

  block := crc32_le(4) vint(HeadSize) <HeadSize bytes>
  block header := vint(Type) vint(Flags) [vint(ExtraSize)] [vint(DataSize)]
  file header   := vint(FileFlags) vint(UnpSize) vint(Attrib)
                   [u32 mtime] [u32 dataCRC32]
                   vint(Method) vint(HostOS) vint(NameLen) bytes(Name)

The declared LZMA dictionary size is packed into the Method varint, NOT stored as
data: bits 0-5 algorithm version, bits 7-9 method, bits 10-14 dictionary exponent.
Get_DictSize64() = 32 << (12 + dictMain), so dictMain=10 declares 128 MiB while
the file itself stays tiny. Method 0 (Store) means the payload is raw, so the
oversized dictionary is never actually allocated - which is exactly the shape of
archive that libarchive's 64 MiB cap rejects and that extract_with_unrar exists
to work around.
"""

import struct
import zlib
import os
import sys

OUT = os.path.dirname(os.path.abspath(__file__))


def vint(v):
    """RAR variable-length integer: 7 bits per byte, LSB first, high bit = more."""
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def block(head_type, flags, body, data=b""):
    """Assemble one RAR5 block with its CRC32 and HeadSize."""
    payload = vint(head_type) + vint(flags) + body
    assert len(data) == 0
    head = vint(len(payload)) + payload
    crc = zlib.crc32(head) & 0xFFFFFFFF
    return struct.pack("<I", crc) + head


def main_header(solid=False):
    # type=1 (kArc), flags=0, then ArchiveFlags varint (no vol, no solid).
    arc_flags = 1 << 2 if solid else 0
    return block(1, 0, vint(arc_flags))


def file_header(name, data, dict_main=0, method=0, algo=0, extra=b""):
    # Method varint: algo | (method << 7) | (dict_main << 10)
    m = algo | (method << 7) | (dict_main << 10)
    name_b = name.encode("utf-8")
    body = (
        vint(1 << 2)                       # FileFlags: kCrc32
        + vint(len(data))                  # UnpackedSize
        + vint(0x20)                       # Attributes (archive bit)
        + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)  # data CRC32
        + vint(m)                          # Method (dict size lives here)
        + vint(0)                          # HostOS = Windows
        + vint(len(name_b))
        + name_b
        + extra
    )
    # flags bit1 (kData) = DataSize present.
    return block(2, 0x0002, vint(len(data)) + body)


def end_header():
    # type=5 (kEndOfArc), flags=0, EndFlags=0
    return block(5, 0, vint(0))


def build(path, name, data, dict_main):
    blob = (bytes([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00])
            + main_header()
            + file_header(name, data, dict_main=dict_main)
            + data
            + end_header())
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


os.makedirs(OUT, exist_ok=True)

payload = b"GMM 7-Zip backend fixture - RAR5 stored file.\n" * 64

# dict_main is the dictionary exponent: Get_DictSize64() = 32 << (12 + dict_main).
# 10 declares 128 MiB and 15 declares 4 GiB, both past the 64 MiB ceiling in
# libarchive's RAR5 reader, and both tiny on disk because Method 0 (Store) never
# allocates the dictionary it declares.
sizes = {
    "big_dict.rar": build(os.path.join(OUT, "big_dict.rar"), "hello.txt",
                          payload, dict_main=10),
    "huge_dict.rar": build(os.path.join(OUT, "huge_dict.rar"), "hello.txt",
                           payload, dict_main=15),
    # Entry names the traversal guard has to refuse. 7-Zip hands both back
    # verbatim, so neither is sanitized upstream of the guard.
    "traversal.rar": build(os.path.join(OUT, "traversal.rar"), "../escape.txt",
                           b"traversal probe\n", dict_main=5),
    "abs.rar": build(os.path.join(OUT, "abs.rar"), "/etc/gmm-probe",
                     b"absolute probe\n", dict_main=5),
}

for k in sorted(sizes):
    print("%-16s %6d bytes" % (k, sizes[k]))
print("payload %d bytes" % len(payload))
