"""Regenerate tiny original test assets using only the Python standard library."""
from pathlib import Path
import struct
import zlib

root = Path(__file__).resolve().parent
svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">'
       '<rect x="2" y="2" width="8" height="12" fill="#ff0000"/>'
       '<rect x="10" y="10" width="4" height="4" fill="#0000ff"/>'
       '<rect x="10" y="2" width="4" height="4" fill="#00ff00" opacity="0.5"/></svg>')
(root / 'artwork.svg').write_bytes(svg.encode())  # Deliberately ends in </svg>.
(root / 'commented.svg').write_bytes(('\ufeff<?xml version="1.0"?>\n'
    '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">\n'
    '<!-- preamble -->\n' + svg + '\n<!-- trailing comment -->\n').encode())
(root / 'alternate.svg').write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect x="2" y="2" width="12" height="12" fill="#ffff00"/></svg>\n')
(root / 'broken.svg').write_bytes(b'This is not SVG XML, despite its suffix.</svg>')
(root / 'binary.bin').write_bytes(b'PE\x00resource\xff\x00\x80')

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

rows = bytearray()
for y in range(16):
    rows.append(0)  # PNG's "None" row filter.
    for x in range(16):
        rgba = (0, 0, 0, 0)
        if 2 <= x < 10 and 2 <= y < 14:
            rgba = (255, 0, 0, 255)
        elif 10 <= x < 14 and 10 <= y < 14:
            rgba = (0, 0, 255, 255)
        elif 10 <= x < 14 and 2 <= y < 6:
            rgba = (0, 255, 0, 128)
        rows.extend(rgba)
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
(root / 'artwork.png').write_bytes(png)
