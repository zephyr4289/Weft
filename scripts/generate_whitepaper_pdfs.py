#!/usr/bin/env python3
"""
scripts/generate_whitepaper_pdfs.py — Generate high-quality publication PDFs
for all 3 Weft Whitepaper Volumes using PyMuPDF Story + Markdown.
"""

import os
import sys
import re
import markdown
import pymupdf
from pygments.formatters import HtmlFormatter

CSS = """
@page {
    size: A4;
    margin: 36pt;
}

body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    font-size: 9.5pt;
    line-height: 1.45;
    color: #1a202c;
    background-color: #ffffff;
}

h1 {
    font-size: 20pt;
    font-weight: 700;
    color: #0f2d59;
    border-bottom: 2pt solid #0f2d59;
    padding-bottom: 4pt;
    margin-top: 14pt;
    margin-bottom: 8pt;
}

h2 {
    font-size: 14pt;
    font-weight: 600;
    color: #1e3a8a;
    border-bottom: 1pt solid #cbd5e1;
    padding-bottom: 3pt;
    margin-top: 12pt;
    margin-bottom: 6pt;
}

h3 {
    font-size: 11.5pt;
    font-weight: 600;
    color: #1e40af;
    margin-top: 10pt;
    margin-bottom: 4pt;
}

h4, h5, h6 {
    font-size: 10pt;
    font-weight: 600;
    color: #334155;
    margin-top: 8pt;
    margin-bottom: 3pt;
}

p {
    margin-top: 0;
    margin-bottom: 6pt;
    text-align: justify;
}

blockquote {
    border-left: 3pt solid #2563eb;
    background-color: #f1f5f9;
    padding: 6pt 10pt;
    margin: 6pt 0;
    color: #334155;
    font-size: 9pt;
}

table {
    width: 100%;
    border-collapse: collapse;
    margin: 8pt 0;
    font-size: 8pt;
}

th, td {
    border: 0.5pt solid #cbd5e1;
    padding: 3.5pt 5pt;
    text-align: left;
    vertical-align: top;
}

th {
    background-color: #f8fafc;
    font-weight: 600;
    color: #0f172a;
    border-bottom: 1.5pt solid #94a3b8;
}

tr:nth-child(even) td {
    background-color: #f8fafc;
}

pre {
    background-color: #f8fafc;
    border: 0.5pt solid #cbd5e1;
    border-left: 2.5pt solid #0284c7;
    padding: 6pt 8pt;
    margin: 6pt 0;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    font-size: 7.5pt;
    line-height: 1.35;
    white-space: pre-wrap;
    word-break: break-all;
}

code {
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    font-size: 8pt;
    background-color: #f1f5f9;
    padding: 1pt 3pt;
    color: #0f172a;
    border: 0.5pt solid #e2e8f0;
}

pre code {
    background-color: transparent;
    padding: 0;
    border: none;
    font-size: 7.5pt;
}

ul, ol {
    margin-top: 0;
    margin-bottom: 6pt;
    padding-left: 16pt;
}

li {
    margin-bottom: 2pt;
}

hr {
    border: none;
    border-top: 0.5pt solid #e2e8f0;
    margin: 10pt 0;
}
"""

def generate_pdf_for_markdown(input_md_path, output_pdf_path, title="Weft Technical Whitepaper"):
    print(f"Reading {input_md_path}...")
    with open(input_md_path, "r", encoding="utf-8") as f:
        md_content = f.read()

    # Convert Markdown to HTML
    html_body = markdown.markdown(
        md_content,
        extensions=[
            "tables",
            "fenced_code",
            "codehilite",
            "sane_lists",
            "toc"
        ]
    )

    full_html = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>{title}</title>
</head>
<body>
{html_body}
</body>
</html>"""

    # Use PyMuPDF Story
    story = pymupdf.Story(html=full_html, user_css=CSS)
    writer = pymupdf.DocumentWriter(output_pdf_path)
    
    # A4 dimensions: 595.32 x 841.92
    page_rect = pymupdf.paper_rect("a4")
    margin_x = 36.0
    margin_y = 36.0
    body_rect = pymupdf.Rect(margin_x, margin_y, page_rect.width - margin_x, page_rect.height - margin_y)

    page_count = 0
    more = True
    while more:
        dev = writer.begin_page(page_rect)
        more, _ = story.place(body_rect)
        story.draw(dev)
        writer.end_page()
        page_count += 1

    writer.close()
    file_size_kb = os.path.getsize(output_pdf_path) / 1024
    print(f"Generated {output_pdf_path}: {page_count} pages ({file_size_kb:.1f} KB)")
    return page_count

def main():
    source_dir = "/sdcard/larp/Warp/White papers md"
    dest_dir = "/sdcard/Download"
    os.makedirs(dest_dir, exist_ok=True)

    volumes = [
        (
            os.path.join(source_dir, "volume-I-core-foundations.md"),
            os.path.join(dest_dir, "Weft-Whitepaper-Volume-I.pdf"),
            "Weft Technical Whitepaper — Volume I: Core Foundations & Concurrency Invariants"
        ),
        (
            os.path.join(source_dir, "Weft-Whitepaper-Vol-II.md"),
            os.path.join(dest_dir, "Weft-Whitepaper-Volume-II.pdf"),
            "Weft Technical Whitepaper — Volume II: Hardware Acceleration, GPU Rings & IPC"
        ),
        (
            os.path.join(source_dir, "weft-whitepaper-volume-3.md"),
            os.path.join(dest_dir, "Weft-Whitepaper-Volume-III.pdf"),
            "Weft Technical Whitepaper — Volume III: Cross-Platform Runtimes, Cadence Governance & Zero-GC"
        ),
    ]

    for md_file, pdf_file, title in volumes:
        if not os.path.exists(md_file):
            print(f"Error: {md_file} not found!", file=sys.stderr)
            continue
        generate_pdf_for_markdown(md_file, pdf_file, title)

if __name__ == "__main__":
    main()
