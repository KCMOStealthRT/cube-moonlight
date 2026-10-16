#!/usr/bin/env python3
"""Dump or compare the modversions CRCs (__versions section) of arm64 kernel modules.

  ko-versions.py A.ko                 print "crc name" for every imported symbol
  ko-versions.py A.ko B.ko            compare: symbols both import, CRC equal or not

Used to check that a module built on the PC matches the box's kernel before insmod.
"""
import struct
import sys


def sections(data):
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise SystemExit('not a little-endian ELF64 file')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    for h in hdrs:
        end = data.index(b'\0', stroff + h[0])
        yield data[stroff + h[0]:end].decode(), data[h[4]:h[4] + h[5]]


def versions(path):
    data = open(path, 'rb').read()
    out = {}
    for name, body in sections(data):
        if name == '__versions':
            for off in range(0, len(body), 64):  # struct modversion_info: u64 crc + char[56]
                crc, = struct.unpack_from('<Q', body, off)
                sym = body[off + 8:off + 64].split(b'\0', 1)[0].decode()
                out[sym] = crc & 0xffffffff
    return out


def vermagic(path):
    data = open(path, 'rb').read()
    i = data.find(b'vermagic=')
    return data[i + 9:data.index(b'\0', i)].decode() if i >= 0 else None


if __name__ == '__main__':
    if len(sys.argv) == 2:
        print('vermagic:', vermagic(sys.argv[1]))
        for s, c in sorted(versions(sys.argv[1]).items()):
            print(f'{c:08x} {s}')
    elif len(sys.argv) == 3:
        a, b = versions(sys.argv[1]), versions(sys.argv[2])
        print('vermagic A:', vermagic(sys.argv[1]))
        print('vermagic B:', vermagic(sys.argv[2]))
        common = sorted(set(a) & set(b))
        bad = [s for s in common if a[s] != b[s]]
        for s in bad:
            print(f'MISMATCH {s}: {a[s]:08x} != {b[s]:08x}')
        print(f'{len(common)} common symbols, {len(bad)} mismatches '
              f'(only in A: {len(set(a) - set(b))}, only in B: {len(set(b) - set(a))})')
        sys.exit(1 if bad else 0)
    else:
        raise SystemExit(__doc__)
