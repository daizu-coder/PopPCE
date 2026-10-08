/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * PopPCE's Windows CE frontend, driving CE/core/pce_core_ce.c - a
 * libretro-shaped adapter over the NitroGrafx PC Engine core. It has the
 * same shape as the sister ports' frontends (PopSG and PopSNES, both
 * hardware-validated): a thin shell that drives
 * retro_init/retro_load_game/retro_run and turns the five retro_set_*
 * callbacks into real GDI/waveOut/key I/O, never patching the core
 * itself. Added for this port: wall-clock frame pacing in WinMain's
 * loop, the ROM's wide path handed to the core directly
 * (pceCoreSetRomPathW, so Japanese file names work), and the CD-ROM
 * System Card lookup.
 */

#include "ce_app_config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include <libretro.h>

#include "ce_log.h"
#include "ce_display.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_fileopen.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"
#include "pce_core_ce.h"

static const wchar_t kWndClassName[] = CE_APP_WND_CLASS;
static const wchar_t kMutexName[]    = CE_APP_MUTEX_NAME;
static const wchar_t kAppTitle[]     = CE_APP_TITLE;

static HWND   g_hwnd     = NULL;
static HANDLE g_mutex    = NULL;
static volatile int g_running = 0;

static wchar_t g_romPath[MAX_PATH] = L""; /* last successfully-loaded ROM's path, for save-state/SRAM file naming */

/* Last frame the core actually rendered (ce_video_refresh), for the main
 * menu's Screenshot button. Cleared on every ROM load so a new game
 * can't save the previous one's frame.
 *
 * The screenshot itself normally comes from ce_display.c's off-screen
 * DIB (CeDisplayGetLastImage - the image as scaled on screen), so these
 * are mainly the "drawn since this ROM loaded" flag. Only the fallback
 * path reads the core's own pointer, which is safe only if the core
 * renders into one buffer it never frees or moves while a game is
 * loaded (true of pce_render.c's static pceFrame); a core that
 * double-buffers or reallocates would need the fallback dropped or the
 * frame copied here instead. */
static const void *g_lastFrame = NULL;
static unsigned g_lastFrameW = 0, g_lastFrameH = 0;
static size_t g_lastFramePitch = 0;

/* g_romLoaded: a game has been successfully retro_load_game()'d at
 * least once (stays true across File>Open reloads until exit).
 * g_paused: the touch-to-reveal menu is up right now - retro_run() is
 * not called while this is true, and CeDisplaySuspend() has released
 * ce_display.c's cached window DC so the menu dialogs can draw. */
static int  g_romLoaded = 0;
static int  g_paused    = 0;

static void CeShutdown(int exitCode); /* used by MainMenuDlgProc, below */
static void CeShowShellChrome(HWND hwnd); /* used by CeShutdown, defined further below */
static void CeHideShellChrome(HWND hwnd); /* used by ShowMainMenuDialog and WinMain, defined further below */

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* Set by the core via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * whenever Video Config's Frame Skip is above 0 (ce_video.c) - NULL
 * otherwise (Frame Skip Off, or before retro_load_game()'s first
 * check_variables() call). Invoked once per frame from WinMain's main
 * loop, right before retro_run(), per that environment call's contract -
 * see the call site below. */
static retro_audio_buffer_status_callback_t g_audioBuffStatusCb = NULL;

static bool ce_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
        /* ce_display.c's DIB is RGB565 (and so is the PC Engine
         * adapter's output); refuse anything else so a core doesn't
         * silently assume a format we can't display. */
        return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Video Config's frame-skip setting (ce_video.c) rides the
         * libretro core-options protocol (pce_frameskip - see
         * checkVariables() in CE/core/pce_core_ce.c) instead of a new side
         * channel - CE just needs to answer that query. */
        struct retro_variable *var = (struct retro_variable *)data;
        return CeVideoEnvGetVariable(var->key, &var->value) ? true : false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Lets a Video Config change made mid-session (ROM already
         * loaded, retro_load_game() - which would otherwise be the only
         * point check_variables() re-reads these - not called again)
         * take effect on the very next retro_run() instead of needing a
         * File>Open reload. */
        *(bool *)data = CeVideoConsumeDirty() ? true : false;
        return true;

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        /* The core only asks for this when frame skip is "auto" (see
         * check_variables()/init_frameskip() in libretro/libretro.c) -
         * without answering it, that mode silently never skips anything
         * (retro_audio_buff_active stays false forever). data is NULL
         * when the core wants to unregister. */
        const struct retro_audio_buffer_status_callback *cb =
            (const struct retro_audio_buffer_status_callback *)data;
        g_audioBuffStatusCb = cb ? cb->callback : NULL;
        return true;
    }

    default:
        /* Everything else (GET_LOG_INTERFACE handled separately by the
         * core itself calling environ_cb before this is even wired up,
         * GET_SYSTEM_DIRECTORY for BIOS lookup, SET_CORE_OPTIONS*,
         * GET_INPUT_BITMASKS, ...) is optional per the libretro API
         * contract - returning false tells the core to use its built-in
         * defaults, which is exactly what we want until there is a
         * settings UI for each of those too. The CD-ROM System Card is
         * not looked up through GET_SYSTEM_DIRECTORY: a narrow path is
         * unreliable on this device whenever a folder name contains
         * non-ASCII characters, so ResolvePceBios() below finds it with
         * the wide API and hands it over with pceCoreSetBiosPathW(). */
        return false;
    }
}

static void ce_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    /* Keeps Video Config's Frame Skip consecutive-skip counter (see
     * ce_video.h) in sync with what the core actually did this frame:
     * data is NULL exactly when the core skipped rendering. */
    CeVideoFrameSkipNotifyRendered(data != NULL);

    if (!data)
        return; /* duplicate/skipped frame - nothing new to draw */

    g_lastFrame = data;
    g_lastFrameW = width;
    g_lastFrameH = height;
    g_lastFramePitch = pitch;

    /* Self-contained per call (width/height/pitch given fresh every
     * time, and always RGB565 per ce_environment()'s SET_PIXEL_FORMAT
     * handling above). */
    CeDisplayBlitRGB565(data, width, height, (unsigned)pitch);
}

static void ce_audio_sample_noop(int16_t left, int16_t right)
{
    /* The core only ever uses retro_set_audio_sample_batch (see
     * retro_set_audio_sample() in libretro.c - it's an intentional
     * no-op setter), but we still register a real callback rather than
     * NULL to avoid relying on that being true forever. */
    (void)left;
    (void)right;
}

/* Temporary perf instrumentation (same pattern as ce_display.c's existing
 * "perf: blit" logging - see CeDisplayBlitRGB565()): platform/libretro/
 * libretro.c calls audio_batch_cb() exactly once per retro_run(), so
 * this is directly comparable, per-frame, to retro_run's own timing
 * below and blit's. Lets retro_run_avg - blit_avg - audio_push_avg
 * stand in for "core CPU/PPU/sound-chip emulation alone", without
 * touching platform/libretro/libretro.c or anything under pico/. Remove
 * once the bottleneck is identified. */
static size_t ce_audio_sample_batch(const int16_t *data, size_t frames)
{
    static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
    DWORD t0 = GetTickCount();
    size_t ret = CeAudioPushSamples(data, frames);
    unsigned elapsed = (unsigned)(GetTickCount() - t0);

    s_accumMs += elapsed;
    if (elapsed > s_maxMs)
        s_maxMs = elapsed;
    if (++s_count >= 60)
    {
        CeLog("perf: audio_push avg=%ums max=%ums over %u frames",
              s_accumMs / s_count, s_maxMs, s_count);
        s_accumMs = 0;
        s_maxMs = 0;
        s_count = 0;
    }
    return ret;
}

static void ce_input_poll(void)
{
    CeInputPoll();
}

static int16_t ce_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return CeInputState(port, device, index, id);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

/* The PC Engine adapter sets need_fullpath (CE/core/pce_core_ce.c), i.e.
 * it opens and reads the ROM itself (HuCard images straight into
 * ROM_Space, CD images through their .cue) rather than have the frontend
 * preload the whole file into memory first. This only picks the path. */
static int PickRom(HWND owner, wchar_t *outPath, size_t outPathCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    memset(outPath, 0, outPathCount * sizeof(wchar_t));

    /* Custom listbox-based picker (ce_fileopen.c), not GetOpenFileNameW()
     * - the standard common dialog has no way to render Japanese folder/
     * file names on this device (see ce_fileopen.c's header comment). */
    if (!CeShowFileOpenDialog(owner, outPath, outPathCount, CE_FILEOPEN_ROM))
    {
        CeLog("PickRom: file picker cancelled");
        return 0;
    }

    hFind = FindFirstFileW(outPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        CeLog("PickRom: selected file no longer exists");
        return 0;
    }
    FindClose(hFind);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Battery-backed cartridge save (SRAM)                                */
/* ------------------------------------------------------------------ */

/* Loads "<romPath>.srm" into the core's SRAM, if this game has any
 * (RETRO_MEMORY_SAVE_RAM) and a save file already exists. This is what
 * makes a game's own in-cartridge save feature survive across app
 * restarts, same as a real battery-backed cartridge would - ported from
 * the sister PopSNES's own CeLoadSram/CeSaveSram, which added this after
 * a real power-off test showed relying only on graceful
 * app-exit/ROM-switch checkpoints lost saves. No .srm file yet is the normal case for a new
 * game (or one with no SRAM at all) and isn't logged as an error. */
/* What the .srm file holds (last loaded or saved), so CeSaveSram() writes
 * only when the game has changed its SRAM: on this device writing even a
 * 2KB file stalls the frame loop for 0.8-1.8s (seen with the CD BRAM and
 * the 30s autosave). s_sramShadowValid is 0 when the file's content
 * isn't known (a failed write or fclose, no memory for the copy), so the
 * next CeSaveSram() writes again - same as the sister PopGBA/PopSG. */
static unsigned char *s_sramShadow;
static size_t s_sramShadowSize;
static int s_sramShadowValid;

static void CeSramRemember(const void *sram, size_t size)
{
    if (size != s_sramShadowSize)
    {
        free(s_sramShadow);
        s_sramShadow = (unsigned char *)malloc(size);
        s_sramShadowSize = s_sramShadow ? size : 0;
    }
    if (s_sramShadow)
        memcpy(s_sramShadow, sram, size);
    s_sramShadowValid = s_sramShadow != NULL;
}

static void CeLoadSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t got;

    free(s_sramShadow);
    s_sramShadow = NULL;
    s_sramShadowSize = 0;
    s_sramShadowValid = 0;
    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"rb");
    if (!f)
    {
        CeLog("CeLoadSram: no .srm file yet (new game, or none saved)");
        CeSramRemember(sram, size); /* nothing to write until the game changes it */
        return;
    }

    /* Read at most `size` bytes - a mismatched-size .srm (shouldn't
     * happen for a given ROM, but don't overrun the core's buffer if it
     * somehow does) is truncated, not rejected outright. */
    got = fread(sram, 1, size, f);
    fclose(f);
    CeSramRemember(sram, size);
    CeLog("CeLoadSram: loaded %lu of %lu bytes", (unsigned long)got, (unsigned long)size);
}

/* Writes the core's current SRAM out to "<romPath>.srm" - the other half
 * of CeLoadSram(). Called whenever a loaded game's SRAM is about to stop
 * being the live one (File>Open loading a different ROM, or app exit),
 * plus a pause-time checkpoint (ShowMainMenuDialog) and a periodic
 * autosave (WinMain's loop), same three checkpoints the sister
 * PopSNES settled on. */
static void CeSaveSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t wrote;
    int closeErr;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM - nothing to save */
    if (s_sramShadowValid && s_sramShadow && s_sramShadowSize == size &&
        memcmp(s_sramShadow, sram, size) == 0)
        return; /* unchanged since the last load/save */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"wb");
    if (!f)
    {
        s_sramShadowValid = 0;
        CeLog("CeSaveSram: failed to open .srm file for write");
        return;
    }

    wrote = fwrite(sram, 1, size, f);
    closeErr = fclose(f);
    if (wrote == size && closeErr == 0)
    {
        CeSramRemember(sram, size);
        CeLog("CeSaveSram: saved %lu bytes", (unsigned long)size);
    }
    else
    {
        /* "wb" already truncated the file - write it again next time,
         * even if the SRAM doesn't change. */
        s_sramShadowValid = 0;
        CeLog("CeSaveSram: failed (wrote %lu of %lu, fclose=%d)",
              (unsigned long)wrote, (unsigned long)size, closeErr);
    }
}

/* ------------------------------------------------------------------ */
/* CD-ROM System Card                                                  */
/* ------------------------------------------------------------------ */

/* Ported from the sister PopSG's Mega CD BIOS lookup (its ce_main.c
 * ResolveMegaCdBiosDir/FindBestMegaCdBiosInDirW/
 * PromptAndSetupMegaCdBios), with the check of the file's contents
 * swapped for the PC Engine one (pce_core_ce.c pceCoreProbeBiosW). The
 * file name does not matter, nothing is copied or renamed, and only the
 * folder is remembered (config key "PceBiosDir"). */
static void CeShowMsgBox(HWND owner, const wchar_t *text); /* defined below */

/* Folder part of path (no trailing backslash) */
static void PathDirOnlyW(const wchar_t *path, wchar_t *out, size_t count)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    size_t len = slash ? (size_t)(slash - path) : 0;

    if (len >= count)
        len = count - 1;
    wcsncpy(out, path, len);
    out[len] = L'\0';
}

/* The best System Card directly in dir (no subfolders): a known Super
 * System Card 3.0 first, else any System Card. Returns 1 and fills
 * outPath if there is one. */
static int FindBestPceBiosInDirW(const wchar_t *dir, wchar_t *outPath, size_t outCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];
    int best = 0;

    if (!dir[0])
        return 0;
    _snwprintf(pattern, MAX_PATH, L"%s\\*.pce", dir);
    pattern[MAX_PATH - 1] = L'\0';
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t full[MAX_PATH];
        int rank;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (fd.nFileSizeLow != 0x40000 && fd.nFileSizeLow != 0x40200)
            continue; /* a System Card is 256KB (+ a copier header) */
        _snwprintf(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        full[MAX_PATH - 1] = L'\0';
        rank = pceCoreProbeBiosW(full);
        if (rank > best)
        {
            best = rank;
            wcsncpy(outPath, full, outCount - 1);
            outPath[outCount - 1] = L'\0';
        }
    } while (best < 2 && FindNextFileW(h, &fd));
    FindClose(h);
    return best != 0;
}

/* Finds a System Card for the CD image cuePath and hands it to the core:
 * in the folder remembered from an earlier pick, else next to the .cue,
 * else next to the exe. Returns 1 if one was found. */
static int ResolvePceBios(const wchar_t *cuePath)
{
    wchar_t dirs[3][MAX_PATH];
    wchar_t found[MAX_PATH];
    char dirUtf8[MAX_PATH * 3] = "";
    int i;

    CeConfigGetString("PceBiosDir", dirUtf8, sizeof(dirUtf8));
    dirs[0][0] = L'\0';
    if (dirUtf8[0])
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8, -1, dirs[0], MAX_PATH);
    dirs[0][MAX_PATH - 1] = L'\0';
    PathDirOnlyW(cuePath, dirs[1], MAX_PATH);
    GetModuleFileNameW(NULL, found, MAX_PATH);
    PathDirOnlyW(found, dirs[2], MAX_PATH);

    for (i = 0; i < 3; i++)
    {
        if (FindBestPceBiosInDirW(dirs[i], found, MAX_PATH))
        {
            pceCoreSetBiosPathW(found);
            CeLog("PceBios: found (%s folder)", i == 0 ? "remembered" : i == 1 ? "CD image" : "exe");
            return 1;
        }
    }
    CeLog("PceBios: none found");
    return 0;
}

/* No System Card found: says so, lets the user pick one (the .pce list
 * of the file picker), checks it and remembers its folder. Returns 1 if
 * the core now has a System Card. */
static int PromptAndPickPceBios(HWND hwnd)
{
    wchar_t biosPath[MAX_PATH];
    wchar_t dir[MAX_PATH];
    char dirUtf8[MAX_PATH * 3];

    CeShowMsgBox(hwnd, CeLangIsJapanese()
        ? L"CD-ROM\x306e\x30b7\x30b9\x30c6\x30e0\x30ab\x30fc\x30c9\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093\x3002\x30b7\x30b9\x30c6\x30e0\x30ab\x30fc\x30c9(.pce)\x3092\x9078\x3093\x3067\x304f\x3060\x3055\x3044\x3002"
          /* CD-ROMのシステムカードが見つかりません。システムカード(.pce)を選んでください。 */
        : L"CD-ROM System Card not found. Select the System Card (.pce) file.");

    if (!CeShowFileOpenDialog(hwnd, biosPath, MAX_PATH, CE_FILEOPEN_BIOS))
        return 0;
    if (!pceCoreProbeBiosW(biosPath))
    {
        CeLog("PceBios: picked file is not a System Card");
        CeShowMsgBox(hwnd, CeLangIsJapanese()
            ? L"\x3053\x306e\x30d5\x30a1\x30a4\x30eb\x306f\x30b7\x30b9\x30c6\x30e0\x30ab\x30fc\x30c9\x3067\x306f\x3042\x308a\x307e\x305b\x3093\x3002"
              /* このファイルはシステムカードではありません。 */
            : L"That file is not a System Card.");
        return 0;
    }

    pceCoreSetBiosPathW(biosPath);
    PathDirOnlyW(biosPath, dir, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dirUtf8, sizeof(dirUtf8), NULL, NULL);
    CeConfigSetString("PceBiosDir", dirUtf8);
    CeConfigSave();
    CeLog("PceBios: picked");
    return 1;
}

/* Picks a ROM (via PickRom) and hands its path to the core. Safe to call
 * both for the first load and for File>Open while a game is already
 * running (unloads the previous game first). Returns 1 on success, 0 if
 * the user cancelled the picker or loading failed - in both failure
 * cases whatever was running before is left untouched. */
static int LoadRomFlow(HWND hwnd)
{
    wchar_t romPath[MAX_PATH];
    struct retro_game_info game;
    struct retro_system_av_info avInfo;
    static char pathUtf8[MAX_PATH * 3]; /* generous size; CP_ACP never expands beyond 2 bytes/char */
    wchar_t title[MAX_PATH + 32];
    wchar_t *base;

    if (!PickRom(hwnd, romPath, MAX_PATH))
        return 0; /* cancelled/failed - PickRom already logged why */

    /* A CD image boots through the System Card. Found before the running
     * game is unloaded, so backing out of the prompt changes nothing. */
    if (pceCoreIsCdImagePath(romPath) && !ResolvePceBios(romPath) && !PromptAndPickPceBios(hwnd))
        return 0;

    if (g_romLoaded)
    {
        CeSaveSram(); /* g_romPath/the core's SRAM still refer to the *previous* game here - new one isn't loaded yet */
        retro_unload_game();
    }

    /* CP_ACP, not CP_UTF8: the core's own fopen()-based file I/O reopens
     * this narrow string as wide via the cegcc CRT's own narrow->wide
     * conversion, which uses the OS default ("ANSI") codepage - not
     * UTF-8. Encoding as UTF-8 here silently breaks any non-ASCII (e.g.
     * Japanese) path component before the core's fopen() ever sees it.
     * CP_ACP is not a full fix either (the dev notes have the
     * hardware-confirmed details: GetACP() isn't guaranteed to be a
     * Japanese-capable codepage even with a Japanese UI, and the
     * conversion has shown non-deterministic behavior on real hardware
     * for genuinely non-ASCII input) - but it's strictly closer to
     * correct than CP_UTF8, which is simply wrong. */
    WideCharToMultiByte(CP_ACP, 0, romPath, -1, pathUtf8, sizeof(pathUtf8), NULL, NULL);
    /* The core opens the ROM itself (need_fullpath); give it the wide
     * path too, so non-ASCII (Japanese) names do not depend on the
     * CP_ACP round trip described above. */
    pceCoreSetRomPathW(romPath);
    memset(&game, 0, sizeof(game));
    game.path = pathUtf8;
    game.data = NULL; /* the core reads the file itself via game.path - see PickRom's comment */
    game.size = 0;

    if (!retro_load_game(&game))
    {
        CeLog("LoadRomFlow: retro_load_game failed");
        MessageBoxW(hwnd, L"Failed to load ROM.", kAppTitle, MB_OK);
        g_romLoaded = 0;
        return 0;
    }

    g_romLoaded = 1;
    g_lastFrame = NULL;
    wcsncpy(g_romPath, romPath, MAX_PATH - 1);
    g_romPath[MAX_PATH - 1] = L'\0';

    CeLoadSram();

    retro_get_system_av_info(&avInfo);
    CeLog("LoadRomFlow: loaded, geometry=%ux%u fps=%.3f sample_rate=%.0f",
          avInfo.geometry.base_width, avInfo.geometry.base_height,
          avInfo.timing.fps, avInfo.timing.sample_rate);
    CeAudioStart(avInfo.timing.sample_rate);

    base = wcsrchr(romPath, L'\\');
    _snwprintf(title, MAX_PATH + 32, L"%s - %s", kAppTitle, base ? base + 1 : romPath);
    SetWindowTextW(g_hwnd, title); /* always the main window, even when
                                     * called with a dialog as `hwnd` */

    return 1;
}

/* ------------------------------------------------------------------ */
/* Generic message box (IDD_MSGBOX)                                    */
/* ------------------------------------------------------------------ */

/* MessageBoxW() draws with whatever system font Windows CE finds, and
 * this device has no CJK-capable one any more (jptahoma.ttc dropped for
 * licensing reasons - see ce_lang.h) - Japanese text through it comes
 * back as tofu. This dialog instead paints its own text with the
 * Shinonome bitmap font (ce_bmpfont.c), the same way every other piece
 * of Japanese UI text in this port already does. Use it for any result
 * message that can carry Japanese text (Save/Load State below); a
 * message that's always English-only (ROM load failures, fatal startup
 * errors) can stay a plain MessageBoxW(). Ported from the sister
 * PopGB / PopNES projects' own CeShowMsgBox()/MsgBoxDlgProc. */
#define CE_MSGBOX_MAX_TEXT 128
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

#define CE_MSGBOX_MAX_LINE 32

/* Splits text into at most two lines that each fit within maxWidth
 * (real pixels). If the whole string already fits, line2 comes back
 * empty and the caller draws a single centered line. */
static void WrapMsgBoxText(const wchar_t *text, int maxWidth,
                            wchar_t *line1, wchar_t *line2, int lineCap)
{
    int n = (int)wcslen(text);
    int split, i;
    wchar_t probe[CE_MSGBOX_MAX_LINE];

    if (CeBmpFontGetTextWidth(text) <= maxWidth)
    {
        wcsncpy(line1, text, lineCap - 1);
        line1[lineCap - 1] = L'\0';
        line2[0] = L'\0';
        return;
    }

    split = 1; /* always keep at least one character on line1, even if it alone overflows */
    for (i = 1; i <= n && i < CE_MSGBOX_MAX_LINE - 1; i++)
    {
        wcsncpy(probe, text, i);
        probe[i] = L'\0';
        if (CeBmpFontGetTextWidth(probe) > maxWidth)
            break;
        split = i;
    }

    wcsncpy(line1, text, split);
    line1[split] = L'\0';
    wcsncpy(line2, text + split, lineCap - 1);
    line2[lineCap - 1] = L'\0';
}

static WNDPROC s_pMsgBoxOkOrigProc = NULL;

/* Same DLGC_WANTALLKEYS/VK_RETURN/VK_SPACE/VK_ESCAPE subclass pattern as
 * every other BS_OWNERDRAW OK button in this port (MainMenuBtnCtrlProc
 * below, SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc) - BS_OWNERDRAW
 * breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter routing and
 * Escape-to-Cancel handling alike, so both have to be reimplemented by
 * hand here too. */
static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        /* IDC_MB_TEXT stays a plain hidden LTEXT, same as IDC_MM_HINT
         * elsewhere in this port - repainted by WM_PAINT below instead
         * of drawn by the control itself. */
        ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
        s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
        return TRUE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
        COLORREF fg;
        int rectW, rectH;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_MB_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
        rectW = rc.right - rc.left;
        rectH = rc.bottom - rc.top;

        WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

        fg = GetSysColor(COLOR_WINDOWTEXT);
        SetBkMode(hdc, TRANSPARENT);

        if (line2[0] == L'\0')
        {
            int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
            CeBmpFontDrawTextW(hdc, x, y, line1, fg);
        }
        else
        {
            int lineH = CE_BMPFONT_HEIGHT + 2;
            int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
            int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
            CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
            CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
               MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Save State: ask + run + acknowledge, all in one self-drawn dialog  */
/* (IDD_SAVECONFIRM)                                                   */
/*                                                                      */
/* Do NOT implement this as "confirm dialog -> close -> result message  */
/* box" (two modal DialogBoxW calls under the main menu): the hand-off  */
/* exposes the main menu for a frame and its owner-draw "ステートセーブ" */
/* button repaints to the front (see the dev notes - the round-22      */
/* nested-modal composition glitch, on the save                         */
/* path; every timing/forced-repaint workaround was tried on hardware   */
/* and none helped). This reuses the IDD_SAVECONFIRM template but never */
/* closes/reopens: on はい/Yes it runs CeSaveState() in place, then     */
/* swaps its own text to the result ("セーブしました。" / "セーブに失敗  */
/* しました。"), hides the No button and relabels Yes to "OK" recentred. */
/* EndDialog only on the OK press (or Esc while showing the result).    */
/* Two owner-draw buttons (はい/Yes = IDC_SF_YES, いいえ/No = IDC_SF_NO) */
/* - the 2-button form section 11 recommends. Shares WrapMsgBoxText()/  */
/* s_msgBoxText with CeShowMsgBox above. Returns 1 if the save ran,     */
/* 0 if declined. */
/* ------------------------------------------------------------------ */

static int CeSaveState(void); /* defined below; used by SaveConfirmDlgProc */

static WNDPROC s_pSaveConfirmBtnOrigProc = NULL;
static int     s_sfPhase                 = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK SaveConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int  id   = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (s_sfPhase == 0) /* the result phase has only the OK button */
                SetFocus(GetDlgItem(hDlg, id == IDC_SF_YES ? IDC_SF_NO : IDC_SF_YES));
            return 0;
        }
    }

    return CallWindowProc(s_pSaveConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK SaveConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_sfPhase = 0;
        ShowWindow(GetDlgItem(hDlg, IDC_SF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_SF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */   : L"Yes");
        SetDlgItemTextW(hDlg, IDC_SF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pSaveConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_YES), GWLP_WNDPROC, (LONG_PTR)SaveConfirmBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_NO),  GWLP_WNDPROC, (LONG_PTR)SaveConfirmBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_SF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
        COLORREF fg;
        int rectW, rectH;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_SF_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
        rectW = rc.right - rc.left;
        rectH = rc.bottom - rc.top;

        WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

        fg = GetSysColor(COLOR_WINDOWTEXT);
        SetBkMode(hdc, TRANSPARENT);

        if (line2[0] == L'\0')
        {
            int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
            CeBmpFontDrawTextW(hdc, x, y, line1, fg);
        }
        else
        {
            int lineH = CE_BMPFONT_HEIGHT + 2;
            int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
            int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
            CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
            CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SF_YES:
            if (s_sfPhase == 0)
            {
                int ok = CeSaveState();
                wcsncpy(s_msgBoxText, CeLangIsJapanese()
                        ? (ok ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002"                     /* セーブしました。 */
                              : L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002")  /* セーブに失敗しました。 */
                        : (ok ? L"State saved." : L"Save failed."),
                        CE_MSGBOX_MAX_TEXT - 1);
                s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
                s_sfPhase = 1;
                ShowWindow(GetDlgItem(hDlg, IDC_SF_NO), SW_HIDE);
                SetDlgItemTextW(hDlg, IDC_SF_YES, L"OK");
                {   /* recentre the lone OK button */
                    RECT rcc, rb;
                    GetClientRect(hDlg, &rcc);
                    GetWindowRect(GetDlgItem(hDlg, IDC_SF_YES), &rb);
                    MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
                    SetWindowPos(GetDlgItem(hDlg, IDC_SF_YES), NULL,
                                 (rcc.right - (rb.right - rb.left)) / 2, rb.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                InvalidateRect(hDlg, NULL, TRUE);
                SetFocus(GetDlgItem(hDlg, IDC_SF_YES));
            }
            else
            {
                EndDialog(hDlg, 1);
            }
            return TRUE;

        case IDC_SF_NO:
        case IDCANCEL:
            EndDialog(hDlg, s_sfPhase ? 1 : 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static int CeConfirmAndSaveState(HWND owner)
{
    wcsncpy(s_msgBoxText, CeLangIsJapanese()
            ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
            : L"Save State?",
            CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    return (int)DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
                           MAKEINTRESOURCEW(IDD_SAVECONFIRM), owner, SaveConfirmDlgProc) == 1;
}

/* ------------------------------------------------------------------ */
/* Save state (single slot per ROM: "<romPath>.state")                */
/* ------------------------------------------------------------------ */

/* Finishing a state file in the background. On the device the write
 * itself only fills the file system cache (40-150ms for 0.6-2.3MB); the
 * CloseHandle that flushes it to flash took 3.8-6.7s, all spent with the
 * save dialog frozen. So the main thread opens and writes a temporary
 * file (failures there are still reported), and a thread closes it and
 * only then replaces "<rom>.state" with it - a power-off during the flush
 * leaves the previous state intact. Below the frame loop's priority, so
 * the game keeps running while it flushes. Anything that reads the state
 * or ends the program waits for it first (CeWaitStateWriter). */
typedef struct
{
    HANDLE  file;
    wchar_t tmpPath[MAX_PATH + 12];
    wchar_t statePath[MAX_PATH + 8];
    DWORD   bytes;
    DWORD   t0;
} CeStateJob;

static CeStateJob s_stateJob;
static HANDLE     s_stateThread = NULL;

static DWORD WINAPI CeStateWriterProc(LPVOID param)
{
    CeStateJob *job = (CeStateJob *)param;
    DWORD tClose = GetTickCount(), tMoved;
    BOOL moved;

    CloseHandle(job->file);
    tMoved = GetTickCount();
    DeleteFileW(job->statePath); /* MoveFileW does not replace */
    moved = MoveFileW(job->tmpPath, job->statePath);
    CeLog("CeSaveState: %lu bytes on flash %lums after the save (close %lums, rename %lums)%s",
          (unsigned long)job->bytes, (unsigned long)(GetTickCount() - job->t0),
          (unsigned long)(tMoved - tClose), (unsigned long)(GetTickCount() - tMoved),
          moved ? "" : " - rename FAILED");
    if (!moved)
        CeLog("CeSaveState: MoveFileW error=%lu", (unsigned long)GetLastError());
    return 0;
}

static void CeWaitStateWriter(void)
{
    DWORD t0;

    if (!s_stateThread)
        return;
    t0 = GetTickCount();
    WaitForSingleObject(s_stateThread, INFINITE);
    CloseHandle(s_stateThread);
    s_stateThread = NULL;
    if (GetTickCount() - t0 > 20)
        CeLog("CeWaitStateWriter: waited %lums for the previous save to reach flash",
              (unsigned long)(GetTickCount() - t0));
}

/* Returns 1 on success, 0 on failure. Shows no UI itself - the caller
 * (SaveConfirmDlgProc) folds the result into its own single dialog so no
 * second modal is ever nested (the dev notes).
 * Success means the state is written to the file system; it reaches the
 * flash a few seconds later (CeStateWriterProc). */
static int CeSaveState(void)
{
    size_t size;
    void *buffer;
    CeStateJob *job = &s_stateJob;
    DWORD t0, tOpen, tWrite, written = 0;
    BOOL ok;

    CeWaitStateWriter(); /* job and the .tmp file are reused */

    size = retro_serialize_size();
    if (size == 0)
    {
        CeLog("CeSaveState: retro_serialize_size returned 0");
        return 0;
    }

    buffer = malloc(size);
    if (!buffer)
    {
        CeLog("CeSaveState: malloc(%lu) failed", (unsigned long)size);
        return 0;
    }

    if (!retro_serialize(buffer, size))
    {
        free(buffer);
        CeLog("CeSaveState: retro_serialize failed");
        return 0;
    }

    /* The core may use less than it asked for (Arcade Card games leave
     * out the unused part of the card's RAM - pceCoreStateUsedSize) */
    if (pceCoreStateUsedSize() && pceCoreStateUsedSize() <= size)
        size = pceCoreStateUsedSize();

    /* One WriteFile for the whole state rather than stdio, which hands
     * the file system small pieces */
    _snwprintf(job->statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    _snwprintf(job->tmpPath, MAX_PATH + 12, L"%s.state.tmp", g_romPath);
    t0 = GetTickCount();
    job->file = CreateFileW(job->tmpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (job->file == INVALID_HANDLE_VALUE)
    {
        free(buffer);
        CeLog("CeSaveState: failed to open state file for write, error=%lu",
              (unsigned long)GetLastError());
        return 0;
    }
    tOpen = GetTickCount();
    ok = WriteFile(job->file, buffer, (DWORD)size, &written, NULL);
    tWrite = GetTickCount();
    free(buffer);
    CeLog("CeSaveState: wrote %lu of %lu bytes (open %lums, write %lums)",
          (unsigned long)written, (unsigned long)size,
          (unsigned long)(tOpen - t0), (unsigned long)(tWrite - tOpen));
    if (!ok || written != size)
    {
        CeLog("CeSaveState: WriteFile failed, error=%lu", (unsigned long)GetLastError());
        CloseHandle(job->file);
        DeleteFileW(job->tmpPath); /* the previous .state stays */
        return 0;
    }

    job->bytes = written;
    job->t0 = t0;
    s_stateThread = CreateThread(NULL, 0, CeStateWriterProc, job, 0, NULL);
    if (s_stateThread)
        SetThreadPriority(s_stateThread, THREAD_PRIORITY_BELOW_NORMAL);
    else
        CeStateWriterProc(job); /* no thread: finish here, as before */
    return 1;
}

static int CeLoadState(HWND owner)
{
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;
    long size;
    void *buffer;
    DWORD t0;

    CeWaitStateWriter(); /* a save still reaching flash is the file to load */
    t0 = GetTickCount();
    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"rb");
    if (!f)
    {
        CeLog("CeLoadState: no state file found");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093\x3002" /* セーブデータが見つかりません。 */
                                                : L"No save state found.");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(f);
        CeLog("CeLoadState: empty/unreadable state file");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    buffer = malloc((size_t)size);
    if (!buffer)
    {
        fclose(f);
        CeLog("CeLoadState: malloc(%ld) failed", size);
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(buffer);
        CeLog("CeLoadState: short read");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }
    fclose(f);

    if (!retro_unserialize(buffer, (size_t)size))
    {
        free(buffer);
        CeLog("CeLoadState: retro_unserialize failed");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f(\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x306e\x5f62\x5f0f\x304c\x7570\x306a\x308b\x53ef\x80fd\x6027\x304c\x3042\x308a\x307e\x3059)\x3002" /* ロードに失敗しました(セーブデータの形式が異なる可能性があります)。 */
                                                : L"Load failed (incompatible save?).");
        return 0;
    }

    free(buffer);
    CeLog("CeLoadState: loaded %ld bytes in %lums", size,
          (unsigned long)(GetTickCount() - t0));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the native pixel format of both sources below, so no conversion and no
 * compression: one fwrite per row). Uses the on-screen image at the
 * current Scale (x1/x1.5/Wide/Full, from ce_display.c's DIB) so the file
 * matches what the user sees; falls back to the core's unscaled frame
 * if the display has nothing. Written to
 * "<exe-dir>\Screenshots\<ROM name>_NNN.bmp", creating the folder on
 * first use and taking the first unused number. The header is built
 * byte by byte rather than from BITMAPFILEHEADER so struct packing
 * can't shift it. Ported from the sister PopSNES
 * (real-hardware-confirmed there). Returns 1 on success. */
static int CeSaveScreenshot(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t romName[MAX_PATH];
    wchar_t path[MAX_PATH + 16];
    wchar_t *p;
    const wchar_t *base;
    unsigned n, y, rowBytes, padBytes;
    unsigned long imageBytes;
    unsigned char hdr[66];
    static const unsigned char pad[4] = { 0, 0, 0, 0 };
    const void *img;
    unsigned imgW, imgH, imgPitch;
    FILE *f;

    /* g_lastFrame doubles as "something was drawn since this ROM
     * loaded" - the display's DIB isn't cleared on ROM switch, so
     * without it a new game could save the previous game's image. */
    if (!g_lastFrame || g_lastFrameW == 0 || g_lastFrameH == 0)
    {
        CeLog("CeSaveScreenshot: no rendered frame yet");
        return 0;
    }
    if (!CeDisplayGetLastImage(&img, &imgW, &imgH, &imgPitch))
    {
        img = g_lastFrame;
        imgW = g_lastFrameW;
        imgH = g_lastFrameH;
        imgPitch = (unsigned)g_lastFramePitch;
    }

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 0;
    p = wcsrchr(dir, L'\\');
    if (!p)
        return 0;
    p[1] = L'\0';
    if (wcslen(dir) + 12 >= MAX_PATH)
        return 0;
    wcscat(dir, L"Screenshots");
    CreateDirectoryW(dir, NULL); /* already existing is fine */
    if (GetFileAttributesW(dir) == 0xFFFFFFFF)
    {
        CeLog("CeSaveScreenshot: can't create Screenshots folder");
        return 0;
    }

    base = wcsrchr(g_romPath, L'\\');
    wcsncpy(romName, base ? base + 1 : g_romPath, MAX_PATH - 1);
    romName[MAX_PATH - 1] = L'\0';
    p = wcsrchr(romName, L'.');
    if (p)
        *p = L'\0';

    for (n = 1; n <= 999; n++)
    {
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
        path[MAX_PATH + 15] = L'\0';
        if (GetFileAttributesW(path) == 0xFFFFFFFF)
            break;
    }
    if (n > 999)
    {
        CeLog("CeSaveScreenshot: all 999 numbers used");
        return 0;
    }

    rowBytes = imgW * 2;
    padBytes = (4 - (rowBytes & 3)) & 3;
    imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
    PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
    PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
    PutLE32(hdr + 18, imgW);
    PutLE32(hdr + 22, imgH);             /* positive = bottom-up */
    PutLE16(hdr + 26, 1);                        /* planes */
    PutLE16(hdr + 28, 16);                       /* bits per pixel */
    PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
    PutLE32(hdr + 34, imageBytes);
    PutLE32(hdr + 38, 2835);                     /* 72 dpi */
    PutLE32(hdr + 42, 2835);
    PutLE32(hdr + 54, 0xF800);                   /* R mask */
    PutLE32(hdr + 58, 0x07E0);                   /* G mask */
    PutLE32(hdr + 62, 0x001F);                   /* B mask */

    f = _wfopen(path, L"wb");
    if (!f)
    {
        CeLog("CeSaveScreenshot: can't open output file");
        return 0;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = imgH; y-- > 0; )
    {
        fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
        if (padBytes)
            fwrite(pad, 1, padBytes, f);
    }
    if (ferror(f))
    {
        fclose(f);
        DeleteFileW(path);
        CeLog("CeSaveScreenshot: write failed");
        return 0;
    }
    fclose(f);

    CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
    return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
    if (CeSaveScreenshot())
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
            : L"Screenshot saved.");
    else
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
            : L"Screenshot failed.");
}

/* ------------------------------------------------------------------ */
/* Menu (touch-to-reveal - hidden during gameplay, shown at startup     */
/* before any ROM is loaded and whenever the screen is tapped mid-game) */
/*                                                                      */
/* Implemented as a modal DialogBoxW (CE/ce_res.rc's IDD_MAINMENU) with */
/* plain PUSHBUTTON controls, not a real HMENU - see ce_resource.h for  */
/* why (this coredll doesn't export SetMenu).                          */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInstance = NULL;

/* Swaps every IDD_MAINMENU button caption between English (the .rc
 * template's own text) and Japanese, gated by CeLangIsJapanese() - see
 * ce_lang.h. The PUSHBUTTONs above are BS_OWNERDRAW (ce_res.rc) and
 * repaint themselves from the text just set (WM_DRAWITEM below, via
 * CeBmpFontDrawOwnerButton() - see ce_bmpfont.c). IDC_MM_HINT is a
 * plain LTEXT - STATIC controls have no ownerdraw style - so it's
 * hidden here instead and repainted by this dialog's own WM_PAINT via
 * CeBmpFontPaintLabel(), which still reads the text set above with
 * GetWindowTextW() even though the control itself is hidden.
 *
 * CeBmpFontPaintLabel() only SetPixel()s the "on" bits of each glyph
 * with a transparent background - it never clears the label's rect
 * first, so a stale glyph from the previous language can survive
 * underneath the new one unless the whole dialog gets a full
 * erase+redraw first. Forcing that unconditionally here (every time
 * this text can change) avoids depending on whatever incidental
 * repaint a sub-dialog closing over part of this one happens to
 * trigger (real-hardware lesson from the sister PopGB /
 * PopNES ports). */
static void ApplyMainMenuLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                             /* ROMを開く... */
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");            /* ステートセーブ */
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");            /* ステートロード */
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                        /* ボタン設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");                  /* サウンド設定 */
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                              /* 画面設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b");                  /* 画面保存する */
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                          /* 終了 */
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
    }

    ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);
    InvalidateRect(hDlg, NULL, TRUE);
}

static const int kMainMenuButtonIds[] = {
    IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
    IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while g_romLoaded is still 0) -
 * EnableWindow() alone only blocks activation, not this dialog's own
 * custom arrow-key cycling below. Bounded to CE_MAINMENU_BUTTON_COUNT
 * steps so it can't spin forever if every button were ever disabled at
 * once (never happens in practice - Open/Exit are always enabled).
 * Ported from the sister PopGB / PopNES projects' own
 * MainMenuNeighbor(). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
    int idx, step;
    for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
        if (kMainMenuButtonIds[idx] == id)
            break;
    if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
        return id;

    for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
    {
        int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
        int nextId = kMainMenuButtonIds[nextIdx];
        if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
            return nextId;
    }
    return id;
}

/* Pastel/rounded main-menu skin, ported from the sister PopSG port
 * (PopSG): cream client background (WM_ERASEBKGND below),
 * rounded colored buttons with small vector icons (WM_DRAWITEM below,
 * CeBmpFontDrawOwnerButtonTheme() in ce_bmpfont.c) and the mascot
 * bitmap in the footer (WM_PAINT). Only this dialog uses it - every
 * other dialog keeps the plain CeBmpFontDrawOwnerButton() look. Colors
 * are PopSG's, confirmed on hardware there: pastel fills with a deeper
 * border of the same hue (the icon is drawn in the border color), text
 * in dark charcoal to match the mascot's outline. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
    { IDC_MM_OPEN,       RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,  0 }, /* blue */
    { IDC_MM_SAVESTATE,  RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,  0 }, /* pastel lemon */
    { IDC_MM_LOADSTATE,  RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,  0 }, /* mint */
    { IDC_MM_VIDEO,      RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO, 1 }, /* green */
    { IDC_MM_SOUND,      RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND, 1 }, /* blue */
    { IDC_MM_INPUT,      RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT, 1 }, /* pink */
    { IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender */
    { IDC_MM_EXIT,       RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,  0 }, /* coral */
};

/* app/poppce_mascot.bmp, embedded as IDB_MAINMENU (ce_res.rc). Loaded
 * in WM_INITDIALOG, blitted by WM_PAINT into the blank strip right of
 * the IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;

static WNDPROC s_pMainMenuOrigProc = NULL;

/* PUSHBUTTON's built-in WM_KEYDOWN -> BN_CLICKED conversion for
 * VK_RETURN/VK_SPACE (and IsDialogMessage()'s own Up/Down/Left/Right
 * focus-cycling) stops working once these buttons become BS_OWNERDRAW
 * (ce_res.rc) - real-hardware-confirmed by the sister PopGB
 * project: touch/stylus taps kept working (WM_LBUTTONUP is unaffected)
 * but the physical decide key did nothing on a focused button, and a
 * first attempt at fixing this via plain WM_KEYDOWN handling (no
 * WM_GETDLGCODE override) still didn't work - IsDialogMessage()
 * apparently never delivers WM_KEYDOWN to an owner-draw button at all
 * without this subclass explicitly claiming
 * DLGC_WANTARROWS | DLGC_WANTALLKEYS, the same claim
 * SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc already make for their
 * own "-/value/+" buttons - so arrow-key navigation is reimplemented
 * here too via MainMenuNeighbor() above instead of leaning on the
 * standard dialog navigation that claim steals control of. Claiming
 * DLGC_WANTALLKEYS also steals the physical Back key (Escape) away
 * from IsDialogMessage()'s normal Cancel-key handling, so it's
 * forwarded to IDCANCEL by hand below, same as those three dialogs
 * already do for their own OK button. */
static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
            return 0;

        case VK_DOWN:
        case VK_RIGHT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
            return 0;

        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
        ApplyMainMenuLanguage(hDlg);

        s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
        for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

        if (!s_hMainMenuBmp)
            s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        unsigned i;
        for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
        {
            if (kMainMenuTheme[i].id == (int)dis->CtlID)
            {
                CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
                                              kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
                return TRUE;
            }
        }
        CeBmpFontDrawOwnerButton(dis); /* fallback, shouldn't hit any control here */
        return TRUE;
    }

    /* Cream client background, scoped to just this dialog. The brush is
     * created once and kept for the process lifetime (this fires on
     * every erase, e.g. each time a sub-dialog closes over part of this
     * one), same as PopSG. */
    case WM_ERASEBKGND:
    {
        static HBRUSH s_hCreamBrush = NULL;
        RECT rc;
        if (!s_hCreamBrush)
            s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, s_hCreamBrush);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hMainMenuBmp)
        {
            DeleteObject(s_hMainMenuBmp);
            s_hMainMenuBmp = NULL;
        }
        return FALSE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

        /* The mascot in the blank strip right of the "戻るキーでゲーム再開"
         * hint: starts just past the hint text (measured with the
         * Shinonome font) and runs to the dialog's right/bottom edge,
         * aspect preserved, bottom-right aligned. */
        if (s_hMainMenuBmp)
        {
            HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
            RECT rcHint, rcClient;
            wchar_t hintText[128];
            BITMAP bm;

            hintText[0] = 0;
            GetWindowTextW(hHint, hintText, 128);
            GetWindowRect(hHint, &rcHint);
            MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
            GetClientRect(hDlg, &rcClient);

            if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
                long boxR = rcClient.right - 4;
                long boxT = rcHint.top;
                long boxB = rcClient.bottom - 2;
                long boxW = boxR - boxL;
                long boxH = boxB - boxT;

                if (boxW > 8 && boxH > 8)
                {
                    long drawW = boxW;
                    long drawH = drawW * bm.bmHeight / bm.bmWidth;
                    HDC memDC;
                    HGDIOBJ oldBmp;

                    if (drawH > boxH)
                    {
                        drawH = boxH;
                        drawW = drawH * bm.bmWidth / bm.bmHeight;
                    }

                    /* No SetStretchBltMode() - this coredll doesn't
                     * export it; the default COLORONCOLOR is fine for
                     * this decorative blit. */
                    memDC = CreateCompatibleDC(hdc);
                    oldBmp = SelectObject(memDC, s_hMainMenuBmp);
                    StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
                               (int)drawW, (int)drawH,
                               memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteDC(memDC);
                }
            }
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MM_OPEN:
            /* Stays open on cancel/failure (LoadRomFlow already showed
             * a MessageBox explaining why); closes only on success, so
             * the caller knows to resume gameplay. */
            if (LoadRomFlow(hDlg))
                EndDialog(hDlg, IDC_MM_OPEN);
            return TRUE;

        case IDC_MM_EXIT:
            CeShutdown(0); /* never returns */
            return TRUE;

        case IDC_MM_SAVESTATE:
            /* Confirm (IDD_SAVECONFIRM), run the save, and acknowledge -
             * all in one self-drawn dialog (never a nested second
             * modal). いいえ/No or Back just stays on the menu. */
            if (g_romLoaded)
                CeConfirmAndSaveState(hDlg);
            return TRUE; /* stays open either way, like Input/Sound Config */

        case IDC_MM_LOADSTATE:
            /* Closes and resumes on success (like Resume Game) so the
             * user immediately sees the loaded state; stays open on
             * failure (CeLoadState already showed why). */
            if (g_romLoaded && CeLoadState(hDlg))
                EndDialog(hDlg, IDC_MM_LOADSTATE);
            return TRUE;

        case IDC_MM_SCREENSHOT:
            if (g_romLoaded)
                CeScreenshotAndReport(hDlg);
            return TRUE; /* stays open, like Save State */

        case IDC_MM_INPUT:
            CeShowInputConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_SOUND:
            CeShowSoundConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_VIDEO:
            CeShowVideoConfigDialog(hDlg);
            /* Video Config is where the Japanese/English toggle lives -
             * re-apply here so switching it and returning to this
             * still-open menu updates it immediately. */
            ApplyMainMenuLanguage(hDlg);
            return TRUE;

        case IDCANCEL:
            /* Hardware Back / OS close gesture: same as Resume if a
             * game is already running (nothing to lose by dismissing),
             * otherwise ignored - there's nothing to go back to yet. */
            if (g_romLoaded)
                EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Pauses (if a game is running), shows the menu dialog modally (blocks
 * until closed), then resumes if a game is loaded when it returns -
 * whether that's the game that was already running, or one just picked
 * via Open ROM. Also how the very first "no ROM loaded" screen is shown
 * from WinMain, where wasPlaying is simply false. */
static void ShowMainMenuDialog(HWND hwnd)
{
    int wasPlaying = g_romLoaded && !g_paused;

    if (wasPlaying)
    {
        g_paused = 1;
        CeAudioSetPaused(1);
        CeDisplaySuspend(); /* releases the cached window DC; no full teardown - see ce_display.h */

        /* Autosave SRAM at every pause, not just at graceful shutdown/
         * ROM switch - a real power-off doesn't run CeShutdown() at all,
         * so relying only on those two checkpoints misses that case
         * entirely (lesson from the sister PopSNES's round 9).
         * Opening the touch-to-reveal menu is a frequent, cheap, natural
         * checkpoint to also save at. */
        CeSaveSram();
    }

    /* WM_PAINT (below) paints the "No ROM loaded" screen black, or
     * redraws the paused game frame from ce_display.c's off-screen DIB. */
    InvalidateRect(hwnd, NULL, TRUE);
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MAINMENU), hwnd, MainMenuDlgProc);

    if (g_romLoaded)
    {
        g_paused = 0;
        CeAudioSetPaused(0);

        /* The physical decide key that dismissed this dialog (Load
         * State, most visibly, since it resumes immediately - but also
         * Open ROM and plain Resume/Cancel) may still be physically
         * held down at this exact moment - input polling was frozen
         * for the whole time the dialog was up, so there's no debounce
         * history to tell "still held from the dialog" apart from "a
         * brand-new press" once polling resumes. See
         * CeInputSuppressStartKey's own comment (ce_input.h) - same
         * root cause and fix as an earlier prototype's
         * g_suppress_start_key, confirmed on real hardware there and on
         * a sister CE port. */
        CeInputSuppressStartKey();

        if (!CeDisplayInit(hwnd))
            CeLog("ShowMainMenuDialog: CeDisplayInit failed - continuing without video output");
        CeDisplayResume(); /* no-op under GDI - see ce_display.h */

        /* Re-assert taskbar hiding every time gameplay is (re-)entered,
         * not just once at WinMain startup - the sister
         * PopSNES found this necessary on this device: a custom DialogBoxW becoming the
         * foreground window (this menu) may cause the shell to restore
         * the taskbar, so it needs re-hiding every time control returns
         * from the menu dialog to gameplay. */
        CeHideShellChrome(hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Window / shutdown                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        /* Touch-to-reveal, same interaction both prior CE ports use.
         * ShowMainMenuDialog() is modal, so this can't re-enter while
         * already showing (input goes to the dialog, not this window). */
        ShowMainMenuDialog(hwnd);
        return 0;

    case WM_PAINT:
    {
        /* GDI video output (ce_display.c) draws straight into this
         * window's own client area, so a WM_PAINT here (a modal dialog
         * covering part of the game window, then closing) needs the last
         * rendered frame redrawn, or the exposed region stays black until
         * the next real emulated frame (which won't happen at all while
         * retro_run() is paused for that same dialog). Erase the
         * invalidated region to black first (covers both the "No ROM
         * loaded" screen and any letterbox border around the game rect),
         * then, if a ROM is loaded, redraw the current frame on top via
         * CeDisplayForceRepaint(). Ported from the sister
         * PopSNES along with ce_display.c itself. */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        if (g_romLoaded)
            CeDisplayForceRepaint();
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void CeShutdown(int exitCode)
{
    /* Lesson from the sister ports on this device (PopSG and PopSNES,
     * see the dev notes): a plain `return` from WinMain lets the
     * normal exit path run, which on this device/toolchain combination
     * can itself crash or hang threads mid-teardown. Always terminate
     * via ExitProcess, called directly from here (not via WM_CLOSE/
     * WM_DESTROY/PostQuitMessage, which an earlier prototype traced a real
     * hang to on this device). CeAudioStop() joins the audio thread
     * cleanly before we ever get here, so ExitProcess() isn't tearing
     * down a thread still mid-waveOutWrite. */
    CeAudioStop();
    CeWaitStateWriter(); /* ExitProcess would cut the flush short */
    if (g_romLoaded)
        CeSaveSram();
    retro_unload_game();
    retro_deinit();

    CeDisplayShutdown();
    CeShowShellChrome(g_hwnd);

    if (g_mutex)
        CloseHandle(g_mutex);

    CeLogShutdown(); /* drain the async logger (ce_log.c) - last CeLog() user */
    ExitProcess((UINT)exitCode);
}

/* "HHTaskBar" is the standard window class of the Windows CE Explorer
 * taskbar on this device - FindWindow+ShowWindow(HIDE) on it is what
 * both prior CE ports on this hardware settled on after aygshell.dll's
 * SHFullScreen proved unreliable/fragile (see either project's
 * the dev notes). Needs nothing beyond coredll.dll. */
static HWND CeFindTaskBarWindow(void)
{
    return FindWindowW(L"HHTaskBar", NULL);
}

static void CeHideShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
    {
        ShowWindow(hTaskBar, SW_HIDE);
        CeLog("CeHideShellChrome: hid HHTaskBar window directly");
    }
    else
    {
        CeLog("CeHideShellChrome: HHTaskBar window not found");
    }
}

/* Restores shell chrome on exit - this is a shared-shell CE device, not
 * a single-purpose game handheld, so leaving the taskbar hidden after
 * this app closes would affect the user's other apps until reboot. */
static void CeShowShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
        ShowWindow(hTaskBar, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    CeLog("WinMain start, build " __DATE__ " " __TIME__);

    g_hInstance = hInstance;

    g_mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWndClassName, NULL);
        if (existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CeLog("WinMain: another instance is already running, exiting");
        CeLogShutdown();
        ExitProcess(0);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));

    if (!RegisterClassW(&wc))
    {
        CeLog("WinMain: RegisterClassW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"RegisterClassW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    /* WS_POPUP (not just WS_VISIBLE): both prior CE ports on this
     * hardware confirmed a plain overlapped window is still managed by
     * the shell as a regular window and doesn't reliably reclaim the
     * taskbar's screen space once CeHideShellChrome() hides it. */
    g_hwnd = CreateWindowW(kWndClassName, CE_APP_TITLE L" - No ROM loaded", WS_VISIBLE | WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
    {
        CeLog("WinMain: CreateWindowW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"CreateWindowW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    CeHideShellChrome(g_hwnd);
    /* Re-assert visibility/layout after hiding the taskbar - without
     * this the window doesn't re-layout to cover the space the taskbar
     * just vacated (confirmed by both prior CE ports). */
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    CeConfigLoad(); /* before any *_Init() below - they read their settings out of this shared table */
    CeLogSetEnabled(CeConfigGetInt("VideoDebugLog", 0)); /* Video Config's "Enable Debug Logging" checkbox - default off */
    CeLangInit(); /* loads the persisted Japanese/English UI preference; see ce_lang.h */
    CeInputInit();
    CeAudioInit();
    CeVideoInit();
    CeFileOpenInit();

    retro_set_environment(ce_environment);
    retro_set_video_refresh(ce_video_refresh);
    retro_set_audio_sample(ce_audio_sample_noop);
    retro_set_audio_sample_batch(ce_audio_sample_batch);
    retro_set_input_poll(ce_input_poll);
    retro_set_input_state(ce_input_state);

    retro_init();
    CeLog("WinMain: retro_init done");

    /* Start on the menu ("No ROM loaded", black background) - blocks
     * here until the user opens a ROM (ShowMainMenuDialog only returns
     * once g_romLoaded is true, or the app has already exited via Exit
     * inside the dialog). */
    ShowMainMenuDialog(g_hwnd);

    g_running = 1;
    while (g_running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (!g_romLoaded || g_paused)
        {
            /* No game running (still on the "No ROM loaded" screen) or
             * the touch-to-reveal menu is up: nothing to emulate/blit
             * this tick. Sleep instead of spinning the message pump at
             * 100% CPU for no reason. */
            Sleep(10);
            continue;
        }

        /* Catch-up state for the frame skip signal below (ce_audio.c). */
        CeAudioUpdatePacing();

        if (g_audioBuffStatusCb)
        {
            /* Contractually "called right before retro_run() every
             * frame" (see the RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_
             * STATUS_CALLBACK doc in libretro.h) - only set while Video
             * Config's Frame Skip is above 0 (ce_video.c), so this is a
             * no-op call when it's Off. */
            int active, underrunLikely;
            unsigned occupancyPercent;

            if (CeVideoFrameSkipShouldForceRender())
            {
                /* Frame Skip's own cap (see ce_video.h) has already been
                 * hit: report "buffer not active" so the core's
                 * FRAMESKIP_AUTO logic (which only skips when told the
                 * buffer is active *and* underrunning) renders this one
                 * frame regardless of the real audio state. */
                active = 0;
                occupancyPercent = 0;
                underrunLikely = 0;
            }
            else
            {
                CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
            }
            g_audioBuffStatusCb(active ? true : false, occupancyPercent, underrunLikely ? true : false);
        }

        {
            /* Temporary perf instrumentation - see ce_audio_sample_batch()
             * and ce_display.c's "perf: blit" for the matching per-frame
             * counters. retro_run()'s own wall time already includes both
             * of those (the core calls ce_video_refresh/
             * ce_audio_sample_batch synchronously from inside it), so
             * comparing this average against "perf: blit" + "perf:
             * audio_push" shows how much is core emulation vs. CE-side
             * I/O. Remove once the bottleneck is identified. */
            static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
            DWORD t0 = GetTickCount();
            unsigned elapsed;

            retro_run();

            elapsed = (unsigned)(GetTickCount() - t0);
            s_accumMs += elapsed;
            if (elapsed > s_maxMs)
                s_maxMs = elapsed;
            if (++s_count >= 60)
            {
                CeLog("perf: retro_run avg=%ums max=%ums over %u frames",
                      s_accumMs / s_count, s_maxMs, s_count);
                s_accumMs = 0;
                s_maxMs = 0;
                s_count = 0;
            }
        }

        /* Frame pacing (the sister ports' frontends relied on their
         * cores being slower than real time): the PCE core runs a
         * frame in well under 16ms on this device, so without this it
         * runs several times too fast.
         *
         * Two paths, ported from the sister PopNES port. While a
         * waveOut device is open, pace off the audio ring's fill level:
         * the drain thread (ce_audio.c) empties it at exactly the output
         * rate, so holding retro_run() until the ring is back down to
         * the target locks emulation speed to real playback - no fps
         * rounding, and the ring can never slowly fill up and overrun.
         * The target (CeAudioGetPacingTargetMs) is at least one and a
         * half waveOut buffers plus a frame - the drain thread takes a
         * whole buffer at once - otherwise half the ring, at most 250ms.
         * A title too heavy for full speed keeps the ring low and never
         * waits; with Frame Skip on, CeAudioUpdatePacing's catch-up then
         * skips drawing until the ring is back at the target.
         *
         * Without a device (waveOutOpen failed), fall back to the
         * wall-clock Bresenham pacer: 1000ms spread over CE_TARGET_FPS
         * frames, remainder carried - no per-frame floating point, this
         * device's VFP is software-emulated. Only slows a frame that is
         * ahead; a late one proceeds at once. */
        if (CeAudioIsActive())
        {
            unsigned highMs = CeAudioGetPacingTargetMs();
            unsigned guard;

            for (guard = 0; guard < 120; guard++)
            {
                if (CeAudioGetBufferedMs() <= highMs)
                    break;
                Sleep(1);
            }
        }
        else
        {
            static DWORD    s_deadlineMs   = 0;
            static unsigned s_fracAccumMs  = 0;
            static int      s_deadlineInit = 0;
            DWORD nowMs = GetTickCount();
            LONG  aheadMs;

            if (!s_deadlineInit)
            {
                s_deadlineMs = nowMs;
                s_fracAccumMs = 0;
                s_deadlineInit = 1;
            }

            s_fracAccumMs += 1000;
            s_deadlineMs  += s_fracAccumMs / CE_TARGET_FPS;
            s_fracAccumMs %= CE_TARGET_FPS;

            aheadMs = (LONG)(s_deadlineMs - nowMs);
            if (aheadMs > 0)
                Sleep((DWORD)aheadMs);
            else if (aheadMs < -500)
            {
                /* Far behind (menu just closed, ROM just loaded, a slow
                 * stretch): resync instead of racing to catch up. */
                s_deadlineMs = nowMs;
            }
        }

        /* Periodic SRAM autosave - the pause-time save in
         * ShowMainMenuDialog() only helps if the menu actually gets
         * opened before the device is powered off; this covers a
         * straight-through play session that never touches the menu at
         * all. ~30s is arbitrary (same margin the sister
         * PopSNES uses) - frequent enough to bound how much an in-game save
         * could be lost, infrequent enough that a `.srm` write is not
         * worth timing/skipping for. */
        {
            static DWORD s_lastSramSaveTick = 0;
            DWORD now = GetTickCount();
            if (s_lastSramSaveTick == 0)
                s_lastSramSaveTick = now; /* first frame of gameplay - start the 30s window now, not at an immediate save */
            else if (now - s_lastSramSaveTick >= 30000)
            {
                CeSaveSram();
                s_lastSramSaveTick = now;
            }
        }
    }

    CeLog("WinMain: normal shutdown");
    CeShutdown(0);
    return 0; /* unreachable - CeShutdown() calls ExitProcess() */
}
