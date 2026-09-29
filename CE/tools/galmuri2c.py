#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
r"""
Galmuri (BDF) を PopPCE の UI フォント (app/ce_bmpfont.c) 用の C ヘッダへ
変換するビルド前処理スクリプト。姉妹アプリ (TCPMP をもとにしたメディア
プレーヤーの移植、the sister TCPMP-based media player port) の
galmuri2c.py を元に、16 ドット(東雲16)の UI 向けに作り直したもの。

    python3 CE/tools/galmuri2c.py 14      # -> CE/app/ce_galmuri14.h
    python3 CE/tools/galmuri2c.py 11      # -> CE/app/ce_galmuri11.h

入力: ../../galmuri/dist/{Galmuri14,GalmuriMono11}.bdf
  (https://github.com/quiple/galmuri, SIL OFL-1.1, Reserved Font Name 指定なし)
出力はコミットして使う (CE 側のビルドでは Python を実行しない)。

- 14: Galmuri14。等幅版が無いので英数字はプロポーショナル (5〜15px)。
  漢字・かなは 15px 送り。
- 11: 等幅版 GalmuriMono11 (TCPMP の姉妹アプリと同じ)。英数字・半角カナ 6px、
  漢字・かな 12px 送り。
- 収録する字は app/ce_shinonome16.h と同じ集合 (ASCII 95 + 半角カナ 63 +
  全角表 6879)。表示できる字は東雲版と変わらない。Galmuri に無い字 (または
  字形が空の字) は東雲16の字形で埋める (生成時フォールバック。TCPMP の姉妹アプリと同じ)。
- 縦方向: 行数 ROWS の枠に、BDF の行 TOP 以降を切り出す。ce_bmpfont.h の
  CE_BMPFONT_HEIGHT (16) はそのまま使う。
    14: ROWS=18, TOP=3。漢字は 1..13 行目、英字のベースラインは 14 行目。
        g j p q y の下の出っ張りだけ 16..17 行目 (枠の2行下) に出る。
        (最初は TOP=4 で漢字 0..12 行目だったが、実機で東雲より上に寄って
        見えたので 2026-09-27 に1行下げた。ce_bmpfont.h の
        CE_BMPFONT_ROW_EXTRA でファイル選択の行を1ドット高くしてある)
    11: ROWS=16, TOP=0。BDF の字の枠 (16 行) をそのまま使う。漢字は
        3..13 行目、英字のベースラインは 12 行目、下の出っ張りは 14 行目まで。
  東雲のフォールバックの字は 0..15 行目にそのまま置く。
- 各字: codepoint、left (字形の左端の送り位置からのずれ、負なら前の字に
  はみ出す)、advance (送り幅)、rows (各行 16bit、bit15 = 左端)。
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CE_DIR = HERE.parent
GALMURI = CE_DIR.parent.parent / "galmuri" / "dist"
SHIN = CE_DIR / "app" / "ce_shinonome16.h"

CONF = {
    "14": dict(bdf="Galmuri14.bdf", name="Galmuri14", rows=18, top=3),
    "11": dict(bdf="GalmuriMono11.bdf", name="GalmuriMono11", rows=16, top=0),
}


def parse_bdf(path):
    glyphs = {}
    ascent = None
    cur = None
    bm = None
    for line in open(path, encoding="utf-8"):
        t = line.split()
        if not t:
            continue
        if t[0] == "FONT_ASCENT":
            ascent = int(t[1])
        elif t[0] == "ENCODING":
            cur = {"cp": int(t[1])}
        elif t[0] == "DWIDTH":
            cur["dw"] = int(t[1])
        elif t[0] == "BBX":
            cur["bbx"] = tuple(map(int, t[1:5]))
        elif t[0] == "BITMAP":
            bm = []
        elif t[0] == "ENDCHAR":
            cur["bm"] = bm
            glyphs[cur["cp"]] = cur
            bm = None
        elif bm is not None:
            bm.append(t[0])
    return ascent, glyphs


def parse_shinonome(path):
    """{cp: (advance, rows16)}。rows は bit15 = 左端にそろえる。"""
    src = Path(path).read_text(encoding="utf-8")
    out = {}
    asc = [[int(x, 16) for x in m.group(1).split(", ")]
           for m in re.finditer(r"^\s*\{ ((?:0x[0-9A-F]{2}, ){15}0x[0-9A-F]{2}) \}, /\* 0x", src, re.M)]
    hk = [[int(x, 16) for x in m.group(1).split(", ")]
          for m in re.finditer(r"^\s*\{ ((?:0x[0-9A-F]{2}, ){15}0x[0-9A-F]{2}) \}, /\* U\+", src, re.M)]
    assert len(asc) == 0x7E - 0x20 + 1, len(asc)
    assert len(hk) == 0xFF9F - 0xFF61 + 1, len(hk)
    for i, r in enumerate(asc):
        out[0x20 + i] = (8, [v << 8 for v in r])
    for i, r in enumerate(hk):
        out[0xFF61 + i] = (8, [v << 8 for v in r])
    for m in re.finditer(r"\{ 0x([0-9A-F]{4}), \{ ([^}]*) \} \}", src):
        cp = int(m.group(1), 16)
        assert cp not in out, hex(cp)
        out[cp] = (16, [int(x, 16) for x in m.group(2).split(", ")])
    return out


def rasterize(g, ascent, rows, top):
    """BDF の字を (left, advance, rows[], clipped) にする。"""
    w, h, xo, yo = g["bbx"]
    left = min(0, xo)
    assert w + xo - left <= 16, g
    out = [0] * rows
    clipped = False
    for i, hx in enumerate(g["bm"]):
        bits = int(hx, 16)
        nb = len(hx) * 4
        r = ascent - (yo + h) + i - top
        line = 0
        for x in range(w):
            if (bits >> (nb - 1 - x)) & 1:
                line |= 1 << (15 - (x + xo - left))
        if not line:
            continue
        if 0 <= r < rows:
            out[r] = line
        else:
            clipped = True
    return left, g["dw"], out, clipped


def cchar(cp):
    c = chr(cp)
    if c in "\\*/" or cp < 0x20 or 0xD800 <= cp <= 0xDFFF:
        return "?"
    return c


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in CONF:
        sys.exit("usage: galmuri2c.py 14|11")
    size = sys.argv[1]
    c = CONF[size]
    rows, top = c["rows"], c["top"]
    ascent, gl = parse_bdf(GALMURI / c["bdf"])
    shin = parse_shinonome(SHIN)
    out = CE_DIR / "app" / ("ce_galmuri%s.h" % size)

    fallback = []
    clipped = []
    entries = []
    for cp in sorted(shin):
        g = gl.get(cp)
        sadv, srows = shin[cp]
        got = None
        if g is not None:
            left, adv, r, clip = rasterize(g, ascent, rows, top)
            # 字形が空なのに東雲では字がある = Galmuri 側に実質無い字
            if any(r) or not any(srows) or clip:
                got = (left, adv, r)
                if clip:
                    clipped.append(cp)
        if got is None:
            fallback.append(cp)
            got = (0, sadv, (srows + [0] * rows)[:rows])
        entries.append((cp,) + got)

    assert [e[0] for e in entries[:95]] == list(range(0x20, 0x7F))

    L = []
    L.append("/* AUTO-GENERATED by CE/tools/galmuri2c.py %s - DO NOT EDIT BY HAND. */" % size)
    L.append("/*")
    L.append(" * Glyph data converted from %s (BDF), part of the Galmuri font" % c["name"])
    L.append(" * family: Copyright (c) 2019-2025 Lee Minseo (quiple@quiple.dev),")
    L.append(" * https://github.com/quiple/galmuri - SIL Open Font License 1.1.")
    L.append(" * Glyphs missing from Galmuri are filled from Shinonome 16dot")
    L.append(" * (app/ce_shinonome16.h). Selected in app/ce_bmpfont.c by")
    L.append(" * -DCE_FONT_GALMURI%s (make CE_FONT=galmuri%s)." % (size, size))
    L.append(" *")
    L.append(" * Sorted by codepoint; entries 0..94 are ASCII 0x20..0x7E. rows[] are")
    L.append(" * bit15 = leftmost pixel, drawn at (pen x + left); the pen then moves")
    L.append(" * by advance. Row 0 is the top of the CE_BMPFONT_HEIGHT cell.")
    L.append(" */")
    L.append("#ifndef CE_GALMURI_FONT_H")
    L.append("#define CE_GALMURI_FONT_H")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("#define CE_GALMURI_ROWS %d" % rows)
    L.append("")
    L.append("typedef struct { uint16_t codepoint; int8_t left; uint8_t advance; uint16_t rows[CE_GALMURI_ROWS]; } CeGalmuriGlyph;")
    L.append("")
    L.append("#define CE_GALMURI_COUNT %d" % len(entries))
    L.append("static const CeGalmuriGlyph s_ceGalmuriGlyph[CE_GALMURI_COUNT] = {")
    for cp, left, adv, r in entries:
        L.append("    { 0x%04X, %d, %d, { %s } }, /* %s */"
                 % (cp, left, adv, ", ".join("0x%04X" % v for v in r), cchar(cp)))
    L.append("};")
    L.append("")
    L.append("#endif /* CE_GALMURI_FONT_H */")
    out.write_text("\n".join(L) + "\n", encoding="utf-8")
    print("wrote %s: %d glyphs, shinonome fallback=%d, clipped=%d"
          % (out, len(entries), len(fallback), len(clipped)))
    print("fallback: " + "".join(cchar(x) for x in fallback))
    print("fallback cps: " + " ".join("%04X" % x for x in fallback))
    if clipped:
        print("clipped: " + "".join(cchar(x) for x in clipped))


if __name__ == "__main__":
    main()
