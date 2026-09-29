// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_core_ce.h - CE-only extras of the libretro-shaped adapter over the
 * NitroGrafx core (CE/core/pce_core_ce.c). The standard retro_* entry points
 * come from libretro.h.
 */
#ifndef PCE_CORE_CE_H
#define PCE_CORE_CE_H

#include <stddef.h>
#include <wchar.h>

/* The ROM's wide path, used instead of retro_game_info.path (narrow,
 * CP_ACP) by the next retro_load_game() so non-ASCII file names work.
 * The string is copied. */
void pceCoreSetRomPathW(const wchar_t *path);

/* CD-ROM: a .cue loads with the System Card set here (wide path, copied;
 * kept for later loads). */
void pceCoreSetBiosPathW(const wchar_t *path);
/* 1 for a path the core loads as a CD image (.cue) */
int pceCoreIsCdImagePath(const wchar_t *path);
/* Checks a file's contents: 0 = not a System Card, 1 = a System Card,
 * 2 = a known Super System Card 3.0 (preferred). */
int pceCoreProbeBiosW(const wchar_t *path);

/* The bytes the last retro_serialize() actually used. Arcade Card games
 * leave out the unused part of the card's RAM, so this can be much less
 * than retro_serialize_size(); the front end writes only this much, and
 * retro_unserialize() takes a state of this size. */
size_t pceCoreStateUsedSize(void);

#endif
