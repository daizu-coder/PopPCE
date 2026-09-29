/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_RESOURCE_H
#define CE_RESOURCE_H

/* App icon (ce_res.rc, CE/icon/poppce.ico) - the lowest-numbered ICON
 * resource, so the shell shows it for AppMain.exe. */
#define IDI_MAIN 100

/* Touch-to-reveal "menu" dialog (CE/ce_res.rc's IDD_MAINMENU): plain
 * DialogBoxW + PUSHBUTTON controls, not a real HMENU - this coredll
 * genuinely does not export SetMenu (checked with nm; CreateMenu/
 * AppendMenuW/DrawMenuBar exist but SetMenu/GetMenu don't - a real
 * limitation of this CE profile, not a mistake), and the Pocket-PC-style
 * alternative (SHCreateMenuBar) is both a different visual paradigm
 * (bottom command bar, not a desktop-style dropdown) and an unverified
 * fit for this CE profile. DialogBox is proven on this exact
 * hardware already (GetOpenFileNameW, the previous CE project's own
 * settings screens).
 *
 * Layout matches an earlier prototype's IDD_MAINMENU control-for-
 * control (same rects, same 210x140 dialog size): Open ROM full width
 * (WS_GROUP - the one thing that makes Up/Down *wrap* at the first/last
 * button on this device, plain sequential move already works without
 * it), Save/Load State as a 2-up row, the three config dialogs as a
 * 3-up row (in the same left-to-right slot the earlier prototype's Misc/Sound/Keys
 * occupy - Video Cfg is this project's "Misc" analogue, it's where the
 * language toggle lives, same reasoning as IDC_VC_JAPANESE below), Exit
 * full width, and a footer hint line instead of a separate Resume
 * button/About button - resuming is just the physical Back key
 * (IDCANCEL), matching the earlier prototype exactly. */
#define IDD_MAINMENU      3000
#define IDC_MM_OPEN       3002
#define IDC_MM_EXIT       3003
#define IDC_MM_INPUT      3004
#define IDC_MM_SOUND      3005
#define IDC_MM_VIDEO      3006
#define IDC_MM_SAVESTATE  3008
#define IDC_MM_LOADSTATE  3009
#define IDC_MM_HINT       3010
#define IDC_MM_SCREENSHOT 3011
/* Main-menu mascot bitmap (ce_res.rc's IDB_MAINMENU, from
 * CE/icon/poppce_mascot.bmp - 260x98 24bpp, the mascot drawing
 * flattened onto the menu's cream background, since this coredll has
 * no TransparentBlt/alpha blit).
 * MainMenuDlgProc's WM_PAINT (ce_main.c) StretchBlt's it into the blank
 * strip right of the IDC_MM_HINT text, aspect preserved, bottom-right
 * aligned. Same scheme as the sister PopSG port. */
#define IDB_MAINMENU      3012

/* Input Config dialog (CE/ce_res.rc's IDD_INPUTCONFIG): one PUSHBUTTON
 * per SNES joypad button, showing the currently-bound key - click it,
 * then press the physical key to bind (see ce_input.c). Native controls
 * again, same reasoning as IDD_MAINMENU above - a plain grid of
 * PUSHBUTTONs, so (like IDD_MAINMENU) it just needs WS_GROUP on the
 * first one for Up/Down wraparound, no per-control subclassing. No
 * Cancel button (see IDD_SOUNDCONFIG's comment on why) - only OK, and
 * the physical Back key (IDCANCEL) does the same thing OK does. No
 * Reset to Defaults button either (round 21, user request) - removed
 * along with its now-unused IDC_IC_RESET id.
 *
 * IDC_IC_BTN_SELECT (Select/Mode) is no longer placed as a control in
 * IDD_INPUTCONFIG - dropped from the remap UI to make room for the
 * diagonal-combo buttons below when a real port's button budget is
 * tight (this is what PopSG did, see its dev notes, round
 * 28/30). The id stays defined and stays in ce_input.c's s_map so
 * Select still works in-game on its existing/default key, it's just
 * not user-remappable from this dialog; ce_input.c's dialog code skips
 * it wherever it loops over s_map for UI purposes (GetDlgItem for this
 * id returns NULL). A port with room to spare can just add its
 * PUSHBUTTON/LTEXT back to ce_res.rc and re-add it to kInputOrder.
 *
 * IDC_IC_BTN_UPRIGHT/RIGHTDOWN/DOWNLEFT/LEFTUP: diagonal-combo buttons
 * (ported from PopSG's own Input Config, itself ported from the
 * sister PopSNES) - binding a physical key here
 * drives two RETRO_DEVICE_ID_JOYPAD_* directions at once (see
 * ce_input.c's s_map idB field and CeInputPoll). */
#define IDD_INPUTCONFIG      3100
#define IDC_IC_BTN_UP        3101
#define IDC_IC_BTN_DOWN      3102
#define IDC_IC_BTN_LEFT      3103
#define IDC_IC_BTN_RIGHT     3104
#define IDC_IC_BTN_SELECT    3105
#define IDC_IC_BTN_START     3106
#define IDC_IC_BTN_A         3107
#define IDC_IC_BTN_B         3108
#define IDC_IC_BTN_X         3109
#define IDC_IC_BTN_Y         3110
#define IDC_IC_BTN_L         3111
#define IDC_IC_BTN_R         3112
#define IDC_IC_BTN_UPRIGHT   3113
#define IDC_IC_BTN_RIGHTDOWN 3114
#define IDC_IC_BTN_DOWNLEFT  3115
#define IDC_IC_BTN_LEFTUP    3116

/* Row captions on the left column (Up/Down/Left/Right/Start) - these
 * were anonymous (-1) LTEXTs until the Japanese UI toggle needed to
 * address them individually via SetDlgItemTextW. The right column
 * (A/B/X/Y/L/R) and the four diagonal-combo captions (Up R/R Down/
 * Down L/L Up) below are left untranslated even in Japanese mode -
 * single-letter SNES button names and terse combo labels, same call
 * the earlier prototype made for its own A/B labels - but still need a real (non -1)
 * id each: this dialog's row captions are BS_OWNERDRAW-adjacent LTEXTs
 * repainted by CeBmpFontPaintLabel() (see ce_bmpfont.c), which resolves
 * its target control via GetDlgItem(hDlg, id) - with id == -1 that call
 * only ever finds *one* of several same-numbered controls (whichever
 * the dialog manager happens to enumerate first), so every LTEXT that
 * needs to be painted this way needs its own unique id even when its
 * text never changes with the language toggle (real-hardware-confirmed
 * lesson from the sister PopNES / PopGB ports' own bitmap-font
 * migration - the dev notes).
 * IDC_IC_LBL_SELECT (Select's row caption) is gone along with its row
 * - see IDC_IC_BTN_SELECT's comment above. */
#define IDC_IC_LBL_UP     3120
#define IDC_IC_LBL_DOWN   3121
#define IDC_IC_LBL_LEFT   3122
#define IDC_IC_LBL_RIGHT  3123
/* PopPCE: Select is back in Input Config (PC Engine pads have one), so
 * its label gets the id this template used to have for it. */
#define IDC_IC_LBL_SELECT 3124
#define IDC_IC_LBL_START  3125
#define IDC_IC_LBL_A         3126
#define IDC_IC_LBL_B         3127
#define IDC_IC_LBL_C         3128
#define IDC_IC_LBL_X         3129
#define IDC_IC_LBL_Y         3130
#define IDC_IC_LBL_Z         3131
#define IDC_IC_LBL_UPRIGHT   3132
#define IDC_IC_LBL_RIGHTDOWN 3133
#define IDC_IC_LBL_DOWNLEFT  3134
#define IDC_IC_LBL_LEFTUP    3135

/* Sound Config dialog (CE/ce_res.rc's IDD_SOUNDCONFIG) - see
 * ce_audio.c. Volume/Rate/Bits/Quality are all "-/value/+" spinners now
 * (ported from an earlier prototype's own Sound Settings dialog),
 * not a COMBOBOX + RADIOBUTTON pairs: the value lives on a WS_TABSTOP
 * PUSHBUTTON in the middle so it gets a native focus rectangle and
 * participates in the same physical-key Up/Down/Left/Right loop as
 * every other control here (see SoundConfigDlgProc's WM_GETDLGCODE
 * subclassing) - the "-"/"+" buttons on either side are touch-only
 * (no WS_TABSTOP), matching the earlier prototype's IDC_FRAMESKIP_MINUS/PLUS. No
 * Cancel button: this device has no meaningful "discard changes"
 * gesture, only "go back" - the physical Back key (IDCANCEL) commits
 * and closes exactly like OK does, same as every settings dialog in
 * the earlier prototype (see SoundConfigDlgProc's IDOK/IDCANCEL handling). */
#define IDD_SOUNDCONFIG      3200
#define IDC_SC_VOLUME_MINUS  3202
#define IDC_SC_VOLUME_VALUE  3203
#define IDC_SC_VOLUME_PLUS   3220
#define IDC_SC_RATE_MINUS    3221
#define IDC_SC_RATE_VALUE    3222
#define IDC_SC_RATE_PLUS     3223
#define IDC_SC_BITS_MINUS    3224
#define IDC_SC_BITS_VALUE    3225
#define IDC_SC_BITS_PLUS     3226
#define IDC_SC_QUALITY_MINUS 3227
#define IDC_SC_QUALITY_VALUE 3228
#define IDC_SC_QUALITY_PLUS  3229
/* Audio ring buffer size (mono frames) - same "-/value/+" spinner style
 * as the four above (ported from the sister PopSNES's
 * round 27, after a report of audible output latency at the old fixed
 * ring size). Steps through kBufferChoices[] in ce_audio.c
 * (2048/4096/8192/16384 = ~64/128/256/512 ms at this device's ~32 kHz
 * native rate); persisted as "SoundBuffer". Bigger = more latency but
 * rides out this device's retro_run() timing spikes without crackle. */
#define IDC_SC_BUFFER_MINUS  3230
#define IDC_SC_BUFFER_VALUE  3231
#define IDC_SC_BUFFER_PLUS   3232

/* Static captions - were anonymous (-1) LTEXTs, need real IDs for the
 * Japanese UI toggle to address them individually. */
#define IDC_SC_LBL_VOLUME    3210
#define IDC_SC_LBL_RATE      3211
#define IDC_SC_LBL_BITS      3212
#define IDC_SC_LBL_QUALITY   3213
#define IDC_SC_LBL_BUFFER    3214

/* Video Config dialog (CE/ce_res.rc's IDD_VIDEOCONFIG) - see
 * ce_video.c. Scale mode (1:1 / Full Screen 1:1 / Expand half / Expand -
 * see CeScaleMode in ce_video.h) is consumed directly by ce_display.c's
 * blit; frame skip is forwarded to the core via the standard libretro
 * core-options environment call (pce_frameskip) rather than poking the
 * core directly, keeping ce_video.c a frontend, not a core patch.
 * Scale is a "-/value/+" spinner (round 21, replacing a 2x2 radio-button
 * grid, user request) cycling through 4 states (displayed as
 * x1/x1.5/Wide/Full - see ce_video.c's kScaleOrder/kScaleLabels), same
 * "-/value/+" pattern as Sound Config's Volume/Rate/Bits/Quality and
 * this dialog's own Frame Skip row below - the value lives on a
 * WS_TABSTOP PUSHBUTTON so it gets a native focus rectangle and a place
 * in the physical-key Up/Down/Left/Right loop; "-"/"+" are touch-only
 * (no WS_TABSTOP). Frame Skip is the same pattern -
 * IDC_VC_FRAMESKIP_LABEL was an inert LTEXT before, now a WS_TABSTOP
 * PUSHBUTTON so it has a place in the physical-key focus loop. No
 * Cancel button - see IDD_SOUNDCONFIG's comment above. */
#define IDD_VIDEOCONFIG          3300
#define IDC_VC_SCALE_MINUS       3301
#define IDC_VC_SCALE_VALUE       3302
#define IDC_VC_SCALE_PLUS        3303
/* "Scale:" caption - was an anonymous (-1) LTEXT, needs a real id now
 * that every LTEXT in this dialog is repainted by CeBmpFontPaintLabel()
 * via WM_DRAWITEM/WM_PAINT (see IDC_IC_LBL_A's comment above for why -1
 * doesn't work with this style of ownerdraw). Deliberately left
 * untranslated even in Japanese mode, same as the scale-mode labels
 * themselves (x1/x1.5/Wide/Full) - see IDC_VC_LBL_FRAMESKIP's comment. */
#define IDC_VC_LBL_SCALE         3304
#define IDC_VC_TRANSPARENCY      3305
#define IDC_VC_FRAMESKIP_LABEL   3306
#define IDC_VC_FRAMESKIP_DOWN    3307
#define IDC_VC_FRAMESKIP_UP      3308

/* Static "Frame Skip:" caption - was an anonymous (-1) LTEXT, distinct
 * from IDC_VC_FRAMESKIP_LABEL above (the live numeric readout). The
 * "Scale:" caption and the four scale-mode labels (x1/x1.5/Wide/Full)
 * are deliberately left untranslated even in Japanese mode - technical
 * mode names, same call the earlier prototype made for its own output-rate radios
 * (11KHz/22KHz/44KHz). */
#define IDC_VC_LBL_FRAMESKIP     3309

/* Japanese/English UI toggle (see ce_lang.h) - lives here rather than a
 * new dialog because this one already has exactly this UI pattern (a
 * row of display options), and is this project's closest analogue to
 * the earlier prototype's Misc dialog (which houses the same toggle). Round 24 (user request) turned this from a CHECKBOX
 * captioned "English" into a third "-/value/+" spinner (same pattern
 * as Scale/Frame Skip above): the value readout (still IDC_VC_JAPANESE,
 * not renamed, for the same reason the earlier prototype kept its own control as
 * IDC_JAPAN even after relabelling it) shows the currently active
 * language's own name ("English" / "\x65e5\x672c\x8a9e" - see
 * ce_video.c's UpdateLanguageLabel) - originally shown as romaji
 * ("GAIKOKU-English"/"NIHON-Japanese") because this button's caption
 * used an OS-standard font that couldn't render Japanese glyphs unless
 * a bundled jptahoma.ttc had loaded; now that it's BS_OWNERDRAW and
 * draws with the baked-in Shinonome bitmap font (ce_bmpfont.c), that
 * constraint is gone (ported from the sister PopGB / PopNES
 * projects' own change). IDC_VC_LANG_MINUS/PLUS are the "-"/"+" touch
 * buttons flanking it; being a two-state
 * toggle, both simply flip it, same as Frame Skip's Up/Down at the 0/30
 * ends of its own range naturally clamping instead of wrapping. */
#define IDC_VC_JAPANESE          3310
#define IDC_VC_LANG_MINUS        3312
#define IDC_VC_LANG_PLUS         3313

/* "Enable Debug Logging" - bottom row of Video Config, same BS_OWNERDRAW
 * checkbox treatment as the "No Sprite Limit" box above. Persisted as "VideoDebugLog" (default 0/off); drives
 * CeLogSetEnabled() (see ce_log.h) so the log file is only written when
 * it's on. Ported from the sister PopSNES (round 28). */
#define IDC_VC_DEBUGLOG          3314

/* Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN, CE/ce_fileopen.c) -
 * replaces GetOpenFileNameW(), which has no way to show Japanese folder/
 * file names (see ce_fileopen.c). Ported from an earlier prototype's
 * own IDD_FILEOPEN/DLGFileOpen. */
#define IDD_FILEOPEN      3400
#define IDC_FO_PATH       3401
#define IDC_FO_LIST       3402

/* Generic message box (CE/ce_res.rc's IDD_MSGBOX, ce_main.c's
 * CeShowMsgBox()/MsgBoxDlgProc) - ported from the sister PopGB /
 * PopNES projects. MessageBoxW() draws with whatever system font
 * Windows CE finds, and this device has no CJK-capable one any more
 * now that jptahoma.ttc is gone (see ce_lang.h); this dialog instead
 * paints its own text with the Shinonome bitmap font, the same way
 * every other piece of Japanese UI text in this port already does. Use
 * it for any result message that can carry Japanese text (e.g.
 * Save/Load State success/failure - see CeSaveState()/CeLoadState() in
 * ce_main.c for both sister ports); a message that's always English-
 * only (ROM load failures, fatal startup errors) can stay a plain
 * MessageBoxW(). */
#define IDD_MSGBOX        3500
#define IDC_MB_TEXT       3501

/* Save State confirmation dialog (CE/ce_res.rc's IDD_SAVECONFIRM,
 * CeConfirmAndSaveState()/SaveConfirmDlgProc() in ce_main.c). Shown when
 * IDC_MM_SAVESTATE is pressed, before the single "<romPath>.state" slot
 * is overwritten. Two owner-draw buttons - はい/Yes (IDC_SF_YES) and
 * いいえ/No (IDC_SF_NO) - with Left/Right/Up/Down moving focus between
 * them and the decide key committing the focused one (the 2-button form
 * the dev notes recommend for new confirm dialogs).
 * On はい the dialog runs the save in place and then swaps its own
 * text/buttons to the result acknowledgement - it never closes and
 * reopens a second (nested) modal, which is what made the main menu's
 * "ステートセーブ" button flash to the front (section 6 / section 11).
 * Same 134x70 dimensions and self-drawn Shinonome-bitmap-font label
 * (WrapMsgBoxText()/s_msgBoxText) as IDD_MSGBOX. */
#define IDD_SAVECONFIRM   3600
#define IDC_SF_TEXT       3601
#define IDC_SF_YES        3602
#define IDC_SF_NO         3603

#endif
