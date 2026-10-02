#!/usr/bin/env python3
# Compute and write the PE image checksum into a driver's optional header.
# The NT 3.51+ OS loader (and the Windows 2000-era NTLDR) validates the PE
# checksum of a boot driver and rejects a zero/stale one as "missing or
# corrupt", so every build must carry a correct checksum. (strip invalidates
# it, hence this runs last.)
import struct, sys

def set_checksum(fn):
    d = bytearray(open(fn, 'rb').read())
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[pe:pe+4] == b'PE\0\0', "not a PE file"
    coff = pe + 24 + 64           # optional header -> CheckSum (PE32)
    struct.pack_into('<I', d, coff, 0)
    s = 0; n = len(d); i = 0
    while i + 1 < n:
        s += d[i] | (d[i+1] << 8); s = (s & 0xffff) + (s >> 16); i += 2
    if i < n:
        s += d[i]; s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    cks = (s + len(d)) & 0xffffffff
    struct.pack_into('<I', d, coff, cks)
    open(fn, 'wb').write(d)
    return cks

if __name__ == '__main__':
    for f in sys.argv[1:]:
        print("checksum %s = 0x%x" % (f, set_checksum(f)))
