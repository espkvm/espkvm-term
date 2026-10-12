#!/usr/bin/env python3
"""Talk to the debug link: `shot.py out.png [full]`, `shot.py tap X Y`, `shot.py chr TEXT`, `shot.py key N`.
The port is /dev/ttyACM0, or the one in TAB5_PORT."""
import os, struct, sys, time, zlib

import serial

PORT = os.environ.get('TAB5_PORT', '/dev/ttyACM0')


def png(path, w, h, rgb565):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            (p,) = struct.unpack_from('<H', rgb565, (y * w + x) * 2)
            raw += bytes(((p >> 8) & 0xF8, (p >> 3) & 0xFC, (p << 3) & 0xF8))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def main():
    # The USB serial resets the chip on RTS high + DTR low. The port opens with
    # both high and pyserial sets DTR first, so keep DTR high and drop only RTS.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.5
    s.dtr = True
    s.rts = False
    s.open()
    if sys.argv[1] in ('tap', 'chr', 'key'):
        s.write((' '.join(sys.argv[1:]) + '\n').encode())
        time.sleep(0.3)
        return
    s.reset_input_buffer()
    s.write(b'shot 1\n' if 'full' in sys.argv else b'shot\n')
    buf = b''
    end = time.time() + 30
    while b'\nENDSHOT\n' not in buf and time.time() < end:
        buf += s.read(65536)
    i = buf.find(b'SHOT ')
    j = buf.find(b'\n', i)
    w, h = map(int, buf[i + 5:j].split())
    data = buf[j + 1:j + 1 + w * h * 2]
    png(sys.argv[1] if sys.argv[1] != 'full' else sys.argv[2], w, h, data)
    print(w, h)


main()
