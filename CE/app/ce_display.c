/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * ce_display.c - GDI display backend (ported from the sister
 * PopSNES, which in turn took it from PopNES).
 *
 * Replaces an earlier GAPI-based backend (ce_gapi.c/ce_gapi.h, removed
 * by this change). Not a technical fix - a licensing-caution one: GAPI
 * requires gx.dll, and it is not clear that the gx.dll available for
 * this device family is an OEM-shipped component that may be relied on
 * and redistributed. Rather than keep shipping on an unclear-provenance
 * dependency, the display path was moved to plain GDI. Two GAPI-free
 * alternatives were investigated on real hardware and ruled out before
 * landing on plain GDI:
 *   - ExtEscape(GETRAWFRAMEBUFFER) - this device's display driver
 *     reports it unsupported (result=0);
 *   - DirectDraw (ddraw.dll) - DirectDrawCreate resolves, but calling
 *     into the object it returns crashed the device outright before any
 *     UI could appear; cegcc also ships no ddraw.h, so continuing down
 *     that path meant guessing at an un-headered COM ABI.
 * GDI (CreateDIBSection/CreateCompatibleDC/BitBlt/PatBlt) is what's
 * left: part of coredll.dll like every other Win32 call this port
 * already makes, so it adds no extra DLL dependency and no licensing
 * question at all - confirmed at real-time speed on this exact device
 * family.
 *
 * The core's retro_video_refresh_t callback (ce_video_refresh() in
 * ce_main.c) hands this module a complete already-RGB565 frame every
 * call (ce_environment() only accepts RGB565) - the exact same (src, srcW, srcH, srcPitchBytes) shape the old
 * GAPI backend's GapiBlitRGB565() consumed - so the scale-and-letterbox
 * math below (nearest-neighbor, 16.16 fixed-point stepping, one divide
 * per axis per frame) is carried over unchanged from ce_gapi.c. Only
 * the destination changed: instead of writing straight into a
 * GXBeginDraw() framebuffer pointer, CeDisplayBlitRGB565() writes into a
 * small scratch region of an off-screen RGB565 DIB section (s_dibBits,
 * sized to the full physical screen but only ever written at rows
 * 0..dstH-1 / columns 0..dstW-1) and transfers just that dstW x dstH
 * rectangle onto the window with a single plain 1:1 BitBlt() at
 * (dstX, dstY) - not StretchDIBits()/StretchBlt(), which the sister
 * ports' investigation found is much slower than doing the scale by
 * hand on this device's driver (no fast path for arbitrary-ratio
 * stretching).
 *
 * The earlier GAPI backend's raw-framebuffer "offX must stay even" 32-bit
 * pixel-packing fast path (two RGB565 pixels per 32-bit store, only
 * valid when cbxPitch==2 - an odd offX from an aspect-ratio scale could
 * fault with a Datatype Misalignment exception on real hardware)
 * doesn't need porting as-is: a DIB section
 * accessed as uint16_t* has no equivalent alignment hazard, since the
 * scratch region this module writes into always starts at DIB column 0
 * (dstX/dstY are applied only at BitBlt time, per the doc comment
 * above) and s_dibPitch is DWORD-aligned by construction. That made it
 * safe to reintroduce the same two-pixels-per-32-bit-store idea,
 * without the old parity guard, in the fixed-ratio branches of
 * CeDisplayBlitRGB565() below (see the comments there). Each fast path
 * is only taken when the source width divides its cycle exactly (e.g.
 * SNES 256/512, NES 256, GB 160 for some ratios); any other width falls
 * back to the generic 16.16 DDA loop (also two pixels per 32-bit
 * store), so a new core needs no changes
 * here (15/8 is run as a doubled 16-source/30-dest cycle to keep the
 * per-cycle byte count a multiple of 4 - see s_scale15over8's branch).
 */
#include "ce_display.h"
#include "ce_log.h"
#include "ce_video.h"

#include <stdint.h>
#include <string.h>

static HWND      s_hwnd      = NULL;
static int       s_physW     = 0;
static int       s_physH     = 0;
static HDC       s_memDC     = NULL;
static HBITMAP   s_dibBitmap = NULL;
static uint16_t *s_dibBits   = NULL;
static int       s_dibPitch  = 0;

/* GetDC/ReleaseDC on this device are documented WinCE performance traps
 * when called every frame (unlike GAPI's GXBeginDraw()/GXEndDraw(),
 * which were purpose-built for lightweight per-frame framebuffer
 * access). s_cachedWndDC holds one HDC for s_hwnd across frames instead
 * of fetching/releasing it on every blit; CeDisplaySuspend() releases
 * it before a dialog takes the screen (matching the old GAPI backend's
 * suspend/resume pairing at those same call sites in ce_main.c), and
 * the next CeDisplayBlitRGB565()/CeDisplayForceRepaint() call lazily
 * re-acquires it via GetCachedWndDC(). */
static HDC s_cachedWndDC = NULL;

static HDC GetCachedWndDC(void)
{
    if (!s_cachedWndDC)
        s_cachedWndDC = GetDC(s_hwnd);
    return s_cachedWndDC;
}

int CeDisplayInit(HWND hwnd)
{
    HDC hdc;
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } dibInfo;

    if (s_dibBitmap)
        return 1; /* already set up - see this function's own doc comment */

    s_hwnd  = hwnd;
    s_physW = GetSystemMetrics(SM_CXSCREEN);
    s_physH = GetSystemMetrics(SM_CYSCREEN);
    CeLog("CeDisplayInit: GetSystemMetrics cx=%d cy=%d", s_physW, s_physH);
    if (s_physW <= 0 || s_physH <= 0)
        return 0;

    hdc = GetDC(hwnd);
    if (!hdc)
    {
        CeLog("CeDisplayInit: GetDC(hwnd) failed");
        return 0;
    }

    /* Paint the physical screen black immediately - otherwise whatever
     * was in video memory before this process ever wrote to it stays
     * visible behind the menu/dialogs until the first real frame blits. */
    PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

    memset(&dibInfo, 0, sizeof(dibInfo));
    dibInfo.h.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.h.biWidth = s_physW;
    dibInfo.h.biHeight = -s_physH; /* negative = top-down, matches the source frame's own row order */
    dibInfo.h.biPlanes = 1;
    dibInfo.h.biBitCount = 16;
    dibInfo.h.biCompression = BI_BITFIELDS;
    dibInfo.masks[0] = 0xF800; /* red:   5 bits @ bit 11 - RGB565, matches RETRO_PIXEL_FORMAT_RGB565 */
    dibInfo.masks[1] = 0x07E0; /* green: 6 bits @ bit 5 */
    dibInfo.masks[2] = 0x001F; /* blue:  5 bits @ bit 0 */

    s_dibBitmap = CreateDIBSection(hdc, (BITMAPINFO *)&dibInfo, DIB_RGB_COLORS, (void **)&s_dibBits, NULL, 0);
    if (!s_dibBitmap || !s_dibBits)
    {
        CeLog("CeDisplayInit: CreateDIBSection failed");
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    s_dibPitch = ((s_physW * 16 + 31) / 32) * 4; /* DIB rows are DWORD-aligned */

    s_memDC = CreateCompatibleDC(hdc);
    if (!s_memDC)
    {
        CeLog("CeDisplayInit: CreateCompatibleDC failed");
        DeleteObject(s_dibBitmap);
        s_dibBitmap = NULL;
        s_dibBits = NULL;
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    SelectObject(s_memDC, s_dibBitmap);

    ReleaseDC(hwnd, hdc);
    return 1;
}

void CeDisplaySuspend(void)
{
    /* Release the cached window DC before a dialog (Main Menu, Input/
     * Sound/Video Config, Save/Load State, the ROM picker, ...) takes
     * the screen - see s_cachedWndDC's doc comment above. */
    if (s_cachedWndDC)
    {
        ReleaseDC(s_hwnd, s_cachedWndDC);
        s_cachedWndDC = NULL;
    }
}

void CeDisplayResume(void)
{
}

void CeDisplayShutdown(void)
{
    if (s_cachedWndDC)
    {
        ReleaseDC(s_hwnd, s_cachedWndDC);
        s_cachedWndDC = NULL;
    }
    if (s_memDC)
    {
        DeleteDC(s_memDC);
        s_memDC = NULL;
    }
    if (s_dibBitmap)
    {
        DeleteObject(s_dibBitmap);
        s_dibBitmap = NULL;
        s_dibBits = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Scale geometry cache                                                */
/* ------------------------------------------------------------------ */

/* Recomputed only when something that affects it actually changes
 * (Scale mode, or the source frame's own dimensions - the SNES varies
 * these between 256x224 / 256x239 / 512-wide hi-res, so nothing here
 * assumes they're fixed), not every frame. This also gates the
 * one-time PatBlt border clear below: a bordered mode's letterbox area
 * only needs clearing once, since this device's GDI surface is a
 * direct, persistent framebuffer - untouched pixels simply stay put
 * across frames. CE_DISPLAY_MAX_DIM comfortably exceeds this device's
 * physical width (480) and bounds s_dstW (destination pixels). */
#define CE_DISPLAY_MAX_DIM 512
static int      s_dstX, s_dstY, s_dstW, s_dstH;
static uint32_t s_yStep;
static int      s_identityX;   /* 1 when s_dstW == srcW: no horizontal scaling, so each row is a straight copy */
static int      s_scale2x;     /* 1 when s_dstW == srcW*2: exact horizontal doubling. Not reachable at the
                                * SNES's normal geometry (256*2 == 512 > this device's 480 physical width),
                                * but kept for hi-res / other source widths - takes a duplicate-and-store
                                * fast path below (two output pixels per aligned 32-bit store). */
static int      s_scale3over2; /* 1 when s_dstW*2 == srcW*3 AND srcW%4==0: exact 3/2 scale
                                * (e.g. 256 -> 384; fixed "x1.5" in PopSNES). srcW%4==0 guarantees the
                                * 4-source/6-dest unrolled cycle below divides srcW exactly. */
static int      s_scale7over4; /* 1 when s_dstW*4 == srcW*7 AND srcW%8==0: exact 7/4 scale
                                * (e.g. 256 -> 448; fixed "Wide" in PopSNES), same reasoning as s_scale3over2
                                * but an 8-source/14-dest cycle. */
static int      s_scale15over8; /* 1 when s_dstW*8 == srcW*15 AND srcW%16==0: exact 15/8 scale
                                 * (e.g. Expand of a 256-wide frame: 256 -> 480). Same packed-store idea
                                 * as s_scale3over2/s_scale7over4, run as a 16-source/30-dest DOUBLE cycle
                                 * so the per-cycle byte count stays a multiple of 4 (round 26). */
static int      s_scale10over7; /* 1 when s_dstW*7 == srcW*10 AND srcW%7==0: exact 10/7 scale
                                 * (Expand of a PCE 7MHz dot clock frame: 336 -> 480). 7 source /
                                 * 10 dest per cycle = 5 packed 32-bit stores (20 bytes), so no
                                 * double cycle is needed. */
/* Any other ratio (the PCE's 7MHz/10MHz dot clock widths, anything a
 * clamp produces) takes the generic 16.16 DDA in CeDisplayBlitRGB565(),
 * two destination pixels per aligned 32-bit store. */
static uint32_t s_xStep;

static int         s_geomValid  = 0;
static unsigned    s_lastSrcW   = 0;
static unsigned    s_lastSrcH   = 0;
static CeScaleMode s_lastScaleMode;

static void ComputeGeometry(unsigned srcW, unsigned srcH, CeScaleMode scaleMode)
{
    unsigned dstW = (unsigned)s_physW;
    unsigned dstH = (unsigned)s_physH;
    unsigned blitW, blitH;

    if (scaleMode == CE_SCALE_1TO1)
    {
        /* Centred, unscaled - crop rather than scale down if the source
         * is somehow larger than the physical display. */
        blitW = (srcW < dstW) ? srcW : dstW;
        blitH = (srcH < dstH) ? srcH : dstH;
    }
    else if (scaleMode == CE_SCALE_FULLSCREEN || scaleMode == CE_SCALE_HALFSTRETCH)
    {
        /* Fill the display top-to-bottom. Full Screen keeps the source's
         * own aspect ratio (width = srcW * dstH / srcH, clamped to the
         * display); Half Stretch leaves half of Full Screen's letterbox
         * width - the same geometry this template's earlier GAPI backend
         * used. (The sister PopSNES instead pins these two to
         * fixed 3/2 and 7/4 ratios so its SNES 256-wide frame always hits
         * the 32-bit-packed fast paths below; that would make both modes
         * as wide as Expand for a 320-wide source, so the template keeps
         * the aspect-based math. The fast paths are still taken whenever
         * a core's geometry happens to land on one of those exact
         * ratios.) */
        unsigned fullW = (unsigned)(((uint32_t)srcW * dstH) / srcH);
        if (fullW > dstW)
            fullW = dstW;

        blitH = dstH;
        if (scaleMode == CE_SCALE_FULLSCREEN)
            blitW = fullW;
        else /* CE_SCALE_HALFSTRETCH */
            blitW = dstW - (dstW - fullW) / 2;
    }
    else /* CE_SCALE_EXPAND */
    {
        blitW = dstW;
        blitH = dstH;
    }

    if (blitW > dstW) blitW = dstW;
    if (blitH > dstH) blitH = dstH;
    if (blitW < 1) blitW = 1;
    if (blitH < 1) blitH = 1;
    if (blitW > CE_DISPLAY_MAX_DIM)
        blitW = CE_DISPLAY_MAX_DIM; /* defensive clamp - see CE_DISPLAY_MAX_DIM's doc comment */

    s_dstW = (int)blitW;
    s_dstH = (int)blitH;
    s_dstX = (int)((dstW - blitW) / 2);
    s_dstY = (int)((dstH - blitH) / 2);

    s_yStep = (srcH << 16) / blitH;

    s_identityX = ((unsigned)s_dstW == srcW);
    s_scale2x = (!s_identityX && (unsigned)s_dstW == srcW * 2);
    s_scale3over2 = (!s_identityX && !s_scale2x
                      && (unsigned)s_dstW * 2 == srcW * 3 && (srcW % 4) == 0);
    s_scale7over4 = (!s_identityX && !s_scale2x && !s_scale3over2
                      && (unsigned)s_dstW * 4 == srcW * 7 && (srcW % 8) == 0);
    s_scale15over8 = (!s_identityX && !s_scale2x && !s_scale3over2 && !s_scale7over4
                      && (unsigned)s_dstW * 8 == srcW * 15 && (srcW % 16) == 0);
    s_scale10over7 = (!s_identityX && !s_scale2x && !s_scale3over2 && !s_scale7over4 && !s_scale15over8
                      && (unsigned)s_dstW * 7 == srcW * 10 && (srcW % 7) == 0);
    /* Any ratio none of the above recognise (a PCE 7MHz/10MHz width,
     * Full Screen / Half Stretch clamped by the display-width cap, ...)
     * takes CeDisplayBlitRGB565()'s generic DDA from s_xStep. */
    s_xStep = (srcW << 16) / blitW;

    /* Once per geometry change: which source sizes a core really
     * produces decides which fixed-ratio fast path is worth adding. */
    CeLog("CeDisplay: geometry src=%ux%u dst=%dx%d path=%s",
          srcW, srcH, s_dstW, s_dstH,
          s_identityX ? "1:1" : s_scale2x ? "2x" : s_scale3over2 ? "3/2" :
          s_scale7over4 ? "7/4" : s_scale15over8 ? "15/8" :
          s_scale10over7 ? "10/7" : "dda");
}

/* Cheap perf instrumentation (no profiler on this device - see
 * CeLog()'s doc comment): logs average/max blit time every 60 calls,
 * then resets - same "perf: blit" shape the old GAPI backend logged,
 * kept because ce_main.c's own "perf: retro_run" comment still leans on
 * it (retro_run_avg minus blit_avg standing in for "core CPU/APU/PPU
 * emulation alone").
 *
 * s_writeAccumMs/s_bitbltAccumMs split the total further, into just the
 * nearest-neighbor write loop below and just the BitBlt() call - a GDI
 * blit is inherently heavier per frame than GAPI's direct framebuffer
 * write was, and this breakdown says which of the two halves is
 * carrying that cost on this device. Both are folded into (and still
 * counted by) the existing total/count above; they just add a
 * breakdown to the same log line. */
static unsigned s_blitAccumMs   = 0;
static unsigned s_blitMaxMs     = 0;
static unsigned s_blitCount     = 0;
static unsigned s_writeAccumMs  = 0;
static unsigned s_writeMaxMs    = 0;
static unsigned s_bitbltAccumMs = 0;
static unsigned s_bitbltMaxMs   = 0;

/* Last avg/max reported on the "perf: blit" line above, kept live across
 * the 60-frame reset so ce_main.c can fold it into its unified
 * "perf: retro_run avg=... blit avg=..." summary line (a common shape
 * logged by every core port for side-by-side comparison). 0 until the
 * first "perf: blit" line has been emitted. */
static unsigned s_lastBlitAvgMs = 0;
static unsigned s_lastBlitMaxMs = 0;

void CeDisplayGetLastBlitPerf(unsigned *avgMs, unsigned *maxMs)
{
    if (avgMs)
        *avgMs = s_lastBlitAvgMs;
    if (maxMs)
        *maxMs = s_lastBlitMaxMs;
}

void CeDisplayBlitRGB565(const void *src, unsigned srcW, unsigned srcH, unsigned srcPitchBytes)
{
    const uint8_t *srcBytes = (const uint8_t *)src;
    CeScaleMode scaleMode;
    HDC hdc;
    uint32_t yAccum;
    int y;
    DWORD t0, elapsed;
    DWORD tWrite0, writeElapsed;
    DWORD tBlit0, bitbltElapsed;

    if (!s_dibBitmap || !srcW || !srcH || !srcPitchBytes)
        return;

    t0 = GetTickCount();

    scaleMode = CeVideoGetScaleMode();
    if (!s_geomValid || srcW != s_lastSrcW || srcH != s_lastSrcH || scaleMode != s_lastScaleMode)
    {
        /* Something that affects the destination rect changed since the
         * last frame (or this is the first frame) - clear the whole
         * physical screen once so a shrinking bordered mode doesn't
         * leave stale pixels from a previous, larger rect around the
         * new one. */
        hdc = GetCachedWndDC();
        if (hdc)
            PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

        ComputeGeometry(srcW, srcH, scaleMode);
        s_lastSrcW = srcW;
        s_lastSrcH = srcH;
        s_lastScaleMode = scaleMode;
        s_geomValid = 1;
    }

    /* Nearest-neighbor stretch into the DIB's top-left dstW x dstH
     * corner (not offset by dstX/dstY within the DIB itself - the
     * offset is applied only once, below, at BitBlt time) - same
     * technique the old GAPI backend used directly on its raw
     * framebuffer pointer. One divide per axis per frame (in
     * ComputeGeometry above), never inside this pixel loop. */
    tWrite0 = GetTickCount();
    yAccum = 0;
    for (y = 0; y < s_dstH; y++)
    {
        unsigned srcY = (unsigned)(yAccum >> 16);
        const uint16_t *srcRow = (const uint16_t *)(srcBytes + (size_t)srcY * srcPitchBytes);
        uint16_t *dstRow = (uint16_t *)((uint8_t *)s_dibBits + (size_t)y * s_dibPitch);

        if (y > 0 && srcY == (unsigned)((yAccum - s_yStep) >> 16))
        {
            /* Same source row as the destination row just built (every
             * vertical upscale repeats rows - 224 -> 320 repeats 96 of
             * them): copy that finished row instead of scaling again.
             * Same row-reuse memcpy as the sister PopGB port. */
            memcpy(dstRow, (const uint8_t *)dstRow - s_dibPitch, (size_t)s_dstW * 2);
        }
        else if (s_identityX)
            memcpy(dstRow, srcRow, (size_t)s_dstW * 2);
        else if (s_scale2x)
        {
            /* Exact horizontal doubling (s_dstW == srcW*2) - every
             * source pixel maps to exactly two adjacent destination
             * pixels, in source order: a sequential read and a
             * duplicate-and-store, no per-pixel multiply. dstRow is
             * always 4-byte aligned here (see this file's header
             * comment), so both copies of a source pixel pack into one
             * aligned 32-bit store. Assumes a little-endian target,
             * same as the DDA path below. */
            unsigned sx;
            uint32_t *dstRow32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx++)
            {
                uint32_t px = srcRow[sx];
                dstRow32[sx] = px | (px << 16);
            }
        }
        else if (s_scale3over2)
        {
            /* Fixed 3/2 ratio (Full Screen "x1.5" - 256 -> 384): 4
             * source pixels become 6 dest pixels, weighted {2,2,1,1}
             * and packed into exactly 3 aligned 32-bit stores per cycle
             * - (S0,S0)(S1,S1)(S2,S3) - so every store is a 32-bit
             * pack, unlike the generic DDA path's per-pixel error
             * stepping. dstRow starts 4-byte aligned (see this file's
             * header comment) and each cycle advances the write pointer
             * by 3 uint32_t (12 bytes, still a multiple of 4), so
             * alignment holds across cycles too - no unaligned 32-bit
             * access ever occurs (see the -mno-unaligned-access build
             * flag / dev notes round 13 for why that matters on this
             * target). srcW%4==0 is guaranteed by the s_scale3over2
             * flag check. Assumes a little-endian target. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 4)
            {
                uint32_t p0 = srcRow[sx];
                uint32_t p1 = srcRow[sx + 1];
                uint32_t p2 = srcRow[sx + 2];
                uint32_t p3 = srcRow[sx + 3];
                d32[0] = p0 | (p0 << 16);
                d32[1] = p1 | (p1 << 16);
                d32[2] = p2 | (p3 << 16);
                d32 += 3;
            }
        }
        else if (s_scale7over4)
        {
            /* Fixed 7/4 ratio (Half Stretch "Wide" - 256 -> 448): 8
             * source pixels become 14 dest pixels, weighted
             * {2,2,2,1,2,2,2,1}, packed into exactly 7 aligned 32-bit
             * stores per cycle - (S0,S0)(S1,S1)(S2,S2)(S3,S4)(S4,S5)
             * (S5,S6)(S6,S7) - same alignment reasoning as s_scale3over2
             * above (each cycle advances the write pointer by 7 uint32_t
             * = 28 bytes, a multiple of 4); srcW%8==0 is guaranteed by
             * the flag check. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 8)
            {
                uint32_t p0 = srcRow[sx];
                uint32_t p1 = srcRow[sx + 1];
                uint32_t p2 = srcRow[sx + 2];
                uint32_t p3 = srcRow[sx + 3];
                uint32_t p4 = srcRow[sx + 4];
                uint32_t p5 = srcRow[sx + 5];
                uint32_t p6 = srcRow[sx + 6];
                uint32_t p7 = srcRow[sx + 7];
                d32[0] = p0 | (p0 << 16);
                d32[1] = p1 | (p1 << 16);
                d32[2] = p2 | (p2 << 16);
                d32[3] = p3 | (p4 << 16);
                d32[4] = p4 | (p5 << 16);
                d32[5] = p5 | (p6 << 16);
                d32[6] = p6 | (p7 << 16);
                d32 += 7;
            }
        }
        else if (s_scale15over8)
        {
            /* Fixed 15/8 ratio (Expand "Full" - 256 -> 480 exactly).
             * Run as a 16-source / 30-dest DOUBLE cycle, weights
             * {2,2,2,2,2,2,2,1, 2,2,2,2,2,2,2,1}, packed into exactly
             * 15 aligned 32-bit stores per cycle -
             * (S0,S0)(S1,S1)(S2,S2)(S3,S3)(S4,S4)(S5,S5)(S6,S6)(S7,S8)
             * (S8,S9)(S9,S10)(S10,S11)(S11,S12)(S12,S13)(S13,S14)(S14,S15).
             * Every store is a 32-bit pack, same as s_scale3over2 /
             * s_scale7over4 above; the generic DDA path (then one
             * 16-bit store per pixel) did 30 stores here (~12ms/row-loop vs ~2-3ms for the packed
             * paths on real hardware, round 26).
             * A single 8-source / 15-dest cycle would leave the write
             * pointer at a 30-byte (not multiple of 4) offset and
             * misalign the next cycle's 32-bit stores - the
             * -mno-unaligned-access hazard (dev notes round 13); the
             * doubled 16/30 cycle is 60 bytes, so alignment holds with
             * no 16-bit leftover. This weighting is the exact 15/8
             * nearest-neighbor result (dest column i samples source
             * floor(i*8/15)); the DDA's truncated step can pick one
             * column earlier at a few cycle boundaries.
             * srcW%16==0 is guaranteed by the s_scale15over8
             * flag check. Assumes a little-endian target, same as the
             * paths above. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 16)
            {
                uint32_t p0  = srcRow[sx];
                uint32_t p1  = srcRow[sx + 1];
                uint32_t p2  = srcRow[sx + 2];
                uint32_t p3  = srcRow[sx + 3];
                uint32_t p4  = srcRow[sx + 4];
                uint32_t p5  = srcRow[sx + 5];
                uint32_t p6  = srcRow[sx + 6];
                uint32_t p7  = srcRow[sx + 7];
                uint32_t p8  = srcRow[sx + 8];
                uint32_t p9  = srcRow[sx + 9];
                uint32_t p10 = srcRow[sx + 10];
                uint32_t p11 = srcRow[sx + 11];
                uint32_t p12 = srcRow[sx + 12];
                uint32_t p13 = srcRow[sx + 13];
                uint32_t p14 = srcRow[sx + 14];
                uint32_t p15 = srcRow[sx + 15];
                d32[0]  = p0  | (p0  << 16);
                d32[1]  = p1  | (p1  << 16);
                d32[2]  = p2  | (p2  << 16);
                d32[3]  = p3  | (p3  << 16);
                d32[4]  = p4  | (p4  << 16);
                d32[5]  = p5  | (p5  << 16);
                d32[6]  = p6  | (p6  << 16);
                d32[7]  = p7  | (p8  << 16);
                d32[8]  = p8  | (p9  << 16);
                d32[9]  = p9  | (p10 << 16);
                d32[10] = p10 | (p11 << 16);
                d32[11] = p11 | (p12 << 16);
                d32[12] = p12 | (p13 << 16);
                d32[13] = p13 | (p14 << 16);
                d32[14] = p14 | (p15 << 16);
                d32 += 15;
            }
        }
        else if (s_scale10over7)
        {
            /* Fixed 10/7 ratio (Expand of a 336-wide PCE frame - the
             * 7MHz dot clock width some games switch to - 336 -> 480):
             * 7 source pixels become 10 dest pixels, weighted
             * {2,1,2,1,2,1,1} - dest column i samples source
             * floor(i*7/10), the exact nearest-neighbor result - packed
             * into 5 aligned 32-bit stores per cycle:
             * (S0,S0)(S1,S2)(S2,S3)(S4,S4)(S5,S6). Each cycle advances
             * the write pointer 20 bytes, a multiple of 4, so alignment
             * holds; srcW%7==0 is guaranteed by the flag check. The
             * generic DDA path measured 7-9ms here on the device against
             * 3ms for the 320-wide 3/2 path. Little-endian, as above. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 7)
            {
                uint32_t p0 = srcRow[sx];
                uint32_t p1 = srcRow[sx + 1];
                uint32_t p2 = srcRow[sx + 2];
                uint32_t p3 = srcRow[sx + 3];
                uint32_t p4 = srcRow[sx + 4];
                uint32_t p5 = srcRow[sx + 5];
                uint32_t p6 = srcRow[sx + 6];
                d32[0] = p0 | (p0 << 16);
                d32[1] = p1 | (p2 << 16);
                d32[2] = p2 | (p3 << 16);
                d32[3] = p4 | (p4 << 16);
                d32[4] = p5 | (p6 << 16);
                d32 += 5;
            }
        }
        else
        {
            /* Generic nearest-neighbor fallback for any ratio none of
             * the fixed fast paths match. A DDA walking the source row
             * sequentially: a running 16.16 error advances the source
             * pointer, no table and no multiply (PopNES round 51 -
             * a lookup-table gather measured slower on this device, in
             * PopNES round 47 and again here in a 2-pixel form,
             * 15ms against 10ms). Two destination pixels per aligned
             * 32-bit store (dstRow is 4-byte aligned, see this file's
             * header comment) instead of one 16-bit store per pixel.
             * The source pointer is advanced only after a pixel is
             * taken and never past the last pixel a destination
             * column uses: the error after the final column is not
             * followed. Works for downscaling too (the while loops). */
            const uint16_t *sp = srcRow;
            uint32_t *d32 = (uint32_t *)dstRow;
            uint32_t step = s_xStep;
            uint32_t err = 0;
            int pairs = s_dstW >> 1;
            int i;
            for (i = 0; i < pairs; i++)
            {
                uint32_t lo, hi;
                lo = *sp;
                err += step;
                while (err >= 0x10000) { err -= 0x10000; sp++; }
                hi = *sp;
                *d32++ = lo | (hi << 16);
                if (i + 1 < pairs || (s_dstW & 1))
                {
                    err += step;
                    while (err >= 0x10000) { err -= 0x10000; sp++; }
                }
            }
            if (s_dstW & 1)
                dstRow[s_dstW - 1] = *sp;
        }

        yAccum += s_yStep;
    }

    writeElapsed = (DWORD)(GetTickCount() - tWrite0);
    s_writeAccumMs += writeElapsed;
    if (writeElapsed > s_writeMaxMs)
        s_writeMaxMs = writeElapsed;

    hdc = GetCachedWndDC();
    if (hdc)
    {
        tBlit0 = GetTickCount();
        BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, 0, 0, SRCCOPY);
        bitbltElapsed = (DWORD)(GetTickCount() - tBlit0);
        s_bitbltAccumMs += bitbltElapsed;
        if (bitbltElapsed > s_bitbltMaxMs)
            s_bitbltMaxMs = bitbltElapsed;
    }

    elapsed = (DWORD)(GetTickCount() - t0);
    s_blitAccumMs += elapsed;
    if (elapsed > s_blitMaxMs)
        s_blitMaxMs = elapsed;
    if (++s_blitCount >= 60)
    {
        s_lastBlitAvgMs = s_blitAccumMs / s_blitCount;
        s_lastBlitMaxMs = s_blitMaxMs;
        CeLog("perf: blit avg=%ums max=%ums (write avg=%ums max=%ums, bitblt avg=%ums max=%ums) over %u frames",
              s_lastBlitAvgMs, s_lastBlitMaxMs,
              s_writeAccumMs / s_blitCount, s_writeMaxMs,
              s_bitbltAccumMs / s_blitCount, s_bitbltMaxMs,
              s_blitCount);
        s_blitAccumMs = 0;
        s_blitMaxMs = 0;
        s_writeAccumMs = 0;
        s_writeMaxMs = 0;
        s_bitbltAccumMs = 0;
        s_bitbltMaxMs = 0;
        s_blitCount = 0;
    }
}

int CeDisplayGetLastImage(const void **bits, unsigned *width, unsigned *height, unsigned *pitchBytes)
{
    if (!s_dibBits || !s_geomValid)
        return 0;
    *bits = s_dibBits;
    *width = (unsigned)s_dstW;
    *height = (unsigned)s_dstH;
    *pitchBytes = (unsigned)s_dibPitch;
    return 1;
}

void CeDisplayForceRepaint(void)
{
    HDC hdc;

    if (!s_dibBitmap || !s_geomValid)
        return; /* nothing blitted yet - the caller's own black fill is all there is to show */

    /* Re-transfers the DIB's already-scaled top-left dstW x dstH corner
     * (last written by CeDisplayBlitRGB565() above, and never touched
     * since) - not the core's own frame pointer, which isn't guaranteed
     * to still be valid/current by the time a WM_PAINT happens to fire.
     * The caller is expected to have already painted the invalidated
     * region black first (ce_main.c's WndProc WM_PAINT) - this only
     * redraws the game rect on top of that. */
    hdc = GetCachedWndDC();
    if (!hdc)
        return;
    BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, 0, 0, SRCCOPY);
}
