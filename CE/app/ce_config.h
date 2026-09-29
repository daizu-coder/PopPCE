/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Plain-text "key=value" settings file next to the exe
 * ("<exe-dir>\" CE_APP_CONFIG_FILENAME, see ce_app_config.h), shared by
 * Input/Sound/Video Config.
 *
 * Replaces per-module HKEY_CURRENT_USER registry storage
 * (ce_input.c/ce_audio.c/ce_video.c all used to write their own
 * registry keys directly): a real power-off test (2026-08-01, round 9)
 * showed the registry doesn't actually survive on this device, even
 * though it does survive a plain app restart - consistent with round 8's
 * discovery that RegFlushKey() returns ERROR_NOT_SUPPORTED here (this
 * device's registry has no reliable "commit to persistent storage now"
 * operation to fall back on). A real fwrite()+fclose() to a file goes
 * through the standard C runtime/OS file write path instead, which is
 * the more dependable persistence mechanism available on this device.
 */
#ifndef CE_CONFIG_H
#define CE_CONFIG_H

#include <stddef.h> /* size_t */

/* Reads the whole file into memory (or starts with an empty table if it
 * doesn't exist yet). Call once from WinMain, before any module's
 * *_Init() calls CeConfigGetInt(). */
void CeConfigLoad(void);

int  CeConfigGetInt(const char *key, int defaultValue);

/* Updates the in-memory table only - does not touch the file. Call
 * CeConfigSave() after a batch of these (each module's own Save
 * function) to actually write it out. */
void CeConfigSetInt(const char *key, int value);

/* String variant (e.g. ce_fileopen.c's remembered last-folder path) -
 * same in-memory table underneath, values just happen to hold text
 * instead of a number. outValue is left untouched (caller already
 * defaulted it) if the key isn't present. */
void CeConfigGetString(const char *key, char *outValue, size_t outValueCount);
void CeConfigSetString(const char *key, const char *value);

/* Rewrites the whole file from the current in-memory table (so it always
 * contains every module's settings, not just the caller's own). */
void CeConfigSave(void);

#endif
