#!/usr/bin/env python3
"""
test_unexepack.py - unexepack against a packed file built here, so the check
needs no game executable: a small EXEPACK encoder turns a module with runs and
literals into fill and copy commands (applied from the end backwards, with an
uncompressed prefix and 0xFF padding), behind an unpacker segment holding the
real CS:IP/SS:SP and a relocation table in two 64 KB frames. Unpacking must
give back the module, the registers and the relocations exactly, for both
header layouts (with and without skip_len).

    python tools/drm/test_unexepack.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unexepack  # noqa: E402


def encode(module, prefix):
    """module[prefix:] as EXEPACK commands, so the decoder, reading backwards
    from the end, rebuilds it. Returns the packed bytes (prefix included)."""
    body = module[prefix:]
    # chunk forwards into (kind, bytes): runs of 4+ become fills
    chunks, i = [], 0
    while i < len(body):
        j = i
        while j < len(body) and body[j] == body[i] and j - i < 0xFFFF:
            j += 1
        if j - i >= 4:
            chunks.append(('fill', body[i:j]))
            i = j
            continue
        k = i
        while k < len(body) and k - i < 0xFFFF:
            r = k
            while r < len(body) and body[r] == body[k]:
                r += 1
            if r - k >= 4:
                break
            k += 1
        chunks.append(('copy', body[i:k]))
        i = k
    # emitted forwards: each chunk's payload, then its length, then its opcode;
    # the decoder meets the LAST chunk first, and the FIRST one ends the stream
    out = bytearray(module[:prefix])
    for n, (kind, data) in enumerate(chunks):
        last = 1 if n == 0 else 0
        if kind == 'fill':
            out += bytes([data[0]]) + struct.pack('<H', len(data)) + bytes([0xB0 | last])
        else:
            out += data + struct.pack('<H', len(data)) + bytes([0xB2 | last])
    return bytes(out)


def build(module, relocs, regs, prefix, skip_len):
    ip, cs, sp, ss = regs
    packed = encode(module, prefix)
    packed += b'\xff' * ((-len(packed)) % 16 or 16)      # padding the decoder skips
    frames = [[o for o, s in relocs if s == f * 0x1000] for f in range(16)]
    table = b''.join(struct.pack('<H', len(fr)) + b''.join(struct.pack('<H', o) for o in fr)
                     for fr in frames)
    stub_code = b'\x8b\xe8\x8c\xc0' + b'\x90' * 40 + unexepack.MESSAGE
    head = struct.pack('<7H', ip, cs, 0, 0, sp, ss, len(module) // 16)
    head += (struct.pack('<H', 1) if skip_len else b'') + b'RB'
    stub = head + stub_code + table
    stub = stub[:6] + struct.pack('<H', len(stub)) + stub[8:]
    image = packed + stub
    pcs = len(packed) // 16
    total = 0x200 + len(image)
    hdr = struct.pack('<2s13H', b'MZ', total % 512, (total + 511) // 512, 0, 0x20,
                      0x100, 0xFFFF, 0, 0x80, 0, len(head), pcs, 0x1C, 0)
    return hdr + bytes(0x200 - len(hdr)) + image


def check(skip_len):
    mod = bytearray()
    for k in range(300):                                 # literals, runs, more literals
        mod += bytes([(k * 7) & 0xFF, k & 0xFF, 0x55])
    mod += b'\0' * 1000 + b'RUN' + b'\xAA' * 70000 + b'tail'
    mod += bytes((-len(mod)) % 16)
    relocs = [(0x18, 0), (0x29, 0), (0x3A, 0), (0x1234, 0x1000)]
    regs = (0x0010, 0x016A, 0x0400, 0x012A)
    packed = build(bytes(mod), relocs, regs, prefix=64, skip_len=skip_len)
    assert unexepack.is_packed(packed)
    out = unexepack.unpack(packed)
    h = struct.unpack_from('<14H', out, 0)
    hdr = h[4] * 16
    assert out[hdr:] == bytes(mod), 'module differs'
    assert (h[10], h[11], h[8], h[7]) == regs, f'registers {h}'
    got = [struct.unpack_from('<HH', out, 0x1C + 4 * k) for k in range(h[3])]
    assert got == relocs, got
    print(f'ok: {"18" if skip_len else "16"}-byte header, {len(mod)} bytes, {len(relocs)} relocations')


if __name__ == '__main__':
    check(False)
    check(True)
