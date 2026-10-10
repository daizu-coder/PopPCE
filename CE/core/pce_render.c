// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_render.c - software renderer for the NitroGrafx VDC/VCE core.
 *
 * The NDS build draws with the DS 2D engine (source/Gfx.s). This file
 * draws the PCE picture in software instead, one line at a time as the
 * emulated beam passes it (pceRenderLine, called from
 * CE/core/GfxCE.s:ceScanlineHook), from the state VDC.s/VCE.s already keep:
 *   pceVRAM      64KB VDC VRAM (BAT at word 0, 16-word 8x8 tiles,
 *                64-word 16x16 sprite patterns)
 *   DIRTYTILES   1 byte per 128 bytes of VRAM, set to 0 by every VRAM
 *                write (VDC.s VRAM_H_W and VRAM DMA)
 *   EMUPALBUFF   512 x BGR555 (0bbbbbgggggrrrrr), refreshed by
 *                VCE.s:paletteTxAll at the start of each frame
 *   vdcSpriteRam the 64-entry sprite attribute table (SATB), copied from
 *                VRAM by VDC.s's sprite DMA during vblank
 *   scrollBuff   CE/core/GfxCE.s, the per-line scroll/VDC CR log, see below
 *
 * Each line is drawn with VRAM as it is when the line ends, so games
 * that stream sprite patterns while the screen is displayed (Street
 * Fighter II) show correctly; scroll and BG/sprite enable are per line.
 * The palette is taken once per frame (VCE.s:paletteTxAll at newFrame),
 * like the DS version. The SATB only changes during vblank, so it is
 * parsed once, at line 0.
 *
 * SuperGrafx (CE/core/SgxCE.s:ceSgxOn): the second VDC (VDC.s copied as
 * VDC2 by CE/core/mkvdc2.sh, all of its state has the same names plus _2)
 * is drawn the same way into its own BG and sprite lines, and the two
 * VDCs' lines are mixed straight into the output with the VPC's priority
 * and window registers (vpcRegs), giving the same result as Mednafen
 * (behaviour reference: beetle-supergrafx-libretro vpc_mix_inner.inc).
 *
 * Not emulated yet: the 16 sprites per line limit, sprite collision and
 * sprite overflow status bits, the 2bpp sprite mode (CG mode bit).
 */
#include "pce_render.h"

#include <stdio.h>
#include <string.h>

/* --- core state (asm labels, see CE/core/GfxCE.s and source/VDC.s) --------- */
extern uint8_t pceVRAM[0x10000];
extern uint8_t DIRTYTILES[0x200];
extern uint8_t ceVramDirty;	/* GfxCE.s; VDC.s writes 0 here on every VRAM write (cefix.sed) */
extern uint16_t EMUPALBUFF[512];
/* VDC.s: 64 sprites x 4 words */
extern uint16_t vdcSpriteRam[256];
/* VDC.s: low half = BG width in pixels - 1, high half = height - 1 */
extern uint32_t vdcScrollMask;
/* VDC.s: first word of the {line, endFrame} pair = displayed lines */
extern uint32_t vdcEndFrameLine;
/* VDC.s: horizontal offset that VDC.s adds to scroll X for the DS
 * screen (depends on HDS and the pixel clock) */
extern int32_t hOffset;
/* VDC.s: bit 6-7 of VDC CR at newFrame; 0 = display off for the frame */
extern uint8_t vdcBurst;
/* Made global by CE/core/cefix.sed: */
/* VDC.s: HDW register (display width in 8-pixel units - 1) */
extern uint8_t vdcHDW;
/* VDC.s: scroll value in effect since line vdcScrollLine, not yet in
 * the log (same format as a log entry) */
extern uint32_t scrollOld;
extern uint32_t vdcScrollLine;
/* VDC.s: VDC CR (log format, low half) in effect since vdcCtrl1Line */
extern uint32_t vdcCtrl1Old;
extern uint32_t vdcCtrl1Line;
/* CE/core/GfxCE.s: the per-line log */
extern uint32_t *scrollBuff;

/* SuperGrafx VDC2 (CE/core/mkvdc2.sh + SgxCE.s), same meanings as above */
extern uint8_t pceVRAM2[0x10000];
extern uint8_t DIRTYTILES2[0x200];
extern uint8_t ceVramDirty2;
extern uint16_t vdcSpriteRam_2[256];
extern uint32_t vdcScrollMask_2;
extern int32_t hOffset_2;
extern uint8_t vdcBurst_2;
extern uint8_t vdcHDW_2;
extern uint32_t scrollOld_2;
extern uint32_t vdcScrollLine_2;
extern uint32_t vdcCtrl1Old_2;
extern uint32_t vdcCtrl1Line_2;
extern uint32_t *scrollBuff2;
/* CE/core/SgxCE.s: nonzero for a SuperGrafx game; the VPC registers
 * (+0/+1 priority, +2/+3 window 1 width, +4/+5 window 2 width) */
extern uint8_t ceSgxOn;
extern uint8_t vpcRegs[8];

/* DIRTYTILES bits. VDC.s clears the whole byte on a write; upstream Gfx.s
 * used bit0 (tile map), bit5 (BG tiles), bit6/7 (sprites). 128 bytes of
 * VRAM is 4 BG tiles and exactly one sprite pattern. */
#define DIRTY_BG_CACHED		0x20
#define DIRTY_SPR_CACHED	0x80

/* scrollBuff entry (4 words per line). VDC.s fills it backwards up to
 * the line of each new write, so only lines before vdcScrollLine /
 * vdcCtrl1Line are final while the frame is running. */
#define SL_SCROLL	0	/* ((X + hOffset) & maskX) | ((Y - write line) << 16) */
#define SL_CTRL		1	/* bit2-3: BG on, bit4-5: sprites on */
#define SL_CTRL_BG	0x0C
#define SL_CTRL_SPR	0x30

/* SATB word 3 */
#define SPR_PAL		0x000F
#define SPR_PRIO	0x0080	/* 1 = in front of the BG */
#define SPR_CGX		0x0100	/* 1 = 32 wide */
#define SPR_HFLIP	0x0800
#define SPR_CGY		0x3000	/* 0 = 16 high, 1 = 32, 2/3 = 64 */
#define SPR_VFLIP	0x8000

/* Sprite line buffer: 0 = no sprite, else sprite palette index (0x100+)
 * plus SPRLINE_FRONT for sprites in front of the BG. */
#define SPRLINE_FRONT	0x8000
/* Room left and right of the visible width for sprites that are partly
 * off screen (a sprite is at most 32 wide). */
#define SPRLINE_MARGIN	32

/* --- output ------------------------------------------------------------ */
/* Word aligned: every row (PCE_FB_PITCH * 2 bytes) is written two
 * pixels per 32-bit store. */
uint16_t pceFrame[PCE_FB_HEIGHT * PCE_FB_PITCH] __attribute__((aligned(8)));
int pceFrameWidth = 256;
int pceFrameHeight = 224;
unsigned pceFrameCount;
int pceSkipRequest;
int pceRenderProbe;

/* --- caches ------------------------------------------------------------ */
/* Per VDC: 2048 BG tiles (all of VRAM), 8x8 pixels, one color index (0-15)
 * per byte. Word aligned: a tile row is read and written as two words. */
static uint8_t bgTiles[2048 * 64] __attribute__((aligned(8)));
static uint8_t bgTiles2[2048 * 64] __attribute__((aligned(8)));
/* Per VDC: 512 sprite patterns (all of VRAM), 16x16 pixels, same format */
static uint8_t sprTiles[512 * 256] __attribute__((aligned(8)));
static uint8_t sprTiles2[512 * 256] __attribute__((aligned(8)));
/* EMUPALBUFF converted to RGB565: 0-255 BG, 256-511 sprites */
static uint16_t pal565[512];

/* Bit spreading for the tile decode (the same bit order as Mednafen
 * pce_fast's vdc_spread8, done with tables, which suits a 32-bit CPU
 * better): bit 7-k of a bitplane byte goes to bit 0 of byte k, k = 0
 * being the leftmost pixel. spreadL covers pixels 0-3, spreadR pixels 4-7. */
#define SPR_L(b)	((((b) >> 7) & 1u) | ((((b) >> 6) & 1u) << 8) \
			| ((((b) >> 5) & 1u) << 16) | ((((b) >> 4) & 1u) << 24))
#define SPR_R(b)	SPR_L((b) << 4)
#define X4(M, n)	M(n), M((n) + 1), M((n) + 2), M((n) + 3)
#define X16(M, n)	X4(M, n), X4(M, (n) + 4), X4(M, (n) + 8), X4(M, (n) + 12)
#define X64(M, n)	X16(M, n), X16(M, (n) + 16), X16(M, (n) + 32), X16(M, (n) + 48)
#define X256(M)		X64(M, 0), X64(M, 64), X64(M, 128), X64(M, 192)
static const uint32_t spreadL[256] = { X256(SPR_L) };
static const uint32_t spreadR[256] = { X256(SPR_R) };

/* One line of BG as palette indices (0 = transparent, shows color 0),
 * one per VDC. Whole tiles are written from the start of the buffer, a
 * word at a time; bgLine points at the first visible pixel (fine scroll
 * 0-7). Room for 512 pixels plus the partly visible tile at the right. */
static uint32_t bgLineBuf[2][(PCE_FB_PITCH + 8) / 4];
static const uint8_t *bgLine;
/* One per VDC. Word aligned (SPRLINE_MARGIN is even): the output reads
 * two pixels at a time */
static uint16_t sprLineBuf[2][SPRLINE_MARGIN + PCE_FB_PITCH + SPRLINE_MARGIN] __attribute__((aligned(4)));
#define sprLine (sprLineBuf[0] + SPRLINE_MARGIN)

/* The SATB parsed once per frame, visible sprites only, in SATB order
 * (lower index = drawn in front). */
typedef struct {
	int y, x;			/* top left on screen */
	int h;				/* height in pixels (16/32/64) */
	int wCells;			/* width in 16 pixel cells (1/2) */
	unsigned pattern;	/* first pattern, size bits already cleared */
	unsigned attr;		/* SATB word 3 */
} Sprite;

/* One VDC: where its state lives in the core, and what is drawn from it */
typedef struct {
	uint8_t *vram;
	uint8_t *dirty;			/* DIRTYTILES */
	uint8_t *vramDirty;		/* ceVramDirty */
	const uint16_t *satb;	/* vdcSpriteRam */
	const uint32_t *scrollMask;
	const int32_t *hOffset;
	const uint8_t *burst;
	const uint8_t *hdw;
	const uint32_t *scrollOld, *scrollLine;
	const uint32_t *ctrlOld, *ctrlLine;
	uint32_t *const *scrollBuff;
	uint8_t *bgTiles, *sprTiles;
	/* Fixed at line 0 for the whole frame */
	int frameOff;
	int spriteCount;
	Sprite sprites[64];
} Vdc;

static Vdc vdcs[2] = {
	{ pceVRAM, DIRTYTILES, &ceVramDirty, vdcSpriteRam, &vdcScrollMask, &hOffset,
	  &vdcBurst, &vdcHDW, &scrollOld, &vdcScrollLine, &vdcCtrl1Old, &vdcCtrl1Line,
	  &scrollBuff, bgTiles, sprTiles },
	{ pceVRAM2, DIRTYTILES2, &ceVramDirty2, vdcSpriteRam_2, &vdcScrollMask_2, &hOffset_2,
	  &vdcBurst_2, &vdcHDW_2, &scrollOld_2, &vdcScrollLine_2, &vdcCtrl1Old_2, &vdcCtrl1Line_2,
	  &scrollBuff2, bgTiles2, sprTiles2 },
};

/* SuperGrafx: what a VDC shows on a line, or doesn't (display off, BG
 * off, no sprites, or left out by the VPC) */
static const uint8_t zeroBg[PCE_FB_PITCH];
static const uint16_t zeroSpr[PCE_FB_PITCH] __attribute__((aligned(4)));

/* Fixed at line 0 for the whole frame */
static int frameWidth = 256;
static int frameSgx;		/* ceSgxOn */
/* pceSkipRequest latched at line 0, so a frame is either drawn whole or
 * skipped whole even if the request changes mid-frame. */
static int frameSkipping;
static int frameProbe;		/* pceRenderProbe latched at line 0 */

/* ----------------------------------------------------------------------- */
/* 4 pixels of one tile row from its 4 bitplane bytes */
#define SPREAD4(t, p0, p1, p2, p3) \
	((t)[p0] | ((t)[p1] << 1) | ((t)[p2] << 2) | ((t)[p3] << 3))

static void decodeBgTile(const Vdc *v, int tile)
{
	const uint16_t *src = (const uint16_t *)v->vram + tile * 16;
	uint32_t *dst = (uint32_t *)(v->bgTiles + tile * 64);
	int row;

	for (row = 0; row < 8; row++) {
		unsigned p01 = src[row];		/* plane 0 = low byte, plane 1 = high */
		unsigned p23 = src[row + 8];	/* plane 2 = low byte, plane 3 = high */
		unsigned p0 = p01 & 0xFF, p1 = p01 >> 8;
		unsigned p2 = p23 & 0xFF, p3 = p23 >> 8;

		dst[0] = SPREAD4(spreadL, p0, p1, p2, p3);
		dst[1] = SPREAD4(spreadR, p0, p1, p2, p3);
		dst += 2;
	}
}

static void decodeSprPattern(const Vdc *v, int pattern)
{
	/* 16 words per plane, one word per row, bit 15 = leftmost pixel */
	const uint16_t *src = (const uint16_t *)v->vram + pattern * 64;
	uint32_t *dst = (uint32_t *)(v->sprTiles + pattern * 256);
	int row;

	for (row = 0; row < 16; row++) {
		unsigned w0 = src[row];
		unsigned w1 = src[row + 16];
		unsigned w2 = src[row + 32];
		unsigned w3 = src[row + 48];
		/* left half = high bytes, right half = low bytes */
		unsigned h0 = w0 >> 8, h1 = w1 >> 8, h2 = w2 >> 8, h3 = w3 >> 8;
		unsigned l0 = w0 & 0xFF, l1 = w1 & 0xFF, l2 = w2 & 0xFF, l3 = w3 & 0xFF;

		dst[0] = SPREAD4(spreadL, h0, h1, h2, h3);
		dst[1] = SPREAD4(spreadR, h0, h1, h2, h3);
		dst[2] = SPREAD4(spreadL, l0, l1, l2, l3);
		dst[3] = SPREAD4(spreadR, l0, l1, l2, l3);
		dst += 4;
	}
}

static void updateTileCaches(const Vdc *v)
{
	uint8_t *dirty = v->dirty;
	const uint32_t *words = (const uint32_t *)dirty;
	int w, i;

	/* Called every line, but most lines see no VRAM write. */
	if (*v->vramDirty & DIRTY_BG_CACHED)
		return;
	*v->vramDirty = DIRTY_BG_CACHED | DIRTY_SPR_CACHED;

	/* BG tiles only: sprite patterns are decoded when a sprite actually
	 * draws them (sprPattern), so a scene change that rewrites all of
	 * VRAM does not also decode 512 patterns nobody shows.
	 * Skip 4 clean bytes at a time. */
	for (w = 0; w < 0x200 / 4; w++) {
		if ((words[w] & 0x20202020) == 0x20202020)
			continue;
		for (i = w * 4; i < w * 4 + 4; i++) {
			unsigned d = dirty[i];
			if (!(d & DIRTY_BG_CACHED)) {
				decodeBgTile(v, i * 4);
				decodeBgTile(v, i * 4 + 1);
				decodeBgTile(v, i * 4 + 2);
				decodeBgTile(v, i * 4 + 3);
				dirty[i] = (uint8_t)(d | DIRTY_BG_CACHED);
			}
		}
	}
}

/* Decoded pixels of one sprite pattern, decoding it first if VRAM changed
 * since. Same VRAM as the line's BG, since both happen while drawing it. */
static const uint8_t *sprPattern(const Vdc *v, unsigned pattern)
{
	unsigned d = v->dirty[pattern];

	if (!(d & DIRTY_SPR_CACHED)) {
		decodeSprPattern(v, (int)pattern);
		v->dirty[pattern] = (uint8_t)(d | DIRTY_SPR_CACHED);
	}
	return v->sprTiles + pattern * 256;
}

static void updatePalette(void)
{
	int i;

	for (i = 0; i < 512; i++) {
		unsigned c = EMUPALBUFF[i];
		unsigned r = c & 0x1F;
		unsigned g = (c >> 5) & 0x1F;
		unsigned b = (c >> 10) & 0x1F;
		pal565[i] = (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
	}
}

/* ----------------------------------------------------------------------- */
static void parseSprites(Vdc *v, int width, int height)
{
	int i;

	v->spriteCount = 0;
	for (i = 0; i < 64; i++) {
		const uint16_t *s = v->satb + i * 4;
		unsigned attr = s[3];
		int y = (int)(s[0] & 0x3FF) - 64;
		int x = (int)(s[1] & 0x3FF) - 32;
		unsigned pattern = (s[2] >> 1) & 0x1FF;	/* 64KB VRAM = 512 patterns */
		int wCells = (attr & SPR_CGX) ? 2 : 1;
		int hCells = 1 << ((attr & SPR_CGY) >> 12);
		Sprite *spr;

		if (hCells > 4)
			hCells = 4;						/* CGY 3 is 64 high too */
		if (y >= height || y + hCells * 16 <= 0 || x >= width || x + wCells * 16 <= 0)
			continue;

		/* Large sprites ignore the low pattern bits: 32 wide uses
		 * patterns n, n+1 side by side, each 16 rows down adds 2. */
		if (wCells == 2)
			pattern &= ~1u;
		if (hCells == 2)
			pattern &= ~2u;
		else if (hCells == 4)
			pattern &= ~6u;

		spr = &v->sprites[v->spriteCount++];
		spr->y = y;
		spr->x = x;
		spr->h = hCells * 16;
		spr->wCells = wCells;
		spr->pattern = pattern;
		spr->attr = attr;
	}
}

/* Fills a sprite line buffer (sprLineBuf[n]) for one line; returns 0 if
 * no sprite is on it. */
static int drawSpriteLine(const Vdc *v, uint16_t *lineBuf, int line, int width)
{
	uint16_t *const lineStart = lineBuf + SPRLINE_MARGIN;
	int i, any = 0;

	for (i = 0; i < v->spriteCount; i++) {
		const Sprite *spr = &v->sprites[i];
		int row = line - spr->y;
		unsigned attr, base;
		int cx;

		if ((unsigned)row >= (unsigned)spr->h)
			continue;
		if (!any) {
			/* Clear only what can be drawn: margin + visible width */
			memset(lineBuf, 0, (SPRLINE_MARGIN + width + SPRLINE_MARGIN) * sizeof(uint16_t));
			any = 1;
		}
		attr = spr->attr;
		if (attr & SPR_VFLIP)
			row = spr->h - 1 - row;
		base = 0x100 | ((attr & SPR_PAL) << 4) | ((attr & SPR_PRIO) ? SPRLINE_FRONT : 0);

		for (cx = 0; cx < spr->wCells; cx++) {
			/* Screen cell: flipped sprites also swap their two cells. */
			int sx = spr->x + ((attr & SPR_HFLIP) ? spr->wCells - 1 - cx : cx) * 16;
			unsigned pattern = (spr->pattern + cx + (row >> 4) * 2) & 0x1FF;
			const uint8_t *pix;
			uint16_t *dst = lineStart + sx;
			int px;

			if (sx >= width || sx + 16 <= 0)
				continue;
			pix = sprPattern(v, pattern) + (row & 15) * 16;
			/* Lower SATB index wins: only fill pixels still empty.
			 * sx is within the margins, so no per-pixel clipping. */
			if (attr & SPR_HFLIP) {
				for (px = 0; px < 16; px++) {
					unsigned c = pix[15 - px];
					if (c && !dst[px])
						dst[px] = (uint16_t)(base | c);
				}
			} else {
				for (px = 0; px < 16; px++) {
					unsigned c = pix[px];
					if (c && !dst[px])
						dst[px] = (uint16_t)(base | c);
				}
			}
		}
	}
	return any;
}

/* ----------------------------------------------------------------------- */
/* One line of BG into buf (bgLineBuf[n]) as palette indices, four
 * pixels per word: a tile row is two words of 0-15 indices in the cache,
 * and each non-zero byte gets the BAT palette in its high nibble while 0
 * stays 0 (transparent), without a branch per pixel. Returns the first
 * visible pixel. */
static const uint8_t *drawBgLine(const Vdc *v, uint32_t *buf, int width, uint32_t scroll, int line)
{
	uint32_t scrollMask = *v->scrollMask;
	unsigned maskX = scrollMask & 0x3FF;
	unsigned maskY = (scrollMask >> 16) & 0x1FF;
	unsigned batWidth = (maskX + 1) >> 3;		/* 32, 64 or 128 tiles */
	/* Undo VDC.s's DS centering offset to get the real BG X. */
	unsigned x = ((scroll & 0xFFFF) - *v->hOffset) & maskX;
	unsigned y = ((scroll >> 16) + line) & maskY;
	const uint8_t *cache = v->bgTiles;
	const uint16_t *bat = (const uint16_t *)v->vram + (y >> 3) * batWidth;
	unsigned tileRow = (y & 7) * 8;
	unsigned tx = x >> 3;
	unsigned px = x & 7;
	int tiles = (int)(px + width + 7) >> 3;
	uint32_t *dst = buf;

	while (tiles-- > 0) {
		unsigned entry = bat[tx];
		const uint32_t *pix = (const uint32_t *)(cache + ((entry & 0x7FF) << 6) + tileRow);
		uint32_t pal = (entry >> 12) * 0x10101010u;
		uint32_t w0 = pix[0], w1 = pix[1];
		/* 0x10 in each byte whose index is not 0 (0-15 + 15 never
		 * carries into the next byte), then widened to 0xFF */
		uint32_t m0 = ((w0 + 0x0F0F0F0Fu) & 0x10101010u) >> 4;
		uint32_t m1 = ((w1 + 0x0F0F0F0Fu) & 0x10101010u) >> 4;

		dst[0] = w0 | (pal & ((m0 << 8) - m0));
		dst[1] = w1 | (pal & ((m1 << 8) - m1));
		dst += 2;
		tx = (tx + 1) & (batWidth - 1);
	}
	return (const uint8_t *)buf + px;
}

static void clearBgLine(int width)
{
	memset(bgLineBuf[0], 0, width);
	bgLine = (const uint8_t *)bgLineBuf[0];
}

/* The outputs write two pixels per 32-bit store (width is a multiple
 * of 8, dst a pceFrame row). */
static void outputBgOnly(uint16_t *dst, int width)
{
	const uint8_t *b = bgLine;
	uint32_t *d = (uint32_t *)dst;
	int x;

	for (x = 0; x < width; x += 2)
		*d++ = pal565[b[x]] | ((uint32_t)pal565[b[x + 1]] << 16);
}

/* A sprite pixel shows if it is in front, or the BG is transparent
 * there. */
static inline unsigned mixPixel(unsigned s, unsigned b)
{
	if (s && ((s & SPRLINE_FRONT) || !b))
		return pal565[s & 0x1FF];
	return pal565[b];
}

static void outputWithSprites(uint16_t *dst, int width)
{
	const uint8_t *b = bgLine;
	const uint16_t *s = sprLine;
	uint32_t *d = (uint32_t *)dst;
	int x;

	for (x = 0; x < width; x += 2) {
		uint32_t s01 = *(const uint32_t *)(s + x);	/* sprLine + x is word aligned */

		if (!s01)
			*d++ = pal565[b[x]] | ((uint32_t)pal565[b[x + 1]] << 16);
		else
			*d++ = mixPixel(s01 & 0xFFFF, b[x]) | (mixPixel(s01 >> 16, b[x + 1]) << 16);
	}
}

/* SuperGrafx: one VDC's line. spr is zeroSpr when no sprite is on it
 * (hasSpr 0), bg is zeroBg when there is no BG. */
typedef struct {
	const uint8_t *bg;
	const uint16_t *spr;
	int hasSpr;
} SgxSrc;

static const SgxSrc sgxNone = { zeroBg, zeroSpr, 0 };

static void drawSgxVdcLine(int n, int line, int width, SgxSrc *out)
{
	const Vdc *v = &vdcs[n];
	uint32_t scroll, ctrl;

	*out = sgxNone;
	if (v->frameOff)
		return;
	updateTileCaches(v);
	scroll = (uint32_t)line < *v->scrollLine ? (*v->scrollBuff)[line * 4 + SL_SCROLL] : *v->scrollOld;
	ctrl = (uint32_t)line < *v->ctrlLine ? (*v->scrollBuff)[line * 4 + SL_CTRL] : *v->ctrlOld;
	if (ctrl & SL_CTRL_BG)
		out->bg = drawBgLine(v, bgLineBuf[n], width, scroll, line);
	if ((ctrl & SL_CTRL_SPR) && drawSpriteLine(v, sprLineBuf[n], line, width)) {
		out->spr = sprLineBuf[n] + SPRLINE_MARGIN;
		out->hasSpr = 1;
	}
}

/* One VDC's pixel as a palette index, the sprite-over-BG choice made:
 * sprites 0x101-0x1FF, BG 0x01-0xFF, 0 = transparent (BG color 0, or
 * nothing drawn). */
static inline unsigned sgxPixel(const uint8_t *bg, const uint16_t *spr, int x)
{
	unsigned b = bg[x], sp = spr[x];

	if (sp && ((sp & SPRLINE_FRONT) || !b))
		return sp & 0x1FF;
	return b;
}

/* The usual rule: VDC1 in front, VDC2 where VDC1 is transparent. Two
 * pixels per 32-bit store (dst is a pceFrame row, so an even x is word
 * aligned, and so is a sprite line at an even x). */
static void mixNormal(uint16_t *dst, int x, int end, const SgxSrc *a, const SgxSrc *b)
{
	const uint8_t *b1 = a->bg, *b2 = b->bg;
	const uint16_t *s1 = a->spr, *s2 = b->spr;
	unsigned c0, c1;

	if (x < end && (x & 1)) {
		c0 = sgxPixel(b1, s1, x);
		dst[x] = pal565[c0 ? c0 : sgxPixel(b2, s2, x)];
		x++;
	}
	if (!a->hasSpr && !b->hasSpr) {
		for (; x + 1 < end; x += 2) {
			c0 = b1[x];
			c1 = b1[x + 1];
			if (!c0)
				c0 = b2[x];
			if (!c1)
				c1 = b2[x + 1];
			*(uint32_t *)(dst + x) = pal565[c0] | ((uint32_t)pal565[c1] << 16);
		}
	} else {
		for (; x + 1 < end; x += 2) {
			if (!(*(const uint32_t *)(s1 + x) | *(const uint32_t *)(s2 + x))) {
				c0 = b1[x];
				c1 = b1[x + 1];
				if (!c0)
					c0 = b2[x];
				if (!c1)
					c1 = b2[x + 1];
			} else {
				c0 = sgxPixel(b1, s1, x);
				c1 = sgxPixel(b1, s1, x + 1);
				if (!c0)
					c0 = sgxPixel(b2, s2, x);
				if (!c1)
					c1 = sgxPixel(b2, s2, x + 1);
			}
			*(uint32_t *)(dst + x) = pal565[c0] | ((uint32_t)pal565[c1] << 16);
		}
	}
	if (x < end) {
		c0 = sgxPixel(b1, s1, x);
		dst[x] = pal565[c0 ? c0 : sgxPixel(b2, s2, x)];
	}
}

/* Special priority modes 1 and 2 (see mixVpcLine), one pixel at a time */
static void mixSpecial(uint16_t *dst, int x, int end, const SgxSrc *a, const SgxSrc *b, unsigned mode)
{
	for (; x < end; x++) {
		unsigned p1 = sgxPixel(a->bg, a->spr, x);
		unsigned p2 = sgxPixel(b->bg, b->spr, x);

		if (mode == 1) {
			if ((p2 & 0x100) && !(p1 & 0x100))
				p1 = 0;
		} else {
			if ((p1 & 0x100) && p2 && !(p2 & 0x100))
				p1 = 0;
		}
		dst[x] = pal565[p1 ? p1 : p2];
	}
}

/* The VPC (HuC6202); behaves like Mednafen's MixVPC: two windows, each
 * covering x < width - 0x40, split the line into up to 4 regions, and each
 * region takes one nibble of the priority register (bits 12-15 outside both
 * windows, 8-11 in window 1 only, 4-7 in window 2 only, 0-3 in both):
 *   bit 0 / bit 1   VDC1 / VDC2 shown
 *   bits 2-3 = 1    VDC2 sprites in front of VDC1's BG (not its sprites)
 *   bits 2-3 = 2    VDC2 BG in front of VDC1's sprites
 *   otherwise       VDC1 in front
 * VDC2 shows wherever VDC1 is transparent. */
static void mixVpcLine(uint16_t *dst, int width, const SgxSrc *src)
{
	unsigned prio = vpcRegs[0] | (vpcRegs[1] << 8);
	int win1 = (int)(vpcRegs[2] | ((vpcRegs[3] & 3) << 8)) - 0x40;
	int win2 = (int)(vpcRegs[4] | ((vpcRegs[5] & 3) << 8)) - 0x40;
	int x = 0;

	while (x < width) {
		int in1 = x < win1, in2 = x < win2;
		int end = width;
		unsigned pb = (prio >> ((3 - (in1 | (in2 << 1))) * 4)) & 0xF;
		unsigned mode = pb >> 2;
		const SgxSrc *a = (pb & 1) ? &src[0] : &sgxNone;
		const SgxSrc *b = (pb & 2) ? &src[1] : &sgxNone;

		if (in1 && win1 < end)
			end = win1;
		if (in2 && win2 < end)
			end = win2;
		/* Mode 1 only matters where VDC2 has sprites, mode 2 only where
		 * VDC1 has; otherwise it is the usual rule. */
		if ((mode == 1 && b->hasSpr) || (mode == 2 && a->hasSpr))
			mixSpecial(dst, x, end, a, b, mode);
		else
			mixNormal(dst, x, end, a, b);
		x = end;
	}
}

static void beginFrame(void)
{
	Vdc *v = &vdcs[0];
	int width = (vdcHDW + 1) * 8;

	if (width > PCE_FB_PITCH)
		width = PCE_FB_PITCH;
	frameWidth = width;
	frameSgx = ceSgxOn;
	/* Same rule as upstream endFrame: BG and sprites both off when the
	 * frame starts blanks the whole frame (burst mode). */
	v->frameOff = (*v->burst == 0);
	updatePalette();
	parseSprites(v, width, PCE_FB_HEIGHT);
	if (frameSgx) {
		/* VDC2 is drawn over VDC1's width and lines */
		v = &vdcs[1];
		v->frameOff = (*v->burst == 0);
		parseSprites(v, width, PCE_FB_HEIGHT);
	}
}

void pceRenderLine(int line)
{
	const Vdc *v = &vdcs[0];
	int width;
	uint32_t scroll, ctrl;
	uint16_t *dst;

	if ((unsigned)line >= PCE_FB_HEIGHT)
		return;
	if (line == 0) {
		frameSkipping = pceSkipRequest;
		frameProbe = pceRenderProbe;
		if (frameSkipping)
			return;
		beginFrame();
	}
	if (frameSkipping)
		return;
	width = frameWidth;
	dst = pceFrame + line * PCE_FB_PITCH;

	if (frameSgx) {
		SgxSrc src[2];

		drawSgxVdcLine(0, line, width, &src[0]);
		drawSgxVdcLine(1, line, width, &src[1]);
		mixVpcLine(dst, width, src);
		return;
	}

	if (v->frameOff) {
		clearBgLine(width);
		outputBgOnly(dst, width);
		return;
	}

	updateTileCaches(v);
	if (frameProbe == PCE_PROBE_SCAN)
		updateTileCaches(v);		/* all clean now: the scan alone */
	scroll = (uint32_t)line < vdcScrollLine ? scrollBuff[line * 4 + SL_SCROLL] : scrollOld;
	ctrl = (uint32_t)line < vdcCtrl1Line ? scrollBuff[line * 4 + SL_CTRL] : vdcCtrl1Old;

	if (ctrl & SL_CTRL_BG) {
		bgLine = drawBgLine(v, bgLineBuf[0], width, scroll, line);
		if (frameProbe == PCE_PROBE_BG)
			bgLine = drawBgLine(v, bgLineBuf[0], width, scroll, line);
	} else {
		clearBgLine(width);
	}

	if ((ctrl & SL_CTRL_SPR) && drawSpriteLine(v, sprLineBuf[0], line, width)) {
		if (frameProbe == PCE_PROBE_SPR)
			drawSpriteLine(v, sprLineBuf[0], line, width);
		outputWithSprites(dst, width);
		if (frameProbe == PCE_PROBE_OUT)
			outputWithSprites(dst, width);
	} else {
		outputBgOnly(dst, width);
		if (frameProbe == PCE_PROBE_OUT)
			outputBgOnly(dst, width);
	}
}

int pceScrollLogRequest;
char pceScrollLog[PCE_SCROLL_LOG_SIZE];

/* Debug: the BG X/Y of each line of the frame just finished, written as
 * "line:x,y" wherever a line does not follow on from the one before
 * (same X, Y + 1), plus where BG/sprites switch on or off. */
static void logScrollLines(int height)
{
	uint32_t maskX = vdcScrollMask & 0x3FF, maskY = (vdcScrollMask >> 16) & 0x1FF;
	unsigned prevX = ~0u, prevY = ~0u, prevCtrl = ~0u;
	char *p = pceScrollLog, *end = pceScrollLog + sizeof(pceScrollLog) - 24;
	int line;

	p += sprintf(p, "scrollLine %u ctrlLine %u:", (unsigned)vdcScrollLine,
		(unsigned)vdcCtrl1Line);
	for (line = 0; line < height && p < end; line++) {
		uint32_t scroll = (uint32_t)line < vdcScrollLine ? scrollBuff[line * 4 + SL_SCROLL] : scrollOld;
		uint32_t ctrl = ((uint32_t)line < vdcCtrl1Line ? scrollBuff[line * 4 + SL_CTRL] : vdcCtrl1Old)
			& (SL_CTRL_BG | SL_CTRL_SPR);
		unsigned x = ((scroll & 0xFFFF) - hOffset) & maskX;
		unsigned y = ((scroll >> 16) + line) & maskY;

		if (x != prevX || y != ((prevY + 1) & maskY))
			p += sprintf(p, " %d:%u,%u", line, x, y);
		if (ctrl != prevCtrl)
			p += sprintf(p, " %d:cr%02x", line, (unsigned)ctrl);
		prevX = x;
		prevY = y;
		prevCtrl = ctrl;
	}
	if (line < height)
		strcpy(p, " ...");
}

void pceEndFrame(void)
{
	int height = (int)vdcEndFrameLine;

	if (height > PCE_FB_HEIGHT)
		height = PCE_FB_HEIGHT;
	if (height < 1)
		height = 1;
	if (pceScrollLogRequest) {
		pceScrollLogRequest = 0;
		logScrollLines(height);
	}
	if (frameSkipping)
		return;			/* pceFrame still holds the last drawn frame */
	pceFrameWidth = frameWidth;
	pceFrameHeight = height;
	pceFrameCount++;
}
