"""Reproduce extraction of the verified ceiling3 OTA using Python standard library.

Run: python unpack.py [path-to-original.bin]
The input OTA is never modified. Output goes into extracted/ next to this script.
"""
import hashlib
import json
import lzma
import pathlib
import struct
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent


def main(src=None):
    source = pathlib.Path(src) if src else (
        pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'yeelink.light.ceiling3-stock-2.0.2_0048.bin'
    )
    data = source.read_bytes()
    if len(data) != 726978 or hashlib.md5(data).hexdigest() != '13b7471a0f84e3cfbac78c116fae7a1c':
        raise SystemExit('Unexpected source file size or MD5; extraction stopped.')
    magic, count = struct.unpack_from('<II', data)
    if magic != 0x004d4d4d or count != 3 or hashlib.sha1(data[:136]).digest() != data[136:156]:
        raise SystemExit('Unexpected container header or SHA-1.')
    crc = zlib.crc32(data[:-4], 0xffffffff) ^ 0xffffffff
    if struct.unpack('<I', data[-4:])[0] != crc:
        raise SystemExit('Trailing CRC32 mismatch.')
    dest = HERE / 'extracted'
    dest.mkdir(exist_ok=True)
    for index in range(count):
        offset, address, size, _part, digest_offset, digest_size, compressed, _res = struct.unpack_from(
            '<8I', data, 8 + index * 32
        )
        if offset + size != digest_offset or digest_size != 20 or digest_offset + digest_size > len(data):
            raise SystemExit('Invalid segment bounds.')
        packed = data[offset:offset + size]
        if hashlib.sha1(packed).digest() != data[digest_offset:digest_offset + digest_size]:
            raise SystemExit('Segment SHA-1 mismatch.')
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=64 * 1024 * 1024)
        raw = decoder.decompress(packed, max_length=16 * 1024 * 1024)
        if compressed != 1 or not decoder.eof or decoder.unused_data:
            raise SystemExit('Unexpected compression or incomplete stream.')
        filename = f'segment-{index}-flash-{address:08x}.bin'
        (dest / filename).write_bytes(raw)
        print(json.dumps({'file': filename, 'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}))


if __name__ == '__main__':
    main()
