/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Buffered/background-thread diagnostic logger - see ce_log.h for the
 * full rationale (this replaces an earlier version that did a
 * synchronous open+append+close on every CeLog() call, which stalled
 * whichever thread called it - almost always the emulation thread).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

#include "ce_log.h"
#include "ce_app_config.h"

/* Off by default - see ce_log.h. Video Config's "Enable Debug Logging"
 * checkbox turns it on. */
static int s_enabled = 0;

/* Formatted lines accumulate here (CeLog(), any thread) until the
 * writer thread drains them. Sized for many seconds' worth of this
 * port's actual logging volume (the heaviest source, the "perf:"
 * instrumentation, is well under 1KB/sec) - a burst that somehow fills
 * it between flushes just drops further lines until the next drain
 * (CeLog() below) rather than blocking the caller or growing without
 * bound, same style as ce_fileopen.c's CE_MAX_FILE_ENTRIES cap. */
#define CE_LOG_BUF_CAP 32768
static CRITICAL_SECTION s_logCs;
static int    s_csReady  = 0;
static char   s_logBuf[CE_LOG_BUF_CAP];
static size_t s_logBufLen = 0;

/* Writer thread wakes on this interval even if nothing crossed the
 * high-water mark below, so a line is never sitting unflushed for more
 * than ~this long. */
#define CE_LOG_FLUSH_INTERVAL_MS 200
/* CeLog() SetEvent()s the writer early once the buffer is this full, so
 * a sudden burst of logging doesn't have to wait out the full interval
 * (and, more importantly, doesn't get dropped by hitting CE_LOG_BUF_CAP
 * before the next scheduled flush). */
#define CE_LOG_FLUSH_HIWATER (CE_LOG_BUF_CAP * 3 / 4)

static HANDLE          s_logThread     = NULL;
static HANDLE          s_logWakeEvent  = NULL;
static volatile int    s_logRunning    = 0;

/* For the perf log (CeLogTakeStats): flushes since the last take, the
 * longest one's wall time (open+write+close), and lines CeLog() dropped
 * because the buffer was full. */
static unsigned s_statFlushes, s_statFlushMaxMs, s_statDropped;

static void BuildLogPath(wchar_t *logPath, size_t count)
{
    wchar_t exePath[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    slash = wcsrchr(exePath, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(logPath, count, L"%s\\" CE_APP_LOG_FILENAME, exePath);
}

/* Drains whatever is currently buffered into one open+write+close cycle.
 * Only ever called from the writer thread (CeLogThreadProc) - including
 * its one extra call right after the wait loop exits, to catch whatever
 * was queued between the last periodic drain and the stop request - so
 * s_local below needs no locking of its own. */
static void FlushLogBuffer(void)
{
    static char s_local[CE_LOG_BUF_CAP];
    size_t len;
    wchar_t logPath[MAX_PATH];
    FILE *f;
    DWORD t0, ms;

    EnterCriticalSection(&s_logCs);
    len = s_logBufLen;
    if (len)
        memcpy(s_local, s_logBuf, len);
    s_logBufLen = 0;
    LeaveCriticalSection(&s_logCs);

    if (len == 0)
        return;

    t0 = GetTickCount();
    BuildLogPath(logPath, MAX_PATH);
    f = _wfopen(logPath, L"a");
    if (!f)
        return;
    fwrite(s_local, 1, len, f);
    fclose(f);
    ms = GetTickCount() - t0;

    EnterCriticalSection(&s_logCs);
    s_statFlushes++;
    if (ms > s_statFlushMaxMs)
        s_statFlushMaxMs = ms;
    LeaveCriticalSection(&s_logCs);
}

static DWORD WINAPI CeLogThreadProc(LPVOID param)
{
    (void)param;
    while (s_logRunning)
    {
        WaitForSingleObject(s_logWakeEvent, CE_LOG_FLUSH_INTERVAL_MS);
        FlushLogBuffer();
    }
    FlushLogBuffer(); /* final drain of anything queued right before stop */
    return 0;
}

void CeLogSetEnabled(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (enabled == s_enabled)
        return;

    if (enabled)
    {
        if (!s_csReady)
        {
            InitializeCriticalSection(&s_logCs);
            s_csReady = 1;
        }
        s_logBufLen   = 0;
        s_logWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        s_logRunning  = 1;
        s_logThread   = CreateThread(NULL, 0, CeLogThreadProc, NULL, 0, NULL);
        /* Below the frame loop (normal). The file system work of a flush
         * runs on the CPU, and at the loop's own priority a flush that
         * takes long (a growing log on flash) held the loop off for up to
         * a 100ms quantum - CD games log a lot (underruns, drive state).
         * Down here it only runs while the loop sleeps; if the loop never
         * sleeps the buffer fills and lines are dropped (counted in
         * CeLogTakeStats). */
        if (s_logThread)
            SetThreadPriority(s_logThread, THREAD_PRIORITY_BELOW_NORMAL);
        s_enabled     = 1;
    }
    else
    {
        /* Flip s_enabled off first so any CeLog() racing this on another
         * thread stops queuing immediately rather than adding a line
         * after the drain below has already run. */
        s_enabled = 0;
        if (s_logThread)
        {
            s_logRunning = 0;
            if (s_logWakeEvent)
                SetEvent(s_logWakeEvent);
            /* Bounded wait, same 500ms margin ce_audio.c's CeAudioStop()
             * uses for its own drain-thread join - the writer thread's
             * own work (one fopen/fwrite/fclose) is far cheaper than
             * that budget needs, this is just a "never hang here"
             * ceiling. */
            WaitForSingleObject(s_logThread, 500);
            CloseHandle(s_logThread);
            s_logThread = NULL;
        }
        if (s_logWakeEvent)
        {
            CloseHandle(s_logWakeEvent);
            s_logWakeEvent = NULL;
        }
    }
}

int CeLogIsEnabled(void)
{
    return s_enabled;
}

void CeLogShutdown(void)
{
    /* Reuses the disable path above - it already stops the writer
     * thread only after a final FlushLogBuffer() drain. Safe to call
     * when logging was never enabled (CeLogSetEnabled(0) on an already-
     * disabled logger is a no-op). */
    CeLogSetEnabled(0);
}

void CeLog(const char *fmt, ...)
{
    char msg[512];
    size_t len;
    va_list ap;
    int hiwater;

    if (!s_enabled)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    len = strlen(msg);

    EnterCriticalSection(&s_logCs);
    if (s_logBufLen + len + 1 <= CE_LOG_BUF_CAP)
    {
        memcpy(s_logBuf + s_logBufLen, msg, len);
        s_logBufLen += len;
        s_logBuf[s_logBufLen++] = '\n';
    }
    /* else: buffer is full between flushes - drop this line rather than
     * block the caller or grow unbounded (see s_logBuf's own comment). */
    else
        s_statDropped++;
    hiwater = (s_logBufLen >= CE_LOG_FLUSH_HIWATER);
    LeaveCriticalSection(&s_logCs);

    if (hiwater && s_logWakeEvent)
        SetEvent(s_logWakeEvent);
}

void *CeLogThread(void)
{
    return s_logThread;
}

void CeLogTakeStats(unsigned *flushes, unsigned *longestMs, unsigned *dropped)
{
    if (!s_csReady)
    {
        *flushes = *longestMs = *dropped = 0;
        return;
    }
    EnterCriticalSection(&s_logCs);
    *flushes   = s_statFlushes;
    *longestMs = s_statFlushMaxMs;
    *dropped   = s_statDropped;
    s_statFlushes = s_statFlushMaxMs = s_statDropped = 0;
    LeaveCriticalSection(&s_logCs);
}

unsigned CeThreadCpuMs(void *thread)
{
    FILETIME created, exited, kernel, user;
    ULONGLONG t;

    if (!thread || !GetThreadTimes((HANDLE)thread, &created, &exited, &kernel, &user))
        return 0;
    t = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime)
      + ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime);
    return (unsigned)(t / 10000); /* 100ns units */
}
