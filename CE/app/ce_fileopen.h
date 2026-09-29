/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN) - a directory-browsing
 * listbox dialog that replaces GetOpenFileNameW(). Ported from an earlier
 * prototype's own IDD_FILEOPEN/DLGFileOpen (CE.c there): the
 * standard common file-open dialog has no way to render Japanese folder/
 * file names on this device (its own font is glyph-less for them,
 * confirmed by the dev notes, and unfixable via OFN_* flags), so
 * the earlier prototype replaced it outright with a self-drawn listbox using the bundled
 * Japanese font (see ce_lang.h) instead.
 */
#ifndef CE_FILEOPEN_H
#define CE_FILEOPEN_H

#include <windows.h>

/* No-op kept for WinMain's call order; the picker has no settings to
 * load any more (the old "Open Last Folder" toggle is always on). */
void CeFileOpenInit(void);

/* Which files the picker lists:
 *  - CE_FILEOPEN_ROM : HuCards (.pce/.sgx) and CD images (.cue)
 *  - CE_FILEOPEN_BIOS: .pce only, for picking the CD-ROM System Card
 *    (ce_main.c PromptAndPickPceBios). */
enum {
    CE_FILEOPEN_ROM  = 0,
    CE_FILEOPEN_BIOS = 1
};

/* Shows the picker, starting in the folder of the last successful pick
 * (falling back to "\Storage Card", then "\", if there is none or it no
 * longer exists). On a successful pick, fills outPath with the chosen
 * file's full path, remembers its folder for next time, and returns 1;
 * returns 0 if the user backed out at the root without picking
 * anything. */
int CeShowFileOpenDialog(HWND owner, wchar_t *outPath, size_t outPathCount, int mode);

#endif
