/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Physical keyboard -> SNES joypad input for the CE frontend. Polling-
 * based (GetAsyncKeyState), not message-based, so it works the same way
 * whether the app has keyboard focus quirks or not, and so the Input
 * Config dialog (remap-by-press) can reuse the exact same primitive.
 */
#ifndef CE_INPUT_H
#define CE_INPUT_H

#include <windows.h>
#include <stdint.h>

/* Loads any saved key mapping from the registry (falls back to the
 * built-in defaults for anything not saved yet). Call once from
 * WinMain before the first CeInputPoll(). */
void CeInputInit(void);

/* retro_input_poll_t hook: samples all mapped keys once per frame. */
void CeInputPoll(void);

/* retro_input_state_t hook: id is a RETRO_DEVICE_ID_JOYPAD_* constant. */
int16_t CeInputState(unsigned port, unsigned device, unsigned index, unsigned id);

/* Modal native dialog (IDD_INPUTCONFIG): click a button, then press the
 * physical key to bind to that SNES button. OK persists the mapping to
 * the registry; Cancel discards changes made in this dialog session. */
void CeShowInputConfigDialog(HWND owner);

/* True while a remap button click is waiting for a key press (see
 * BeginWaitForKey in ce_input.c). ce_main.c's WndProc uses this to log
 * messages that reach the *main* window during that window - diagnostic
 * for the still-undetected "\x6c7a\x5b9a" (Decide/OK) button: if its
 * WM_KEYDOWN (or anything else) is being routed to the main frame window
 * instead of the modal IDD_INPUTCONFIG dialog, InputConfigDlgProc's own
 * diagnostic logging would never see it, but this would. */
int CeInputIsWaitingForKeyRemap(void);

/* Call right before gameplay (re)starts after a modal dialog closed via
 * the physical decide/OK key - Main Menu's Load State (most visibly,
 * since it resumes immediately) but really any decide-key dismissal,
 * including Open ROM and Resume/Cancel. Makes CeInputPoll() report Start
 * as not-pressed until the key is actually observed released at least
 * once, so a physical decide-key press still in progress at the moment
 * polling resumes can't be misread as a fresh Start press and trigger
 * the game's own in-game pause. Root cause: input polling is frozen
 * while a modal dialog is up (see ce_main.c's ShowMainMenuDialog), so
 * there's no debounce history to tell "still held from before the
 * dialog opened" apart from "a brand-new press" once polling resumes -
 * a real problem whenever Start happens to share (or be rebound to) the
 * same physical key as the dialog's own decide/OK action. First
 * confirmed and fixed on an earlier prototype (its
 * g_suppress_start_key - the dev notes), then
 * independently re-confirmed necessary on a second CE port built from
 * this template, which is why it now lives here instead of only in
 * the earlier prototype's own, differently-structured input code. */
void CeInputSuppressStartKey(void);

#endif
