/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#include "ce_bmpfont.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(CE_FONT_GALMURI14) || defined(CE_FONT_GALMURI11)
/* Galmuri(OFL-1.1)に差し替えたビルド(make CE_FONT=galmuri14 / galmuri11)。
 * どちらも定義しなければ下の #else の東雲16がそのまま使われ、その経路は
 * 差し替え前と1文字も変わらない(Pop 系の他のアプリと共通のコード)。
 *
 * ce_galmuriNN.h は CE/tools/galmuri2c.py の生成物。収録する字は東雲16と
 * 同じ集合で、Galmuri に無い字は生成時に東雲16の字形で埋めてある。
 * 英数字は字ごとに送り幅が違う(Galmuri14 はプロポーショナル)ので、幅は
 * 表の advance で数える。行数は CE_GALMURI_ROWS(Galmuri14 は 18 = 16行の
 * 枠 + g j p q y の下の出っ張り2行)。CE_BMPFONT_HEIGHT(16)は変えない。 */
#if defined(CE_FONT_GALMURI14)
#include "ce_galmuri14.h"
#else
#include "ce_galmuri11.h"
#endif

/* 表はコードポイント昇順で、先頭の95個が ASCII 0x20..0x7E。 */
static const CeGalmuriGlyph *FindGlyph(wchar_t ch)
{
    int lo = 0x7E - 0x20 + 1, hi = CE_GALMURI_COUNT - 1;

    if (ch >= 0x20 && ch <= 0x7E)
        return &s_ceGalmuriGlyph[ch - 0x20];
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint16_t c = s_ceGalmuriGlyph[mid].codepoint;

        if (c == ch)
            return &s_ceGalmuriGlyph[mid];
        if (c < ch)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* 未収録の字を空白で送るときの幅(空白の字の送り幅)。 */
#define MISSING_ADVANCE (s_ceGalmuriGlyph[0].advance)

static int DrawGlyph(HDC hdc, int x, int y, const CeGalmuriGlyph *g, COLORREF fg)
{
    int row, col;

    x += g->left;
    for (row = 0; row < CE_GALMURI_ROWS; row++)
    {
        uint16_t bits = g->rows[row];
        if (!bits)
            continue;
        for (col = 0; col < 16; col++)
            if (bits & (0x8000u >> col))
                SetPixel(hdc, x + col, y + row, fg);
    }
    return g->advance;
}

/* □ U+25A1(東雲16と同じく、ファイル選択での未収録文字の代わり)。 */
static const CeGalmuriGlyph *MissingBoxGlyph(void)
{
    static const CeGalmuriGlyph *s_box = NULL;
    static int s_looked = 0;

    if (!s_looked)
    {
        s_box = FindGlyph(0x25A1);
        s_looked = 1;
    }
    return s_box;
}

static int DrawTextInternal(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg, int boxMissing)
{
    int curX = x;
    const wchar_t *p;

    for (p = text; *p; p++)
    {
        wchar_t ch = *p;
        const CeGalmuriGlyph *g;

        /* サロゲートペアは1文字(東雲16の経路と同じ扱い)。 */
        if (ch >= 0xD800 && ch <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
            p++;

        g = FindGlyph(ch);
        if (!g && boxMissing)
            g = MissingBoxGlyph();
        if (g)
            curX += DrawGlyph(hdc, curX, y, g, fg);
        else
            curX += MISSING_ADVANCE;
    }
    return curX - x;
}

int CeBmpFontDrawTextW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg)
{
    return DrawTextInternal(hdc, x, y, text, fg, 0);
}

int CeBmpFontDrawTextBoxedW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg)
{
    return DrawTextInternal(hdc, x, y, text, fg, 1);
}

int CeBmpFontGetTextWidth(const wchar_t *text)
{
    int w = 0;
    const wchar_t *p;

    for (p = text; *p; p++)
    {
        wchar_t ch = *p;
        const CeGalmuriGlyph *g;

        if (ch >= 0xD800 && ch <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
        {
            p++;
            w += MISSING_ADVANCE;
            continue;
        }
        g = FindGlyph(ch);
        w += g ? g->advance : MISSING_ADVANCE;
    }
    return w;
}

#else /* 東雲16(既定) */
#include "ce_shinonome16.h"

/* s_ceKanjiGlyph16[] は shinonome2c.py の生成時点でコードポイント昇順
 * にソートされている(Python の sorted() をそのまま出力しているため)
 * ので、単純な二分探索でよい。 */
static const CeKanjiGlyph16 *FindKanjiGlyph(uint16_t cp)
{
    int lo = 0, hi = CE_KANJI16_COUNT - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint16_t c = s_ceKanjiGlyph16[mid].codepoint;

        if (c == cp)
            return &s_ceKanjiGlyph16[mid];
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static int DrawGlyph8(HDC hdc, int x, int y, const uint8_t rows[16], COLORREF fg)
{
    int row, col;

    for (row = 0; row < 16; row++)
    {
        uint8_t bits = rows[row];
        if (!bits)
            continue;
        for (col = 0; col < 8; col++)
            if (bits & (0x80 >> col))
                SetPixel(hdc, x + col, y + row, fg);
    }
    return 8;
}

static int DrawGlyph16(HDC hdc, int x, int y, const uint16_t rows[16], COLORREF fg)
{
    int row, col;

    for (row = 0; row < 16; row++)
    {
        uint16_t bits = rows[row];
        if (!bits)
            continue;
        for (col = 0; col < 16; col++)
            if (bits & (0x8000u >> col))
                SetPixel(hdc, x + col, y + row, fg);
    }
    return 16;
}

static int IsHalfWidth(wchar_t ch)
{
    return (ch >= CE_ASCII16_FIRST && ch <= CE_ASCII16_LAST) ||
           (ch >= CE_HALFKANA16_FIRST && ch <= CE_HALFKANA16_LAST);
}

/* □ U+25A1 WHITE SQUARE。東雲16の全角グリフ表に収録されているので
 * それを1回だけ引いてキャッシュする。ファイル選択ダイアログでのみ
 * 未収録文字の置き換えに使う(CeBmpFontDrawTextBoxedW 経由)。 */
static const CeKanjiGlyph16 *MissingBoxGlyph(void)
{
    static const CeKanjiGlyph16 *s_box = NULL;
    static int s_looked = 0;

    if (!s_looked)
    {
        s_box = FindKanjiGlyph(0x25A1);
        s_looked = 1;
    }
    return s_box;
}

static int DrawTextInternal(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg, int boxMissing)
{
    int curX = x;
    const wchar_t *p;

    for (p = text; *p; p++)
    {
        wchar_t ch = *p;

        if (ch >= CE_ASCII16_FIRST && ch <= CE_ASCII16_LAST)
        {
            curX += DrawGlyph8(hdc, curX, y, s_ceAsciiGlyph16[ch - CE_ASCII16_FIRST], fg);
        }
        else if (ch >= CE_HALFKANA16_FIRST && ch <= CE_HALFKANA16_LAST)
        {
            curX += DrawGlyph8(hdc, curX, y, s_ceHalfKanaGlyph16[ch - CE_HALFKANA16_FIRST], fg);
        }
        else
        {
            const CeKanjiGlyph16 *g;

            /* サロゲートペア(BMP外 = U+10000以降の文字)は wchar_t 2個で
             * 1文字。東雲16はBMPしか持たないので必ず未収録になるが、
             * 1文字あたり □ 1個(または空白1個)になるよう低サロゲートを
             * ここで読み飛ばす。末尾の孤立高サロゲートは p[1]==0 で
             * 条件を満たさず、そのまま1個分として扱われる。 */
            if (ch >= 0xD800 && ch <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
                p++;

            g = FindKanjiGlyph((uint16_t)ch);
            if (!g && boxMissing)
                g = MissingBoxGlyph();
            if (g)
                curX += DrawGlyph16(hdc, curX, y, g->rows, fg);
            else
                curX += 8; /* 未収録文字 - tofuの代わりに単純に空白を送る */
        }
    }
    return curX - x;
}

int CeBmpFontDrawTextW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg)
{
    return DrawTextInternal(hdc, x, y, text, fg, 0);
}

int CeBmpFontDrawTextBoxedW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg)
{
    return DrawTextInternal(hdc, x, y, text, fg, 1);
}

int CeBmpFontGetTextWidth(const wchar_t *text)
{
    int w = 0;
    const wchar_t *p;

    for (p = text; *p; p++)
    {
        wchar_t ch = *p;

        if (ch >= 0xD800 && ch <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF)
        {
            p++;      /* サロゲートペア = 1文字(DrawTextInternal と同じ扱い) */
            w += 8;
        }
        else if (IsHalfWidth(ch))
            w += 8;
        else
            w += FindKanjiGlyph((uint16_t)ch) ? 16 : 8;
    }
    return w;
}
#endif /* CE_FONT_GALMURI14 / CE_FONT_GALMURI11 */

void CeBmpFontDrawOwnerButton(const DRAWITEMSTRUCT *dis)
{
    wchar_t text[128];
    int textLen;
    UINT state = DFCS_BUTTONPUSH;
    RECT rc = dis->rcItem;
    int textW, x, y;

    if (dis->itemState & ODS_SELECTED)
        state |= DFCS_PUSHED;
    if (dis->itemState & ODS_DISABLED)
        state |= DFCS_INACTIVE;

    DrawFrameControl(dis->hDC, &rc, DFC_BUTTON, state);

    textLen = GetWindowTextW(dis->hwndItem, text, 128);
    if (textLen > 0)
    {
        textW = CeBmpFontGetTextWidth(text);
        x = rc.left + ((rc.right - rc.left) - textW) / 2;
        y = rc.top + ((rc.bottom - rc.top) - CE_BMPFONT_HEIGHT) / 2;
        if (state & DFCS_PUSHED)
        {
            /* DrawFrameControl() shifts a pushed button's frame down/right
             * by one pixel on this device - keep the label centered inside
             * it instead of centered on the unpushed rcItem. */
            x++;
            y++;
        }
        SetBkMode(dis->hDC, TRANSPARENT);
        CeBmpFontDrawTextW(dis->hDC, x, y, text,
                            GetSysColor((state & DFCS_INACTIVE) ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }

    if (dis->itemState & ODS_FOCUS)
    {
        RECT focusRc = rc;
        InflateRect(&focusRc, -3, -3);
        DrawFocusRect(dis->hDC, &focusRc);
    }
}

/* Scales each RGB channel by num/16 (integer math, no float) - used to
 * darken a theme color for the pressed state below. num<16 darkens. */
static COLORREF ScaleColor(COLORREF c, int num)
{
    int r = (GetRValue(c) * num) / 16;
    int g = (GetGValue(c) * num) / 16;
    int b = (GetBValue(c) * num) / 16;
    return RGB(r, g, b);
}

/* Traces a circular arc as a Polyline() - this coredll exports no
 * Arc()/Pie()/Chord() (checked via `arm-mingw32ce-nm libcoredll.a`), so
 * the power-symbol icon and the sound-wave icon below approximate one
 * with short line segments instead. 0 degrees is straight up from
 * (cx,cy), increasing clockwise (matches how the icons below are
 * described) - math.h sin/cos are already linked project-wide (-lm,
 * see CE/Makefile) and this runs only when a dialog repaints, never
 * per-frame, so the float cost is irrelevant on this ARM soft-float
 * target. */
static void DrawArcPoly(HDC hdc, int cx, int cy, int r, double startDeg, double endDeg, int steps)
{
    POINT pts[33];
    int i;
    double range = endDeg - startDeg;

    if (steps > 32)
        steps = 32;
    for (i = 0; i <= steps; i++)
    {
        double deg = startDeg + range * i / steps;
        double rad = deg * M_PI / 180.0;
        pts[i].x = cx + (int)(r * sin(rad));
        pts[i].y = cy - (int)(r * cos(rad));
    }
    Polyline(hdc, pts, steps + 1);
}

/* Draws one of the CeMenuIcon glyphs (ce_bmpfont.h) as a small outline
 * pictogram, hand-built from GDI primitives rather than a bitmap so it
 * scales/recolors with the theme for free. `box` is the icon's edge
 * length in px; (cx,cy) is its center. Caller must already have `pen`
 * (and, for CE_MENU_ICON_INPUT's two face buttons, a matching solid
 * brush) selected into hdc - this only draws, it doesn't touch pen/
 * brush selection, so CeBmpFontDrawOwnerButtonTheme() below can reuse
 * the same border-color pen it already set up for the RoundRect(). */
static void DrawMenuIcon(HDC hdc, CeMenuIcon icon, int cx, int cy, int box)
{
    int x0 = cx - box / 2, y0 = cy - box / 2, x1 = x0 + box, y1 = y0 + box;

    switch (icon)
    {
    case CE_MENU_ICON_OPEN: /* folder + a small "+" */
    {
        int tabW = box * 4 / 10;
        int bodyTop = y0 + box * 3 / 10;
        Rectangle(hdc, x0, bodyTop, x1, y1);
        Rectangle(hdc, x0, y0, x0 + tabW, bodyTop + 1);
        MoveToEx(hdc, x1 - box / 3, y1 - 2, NULL);
        LineTo(hdc, x1 - box / 3, y1 - 2 - box * 4 / 10);
        MoveToEx(hdc, x1 - box / 3 - box / 5, y1 - 2 - box * 2 / 10, NULL);
        LineTo(hdc, x1 - box / 3 + box / 5, y1 - 2 - box * 2 / 10);
        break;
    }

    case CE_MENU_ICON_SAVE: /* floppy disk */
    {
        int labelW = box * 3 / 5;
        int shutterW = box / 2;
        Rectangle(hdc, x0, y0, x1, y1);
        Rectangle(hdc, cx - labelW / 2, y0 + 2, cx + labelW / 2, y0 + box * 4 / 10);
        Rectangle(hdc, cx - shutterW / 2, y1 - box * 3 / 10, cx + shutterW / 2, y1 - 2);
        break;
    }

    case CE_MENU_ICON_LOAD: /* plain folder (no "+") */
    {
        int tabW = box * 4 / 10;
        int bodyTop = y0 + box * 3 / 10;
        Rectangle(hdc, x0, bodyTop, x1, y1);
        Rectangle(hdc, x0, y0, x0 + tabW, bodyTop + 1);
        break;
    }

    case CE_MENU_ICON_VIDEO: /* monitor on a stand */
    {
        int screenBot = y0 + box * 6 / 10;
        Rectangle(hdc, x0, y0, x1, screenBot);
        MoveToEx(hdc, cx, screenBot, NULL);
        LineTo(hdc, cx, y1 - box / 8);
        MoveToEx(hdc, cx - box / 4, y1 - 1, NULL);
        LineTo(hdc, cx + box / 4, y1 - 1);
        break;
    }

    case CE_MENU_ICON_SOUND: /* speaker cone + two sound-wave arcs */
    {
        int coneW = box * 4 / 10;
        POINT cone[4];
        Rectangle(hdc, x0, y0 + box * 3 / 10, x0 + coneW / 2, y1 - box * 3 / 10);
        cone[0].x = x0 + coneW / 2; cone[0].y = y0 + box * 3 / 10;
        cone[1].x = x0 + coneW;     cone[1].y = y0;
        cone[2].x = x0 + coneW;     cone[2].y = y1;
        cone[3].x = x0 + coneW / 2; cone[3].y = y1 - box * 3 / 10;
        Polygon(hdc, cone, 4);
        /* DrawArcPoly()'s 0deg is straight up, clockwise - these two
         * "))" wave arcs need to open to the right (away from the
         * cone), i.e. swept around 90deg (right), not around 0deg
         * (top, which drew them as a blob above the speaker instead -
         * a real bug caught from an on-device photo, not just a look
         * change). */
        DrawArcPoly(hdc, cx - box / 12, cy, box * 3 / 10, 90.0 - 55.0, 90.0 + 55.0, 8);
        DrawArcPoly(hdc, cx - box / 12, cy, box * 1 / 2, 90.0 - 50.0, 90.0 + 50.0, 10);
        break;
    }

    case CE_MENU_ICON_INPUT: /* gamepad: rounded body + d-pad + two buttons */
    {
        int bodyTop = y0 + box * 2 / 10, bodyBot = y1 - box * 2 / 10;
        int padCx = x0 + box * 3 / 10, padCy = cy;
        int padArm = box / 5;
        RoundRect(hdc, x0, bodyTop, x1, bodyBot, box / 3, box / 3);
        MoveToEx(hdc, padCx - padArm, padCy, NULL);
        LineTo(hdc, padCx + padArm, padCy);
        MoveToEx(hdc, padCx, padCy - padArm, NULL);
        LineTo(hdc, padCx, padCy + padArm);
        Ellipse(hdc, x1 - box * 4 / 10, bodyTop + box / 10, x1 - box * 4 / 10 + box / 5, bodyTop + box / 10 + box / 5);
        Ellipse(hdc, x1 - box * 22 / 100, bodyBot - box / 10 - box / 5, x1 - box * 22 / 100 + box / 5, bodyBot - box / 10);
        break;
    }

    case CE_MENU_ICON_SCREENSHOT: /* camera: body + viewfinder bump + round lens */
    {
        int bodyTop = y0 + box * 3 / 10;
        int bodyBot = y1 - box / 10;
        int lensCy = (bodyTop + bodyBot) / 2;
        int r = box / 4;
        Rectangle(hdc, x0, bodyTop, x1, bodyBot);
        Rectangle(hdc, cx - box / 5, y0 + box / 10, cx + box / 5, bodyTop + 1);
        Ellipse(hdc, cx - r, lensCy - r, cx + r, lensCy + r);
        break;
    }

    case CE_MENU_ICON_EXIT: /* power symbol: circle with a gap + stem */
        DrawArcPoly(hdc, cx, cy, box / 2 - 1, 40.0, 320.0, 16);
        MoveToEx(hdc, cx, y0 - 1, NULL);
        LineTo(hdc, cx, cy - box / 6);
        break;

    default:
        break;
    }
}

/* Width in px of the focused main-menu button's white ring - see the
 * focus-ring block inside CeBmpFontDrawOwnerButtonTheme() below. */
#define CE_MENU_FOCUS_BORDER_PX 3

void CeBmpFontDrawOwnerButtonTheme(const DRAWITEMSTRUCT *dis, COLORREF bg, COLORREF border, COLORREF text,
                                    CeMenuIcon icon, int stacked, COLORREF windowBg)
{
    wchar_t buf[128];
    int textLen;
    RECT rc = dis->rcItem;
    int rcW = rc.right - rc.left, rcH = rc.bottom - rc.top;
    int textW = 0, iconBox, shiftX = 0, shiftY = 0;
    int focused = (dis->itemState & ODS_FOCUS) != 0;
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    int corner;
    HBRUSH hWinBgBrush, hBrush, hOldBrush;
    HPEN hPen, hOldPen;

    if (disabled)
    {
        /* Ignore the caller's theme colors entirely and fall back to a
         * flat gray look, same intent as CeBmpFontDrawOwnerButton()'s
         * DFCS_INACTIVE handling - Save/Load State sit in this state
         * whenever no ROM is loaded yet. */
        bg = RGB(0xD8, 0xD8, 0xD8);
        border = RGB(0x8A, 0x8A, 0x8A);
        text = GetSysColor(COLOR_GRAYTEXT);
    }
    else if (focused)
    {
        /* Focused = reverse video (solid fill, white border/icon/text)
         * rather than the plain white fill an even earlier cut of this
         * skin used - unambiguous even for someone not distinguishing
         * this button's own hue from its neighbors. The fill itself
         * was black at first (user request: "invert to black and
         * white"), then red (user request: "change the black/white
         * inversion to red/white"), then blue (user request: "change
         * the red/white inversion to blue/white"), then green (user
         * request: "change the blue/white inversion to green/white"),
         * then yellow (user request: "change the green/white inversion
         * to yellow/white"), then back to blue (user request: "change
         * the focus reversal color to blue/white"), then a calmer
         * blue (user request: "a more subdued blue than the current
         * one") - still white border/icon/text every time, only the
         * fill color changed. That same round also thickened the white
         * ring (4px then, now 3px - see the focus-ring block further down). The
         * current #2F5FA8 has a 6.32:1 contrast ratio against the white
         * text (the brighter #2A6BCC it replaced: 5.16:1). A tap/decide
         * darkens the fill to about 3/4 of each channel (#23477E,
         * 9.23:1) for visible press feedback, and nudges the label/icon
         * down/right by 1px - matches the shift DrawFrameControl()
         * itself applies to a pushed button on this device (see
         * CeBmpFontDrawOwnerButton()'s own comment). */
        bg = pressed ? RGB(0x23, 0x47, 0x7E) : RGB(0x2F, 0x5F, 0xA8);
        border = RGB(0xFF, 0xFF, 0xFF);
        text = RGB(0xFF, 0xFF, 0xFF);
        if (pressed)
            shiftX = shiftY = 1;
    }
    else if (pressed)
    {
        bg = ScaleColor(bg, 13);
        shiftX = shiftY = 1;
    }

    /* Fills the *whole* rcItem in the dialog's own background color
     * before the RoundRect() below - rcItem is this owner-draw button's
     * full rectangular window area, but RoundRect() only paints the
     * pill shape inscribed in it, leaving its four corner triangles
     * (outside that pill) showing whatever was there before this
     * WM_DRAWITEM call painted anything - the button window's own
     * pre-owner-draw background, which read as stray white patches in
     * an on-device photo (user report: "make the colored rounded
     * buttons' white corner gaps the window background color"). */
    hWinBgBrush = CreateSolidBrush(windowBg);
    FillRect(dis->hDC, &rc, hWinBgBrush);
    DeleteObject(hWinBgBrush);

    hBrush = CreateSolidBrush(bg);
    hPen = CreatePen(PS_SOLID, 2, border);
    hOldBrush = (HBRUSH)SelectObject(dis->hDC, hBrush);
    hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
    /* Corner diameter set to the button's own (shorter) side, i.e. a
     * full stadium/pill shape - RoundRect()'s last two args are the
     * corner ellipse's full width/height, not a radius, so this is the
     * roundest a rectangle of this size can get. Two prior, smaller
     * fixed diameters (14, then 24) each still read as only mildly
     * rounded in an on-device photo, so this cut stops guessing at a
     * fixed px value and instead ties it to the actual rcItem. */
    corner = rcH < rcW ? rcH : rcW;
    RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, corner, corner);
    SelectObject(dis->hDC, hOldBrush);
    DeleteObject(hBrush);

    /* Switch to a hollow brush before the icon draws below - several of
     * DrawMenuIcon()'s shapes (the gamepad's RoundRect() body, its two
     * Ellipse() buttons, the speaker's Polygon() cone) are filled
     * primitives, and leaving whatever brush was selected into hDC
     * *before* this WM_DRAWITEM call (unknown, and never this button's
     * own bg - that brush was just deselected on the line above) would
     * paint solid interiors instead of the intended outline-only icons.
     * A stock object, so no matching DeleteObject(). */
    SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));

    /* Focus ring (user requests: "make the focus outline thicker, 4px", later 1px thinner, now 3px):
     * a separate CE_MENU_FOCUS_BORDER_PX pen drawn on top of the normal
     * 2px outline above, inset by half its width so its outer edge
     * lines up with the button's own edge instead of half of it being
     * clipped away by the button window. The pill keeps the same outer
     * size focused or not - only the white ring grows inward. The icon
     * below keeps the 2px pen (hPen is re-selected right after). The
     * inner corner diameter is clamped at 0 for safety, though every
     * IDD_MAINMENU button is at least 20 DLU on its shorter side, far
     * above CE_MENU_FOCUS_BORDER_PX. */
    if (focused && !disabled)
    {
        HPEN hFocusPen = CreatePen(PS_SOLID, CE_MENU_FOCUS_BORDER_PX, border);
        int inset = CE_MENU_FOCUS_BORDER_PX / 2;
        int focusCorner = corner - 2 * inset; /* concentric with the pill edge for odd widths too */
        if (focusCorner < 0)
            focusCorner = 0;
        SelectObject(dis->hDC, hFocusPen);
        RoundRect(dis->hDC, rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset,
                  focusCorner, focusCorner);
        SelectObject(dis->hDC, hPen);
        DeleteObject(hFocusPen);
    }

    /* Icon reuses the border-color pen (thicker, 2px) already selected
     * above rather than switching to the text color - keeps the icon
     * visually tied to the button's own border instead of matching
     * every other button's icon in the same flat text color. */
    textLen = GetWindowTextW(dis->hwndItem, buf, 128);
    textW = textLen > 0 ? CeBmpFontGetTextWidth(buf) : 0;

    if (icon != CE_MENU_ICON_NONE)
    {
        if (stacked)
        {
            /* Icon centered in the button's top half, label centered
             * in the bottom half - the 3-up Video/Sound/Input row's
             * own rcItem is tall enough for this only because
             * ce_res.rc's IDD_MAINMENU grew that row for exactly this
             * layout (see its own comment). */
            iconBox = rcH * 9 / 20;
            if (iconBox > rcW - 6)
                iconBox = rcW - 6;
            DrawMenuIcon(dis->hDC, icon, rc.left + rcW / 2 + shiftX, rc.top + rcH * 3 / 10 + shiftY, iconBox);
            if (textLen > 0)
            {
                /* -12 rather than -3: five rounds of nudging just this
                 * label further up (two 3px rounds, then three more 1px
                 * rounds), off the bottom edge it was crowding in
                 * on-device photos (the icon above it, separately
                 * positioned, is untouched). */
                int x = rc.left + (rcW - textW) / 2 + shiftX;
                int y = rc.bottom - CE_BMPFONT_HEIGHT - 12 + shiftY;
                SetBkMode(dis->hDC, TRANSPARENT);
                CeBmpFontDrawTextW(dis->hDC, x, y, buf, text);
            }
        }
        else
        {
            /* Icon to the left of the label, the pair centered
             * together in the button - matches the design mockup's
             * single-row buttons (Open ROM/Save/Load/Exit). */
            int gap = 6;
            iconBox = CE_BMPFONT_HEIGHT;
            {
                int totalW = iconBox + gap + textW;
                int groupX = rc.left + (rcW - totalW) / 2;
                int midY = rc.top + rcH / 2;
                DrawMenuIcon(dis->hDC, icon, groupX + iconBox / 2 + shiftX, midY + shiftY, iconBox);
                if (textLen > 0)
                {
                    SetBkMode(dis->hDC, TRANSPARENT);
                    CeBmpFontDrawTextW(dis->hDC, groupX + iconBox + gap + shiftX, midY - CE_BMPFONT_HEIGHT / 2 + shiftY, buf, text);
                }
            }
        }
    }
    else if (textLen > 0)
    {
        int x = rc.left + (rcW - textW) / 2 + shiftX;
        int y = rc.top + (rcH - CE_BMPFONT_HEIGHT) / 2 + shiftY;
        SetBkMode(dis->hDC, TRANSPARENT);
        CeBmpFontDrawTextW(dis->hDC, x, y, buf, text);
    }

    SelectObject(dis->hDC, hOldPen);
    DeleteObject(hPen);
}

void CeBmpFontDrawOwnerCheckbox(const DRAWITEMSTRUCT *dis, int checked)
{
    wchar_t text[128];
    int textLen;
    RECT rc = dis->rcItem;
    RECT boxRc;
    UINT state = DFCS_BUTTONCHECK;

    if (checked)
        state |= DFCS_CHECKED;
    if (dis->itemState & ODS_DISABLED)
        state |= DFCS_INACTIVE;

    boxRc.left = rc.left;
    boxRc.top = rc.top + ((rc.bottom - rc.top) - 12) / 2;
    boxRc.right = boxRc.left + 12;
    boxRc.bottom = boxRc.top + 12;
    DrawFrameControl(dis->hDC, &boxRc, DFC_BUTTON, state);

    textLen = GetWindowTextW(dis->hwndItem, text, 128);
    if (textLen > 0)
    {
        SetBkMode(dis->hDC, TRANSPARENT);
        CeBmpFontDrawTextW(dis->hDC, boxRc.right + 4,
                            rc.top + ((rc.bottom - rc.top) - CE_BMPFONT_HEIGHT) / 2, text,
                            GetSysColor((state & DFCS_INACTIVE) ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
    }

    if (dis->itemState & ODS_FOCUS)
        DrawFocusRect(dis->hDC, &rc);
}

static void PaintLabelInternal(HDC hdc, HWND hDlg, int ctrlId, int boxMissing)
{
    HWND hCtrl = GetDlgItem(hDlg, ctrlId);
    wchar_t text[128];
    RECT rc;
    int textLen;

    if (!hCtrl)
        return;
    textLen = GetWindowTextW(hCtrl, text, 128);
    if (textLen <= 0)
        return;

    GetWindowRect(hCtrl, &rc);
    MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

    SetBkMode(hdc, TRANSPARENT);
    DrawTextInternal(hdc, rc.left, rc.top + ((rc.bottom - rc.top) - CE_BMPFONT_HEIGHT) / 2, text,
                        GetSysColor(COLOR_WINDOWTEXT), boxMissing);
}

void CeBmpFontPaintLabel(HDC hdc, HWND hDlg, int ctrlId)
{
    PaintLabelInternal(hdc, hDlg, ctrlId, 0);
}

void CeBmpFontPaintLabelBoxed(HDC hdc, HWND hDlg, int ctrlId)
{
    PaintLabelInternal(hdc, hDlg, ctrlId, 1);
}
