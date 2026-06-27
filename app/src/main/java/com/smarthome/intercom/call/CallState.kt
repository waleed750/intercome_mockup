package com.smarthome.intercom.call

/**
 * Call lifecycle per the architecture doc:
 * `IDLE -> RINGING -> CONNECTING -> CONNECTED`, with any terminal transition
 * (remote HangUp or user End/Decline) returning to `IDLE`.
 */
enum class CallPhase { IDLE, RINGING, CONNECTING, CONNECTED }

/** Everything the UI needs to render, derived from the call + network state. */
data class CallUiState(
    val phase: CallPhase = CallPhase.IDLE,
    val callerLabel: String = "Front Door",
    val muted: Boolean = false,
    /** False after a decode failure — show "Video unavailable" but keep audio. */
    val videoAvailable: Boolean = true,
    /** True once the first frame has rendered (hides the "Connecting…" placeholder). */
    val hasVideoFrames: Boolean = false,
    /** False if mic permission is denied/unavailable — show the in-call banner. */
    val micAvailable: Boolean = true,
    val onWifi: Boolean = true,
    /** Momentary toast-style copy: "Door unlocked", "Call ended". */
    val transientMessage: String? = null,
    val unitName: String = "",
    val pairedDoor: String = "Front Door",
) {
    val isInCall: Boolean get() = phase == CallPhase.CONNECTING || phase == CallPhase.CONNECTED
    val isRinging: Boolean get() = phase == CallPhase.RINGING
}
