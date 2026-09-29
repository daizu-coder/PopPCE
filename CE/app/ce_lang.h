/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Japanese/English UI text toggle - a persisted on/off flag consulted
 * by every dialog's own ApplyXLanguage() function (ce_main.c/
 * ce_input.c/ce_audio.c/ce_video.c/ce_fileopen.c) plus the Shinonome
 * bitmap-font renderer (ce_bmpfont.c/.h, core/app/ce_shinonome16.h) that
 * actually draws the text.
 *
 * This file used to also own a bundled "jptahoma.ttc" TrueType font -
 * AddFontResourceW() on a background thread, plus a registry flag
 * guarding against repeat-registration corruption ("tofu" glyphs) on
 * every relaunch within the same power-on session (see this file's
 * revision history in the sister PopNES / PopGB projects for
 * that design). jptahoma.ttc was removed: it was very likely a
 * Microsoft-proprietary font extracted from another device's ROM and
 * redistributed without permission, which made shipping it a licensing
 * problem (the dev notes). It's replaced by
 * CeBmpFontDrawTextW() (ce_bmpfont.c) - a Shinonome bitmap font baked
 * into the binary at build time instead of loaded from a file at
 * runtime, so there's no load-failure case to guard against and no
 * font resource to leak across relaunches any more. See
 * THIRDPARTY_LICENSES.txt at the repository root for the font's
 * license text and author credit.
 *
 * The old jptahoma.ttc-era API surface (CeLangGetUIFont(),
 * CeLangFontLoadFailed(), CeLangShutdown()) is gone along with it - the
 * bitmap font never fails to load and owns no HFONT/thread to release,
 * so every ApplyXLanguage() in this port now just calls
 * CeBmpFontDrawTextW()/CeBmpFontDrawOwnerButton()/CeBmpFontPaintLabel()
 * directly (see ce_bmpfont.h) instead of fetching a font handle first.
 */
#ifndef CE_LANG_H
#define CE_LANG_H

/* Loads the persisted UILanguageJapanese flag from CeConfigLoad()'s
 * table - call once from WinMain, after CeConfigLoad(). */
void CeLangInit(void);

/* Persisted preference (CeConfigLoad()'s table, key "UILanguageJapanese"),
 * default Japanese (1) on a fresh config file - still a user-visible
 * toggle either way (Video Config's language spinner, ce_video.c),
 * matching both the sister PopNES and PopGB projects. A port
 * that wants English as the out-of-the-box default can simply change
 * the fallback value CeLangInit() passes to CeConfigGetInt() in
 * ce_lang.c. */
int  CeLangIsJapanese(void);
void CeLangSetJapanese(int japanese);

#endif
