// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_cd_ce.h - CD-ROM image access (CE/core/pce_cd_ce.c), used by
 * CE/core/pce_core_ce.c.
 */
#ifndef PCE_CD_CE_H
#define PCE_CD_CE_H

#include <stddef.h>
#include <wchar.h>

/* Opens a .cue and the single .bin it names, builds the TOC and marks the
 * disc inserted (cdrom.s:cdInserted). Returns 1 on success. */
int pceCdOpenCue(const wchar_t *cuePath);
/* The disc's IPL sector (PCE_CD_IPL_SIZE bytes), or NULL if the data
 * track has none. Set by pceCdOpenCue. */
#define PCE_CD_IPL_SIZE		2048
const unsigned char *pceCdIpl(void);
/* Closes the .bin and ejects the disc. */
void pceCdClose(void);

/* The CD part of a save state (see pce_cd_ce.c) */
size_t pceCdStateSize(void);
void pceCdSaveState(void *dest);
/* 1 if src starts with the CD part's tag (checked before anything is
 * loaded) */
int pceCdStateValid(const void *src);
int pceCdLoadState(const void *src);

/* The Arcade Card part (registers and RAM, all-zero 16KB blocks left out),
 * appended after the CD part by games on a card system. MaxSize is the
 * size with every block present; Save returns the bytes written; Load
 * checks the tag and that size covers the blocks, and returns 1 if it
 * loaded. */
size_t pceAcStateMaxSize(void);
size_t pceAcSaveState(void *dest);
int pceAcStateValid(const void *src, size_t size);
int pceAcLoadState(const void *src, size_t size);

/* Called after each frame of a CD game while debug logging is on: logs
 * where the CPU spends its time and the drive/ADPCM registers every 2 s. */
void pceCdDebugFrame(void);

/* For the perf log (the caller resets them): the read-ahead thread's reads
 * of the .bin (total ms, longest in ms, count, and how many moved the file
 * pointer first), and the core's waits for it (total ms, longest, count). */
extern unsigned pceCdReadMs, pceCdReadMaxMs, pceCdReads, pceCdSeeks;
extern unsigned pceCdWaitMs, pceCdWaitMaxMs, pceCdWaits;
/* The read-ahead thread's handle (NULL when no disc is open), for its CPU
 * time in the perf log */
void *pceCdReaderThread(void);

#endif
