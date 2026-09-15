#!/usr/bin/env python3
"""
Weft Spec — final assembly: merge cover + body into one PDF.
Normalizes cover page size to match body's A4 dimensions.
"""
import sys
from pypdf import PdfReader, PdfWriter
from pypdf.generic import RectangleObject

COVER = '/home/z/my-project/scripts/weft_cover.pdf'
BODY  = '/home/z/my-project/scripts/weft_body.pdf'
OUT   = '/home/z/my-project/download/Weft-Specification-v0.1.pdf'

writer = PdfWriter()

# Body first to get the canonical A4 size
body = PdfReader(BODY)
A4_W = float(body.pages[0].mediabox.width)
A4_H = float(body.pages[1].mediabox.height)
print(f'Body A4 target: {A4_W:.2f} x {A4_H:.2f} pt')

# Cover — normalize mediabox to match body
cover = PdfReader(COVER)
cover_page = cover.pages[0]
cw = float(cover_page.mediabox.width)
ch = float(cover_page.mediabox.height)
print(f'Cover original: {cw:.2f} x {ch:.2f} pt')

# Set cover mediabox to exact A4 (slight crop of <1pt — invisible)
cover_page.mediabox = RectangleObject((0, 0, A4_W, A4_H))
cover_page.cropbox = RectangleObject((0, 0, A4_W, A4_H))
writer.add_page(cover_page)

# Body pages
for page in body.pages:
    writer.add_page(page)

# Metadata
writer.add_metadata({
    '/Title':    'Weft Specification v0.1',
    '/Author':   'Zephyr (zephyr4289)',
    '/Subject':  'A specification for the continuous-state plane in declarative UI frameworks',
    '/Creator':  'Weft spec generator',
    '/Producer': 'Z.ai PDF pipeline',
    '/Keywords': 'weft, declarative UI, zero-copy, off-heap, 120 FPS, Triad Protocol, Steward, Heddle',
})

with open(OUT, 'wb') as f:
    writer.write(f)

print(f'Final PDF: {OUT}')
print(f'Pages: {len(writer.pages)}')
