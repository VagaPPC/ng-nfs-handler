#!/usr/bin/env python3
"""Legge e riscrive i tooltype di un'icona Amiga (.info) da Linux.

  icontool.py list ICON.info
  icontool.py set  ICON.info OUT.info TOOLTYPE...   (lista vuota = nessuno)

Formato: struct DiskObject (78 byte), eventuale DrawerData, Image normale e
selezionata, DefaultTool, ToolTypes, ToolWindow, DrawerData2, poi l'eventuale
blocco IFF FORM ICON (GlowIcons OS3.5+/OS4), conservato com'e'. Le righe
NewIcons (IM1=, IM2=, "*** DON'T EDIT") restano in fondo alla lista.
"""
import struct
import sys


def _image_size(buf, off):
    w, h, d = struct.unpack_from('>hhh', buf, off + 4)
    data = struct.unpack_from('>I', buf, off + 10)[0]
    size = 20
    if data:
        size += ((w + 15) >> 4) * 2 * h * d
    return size


def _string(buf, off):
    n = struct.unpack_from('>I', buf, off)[0]
    return buf[off + 4:off + 4 + n].split(b'\0', 1)[0], off + 4 + n


def parse(buf):
    if struct.unpack_from('>HH', buf, 0) != (0xE310, 1):
        raise ValueError('non e\' un\'icona Amiga')
    flags = struct.unpack_from('>H', buf, 4 + 12)[0]
    gad_render, sel_render = struct.unpack_from('>II', buf, 4 + 18)
    userdata = struct.unpack_from('>I', buf, 4 + 40)[0]
    (deftool, tooltypes, _cx, _cy, drawer, toolwin,
     _stack) = struct.unpack_from('>IIiiIII', buf, 50)
    off = 78
    if drawer:
        off += 56
    if gad_render:
        off += _image_size(buf, off)
    if sel_render and (flags & 0x0002):          # GFLG_GADGHIMAGE
        off += _image_size(buf, off)
    if deftool:
        _, off = _string(buf, off)
    tt_start = off
    tts = []
    if tooltypes:
        count = struct.unpack_from('>I', buf, off)[0] // 4 - 1
        off += 4
        for _ in range(count):
            s, off = _string(buf, off)
            tts.append(s)
    return tt_start, off, tts, bool(tooltypes)


def is_newicon(t):
    return t.startswith((b'IM1=', b'IM2=')) or t.startswith(b'*** DON')


def encode_tooltypes(tts):
    out = struct.pack('>I', (len(tts) + 1) * 4)
    for t in tts:
        out += struct.pack('>I', len(t) + 1) + t + b'\0'
    return out


def main(argv):
    if len(argv) >= 3 and argv[1] == 'list':
        buf = open(argv[2], 'rb').read()
        for t in parse(buf)[2]:
            print(t.decode('latin-1'))
        return 0
    if len(argv) >= 4 and argv[1] == 'set':
        buf = bytearray(open(argv[2], 'rb').read())
        start, end, old, _ = parse(buf)
        new = [a.encode('latin-1') for a in argv[4:]]
        new += [t for t in old if is_newicon(t)]
        body = encode_tooltypes(new) if new else b''
        out = bytearray(buf[:start]) + body + buf[end:]
        struct.pack_into('>I', out, 54, 0x00DEAD00 if new else 0)
        open(argv[3], 'wb').write(out)
        assert parse(out)[2] == new
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
