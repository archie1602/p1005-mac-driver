#!/usr/bin/env python3
"""Generate a 2-page vector test PDF for the P1005 driver (stdlib only).

Page 1: margin frame, centre crosshair, text sizes, gray steps, gradient,
        line widths.  Page 2: large page number (multi-page / order check).

Usage: python3 -I make_testpage.py OUT.pdf [WIDTH_PT HEIGHT_PT]
Default size is A4 (595 x 842 pt).
"""
import sys
import time


def esc(s):
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def text(x, y, size, s, font="F1"):
    return f"BT /{font} {size} Tf {x:.2f} {y:.2f} Td ({esc(s)}) Tj ET\n"


def page1(w, h):
    c = []
    inch = 72.0
    m = 0.3 * inch  # frame inset: inside HP's 0.25"/0.2" unprintable margins
    c.append("0 G 1 w\n")
    c.append(f"{m:.2f} {m:.2f} {w - 2*m:.2f} {h - 2*m:.2f} re S\n")
    # centre crosshair
    cx, cy = w / 2, h / 2
    c.append(f"{cx-20:.2f} {cy:.2f} m {cx+20:.2f} {cy:.2f} l S {cx:.2f} {cy-20:.2f} m {cx:.2f} {cy+20:.2f} l S\n")
    # corner tick labels
    c.append(text(m + 4, h - m - 12, 8, "top-left: frame is 0.30 in from each paper edge"))
    c.append(text(m + 4, m + 4, 8, "bottom-left"))

    y = h - 1.0 * inch
    c.append(text(0.6 * inch, y, 20, "HP LaserJet P1005 - native Apple Silicon driver", "F2"))
    y -= 22
    c.append(text(0.6 * inch, y, 10, "Test page generated " + time.strftime("%Y-%m-%d %H:%M:%S")
                  + "  |  page size %.0f x %.0f pt" % (w, h)))
    y -= 30
    for size in (6, 8, 10, 12, 16, 24):
        sample = "The quick brown fox jumps" if size > 16 else "The quick brown fox jumps over the lazy dog 0123456789"
        c.append(text(0.6 * inch, y, size, f"{size} pt: {sample}"))
        y -= size * 1.5 + 4

    # gray steps 0..100 %
    y -= 20
    c.append(text(0.6 * inch, y + 48, 10, "Gray steps (percent black):", "F2"))
    bw = (w - 1.2 * inch) / 11
    for i in range(11):
        g = 1.0 - i / 10.0
        x = 0.6 * inch + i * bw
        c.append(f"{g:.2f} g {x:.2f} {y:.2f} {bw - 4:.2f} 40 re f\n")
        c.append("0 g\n" + text(x + 4, y - 12, 8, f"{i*10}%"))

    # smooth gradient
    y -= 70
    c.append(text(0.6 * inch, y + 46, 10, "Gradient (white to black):", "F2"))
    n = 256
    gw = (w - 1.2 * inch) / n
    for i in range(n):
        g = 1.0 - i / (n - 1)
        x = 0.6 * inch + i * gw
        c.append(f"{g:.4f} g {x:.3f} {y:.2f} {gw + 0.05:.3f} 40 re f\n")
    c.append("0 g\n")

    # line widths
    y -= 50
    c.append(text(0.6 * inch, y, 10, "Line widths: 0.25, 0.5, 1, 2 pt", "F2"))
    for i, lw in enumerate((0.25, 0.5, 1, 2)):
        yy = y - 16 - i * 12
        c.append(f"{lw} w {0.6*inch:.2f} {yy:.2f} m {w - 0.6*inch:.2f} {yy:.2f} l S\n")
    c.append("1 w\n")

    # fine checker (1 pt cells) to show resolution
    y -= 90
    c.append(text(0.6 * inch, y + 4, 10, "1 pt checkerboard:", "F2"))
    for r in range(24):
        for k in range(48):
            if (r + k) % 2 == 0:
                c.append(f"{0.6*inch + 120 + k:.2f} {y + r - 10:.2f} 1 1 re f\n")
    return "".join(c)


def page2(w, h):
    return text(w / 2 - 110, h / 2, 96, "Page 2", "F2") + text(72, 72, 12, "Multi-page test: this should be the second sheet.")


def build(path, w, h):
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)

    f1 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    f2 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>")
    contents = [page1(w, h), page2(w, h)]
    content_ids = []
    for s in contents:
        data = s.encode("latin-1")
        content_ids.append(add(f"<< /Length {len(data)} >>\nstream\n".encode("latin-1") + data + b"\nendstream"))
    page_ids = []
    pages_id = len(objs) + len(contents) + 1
    for cid in content_ids:
        page_ids.append(add(f"<< /Type /Page /Parent {pages_id} 0 R /MediaBox [0 0 {w} {h}] "
                            f"/Resources << /Font << /F1 {f1} 0 R /F2 {f2} 0 R >> >> /Contents {cid} 0 R >>"))
    assert add("<< /Type /Pages /Kids [" + " ".join(f"{p} 0 R" for p in page_ids)
               + f"] /Count {len(page_ids)} >>") == pages_id
    cat = add(f"<< /Type /Catalog /Pages {pages_id} 0 R >>")

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        if isinstance(body, str):
            body = body.encode("latin-1")
        out += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objs)+1}\n0000000000 65535 f \n".encode()
    for o in offsets:
        out += f"{o:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objs)+1} /Root {cat} 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    with open(path, "wb") as fp:
        fp.write(out)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    w, h = (595, 842) if len(sys.argv) == 2 else (float(sys.argv[2]), float(sys.argv[3]))
    build(sys.argv[1], w, h)
