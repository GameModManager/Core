#!/usr/bin/env python3
"""Hand-craft the AES-encrypted RAR5 fixture the 7-Zip backend tests use.

The key schedule is RAR5's variant of PBKDF2-HMAC-SHA256, transcribed from
CPP/7zip/Crypto/Rar5Aes.cpp:195 CalcKey_and_CheckPassword():

    base  = HMAC-SHA256(password)
    u     = base(salt || 01 00 00 00)          # PBKDF2 block 1, big-endian
    key   = u
    n     = (1 << iterationsLog) - 1
    repeat 3 times:                            # 0: AES key, 1: HMAC key, 2: check
        n times:  u = base(u); key ^= u
        n = 16

The password-check bytes are check[i] = psw[i] ^ psw[i+8] ^ psw[i+16] ^ psw[i+24].

The Crypto extra record (extra id 1) holds:
    vint Version(0) | vint Flags | byte iterationsLog | salt[16] | iv[16] | check[8]

The archive is validated against real unrar and real 7z, not just against the
reader it was derived from. The password is PASSWORD below; it is a literal in
the test that uses the fixture, and the test asserts it never appears anywhere
the user or the operator can see.
"""

import hashlib
import hmac
import os
import struct
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

OUT = os.path.dirname(os.path.abspath(__file__))
PASSWORD = b"correct horse battery staple"
SALT = bytes(range(16))
# Pinned rather than random so re-running this script reproduces the committed
# fixture byte for byte, and a diff on the .rar means the format really changed.
IV = bytes(range(0x10, 0x20))
ITER_LOG = 0x0F  # 32767 iterations, what RAR itself uses


def vint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def block(head_type, flags, body):
    payload = vint(head_type) + vint(flags) + body
    head = vint(len(payload)) + payload
    return struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF) + head


def main_header():
    return block(1, 0, vint(0))  # type=1 kArc, flags=0, ArchiveFlags=0


def end_header():
    return block(5, 0, vint(0))  # type=5 kEndOfArc, EndFlags=0


def derive_keys(password, salt, log):
    def base(msg):
        return hmac.new(password, msg, hashlib.sha256).digest()

    # SetUi32a() is a little-endian store on x86, so the PBKDF2 block index
    # reaches the hash as 00 00 00 01, not 01 00 00 00.
    u = base(salt + b"\x00\x00\x00\x01")
    key = bytearray(u)
    n = (1 << log) - 1
    rounds = []
    for _ in range(3):
        for _ in range(n):
            u = base(u)
            for j in range(32):
                key[j] ^= u[j]
        rounds.append(bytes(key))
        n = 16
    aes_key, mac_key, psw = rounds
    check = bytes(psw[i] ^ psw[i + 8] ^ psw[i + 16] ^ psw[i + 24] for i in range(8))
    return aes_key, mac_key, check


def crypto_record(aes_key, iv, check):
    # kCheckSize32 * 4 == 12 bytes: 8-byte password check + 4-byte SHA-256 csum.
    csum = hashlib.sha256(check).digest()[:4]
    data = vint(0) + vint(1) + bytes([ITER_LOG]) + SALT + iv + check + csum
    # RAR5 extra records: Size counts the Type byte too, so recordDataSize
    # as seen by FindExtra() is Size - 1.
    extra = vint(1 + len(data)) + vint(1) + data  # extra id 1 = kCrypto
    return extra, len(data)


def build(path, name, plain, dict_main=10):
    iv = IV
    aes_key, _, check = derive_keys(PASSWORD, SALT, ITER_LOG)

    # RAR5 zero-pads the last AES block; the decoder truncates to the declared
    # unpacked size.
    pad = (-len(plain)) % 16
    padded = plain + b"\x00" * pad
    enc = Cipher(algorithms.AES(aes_key), modes.CBC(iv)).encryptor()
    cipher_text = enc.update(padded) + enc.finalize()

    extra, crypto_size = crypto_record(aes_key, iv, check)
    # version(1) + flags(1) + KDFcount(1) + salt(16) + iv(16) + check(8) + csum(4)
    assert crypto_size == 1 + 1 + 1 + 16 + 16 + 8 + 4, crypto_size

    name_b = name.encode("utf-8")
    m = 10 << 10  # algo 0, method 0 (Store), dictionary exponent 10 = 128 MiB
    file_fields = (
        vint(1 << 2)  # FileFlags: kCrc32
        + vint(len(plain))
        + vint(0x20)
        + struct.pack("<I", zlib.crc32(plain) & 0xFFFFFFFF)
        + vint(m)
        + vint(0)  # HostOS = Windows
        + vint(len(name_b))
        + name_b
    )
    # Flags order in ReadBlockHeader is: Type, Flags, ExtraSize, DataSize, ...
    # and the extra area physically follows the name.
    hdr = block(2, 0x0003, vint(len(extra)) + vint(len(cipher_text)) + file_fields + extra)

    blob = (bytes([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00])
            + main_header() + hdr + cipher_text + end_header())
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


os.makedirs(OUT, exist_ok=True)
payload = b"GMM 7-Zip backend fixture - ENCRYPTED RAR5 file.\n" * 64
n = build(os.path.join(OUT, "encrypted.rar"), "secret.txt", payload)
print("encrypted.rar %d bytes (plaintext %d, password %r)"
      % (n, len(payload), PASSWORD.decode()))
