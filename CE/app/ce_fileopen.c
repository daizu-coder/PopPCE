/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Custom ROM picker dialog (IDD_FILEOPEN in ce_res.rc) - see
 * ce_fileopen.h. Ported to match an earlier prototype's own
 * IDD_FILEOPEN/DLGFileOpen (CE.c there) as closely as possible: same
 * up-navigation-as-"<"-entry, same directories-first sort, same
 * WM_SETLISTFOCUS belt-and-suspenders focus fix, same "start over at
 * \Storage Card every time it's opened" default. ROM/folder names
 * (potentially Japanese regardless of the UI language setting) and
 * IDOK's own caption are drawn with the Shinonome bitmap font
 * (ce_bmpfont.c) instead of an OS-native font - see IDC_FO_LIST's
 * LBS_OWNERDRAWFIXED comment in ce_res.rc and this file's own
 * WM_MEASUREITEM/WM_DRAWITEM handling below. IDC_FO_LIST rows and the
 * IDC_FO_PATH label use the *Boxed* variants
 * (CeBmpFontDrawTextBoxedW / CeBmpFontPaintLabelBoxed) so codepoints the
 * Shinonome font lacks (non-JIS-X-0208 kanji, surrogate-pair chars, ...)
 * render as one □ (U+25A1) here instead of silently collapsing to blank
 * - user-supplied names must stay distinguishable and it must be obvious
 * when part of a name can't be shown.
 */
#include "ce_fileopen.h"
#include "ce_log.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_config.h"
#include "ce_resource.h"

#include <stdlib.h>
#include <wchar.h>

/* The picker always reopens where the last successful pick left off
 * (ported from the earlier prototype's "Open Last Folder", which used to be a Video
 * Config checkbox; the sister PopSNES made it always-on and
 * dropped the checkbox, since nobody turned it off). The remembered
 * folder is stored CP_UTF8 as "VideoLastDir", matching every other
 * wchar_t<->char boundary the earlier prototype crossed for Japanese-safe text. On the
 * very first launch (no "VideoLastDir" key yet) InitStartDir() falls
 * back to \Storage Card. */
void CeFileOpenInit(void)
{
}

/* Deferred re-assertion of listbox focus - see DLGFileOpenProc()'s
 * WM_INITDIALOG/WM_ACTIVATE for why. */
#define WM_SETLISTFOCUS (WM_APP + 100)

typedef struct
{
    wchar_t name[MAX_PATH];
    BOOL    isDir;
} CeFileEntry;

/* Left/Right page size (FileListCtrlProc below) - also needed here in
 * FillFileList() to pad the list out to a page-boundary-aligned count,
 * so paging to the final (possibly partial) page can actually land its
 * first real entry on the listbox's top row - see FillFileList()'s own
 * comment on the padding entries for why. Fixed at a real-hardware-
 * confirmed row count for IDC_FO_LIST's current height (ce_res.rc)
 * rather than computed from the listbox's own height at runtime -
 * ported from the sister PopGB / PopNES projects' own
 * ce_fileopen.c (both hardware-confirmed this boundary-aligned design:
 * an earlier "current position +/- one screenful" version landed
 * wherever the cursor happened to be within the page instead of always
 * the first row of the next/previous one). A new port should re-verify
 * this row count on its own hardware if IDC_FO_LIST's height changes -
 * the dev notes have the DLU-vs-pixel
 * mismatch that made a hand-calculated row count unreliable here in
 * the first place. */
#define CE_FILELIST_PAGE_SIZE 6

#define CE_MAX_FILE_ENTRIES 512
static CeFileEntry s_fileEntries[CE_MAX_FILE_ENTRIES];
static int         s_fileEntryCount;
static wchar_t     s_currentDir[MAX_PATH];
static wchar_t     s_lastPickedFile[MAX_PATH];

static WNDPROC s_pFileListOrigProc = NULL;

/* Authoritative current page for the file listbox's Left/Right paging.
 * Deriving the page purely from LB_GETCURSEL each keypress (this file's
 * previous, non-wrapping version) proved unreliable on the sister PopGBA
 * project's identical owner-draw listbox once wraparound was added there
 * - real-hardware report: paging stopped wrapping, the target row was
 * not pulled to the top, and mid-list the keys degraded into a
 * one-row-at-a-time drift. FillFileList() resets this to 0;
 * FileListCtrlProc() below is the only thing that advances it
 * (re-syncing from LB_GETCURSEL first so Up/Down moves between page
 * jumps are still respected). */
static int s_fileListPage = 0;

/* Left/Right = jump to the first row of the next/previous page, wrapping
 * round at both ends (Right off the last page -> first page, Left off
 * the first -> last) - user request, ported from the sister PopGBA
 * project's own FileListCtrlProc (see s_fileListPage's comment above for
 * why it tracks the page in a static instead of deriving it fresh from
 * LB_GETCURSEL each time). Page size is fixed at CE_FILELIST_PAGE_SIZE
 * (see its comment above) so boundaries always land on '0 -> 6 -> 12 ->
 * ...'. FillFileList()'s blank-padding entries are what make a trailing
 * partial page's remaining rows come out genuinely blank instead of the
 * list silently scrolling back to stay full - see that function's
 * comment. WM_GETDLGCODE forwards to the listbox's own default response
 * and just ORs in DLGC_WANTARROWS - it almost certainly already claims
 * that (Up/Down navigation already worked before this subclass existed),
 * so this is a defensive addition, not a replacement of whatever made
 * that work. */
static LRESULT CALLBACK FileListCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return CallWindowProc(s_pFileListOrigProc, hWnd, message, wParam, lParam) | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT))
    {
        LRESULT count = SendMessage(hWnd, LB_GETCOUNT, 0, 0);
        LRESULT cur;
        int pageCount, newTop;

        if (count <= 0)
            return 0;

        pageCount = ((int)count + CE_FILELIST_PAGE_SIZE - 1) / CE_FILELIST_PAGE_SIZE;

        /* Re-sync from the live selection (Up/Down may have moved it
         * since the last page jump), then step with wraparound. */
        cur = SendMessage(hWnd, LB_GETCURSEL, 0, 0);
        if (cur != LB_ERR)
            s_fileListPage = (int)cur / CE_FILELIST_PAGE_SIZE;
        if (s_fileListPage < 0 || s_fileListPage >= pageCount)
            s_fileListPage = 0;

        if (wParam == VK_RIGHT)
            s_fileListPage = (s_fileListPage + 1) % pageCount;
        else
            s_fileListPage = (s_fileListPage + pageCount - 1) % pageCount;

        newTop = s_fileListPage * CE_FILELIST_PAGE_SIZE;
        if (newTop > (int)count - 1)
            newTop = (int)count - 1;

        /* Order matters here (real-hardware lesson from the sister
         * PopNES project: focus visibly flickered down a row or
         * more before snapping back up to the top) - LB_SETCURSEL's own
         * "scroll the new selection into view" logic runs *before*
         * anything else gets a chance to fix the scroll position, so
         * calling it first always painted one intermediate frame with
         * newTop scrolled to wherever the control's default landed it,
         * immediately followed by a second repaint once LB_SETTOPINDEX
         * corrected it back to the top - two real paints, hence the
         * visible bounce. LB_SETTOPINDEX first instead means newTop is
         * already the top (and therefore already "fully visible") row
         * by the time LB_SETCURSEL runs, so its own scroll-into-view has
         * nothing left to do and it only updates the highlight -
         * collapses back to one paint on its own. The trailing
         * LB_SETTOPINDEX is a second belt-and-suspenders shove for when
         * LB_SETCURSEL's own scroll-into-view still drags the view off
         * the boundary anyway (PopGBA real-hardware report).
         *
         * Deliberately NOT wrapped in WM_SETREDRAW FALSE/TRUE + a forced
         * InvalidateRect(..., TRUE) as a "belt and suspenders" on top of
         * the reordering above - that turned out to be the cause of a
         * *different* real-hardware complaint on the sister PopNES
         * project (page jumps rendering visibly "one character at a
         * time", see CeBmpFontDrawTextW's per-pixel SetPixel() cost in
         * ce_bmpfont.c): forcing a full owner-draw invalidate on every
         * page jump makes all CE_FILELIST_PAGE_SIZE visible rows redraw
         * through that slow glyph path at once, instead of the listbox's
         * own narrower, ScrollWindow-based internal repaint (a fast
         * blit for rows that just moved, no owner-draw glyph re-render
         * needed) that LB_SETTOPINDEX/LB_SETCURSEL already trigger on
         * their own without any extra help. */
        SendMessage(hWnd, LB_SETTOPINDEX, (WPARAM)newTop, 0);
        SendMessage(hWnd, LB_SETCURSEL, (WPARAM)newTop, 0);
        SendMessage(hWnd, LB_SETTOPINDEX, (WPARAM)newTop, 0);
        return 0;
    }

    return CallWindowProc(s_pFileListOrigProc, hWnd, message, wParam, lParam);
}

static int CompareFileEntries(const void *a, const void *b)
{
    const CeFileEntry *ea = (const CeFileEntry *)a;
    const CeFileEntry *eb = (const CeFileEntry *)b;
    if (ea->isDir != eb->isDir)
        return eb->isDir - ea->isDir; /* directories first */
    return lstrcmpiW(ea->name, eb->name);
}

static BOOL IsRootDir(void)
{
    return wcscmp(s_currentDir, L"\\") == 0;
}

static void NavigateUp(void)
{
    wchar_t *slash = wcsrchr(s_currentDir, L'\\');
    if (slash == s_currentDir)
        slash[1] = L'\0';
    else if (slash != NULL)
        slash[0] = L'\0';
    if (s_currentDir[0] == L'\0')
        wcscpy(s_currentDir, L"\\");
}

/* Which mode CeShowFileOpenDialog() was called in (CE_FILEOPEN_ROM /
 * CE_FILEOPEN_BIOS - see ce_fileopen.h). Set once at the top of that
 * function, consumed by FileMatchesPickMode() from FillFileList().
 * Ported from the sister PopSG. */
static int s_pickMode = CE_FILEOPEN_ROM;

/* What the NitroGrafx core loads (CE/core/pce_core_ce.c): HuCards,
 * SuperGrafx cards, and CD images as a .cue (with its single .bin, which
 * is not listed - it is opened through the .cue). */
static BOOL IsRomExtension(const wchar_t *name)
{
    static const wchar_t *const kExt[] = {
        L".pce", L".sgx", L".cue"
    };
    const wchar_t *ext = wcsrchr(name, L'.');
    unsigned i;

    if (!ext)
        return FALSE;
    for (i = 0; i < sizeof(kExt) / sizeof(kExt[0]); i++)
        if (lstrcmpiW(ext, kExt[i]) == 0)
            return TRUE;
    return FALSE;
}

/* True if this filename should be listed given the current pick mode.
 * The System Card is a HuCard image, so the BIOS pick lists .pce only. */
static BOOL FileMatchesPickMode(const wchar_t *name)
{
    if (s_pickMode == CE_FILEOPEN_BIOS)
    {
        const wchar_t *ext = wcsrchr(name, L'.');
        return ext && lstrcmpiW(ext, L".pce") == 0;
    }
    return IsRomExtension(name);
}

static void FillFileList(HWND hDlg)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    wchar_t pattern[MAX_PATH];
    HWND hList = GetDlgItem(hDlg, IDC_FO_LIST);
    int start;
    int i;

    s_fileEntryCount = 0;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    if (!IsRootDir() && s_fileEntryCount < CE_MAX_FILE_ENTRIES)
    {
        wcscpy(s_fileEntries[s_fileEntryCount].name, L"<");
        s_fileEntries[s_fileEntryCount].isDir = TRUE;
        s_fileEntryCount++;
    }

    wcscpy(pattern, s_currentDir);
    if (pattern[wcslen(pattern) - 1] != L'\\')
        wcscat(pattern, L"\\");
    wcscat(pattern, L"*.*");

    hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (s_fileEntryCount >= CE_MAX_FILE_ENTRIES)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                wcsncpy(s_fileEntries[s_fileEntryCount].name, fd.cFileName, MAX_PATH - 1);
                s_fileEntries[s_fileEntryCount].name[MAX_PATH - 1] = L'\0';
                s_fileEntries[s_fileEntryCount].isDir = TRUE;
                s_fileEntryCount++;
            }
            else if (FileMatchesPickMode(fd.cFileName))
            {
                wcsncpy(s_fileEntries[s_fileEntryCount].name, fd.cFileName, MAX_PATH - 1);
                s_fileEntries[s_fileEntryCount].name[MAX_PATH - 1] = L'\0';
                s_fileEntries[s_fileEntryCount].isDir = FALSE;
                s_fileEntryCount++;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    start = IsRootDir() ? 0 : 1; /* leave the "<" up-entry, if any, pinned at index 0 */
    qsort(&s_fileEntries[start], s_fileEntryCount - start, sizeof(CeFileEntry), CompareFileEntries);

    /* Pad the list with blank (name[0] == '\0') entries out to the next
     * CE_FILELIST_PAGE_SIZE boundary - user request, ported from PopGB /
     * PopNES's own FillFileList(): without this, paging
     * (FileListCtrlProc's Left/Right above) onto a final page shorter
     * than a full page left it looking like nothing had happened,
     * because LB_SETTOPINDEX only scrolls "up to the maximum scroll
     * range" (documented Win32 listbox behavior) - it refuses to put an
     * item within the last screenful at the very top if doing so would
     * leave blank space below, and instead silently clamps back to keep
     * the view full of real rows. Real blank rows in the list give it
     * somewhere to actually scroll to, so the newly-focused entry lands
     * on the top row with genuinely empty rows below it, matching what a
     * fixed page size implies. EnterSelected() below treats a blank
     * entry as a no-op, the same as it already does for "<" and
     * directories, so these can't be selected. */
    if (s_fileEntryCount % CE_FILELIST_PAGE_SIZE != 0)
    {
        int padTo = ((s_fileEntryCount / CE_FILELIST_PAGE_SIZE) + 1) * CE_FILELIST_PAGE_SIZE;
        if (padTo > CE_MAX_FILE_ENTRIES)
            padTo = CE_MAX_FILE_ENTRIES;
        while (s_fileEntryCount < padTo)
        {
            s_fileEntries[s_fileEntryCount].name[0] = L'\0';
            s_fileEntries[s_fileEntryCount].isDir = FALSE;
            s_fileEntryCount++;
        }
    }

    for (i = 0; i < s_fileEntryCount; i++)
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s_fileEntries[i].name);
    SendMessageW(hList, LB_SETCURSEL, 0, 0);
    s_fileListPage = 0; /* FileListCtrlProc's Left/Right paging starts fresh */

    SetDlgItemTextW(hDlg, IDC_FO_PATH, s_currentDir);
}

/* One-shot at WM_INITDIALOG - the OK button is the only translatable
 * text in this dialog (path/filenames are literal filesystem content,
 * and the "<" up-entry is deliberately language-neutral). The window
 * title bar ("Select ROM") is never touched, same reasoning as every
 * other ApplyXLanguage() in this port - it's non-client area the OS
 * paints with its own caption font. IDOK is BS_OWNERDRAW (ce_res.rc)
 * and redraws itself from this text via WM_DRAWITEM - no font handle
 * needed any more. */
static void ApplyFileOpenLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
        SetDlgItemTextW(hDlg, IDOK, L"\x6c7a\x5b9a"); /* 決定 */
    else
        SetDlgItemTextW(hDlg, IDOK, L"OK");
}

static void InitStartDir(void)
{
    DWORD attrs = 0xFFFFFFFF;

    {
        char lastDirUtf8[MAX_PATH] = "";
        CeConfigGetString("VideoLastDir", lastDirUtf8, MAX_PATH);
        if (lastDirUtf8[0] != '\0')
        {
            MultiByteToWideChar(CP_UTF8, 0, lastDirUtf8, -1, s_currentDir, MAX_PATH);
            attrs = GetFileAttributesW(s_currentDir);
        }
    }

    if (attrs == 0xFFFFFFFF || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        wcscpy(s_currentDir, L"\\Storage Card");
        attrs = GetFileAttributesW(s_currentDir);
        if (attrs == 0xFFFFFFFF || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            wcscpy(s_currentDir, L"\\");
    }
}

/* Enters the current selection (the "<" up-entry or a folder) or, for a
 * file, records the full path in s_lastPickedFile and returns TRUE so
 * the caller can close the dialog. */
static BOOL EnterSelected(HWND hDlg)
{
    LRESULT idx = SendDlgItemMessageW(hDlg, IDC_FO_LIST, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR || idx >= s_fileEntryCount)
        return FALSE;

    if (s_fileEntries[idx].name[0] == L'\0') /* blank padding row (FillFileList) - nothing to enter */
        return FALSE;

    if (wcscmp(s_fileEntries[idx].name, L"<") == 0)
    {
        NavigateUp();
        FillFileList(hDlg);
        return FALSE;
    }
    if (s_fileEntries[idx].isDir)
    {
        if (s_currentDir[wcslen(s_currentDir) - 1] != L'\\')
            wcscat(s_currentDir, L"\\");
        wcscat(s_currentDir, s_fileEntries[idx].name);
        FillFileList(hDlg);
        return FALSE;
    }

    wcscpy(s_lastPickedFile, s_currentDir);
    if (s_lastPickedFile[wcslen(s_lastPickedFile) - 1] != L'\\')
        wcscat(s_lastPickedFile, L"\\");
    wcscat(s_lastPickedFile, s_fileEntries[idx].name);
    return TRUE;
}

static INT_PTR CALLBACK FileOpenDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        /* IDC_FO_PATH is a plain LTEXT showing the current directory -
         * possibly containing Japanese characters - so it's hidden here
         * and repainted by WM_PAINT via CeBmpFontPaintLabel() instead,
         * same as ce_main.c's IDC_MM_HINT. FillFileList() below still
         * sets its text normally with SetDlgItemTextW() every time the
         * directory changes; only the paint path is different. */
        ShowWindow(GetDlgItem(hDlg, IDC_FO_PATH), SW_HIDE);

        ApplyFileOpenLanguage(hDlg);

        InitStartDir();
        FillFileList(hDlg);

        /* Left/Right page-jump - see FileListCtrlProc's own comment. */
        s_pFileListOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_FO_LIST), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_FO_LIST), GWLP_WNDPROC, (LONG_PTR)FileListCtrlProc);

        /* Without this, initial keyboard focus landed somewhere that
         * doesn't respond to the direction keys (decide/OK still
         * worked, since Enter always routes to the DEFPUSHBUTTON
         * regardless of focus - only arrow-key list navigation was
         * silently broken, per the earlier prototype's own hardware testing of this
         * exact dialog). Returning FALSE tells the dialog manager we've
         * already set focus ourselves. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_FO_LIST));
        PostMessage(hDlg, WM_SETLISTFOCUS, 0, 0);
        return FALSE;
    }

    case WM_MEASUREITEM:
        /* IDC_FO_LIST is LBS_OWNERDRAWFIXED (ce_res.rc) so ROM/directory
         * names with Japanese characters draw with the Shinonome bitmap
         * font (WM_DRAWITEM below) instead of tofu-ing under a TrueType
         * face that no longer exists - every row is the same fixed
         * glyph-strip height. */
        if (((MEASUREITEMSTRUCT *)lParam)->CtlID == IDC_FO_LIST)
        {
            ((MEASUREITEMSTRUCT *)lParam)->itemHeight = CE_BMPFONT_HEIGHT + 2 + CE_BMPFONT_ROW_EXTRA;
            return TRUE;
        }
        return FALSE;

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;

        if (dis->CtlID == IDC_FO_LIST)
        {
            RECT rc = dis->rcItem;
            int selected = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
            HBRUSH hBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &rc, hBrush);
            DeleteObject(hBrush);

            if (dis->itemID != (UINT)-1)
            {
                wchar_t text[MAX_PATH];
                SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, (LPARAM)text);
                SetBkMode(dis->hDC, TRANSPARENT);
                CeBmpFontDrawTextBoxedW(dis->hDC, rc.left + 2, rc.top + 1, text,
                                    GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            }

            if (dis->itemState & ODS_FOCUS)
                DrawFocusRect(dis->hDC, &rc);
            return TRUE;
        }

        CeBmpFontDrawOwnerButton(dis); /* IDOK */
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabelBoxed(hdc, hDlg, IDC_FO_PATH);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        /* Belt-and-suspenders for the same focus problem - re-assert
         * the moment the dialog is actually activated, a more reliable
         * point in the sequence on this device than WM_INITDIALOG
         * alone (per the earlier prototype's hardware testing). */
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_FO_LIST));
            PostMessage(hDlg, WM_SETLISTFOCUS, 0, 0);
        }
        break;

    case WM_SETLISTFOCUS:
        /* Re-asserted once more from a posted message (processed only
         * after WM_INITDIALOG/WM_ACTIVATE and whatever else was already
         * queued behind them finishes) - a synchronous SetFocus() from
         * either of those sometimes lost a race with this device's own
         * dialog-activation sequence and silently got overridden,
         * leaving only the plain selection highlight instead of a real
         * focus rectangle (confirmed on this exact dialog by the earlier prototype). */
        SetFocus(GetDlgItem(hDlg, IDC_FO_LIST));
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_FO_LIST && HIWORD(wParam) == LBN_DBLCLK)
        {
            if (EnterSelected(hDlg))
                EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK)
        {
            if (EnterSelected(hDlg))
                EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            if (IsRootDir())
                EndDialog(hDlg, IDCANCEL);
            else
            {
                NavigateUp();
                FillFileList(hDlg);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int CeShowFileOpenDialog(HWND owner, wchar_t *outPath, size_t outPathCount, int mode)
{
    int ret;

    s_pickMode = (mode == CE_FILEOPEN_BIOS) ? CE_FILEOPEN_BIOS : CE_FILEOPEN_ROM;
    ret = (int)DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
                               MAKEINTRESOURCEW(IDD_FILEOPEN), owner, FileOpenDlgProc);

    if (ret != IDOK)
    {
        CeLog("CeShowFileOpenDialog: cancelled");
        return 0;
    }

    wcsncpy(outPath, s_lastPickedFile, outPathCount - 1);
    outPath[outPathCount - 1] = L'\0';

    /* Remembered for the next open (see CeFileOpenInit). CP_UTF8, not the narrow CRT default, so a Japanese folder
     * name round-trips through ce_config.c's plain-text file intact. */
    {
        char dirUtf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, s_currentDir, -1, dirUtf8, MAX_PATH, NULL, NULL);
        CeConfigSetString("VideoLastDir", dirUtf8);
        CeConfigSave();
    }

    CeLog("CeShowFileOpenDialog: picked");
    return 1;
}
