/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Per-port identity macros for PopPCE (the same set of macros the sister
 * ports' frontends use, filled in for this port).
 */
#ifndef CE_APP_CONFIG_H
#define CE_APP_CONFIG_H

/* Human-readable name shown in message boxes, the title bar, the
 * "no ROM loaded" placeholder window title, AND the main menu dialog's
 * own title bar (IDD_MAINMENU's CAPTION in ce_res.rc, via
 * CE_APP_TITLE_A below). Keep it short - it's concatenated with a
 * filename in the title bar (see CE_APP_TITLE_FMT usage in ce_main.c)
 * on a 320px-wide screen.
 *
 * Edit CE_APP_TITLE_A (the narrow form) below, not CE_APP_TITLE
 * directly - CE_APP_TITLE is mechanically derived from it via token
 * pasting so the C-side wide string and ce_res.rc's dialog CAPTION
 * (which must be a narrow literal - a wide L"..." CAPTION is unverified
 * on this toolchain) can never drift apart. An earlier design kept the
 * CAPTION as a second, independent literal, and a sister port shipped
 * test builds with the old placeholder title in its menu that way. */
#define CE_APP_TITLE_A       "PopPCE"
#define CE_APP_WIDEN_(s)     L##s
#define CE_APP_WIDEN(s)      CE_APP_WIDEN_(s)
#define CE_APP_TITLE         CE_APP_WIDEN(CE_APP_TITLE_A)

/* Window class name. Must be unique per app on the device - reused as
 * the FindWindowW() target for single-instance detection, so two ports
 * sharing this frontend must NOT share this string. */
#define CE_APP_WND_CLASS     L"PopPCEWnd"

/* Named mutex for the single-instance check in WinMain(). Same
 * uniqueness requirement as CE_APP_WND_CLASS. */
#define CE_APP_MUTEX_NAME    L"PopPCE_SingleInstance"

/* Config file, written next to AppMain.exe via GetModuleFileNameW() +
 * truncate-to-last-backslash (see CeConfigGetPath() in ce_config.c). */
#define CE_APP_CONFIG_FILENAME  L"poppce.cfg"

/* Debug log file, same directory convention as the config file (see
 * CeLogInit() in ce_log.c). */
#define CE_APP_LOG_FILENAME     L"poppce_debug.log"

/* HKCU registry subkey an earlier, TrueType-font-based revision of
 * ce_lang.c used. Now that the UI font is baked in as a bitmap
 * (ce_bmpfont.c), nothing reads or writes this key any more - kept
 * only as this app's registry namespace. Keep the "Software\\" prefix
 * if reused. */
#define CE_APP_LANG_REGKEY   L"Software\\PopPCE\\Lang"

/* Prefix of the libretro-style core-option keys the PC Engine adapter
 * (CE/core/pce_core_ce.c) asks for - currently only "pce_frameskip".
 * ce_video.c's CeVideoEnvGetVariable() hardcodes the specific keys it
 * answers rather than deriving them from this prefix; this macro is
 * documentation of that prefix, not something the current code
 * consumes. */
#define CE_APP_CORE_OPT_PREFIX  "pce_"

/* Frame pacing target for WinMain's loop (whole fps, integer pacer). The
 * PC Engine runs at ~59.83 Hz; 60 is close enough. */
#define CE_TARGET_FPS  60

#endif /* CE_APP_CONFIG_H */
