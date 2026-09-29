/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_LOG_H
#define CE_LOG_H

/* Crash-safe-ish diagnostic logger: formats each line into a small
 * in-memory buffer and a dedicated background thread periodically
 * appends the buffer to "<exe-dir>\" CE_APP_LOG_FILENAME (ce_app_config.h)
 * in one open+write+close cycle. Ported from the sister PopSG
 * port, round 53. There is no debugger on this device, so this (plus
 * Windows CE's own BER crash log) is the only way to see what happened
 * after a real-hardware run.
 *
 * This used to open+append+close the log file on every single CeLog()
 * call, synchronously, on whichever thread called it - almost always
 * the main/emulation thread (retro_run() and its CE-side callers).
 * Real-hardware logs showed this device's flash storage easily costs
 * several ms per open/write/close cycle, and with "perf:" instrumentation
 * alone emitting ~8 lines/sec, that was 8+ synchronous flash stalls per
 * second stealing time from retro_run() - enough on its own to push a
 * CPU-bound core (Mega CD) into audible audio underruns that don't
 * happen with logging off (round 2026-09-02 investigation). The
 * background-thread + buffer design here moves *all* file I/O off the
 * caller's thread; CeLog() itself only formats a string and memcpy()s it
 * into a lock-protected buffer.
 *
 * Durability tradeoff: a line is on flash within CE_LOG_FLUSH_INTERVAL_MS
 * (ce_log.c) of being logged, not immediately - the writer thread also
 * wakes early once the buffer crosses a high-water mark, so a burst of
 * logging doesn't sit unflushed for the full interval either.
 * CeLogShutdown() (below) makes the writer thread drain once more right
 * before it exits, so disabling logging or a normal app exit never drops
 * whatever was queued at that moment. On a hang - this device's actual
 * observed failure mode more often than a hard power-loss (see
 * CeShutdown()'s own comment in ce_main.c on why WinMain avoids
 * WM_CLOSE/PostQuitMessage after tracing an exit hang to them) - the
 * writer thread keeps running and draining even though the main thread
 * is stuck, so the log still ends up complete once you cut power. Only a
 * true instantaneous power-loss can lose the last few hundred ms of
 * lines, which the old per-call fclose() design was never perfectly
 * immune to either (the OS/storage stack's own write-behind caching is
 * outside this code's control regardless).
 *
 * Gated by CeLogSetEnabled(): while disabled (the default) CeLog() is a
 * no-op and no log file is created or appended. Video Config's "Enable
 * Debug Logging" checkbox is the switch, persisted as "VideoDebugLog"
 * (default 0/off). WinMain flips it on right after CeConfigLoad(), so
 * the handful of CeLog() calls before that point are always
 * suppressed. */
void CeLog(const char *fmt, ...);

void CeLogSetEnabled(int enabled);
int  CeLogIsEnabled(void);

/* Stops the background writer thread (if running) after a final drain,
 * so no buffered lines are lost. Call once from CeShutdown() (ce_main.c)
 * after every other CeLog() call in the teardown path, right before
 * ExitProcess(). Safe to call even when logging was never enabled. */
void CeLogShutdown(void);

/* For the perf log. The writer thread's handle (NULL while logging is
 * off). Flushes since the last call, the longest one in ms (wall time),
 * and lines dropped because the buffer was full; resets them. */
void *CeLogThread(void);
void CeLogTakeStats(unsigned *flushes, unsigned *longestMs, unsigned *dropped);
/* CPU time (kernel + user) a thread has used so far, in ms; 0 if thread
 * is NULL or GetThreadTimes fails. thread is a HANDLE (GetCurrentThread()
 * works too). */
unsigned CeThreadCpuMs(void *thread);

#endif
