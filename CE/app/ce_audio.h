/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * waveOut-backed audio output for the CE frontend: a lock-protected
 * ring buffer fed by retro_audio_sample_batch_t (called from the main
 * thread, inside retro_run - must never block) and drained by a
 * dedicated high-priority thread that owns the actual waveOut device,
 * same "producer on the emulation thread, consumer on its own thread"
 * split as the previous WSNES9X-based CE port used.
 */
#ifndef CE_AUDIO_H
#define CE_AUDIO_H

#include <windows.h>
#include <stdint.h>

/* Loads saved volume/mute from the registry and readies the ring
 * buffer's lock. Call once from WinMain, before any ROM is loaded. */
void CeAudioInit(void);

/* Opens (or re-opens, if the rate changed) the waveOut device at
 * sampleRateHz and starts the drain thread. Safe to call every time a
 * ROM loads (including File>Open reloads) - a no-op if already running
 * at the same rate. */
void CeAudioStart(double sampleRateHz);

/* Stops the drain thread and closes the waveOut device. Safe to call
 * even if never started. */
void CeAudioStop(void);

/* retro_audio_sample_batch_t hook - copies interleaved L/R int16
 * samples into the ring buffer (applying the current volume/mute),
 * dropping the oldest buffered audio on overrun rather than blocking.
 * Always returns frames (all "consumed" from the core's point of
 * view), matching the libretro contract. */
size_t CeAudioPushSamples(const int16_t *data, size_t frames);

void CeShowSoundConfigDialog(HWND owner);

/* Reports this device's actual audio ring buffer fill level, for
 * ce_main.c to forward to the core's retro_audio_buffer_status_callback_t
 * (RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK) once per frame -
 * what Video Config's frame skip "Auto" mode (ce_video.c) needs to
 * actually do anything, since the core's FRAMESKIP_AUTO/AUTO_THRESHOLD
 * logic only skips frames based on values reported through that
 * callback (see libretro/libretro.c's check_variables/retro_run). Safe
 * to call every frame regardless of whether a device is open. */
void CeAudioGetBufferStatus(int *active, unsigned *occupancyPercent, int *underrunLikely);

/* Tells the drain thread that retro_run() has stopped being called (the
 * touch-to-reveal menu / a settings dialog is up, ce_main.c's g_paused).
 * Ring will legitimately starve during this time since nothing is
 * pushing new samples - CeAudioThreadProc keeps playing silence exactly
 * as before, this only stops it from misreporting that starvation as an
 * "audio underrun" (which the log/counter otherwise can't tell apart
 * from the emulator genuinely falling behind - see round-20 dev notes
 * investigation). Call from ce_main.c wherever g_paused is set/cleared. */
void CeAudioSetPaused(int paused);

/* Frame pacing (ce_main.c's WinMain loop, ported from the sister
 * PopNES port): nonzero while a waveOut device is open; how much audio the
 * ring currently holds, in ms at the device's output rate (0 when no
 * device is open). */
int CeAudioIsActive(void);
unsigned CeAudioGetBufferedMs(void);

/* The ring level (ms) the pacer holds retro_run() back to - at least 1.5
 * waveOut buffers plus a frame, otherwise half the ring, at most 250ms.
 * 0 when no device is open. */
unsigned CeAudioGetPacingTargetMs(void);

/* Call once per frame, before CeAudioGetBufferStatus(): updates the
 * catch-up state that CeAudioGetBufferStatus() reports as
 * underrunLikely (set below half the target, cleared at the target) and
 * returns CeAudioGetPacingTargetMs(). */
unsigned CeAudioUpdatePacing(void);

#endif
