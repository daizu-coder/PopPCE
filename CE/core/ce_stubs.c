// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * ce_stubs.c - symbols the upstream NitroGrafx asm core expects from the
 * DS-side C code (Main.c, Shared/EmuMenu.c, FileHandling.c) and libnds.
 *
 * Step 1 placeholder: just enough to link. Each group is replaced by real
 * CE code in a later step (noted per group). `make undefined` in CE/
 * lists what the core asm still needs from outside itself.
 */
#include <windows.h>
#include <string.h>

typedef unsigned char u8;

/* --- Main.c ------------------------------------------------------------
 * Replaced by the CE frontend adapter (step 4). */
u8 powerIsOn = 0;

/* --- Shared/EmuMenu.c (DS on-screen menu) -------------------------------
 * The CE build uses its own Win32 dialogs (CE/app/) instead of EmuMenu, so
 * these stay as small CE-side definitions. */
u8 gGammaValue = 0;
u8 gDebugSet = 0;
u8 pauseEmulation = 0;

static u8 bin2Bcd(int v)
{
	return (u8)(((v / 10) << 4) | (v % 10));
}

/* Shared/AsmExtra.s:getTime reads 6 BCD bytes: hour, min, sec, year
 * (years since 1900, as struct tm), month, day. Used by the CD-ROM/Arcade
 * Card clock. The DS version also redraws the menu clock here. */
char *updateTime(void)
{
	static char timeBuffer[6];
	SYSTEMTIME st;

	GetLocalTime(&st);
	timeBuffer[0] = bin2Bcd(st.wHour);
	timeBuffer[1] = bin2Bcd(st.wMinute);
	timeBuffer[2] = bin2Bcd(st.wSecond);
	timeBuffer[3] = bin2Bcd((st.wYear - 1900) % 100);
	timeBuffer[4] = bin2Bcd(st.wMonth);
	timeBuffer[5] = bin2Bcd(st.wDay);
	return timeBuffer;
}

/* --- FileHandling.c (CD-ROM image access) -------------------------------
 * CE/core/pce_cd_ce.c. */

/* --- PCEPSG self-modifying code ------------------------------------------
 * pcepsg.s:updateAmplitudes patches the channel volumes into the
 * instructions of PCEPSGMixer (on the DS that code sits in uncached ITCM).
 * The ARM926 has separate instruction and data caches, so the new bytes
 * must be written back from the D-cache and the stale I-cache lines
 * dropped, or the mixer keeps playing old volumes. Called from
 * CE/core/ce_asmfix.s:cePsgCodeChanged. The mixer loop is about 360 bytes. */
#define PSG_MIXER_CODE_SIZE	512
extern const char PCEPSGMixer[];
/* The patched instructions (made global by CE/core/cefix.sed). Each one's low
 * byte is the 8-bit volume immediate updateAmplitudes stores. */
extern const unsigned char vol0_L[], vol0_R[], vol1_L[], vol1_R[], vol2_L[], vol2_R[],
	vol3_L[], vol3_R[], vol4_L[], vol4_R[], vol5_L[], vol5_R[];

/* Read by pce_core_ce.c's perf log: calls, and calls that flushed. */
unsigned cePsgSyncCalls, cePsgSyncFlushes;

void ceSyncPsgCode(void)
{
	/* With SAMPLE_PLAYING the mixer runs every scanline, and games that
	 * play DDA samples rewrite the channel control register (so
	 * amplitudeChg) far more often than the volumes actually change.
	 * Only pay for the cache sync when a volume byte really differs:
	 * 12 byte compares, where this used to memcmp the whole 512 bytes
	 * of mixer code. The first call always syncs (the shadow does not
	 * yet hold what the I-cache may have). */
	static const unsigned char *const imm[12] = {
		vol0_L, vol0_R, vol1_L, vol1_R, vol2_L, vol2_R,
		vol3_L, vol3_R, vol4_L, vol4_R, vol5_L, vol5_R
	};
	static unsigned char shadow[12];
	static int valid;
	int i, changed = !valid;

	cePsgSyncCalls++;
	for (i = 0; i < 12; i++) {
		unsigned char b = *imm[i];
		if (b != shadow[i]) {
			shadow[i] = b;
			changed = 1;
		}
	}
	if (!changed)
		return;
	valid = 1;
	cePsgSyncFlushes++;
	FlushInstructionCache(GetCurrentProcess(), PCEPSGMixer, PSG_MIXER_CODE_SIZE);
}
