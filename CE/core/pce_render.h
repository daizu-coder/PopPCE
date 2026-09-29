// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_render.h - software renderer for the NitroGrafx VDC/VCE core
 * (replaces the NDS 2D hardware path of source/Gfx.s).
 */
#ifndef PCE_RENDER_H
#define PCE_RENDER_H

#include <stdint.h>

#define PCE_FB_PITCH	512		/* pixels per row of pceFrame */
#define PCE_FB_HEIGHT	240

/* Last finished frame, RGB565. Only the top-left pceFrameWidth x
 * pceFrameHeight pixels are valid. */
extern uint16_t pceFrame[PCE_FB_HEIGHT * PCE_FB_PITCH];
extern int pceFrameWidth;
extern int pceFrameHeight;
/* Incremented once per finished (drawn) frame. */
extern unsigned pceFrameCount;
/* Frame skip: nonzero when line 0 of a frame is reached means that frame
 * is not drawn at all (no tile/sprite work, pceFrame and pceFrameCount
 * left unchanged). The emulation itself still runs every line. */
extern int pceSkipRequest;
/* Debug perf probe (pce_core_ce.c, debug logging only), latched at line 0
 * like pceSkipRequest: one part of every line is done twice, so the extra
 * time of such a frame is that part's cost. The second pass redoes the
 * same work, so the picture is unchanged. */
enum { PCE_PROBE_NONE, PCE_PROBE_BG, PCE_PROBE_SPR, PCE_PROBE_OUT, PCE_PROBE_SCAN, PCE_PROBE_COUNT };
extern int pceRenderProbe;
/* Debug: set to 1 and the next pceEndFrame fills pceScrollLog with the
 * BG scroll of that frame's lines (see pce_render.c), then clears it. */
#define PCE_SCROLL_LOG_SIZE	1024
extern int pceScrollLogRequest;
extern char pceScrollLog[PCE_SCROLL_LOG_SIZE];

/* Called from CE/core/GfxCE.s: ceScanlineHook for each displayed line as it
 * ends, endFrame once all of them are drawn. */
void pceRenderLine(int line);
void pceEndFrame(void);

#endif
