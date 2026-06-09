#pragma once

#include <stdbool.h>
#include "board.h"

#if HAS_VOICE_INPUT

/**
 * Initialize the voice input subsystem.
 * Must be called after config_manager_init(), mic_driver_init(), and
 * wifi_manager_init().
 */
void voice_input_init(void);

/**
 * Start a voice recognition session.
 * Opens a WebSocket to Baidu Real-Time ASR, starts mic capture, and streams
 * PCM audio.  When recognition completes, the result text is typed as HID
 * keyboard input automatically.
 *
 * No-op if a session is already active, ASR is disabled in config, or WiFi
 * is not connected.
 */
bool voice_input_start(void);

/**
 * Stop the current voice recognition session (if any).
 * Sends a FINISH frame, waits briefly for the final result, then tears down
 * the WebSocket and stops mic capture.
 */
void voice_input_stop(void);

/**
 * Cancel the current voice recognition session (if any).
 * Sends a CANCEL frame and tears down immediately without waiting for a
 * result.
 */
void voice_input_cancel(void);

/** Return true if a voice recognition session is currently active. */
bool voice_input_is_active(void);

#else

static inline void voice_input_init(void) {}
static inline bool voice_input_start(void) { return false; }
static inline void voice_input_stop(void) {}
static inline void voice_input_cancel(void) {}
static inline bool voice_input_is_active(void) { return false; }

#endif /* HAS_VOICE_INPUT */
