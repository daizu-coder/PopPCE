/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Video Config dialog (IDD_VIDEOCONFIG in ce_res.rc) - see ce_video.h.
 *
 * Scale mode is a pure CE-frontend concern (ce_display.c's blit reads
 * CeVideoGetScaleMode() directly, no core involvement). Frame skip is
 * asked for by the PC Engine adapter (CE/core/pce_core_ce.c's
 * checkVariables()) through the standard libretro core-options
 * environment call (pce_frameskip) - CE answers that query from
 * ce_main.c's ce_environment() using the accessors below instead of
 * poking the core directly, keeping this a libretro-style *frontend*. Frame skip's *cap* on
 * consecutive skips (CeVideoFrameSkipShouldForceRender/NotifyRendered)
 * is CE-side, not a core option - see ce_video.h for why.
 */
#include "ce_video.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

#include <string.h>

static CeScaleMode s_scaleMode  = CE_SCALE_EXPAND; /* matches pre-Video-Config behaviour (always stretch-to-fill) */
/* PopPCE: the template's "No Sprite Limit" checkbox is removed - the
 * PC Engine core has no such option. */
/* Frame Skip: 0 = off (never skips); 1..30 = the maximum number of
 * consecutive frames the core is allowed to skip while it's actually
 * falling behind (pce_frameskip "auto" - the core skips while the
 * audio callback reports an underrun), enforced by
 * CeVideoFrameSkipShouldForceRender()/NotifyRendered() below - see
 * ce_video.h and ce_main.c's WinMain loop. The PC Engine adapter has no
 * cap of its own, so this dial is the only limit.
 * A round 7 fixed always-skip-N cadence ("skip N after every rendered
 * frame") felt uneven; a round 9 percentage "Speed" dial on top of the
 * core's own audio-occupancy threshold made no felt difference on
 * hardware and was removed at the user's request in favour of this. */
static int         s_frameSkip  = 0;
static int         s_frameSkipConsecutive = 0; /* frames skipped in a row since the last real render - see CeVideoFrameSkipShouldForceRender/NotifyRendered */
static int         s_debugLog   = 0;           /* "Enable Debug Logging" checkbox - default off; drives CeLogSetEnabled() (see ce_log.h) */

static int s_dirty = 0; /* consumed by CeVideoConsumeDirty() - see ce_video.h */

void CeVideoInit(void)
{
    int savedScaleMode;

    savedScaleMode  = CeConfigGetInt("VideoScaleMode", (int)s_scaleMode);
    switch (savedScaleMode)
    {
    case CE_SCALE_1TO1:
    case CE_SCALE_EXPAND:
    case CE_SCALE_FULLSCREEN:
    case CE_SCALE_HALFSTRETCH:
        s_scaleMode = (CeScaleMode)savedScaleMode;
        break;
    default:
        s_scaleMode = CE_SCALE_EXPAND; /* unrecognised value in an old/corrupt config file */
        break;
    }
    s_frameSkip     = CeConfigGetInt("VideoFrameSkip", s_frameSkip);
    s_debugLog      = CeConfigGetInt("VideoDebugLog", s_debugLog);
    CeLogSetEnabled(s_debugLog); /* WinMain already did this right after CeConfigLoad(); keep the two in sync */

    CeLog("CeVideoInit: loaded scaleMode=%d frameSkip=%d debugLog=%d from config file",
          (int)s_scaleMode, s_frameSkip, s_debugLog);
}

/* Registry-based persistence (samDesired/RegFlushKey lessons of round
 * 3/8 - see the dev notes) didn't survive an actual power-off on this
 * device (round 9 user report) - now goes through ce_config.c's plain
 * config file instead, same as ce_input.c/ce_audio.c. */
static void CeVideoSaveConfig(void)
{
    CeConfigSetInt("VideoScaleMode", (int)s_scaleMode);
    CeConfigSetInt("VideoFrameSkip", s_frameSkip);
    CeConfigSetInt("VideoDebugLog", s_debugLog);
    CeConfigSave();

    /* Apply the new logging state immediately (before the CeLog() below,
     * so turning it *on* captures this line too). */
    CeLogSetEnabled(s_debugLog);

    CeLog("CeVideoSaveConfig: saved scaleMode=%d frameSkip=%d debugLog=%d",
          (int)s_scaleMode, s_frameSkip, s_debugLog);
}

CeScaleMode CeVideoGetScaleMode(void)
{
    return s_scaleMode;
}

/* Core options the PC Engine adapter (CE/core/pce_core_ce.c) asks for. Only
 * frame skip exists; the template's other keys (sprite limit,
 * region) have no PC Engine counterpart. */
int CeVideoEnvGetVariable(const char *key, const char **outValue)
{
    if (strcmp(key, "pce_frameskip") == 0)
    {
        /* Plain "auto": the core skips drawing a frame while the audio
         * ring reports an underrun (CE/core/pce_core_ce.c:retro_run) -
         * CeVideoFrameSkipShouldForceRender()/NotifyRendered() (see
         * ce_video.h and ce_main.c's WinMain loop) cap how many of those
         * skips can happen in a row at s_frameSkip. */
        *outValue = (s_frameSkip > 0) ? "auto" : "disabled";
        return 1;
    }

    return 0;
}

int CeVideoConsumeDirty(void)
{
    int wasDirty = s_dirty;
    s_dirty = 0;
    return wasDirty;
}

int CeVideoFrameSkipShouldForceRender(void)
{
    return (s_frameSkip > 0 && s_frameSkipConsecutive >= s_frameSkip) ? 1 : 0;
}

void CeVideoFrameSkipNotifyRendered(int rendered)
{
    s_frameSkipConsecutive = rendered ? 0 : (s_frameSkipConsecutive + 1);
}

/* ------------------------------------------------------------------ */
/* Dialog                                                              */
/* ------------------------------------------------------------------ */

/* Display order for the Scale spinner (round 21, user request) - x1 =
 * CE_SCALE_1TO1, x1.5 = CE_SCALE_FULLSCREEN ("Full Screen 1:1"), Wide =
 * CE_SCALE_HALFSTRETCH ("Expand half"), Full = CE_SCALE_EXPAND. Cycles
 * with wraparound in both directions (unlike Frame Skip's 0..30 range,
 * these are 4 unordered categorical choices, not a quantity with a
 * natural minimum/maximum), same left/right-only interaction as Sound
 * Config's Bits/Quality pairs. */
static const CeScaleMode kScaleOrder[4] = {
    CE_SCALE_1TO1, CE_SCALE_FULLSCREEN, CE_SCALE_HALFSTRETCH, CE_SCALE_EXPAND
};
static const wchar_t *kScaleLabels[4] = { L"x1", L"x1.5", L"Wide", L"Full" };
#define CE_SCALE_CHOICE_COUNT 4

static int ScaleModeToIndex(CeScaleMode m)
{
    int i;
    for (i = 0; i < CE_SCALE_CHOICE_COUNT; i++)
        if (kScaleOrder[i] == m)
            return i;
    return 0; /* fallback: x1 */
}

static void UpdateScaleLabel(HWND hDlg)
{
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE), kScaleLabels[ScaleModeToIndex(s_scaleMode)]);
}

static void StepScaleMode(HWND hDlg, int delta)
{
    int idx = (ScaleModeToIndex(s_scaleMode) + delta + CE_SCALE_CHOICE_COUNT) % CE_SCALE_CHOICE_COUNT;
    s_scaleMode = kScaleOrder[idx];
    UpdateScaleLabel(hDlg);
}

/* IDC_VC_JAPANESE's own value readout (round 24, replacing a fixed
 * "English" CHECKBOX caption - see ce_resource.h). Originally shown as
 * romaji ("GAIKOKU-English"/"NIHON-Japanese") on the theory that this
 * button's caption used the OS-standard font, which couldn't render
 * Japanese glyphs unless a bundled jptahoma.ttc had loaded successfully
 * - now that this button is BS_OWNERDRAW and draws with the Shinonome
 * bitmap font (ce_bmpfont.c, baked into the binary - no load-failure
 * case to guard against any more), that constraint is gone, so this
 * shows the actual language name in both languages instead (ported
 * from the sister PopGB / PopNES projects' own change). Same
 * "leave it alone in ApplyVideoConfigLanguage" treatment as the
 * Scale/Frame Skip value readouts (kScaleLabels, "Off"/a number) - see
 * this dialog's own ApplyVideoConfigLanguage. */
static void UpdateLanguageLabel(HWND hDlg)
{
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_JAPANESE),
                   CeLangIsJapanese() ? L"\x65e5\x672c\x8a9e" /* 日本語 */ : L"English");
}

static void UpdateFrameSkipLabel(HWND hDlg)
{
    wchar_t text[8];
    if (s_frameSkip <= 0)
        _snwprintf(text, 8, L"Off");
    else
        _snwprintf(text, 8, L"%d", s_frameSkip);
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL), text);
}

/* Auto-repeat for the Frame Skip -/+ buttons (user request: stepping
 * from 0 to 30 one tap at a time is tiring). Standard WinCE PUSHBUTTON
 * controls don't auto-repeat on their own, so both buttons are
 * subclassed in WM_INITDIALOG to watch WM_LBUTTONDOWN/UP directly: press
 * starts a one-shot timer at FRAMESKIP_REPEAT_INITIAL_MS, its first fire
 * switches the same timer to the faster FRAMESKIP_REPEAT_INTERVAL_MS
 * cadence (so a quick tap doesn't also trigger a repeat), and each
 * subsequent fire steps s_frameSkip once. WM_LBUTTONUP/WM_CAPTURECHANGED
 * (capture is held by the button itself once pressed, so these always
 * reach it even if a finger drags off the button on this touch device)
 * kill the timer. The button's own BN_CLICKED (handled below, unchanged)
 * still fires once on release for a normal click - if a hold happened to
 * land exactly on a repeat tick just before release, that's at most one
 * extra step out of 0..30, not worth suppressing. Both buttons are
 * standard BUTTON-class controls, so they share one original window
 * proc address; it's re-fetched from IDC_VC_FRAMESKIP_UP each time the
 * dialog opens rather than cached process-wide. Neither button is
 * WS_TABSTOP (ce_res.rc) - touch-only, like Sound Config's own -/+
 * pairs, so VideoCtrlProc below never subclasses these two; the value
 * readout between them (IDC_VC_FRAMESKIP_LABEL) is the WS_TABSTOP
 * stand-in for physical-key adjustment, same split the earlier prototype uses for its
 * own -/+ pairs. */
#define FRAMESKIP_REPEAT_TIMER_ID     1
#define FRAMESKIP_REPEAT_INITIAL_MS   500
#define FRAMESKIP_REPEAT_INTERVAL_MS  120

static WNDPROC s_origFrameSkipBtnProc = NULL;
static int     s_frameSkipRepeatDir   = 0; /* +1 = up, -1 = down, 0 = not held */
static int     s_frameSkipRepeatFast  = 0; /* 0 until the initial-delay timer has fired once */

static LRESULT CALLBACK FrameSkipButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        s_frameSkipRepeatDir  = (GetDlgCtrlID(hwnd) == IDC_VC_FRAMESKIP_UP) ? 1 : -1;
        s_frameSkipRepeatFast = 0;
        SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INITIAL_MS, NULL);
        break;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        KillTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID);
        s_frameSkipRepeatDir = 0;
        break;

    case WM_TIMER:
        if (wParam == FRAMESKIP_REPEAT_TIMER_ID && s_frameSkipRepeatDir != 0)
        {
            if (!s_frameSkipRepeatFast)
            {
                s_frameSkipRepeatFast = 1;
                SetTimer(hwnd, FRAMESKIP_REPEAT_TIMER_ID, FRAMESKIP_REPEAT_INTERVAL_MS, NULL);
            }

            if (s_frameSkipRepeatDir > 0)
            {
                if (s_frameSkip < 30)
                    s_frameSkip++;
            }
            else
            {
                if (s_frameSkip > 0)
                    s_frameSkip--;
            }
            UpdateFrameSkipLabel(GetParent(hwnd));
        }
        break;
    }

    return CallWindowProc(s_origFrameSkipBtnProc, hwnd, msg, wParam, lParam);
}

static void  RefreshVideoConfigLanguage(HWND hDlg);
static void  ToggleLanguage(HWND hDlg);

/* Physical-key focus chain for this dialog: Scale -> Transparency ->
 * Frame Skip -> language spinner -> Enable Debug
 * Logging -> OK, wrapping back to Scale - same technique and the same
 * reason as an earlier
 * prototype's MiscNeighbor/MiscCtrlProc:
 * this device's dialog manager doesn't reliably move focus with the
 * arrow keys between dissimilar control types, and always routes decide
 * (Enter) to the DEFPUSHBUTTON (OK) regardless of what's actually
 * focused unless a control claims WANTALLKEYS and handles it itself -
 * OK included, so it can participate in the wrap instead of being an
 * arrow-key dead end. */
#define WM_SETVIDEOFOCUS (WM_APP + 202)

static int VideoNeighborDown(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_JAPANESE;
    case IDC_VC_JAPANESE:          return IDC_VC_DEBUGLOG;
    case IDC_VC_DEBUGLOG:          return IDOK;
    case IDOK:                     return IDC_VC_SCALE_VALUE;
    }
    return id;
}

static int VideoNeighborUp(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDOK;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_SCALE_VALUE;
    case IDC_VC_JAPANESE:          return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_DEBUGLOG:          return IDC_VC_JAPANESE;
    case IDOK:                     return IDC_VC_DEBUGLOG;
    }
    return id;
}

static WNDPROC s_pVideoOrigProc = NULL;

/* Subclasses the Scale value readout, Transparency, the Frame Skip value
 * readout, the language spinner, Enable Debug Logging
 * and OK - claims every
 * key unconditionally (DLGC_WANTARROWS | DLGC_WANTALLKEYS), same blanket
 * approach as the earlier prototype's MiscCtrlProc/SoundCtrlProc for the reasons given
 * in VideoNeighborDown's comment above. Unclaimed keys still fall
 * through to the native BUTTON control via CallWindowProc at the
 * bottom. */
static LRESULT CALLBACK VideoCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);

    if (message == WM_GETDLGCODE)
    {
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    }
    else if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborUp(id)));
            return 0;

        case VK_DOWN:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
            if (id == IDC_VC_SCALE_VALUE)
            {
                StepScaleMode(GetParent(hWnd), (wParam == VK_LEFT) ? -1 : 1);
            }
            else if (id == IDC_VC_FRAMESKIP_LABEL)
            {
                if (wParam == VK_LEFT)
                {
                    if (s_frameSkip > 0) s_frameSkip--;
                }
                else
                {
                    if (s_frameSkip < 30) s_frameSkip++;
                }
                UpdateFrameSkipLabel(GetParent(hWnd));
            }
            else if (id == IDC_VC_JAPANESE)
            {
                /* Two-state toggle - Left and Right both just flip it,
                 * same reasoning as ToggleLanguage's own comment. */
                ToggleLanguage(GetParent(hWnd));
            }
            return 0;

        case VK_RETURN:
            if (id == IDC_VC_SCALE_VALUE || id == IDC_VC_FRAMESKIP_LABEL || id == IDC_VC_JAPANESE)
            {
                /* Spinner, not a toggle - decide just moves on, same as
                 * Sound Config's own value spinners. */
                SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            }
            else if (id == IDC_VC_DEBUGLOG)
            {
                /* Same explicit-bool + InvalidateRect treatment as
                 * Transparency above (BS_OWNERDRAW checkbox) - persisted
                 * to disk and applied to CeLogSetEnabled() by the OK
                 * handler (CeVideoSaveConfig). */
                s_debugLog = !s_debugLog;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            else if (id == IDOK)
            {
                /* IDOK is subclassed too now (for the Up/Down wrap), so
                 * its own decide press has to be forwarded explicitly
                 * instead of falling through with no effect. */
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            return 0;

        case VK_ESCAPE:
            /* Claiming WANTALLKEYS above means this control, not the
             * dialog manager, now sees the physical Back key too -
             * without this it would silently do nothing while focus was
             * on one of these controls, instead of committing and
             * closing like OK does (see IDOK/IDCANCEL below). */
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pVideoOrigProc, hWnd, message, wParam, lParam);
}

/* Fixed LTEXT caption repainted by WM_PAINT via CeBmpFontPaintLabel()
 * (Shinonome bitmap-font migration, ported from the sister PopGB /
 * PopNES projects) - IDC_VC_LBL_SCALE ("Scale:") is untranslated
 * in both languages (see its own comment in ce_resource.h) but still
 * needs a hide+repaint now that OS-native STATIC rendering is gone.
 * IDC_VC_DEBUGLOG is a CHECKBOX, not LTEXT -
 * BS_OWNERDRAW (ce_res.rc) and redrawn via WM_DRAWITEM's
 * CeBmpFontDrawOwnerCheckbox() instead (see VideoConfigDlgProc's
 * WM_DRAWITEM case below). */
static const int kVideoLabelIds[] = {
    IDC_VC_LBL_SCALE, IDC_VC_LBL_FRAMESKIP,
};
#define CE_VIDEO_LABEL_COUNT (sizeof(kVideoLabelIds) / sizeof(kVideoLabelIds[0]))

/* One-shot at WM_INITDIALOG, and again right after IDC_VC_JAPANESE is
 * toggled so this still-open dialog reflects the change immediately
 * instead of only the next time it's reopened. The "Scale:" caption and
 * the four scale-mode radio captions (1:1/Expand/Full screen 1:1/Expand
 * half) are deliberately left untranslated even in Japanese mode - see
 * ce_resource.h's comment on IDC_VC_LBL_FRAMESKIP. */
static void ApplyVideoConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"\x30d5\x30ec\x30fc\x30e0\x30b9\x30ad\x30c3\x30d7\x3a"); /* フレームスキップ: */
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"\x30c7\x30d0\x30c3\x30b0\x30ed\x30b0\x3092\x6709\x52b9\x306b\x3059\x308b"); /* デバッグログを有効にする */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"Frame Skip:");
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"Enable Debug Logging");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kVideoLabelIds[i]), SW_HIDE);
    InvalidateRect(hDlg, NULL, TRUE);
}

/* Re-run every time the language spinner is toggled ("-"/"+" tap or
 * decide-key path, both below) - also refreshes IDC_VC_JAPANESE's own
 * value text via UpdateLanguageLabel(). */
static void RefreshVideoConfigLanguage(HWND hDlg)
{
    UpdateLanguageLabel(hDlg);
    ApplyVideoConfigLanguage(hDlg);
}

/* Shared by the WM_COMMAND MINUS/PLUS handlers and the physical-key
 * Left/Right path in VideoCtrlProc below - a two-state toggle has no
 * real "-" vs. "+" direction, so both simply flip it, same as Frame
 * Skip's Up/Down naturally clamping instead of wrapping at the ends of
 * its own range. */
static void ToggleLanguage(HWND hDlg)
{
    CeLangSetJapanese(!CeLangIsJapanese());
    RefreshVideoConfigLanguage(hDlg);
}

static INT_PTR CALLBACK VideoConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        /* No CheckDlgButton() here any more - IDC_VC_DEBUGLOG is
         * BS_OWNERDRAW
         * (ce_res.rc) and reads its checked state directly from
         * s_debugLog at
         * draw time (WM_DRAWITEM below),
         * not from BM_GETCHECK/BM_SETCHECK - see VideoCtrlProc's own
         * comment on why those stopped working once BS_OWNERDRAW was
         * added. */
        UpdateScaleLabel(hDlg);
        UpdateFrameSkipLabel(hDlg);
        RefreshVideoConfigLanguage(hDlg);

        s_frameSkipRepeatDir = 0;
        s_origFrameSkipBtnProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_UP), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_DOWN), GWLP_WNDPROC, (LONG_PTR)FrameSkipButtonSubclassProc);

        s_pVideoOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE),       GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL),   GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_JAPANESE),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_DEBUGLOG),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                     GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);

        /* Belt-and-suspenders initial focus, same pattern as every other
         * dialog in this port (ce_fileopen.c's WM_SETLISTFOCUS is the
         * first/most-documented instance): a synchronous SetFocus() from
         * WM_INITDIALOG alone doesn't always stick on this device. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlID == IDC_VC_DEBUGLOG)
            CeBmpFontDrawOwnerCheckbox(dis, s_debugLog);
        else
            CeBmpFontDrawOwnerButton(dis);
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        unsigned i;
        hdc = BeginPaint(hDlg, &ps);
        for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kVideoLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
            PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        }
        break;

    case WM_SETVIDEOFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_VC_SCALE_MINUS:
            StepScaleMode(hDlg, -1);
            return TRUE;

        case IDC_VC_SCALE_PLUS:
            StepScaleMode(hDlg, 1);
            return TRUE;

        case IDC_VC_LANG_MINUS:
        case IDC_VC_LANG_PLUS:
            /* Same control on both IDs - see ToggleLanguage's comment
             * on why a two-state spinner has no real "-" vs. "+"
             * direction. IDC_VC_JAPANESE itself (the value readout in
             * between) intentionally has no case here, same as
             * IDC_VC_SCALE_VALUE above it - tapping the readout does
             * nothing, only "-"/"+" (or Left/Right when it has focus,
             * see VideoCtrlProc) change it. */
            ToggleLanguage(hDlg);
            return TRUE;

        case IDC_VC_DEBUGLOG:
            /* Explicit bool flip + InvalidateRect, same as Transparency
             * (BS_OWNERDRAW checkbox). Persisted / applied to
             * CeLogSetEnabled() by the IDOK handler below. */
            s_debugLog = !s_debugLog;
            InvalidateRect(GetDlgItem(hDlg, IDC_VC_DEBUGLOG), NULL, TRUE);
            return TRUE;

        case IDC_VC_FRAMESKIP_DOWN:
            if (s_frameSkip > 0)
                s_frameSkip--;
            UpdateFrameSkipLabel(hDlg);
            return TRUE;

        case IDC_VC_FRAMESKIP_UP:
            if (s_frameSkip < 30)
                s_frameSkip++;
            UpdateFrameSkipLabel(hDlg);
            return TRUE;

        case IDOK:
        case IDCANCEL:
            /* Physical Back (IDCANCEL) acts the same as touching OK
             * here - this device has no meaningful "discard changes"
             * gesture, only "go back", so both commit and close (same
             * philosophy as every settings dialog in an earlier
             * prototype). Scale mode is already applied
             * live (ce_display.c reads it straight from module state); this just persists everything to disk and
             * pokes the core to re-poll its two options. */
            CeVideoSaveConfig();
            s_dirty = 1;
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

void CeShowVideoConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_VIDEOCONFIG),
               owner, VideoConfigDlgProc);
}
