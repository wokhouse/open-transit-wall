#!/usr/bin/env python3
"""Outline the text in assets/logo.svg so the logo depends on no installed fonts.

The SVG from the designer carries live <text> nodes in Helvetica Neue and
Helvetica LT Std Black Oblique — any machine without those faces (CI, the
Pages build, a stranger's browser) renders the wordmark wrong. This script
replaces each text node with the <path> outlines pulled from the actual
font files, keeping the original positions, sizes, and group transforms.

One-shot: the outlined SVG is the tracked asset from here on. Re-run only
if the text in the source SVG changes.

Usage: uvx --from fonttools python3 tools/svg_outline_text.py [svg]
"""
import re
import sys
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SRC = REPO / "assets/logo.svg"

WORDMARK_OTF = Path.home() / "Library/Fonts/Helvetica LT Std Black Oblique.otf"
REGMARK_TTC = Path("/System/Library/Fonts/HelveticaNeue.ttc")

TEXT_RE = re.compile(
    r'<text x="([\d.]+)px" y="([\d.]+)px"[^>]*>(.*?)</text>', re.S)
FILL_RE = re.compile(r"fill:(#[0-9a-fA-F]+)")
TSPAN_RE = re.compile(r'<tspan x="([^"]*)"[^>]*y="[^"]*"\s*>([^<]*)</tspan>')


def svg_outline(font, ch, size, pen_x, pen_y):
    """Outline one char at pen position (pen_x, pen_y) in SVG user units
    (baseline-left), y flipped like the renderer does."""
    cmap = font.getBestCmap()
    glyph_set = font.getGlyphSet()
    upem = font["head"].unitsPerEm
    name = cmap[ord(ch)]
    path_pen = SVGPathPen(glyph_set)
    glyph_set[name].draw(path_pen)
    cmds = path_pen.getCommands()
    # font units -> px, flip y, move baseline-left origin to (pen_x, pen_y)
    m = size / upem
    return (f'transform="translate({pen_x} {pen_y}) scale({m:.6f} '
            f'{-m:.6f})" d="{cmds}"')


def wordmark_path(text_svg):
    """Outline the 'otw' wordmark: first char at the text x/y, tspan chars
    at their explicit x positions (the designer's manual kerning)."""
    font = TTFont(WORDMARK_OTF)
    m = TEXT_RE.search(text_svg)
    x0, y0, body = float(m.group(1)), float(m.group(2)), m.group(3)
    fill = FILL_RE.search(m.group(0)).group(1)
    size = 117.167  # font-size from the style attr; keep in sync if edited
    parts = [svg_outline(font, body[0], size, x0, y0)]
    head = body[: body.index("<tspan")]
    for xs_attr, chars in TSPAN_RE.findall(body):
        # designer's x list carries a px suffix per value
        xs = [float(v.rstrip("px")) for v in xs_attr.split()]
        assert len(xs) == len(chars), "tspan x list must match chars"
        for ch, x in zip(chars, xs, strict=False):
            parts.append(svg_outline(font, ch, size, x, y0))
    assert head.strip() == body[0], "unexpected wordmark structure"
    return "".join(f'<path style="fill:{fill};" {p}/>' for p in parts)


def regmark_path(text_svg):
    # face 0 of the .ttc is "Helvetica Neue Regular", the face the SVG asks for
    font = TTFont(REGMARK_TTC, fontNumber=0)
    m = TEXT_RE.search(text_svg)
    x0, y0, body = float(m.group(1)), float(m.group(2)), m.group(3)
    fill = FILL_RE.search(m.group(0)).group(1)
    size = 16.61
    return f'<path style="fill:{fill};" {svg_outline(font, body, size, x0, y0)}/>'


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    svg = src.read_text()

    texts = list(TEXT_RE.finditer(svg))
    assert len(texts) == 2, f"expected 2 text nodes, found {len(texts)}"

    # Replace right-to-left so earlier offsets stay valid.
    for m in reversed(texts):
        body = m.group(3)
        if body == "®":
            repl = regmark_path(m.group(0))
        elif body.startswith("o") and "tw" in body:
            repl = wordmark_path(m.group(0))
        else:
            sys.exit(f"unrecognized text node: {body!r}")
        svg = svg[: m.start()] + repl + svg[m.end():]

    # The empty serif-export group and the serif namespace are now dead.
    svg = re.sub(r"<g transform=\"matrix\([^)]*\)\"></g>", "", svg)
    svg = svg.replace(' xmlns:serif="http://www.serif.com/"', "")

    src.write_text(svg)
    print(f"outlined text in {src} -> {len(svg)} bytes, "
          f"paths: {svg.count('<path')}")


if __name__ == "__main__":
    main()
