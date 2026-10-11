#!/usr/bin/env python3
"""
unexepack.py - undo Microsoft EXEPACK compression of a DOS MZ executable.

EXEPACK shipped with MASM and the Microsoft linker (LINK /EXEPACK), so it turns
up wherever a program was linked with Microsoft's tools: the packed file is the
load module run-length encoded, followed by a small unpacker segment. The MZ
header points CS:IP at the unpacker, so the disassembler sees only that.

Layout of the unpacker segment (at the packed CS:0000):

    u16 real_ip, real_cs     the program's own entry, CS relative to the image
    u16 mem_start            scratch for the unpacker
    u16 exepack_size         header + unpacker + relocation table, in bytes
    u16 real_sp, real_ss     the program's own stack
    u16 dest_len             unpacked load module size, in paragraphs
    [u16 skip_len]           later versions only: paragraphs of slack
    u16 signature            "RB"
    ... the unpacker code, the message "Packed file is corrupt", then the
    relocation table: for each 64 KB frame 0000, 1000, ..., F000 a u16 count
    followed by that many u16 offsets.

The compressed module is decoded from its END backwards: trailing 0xFF
padding is skipped, then commands -- read backwards, so the opcode byte comes
first, then its 16-bit length -- are applied until one has its low bit set:

    B0/B1 len fill    write `fill` len times
    B2/B3 len         copy the len bytes that precede the command

Whatever lies before the first command is already in place, uncompressed.

Pure Python and headless: the unpacker is never run. The output is a plain MZ
with the original entry, stack and relocation table, which is what decode16 and
the lifters expect.

    python unexepack.py PACKED.EXE OUT.EXE
"""
import struct
import sys

MESSAGE = b'Packed file is corrupt'


def is_packed(data: bytes) -> bool:
    """True if this MZ's entry segment is an EXEPACK unpacker."""
    if data[:2] != b'MZ' or len(data) < 0x40:
        return False
    hdr = struct.unpack_from('<H', data, 8)[0] * 16
    cs = struct.unpack_from('<H', data, 0x16)[0]
    p = hdr + cs * 16
    return data[p + 14:p + 16] == b'RB' or data[p + 16:p + 18] == b'RB'


def unpack(data: bytes) -> bytes:
    if not is_packed(data):
        raise ValueError('not an EXEPACK file')
    hdr = struct.unpack_from('<H', data, 8)[0] * 16
    min_extra, max_extra = struct.unpack_from('<HH', data, 0x0A)
    cs = struct.unpack_from('<H', data, 0x16)[0]
    seg = hdr + cs * 16
    ip, rcs, _mem, size, sp, ss, dest = struct.unpack_from('<7H', data, seg)
    hlen = 16 if data[seg + 14:seg + 16] == b'RB' else 18
    skip = struct.unpack_from('<H', data, seg + 14)[0] if hlen == 18 else 1

    # The unpacker works in place, and the packer only emits streams where
    # the output never overtakes unread input. Reading from an untouched copy
    # gives the same bytes for every such file, and cannot corrupt itself on
    # one that is not.
    packed = data[hdr:seg]
    out = bytearray(max(dest * 16, len(packed)))
    out[:len(packed)] = packed
    src = len(packed)
    dst = (dest - (skip - 1)) * 16 if skip else dest * 16
    while src > 0 and packed[src - 1] == 0xFF:
        src -= 1
    while True:
        if src < 3:
            raise ValueError('EXEPACK stream runs off the start of the image')
        cmd = packed[src - 1]
        n = packed[src - 2] << 8 | packed[src - 3]
        src -= 3
        op = cmd & 0xFE
        if op == 0xB0:
            fill = packed[src - 1]
            src -= 1
            out[dst - n:dst] = bytes([fill]) * n
            dst -= n
        elif op == 0xB2:
            out[dst - n:dst] = packed[src - n:src]
            src -= n
            dst -= n
        else:
            raise ValueError(f'unknown EXEPACK command {cmd:02X} at {src + 2:X}')
        if cmd & 1:
            break
    module = bytes(out[:dest * 16])

    # The relocation table follows the unpacker's error message.
    stub = data[seg:seg + size]
    at = stub.find(MESSAGE)
    if at < 0:
        raise ValueError('EXEPACK unpacker without its message; unknown version')
    p = at + len(MESSAGE)
    relocs = []
    for frame in range(16):
        count = struct.unpack_from('<H', stub, p)[0]
        p += 2
        for k in range(count):
            off = struct.unpack_from('<H', stub, p + 2 * k)[0]
            relocs.append((off, frame * 0x1000))
        p += 2 * count

    # A fresh MZ: 512-byte header, the relocations, the module.
    nhdr = (0x1C + 4 * len(relocs) + 511) // 512 * 512
    total = nhdr + len(module)
    # Memory the packed program asked for beyond its file, minus what the
    # module grew by unpacking.
    grown = (len(module) - len(packed)) // 16
    min_extra = max(0, min_extra - grown) if min_extra else 0
    h = struct.pack('<2s13H', b'MZ', total % 512, (total + 511) // 512, len(relocs),
                    nhdr // 16, min_extra, max_extra, ss, sp, 0, ip, rcs, 0x1C, 0)
    out = bytearray(h)
    for off, s in relocs:
        out += struct.pack('<HH', off, s)
    out += bytes(nhdr - len(out))
    return bytes(out + module)


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    data = open(argv[1], 'rb').read()
    out = unpack(data)
    open(argv[2], 'wb').write(out)
    nrel, hdr = struct.unpack_from('<H', out, 6)[0], struct.unpack_from('<H', out, 8)[0]
    ip, cs = struct.unpack_from('<HH', out, 0x14)
    print(f'{argv[1]}: {len(data)} -> {len(out)} bytes, {nrel} relocations, '
          f'entry {cs:04X}:{ip:04X}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
