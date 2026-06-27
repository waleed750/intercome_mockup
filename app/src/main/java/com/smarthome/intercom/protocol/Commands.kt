package com.smarthome.intercom.protocol

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/** Control commands the door sends to us on the CONTROL channel. */
enum class InboundCommand {
    /** Visitor pressed the call button — start ringing. */
    CALL,

    /** Door is re-querying call state — re-confirm our answer. */
    GET_CALL_INFO,

    /** Door (or its other indoor units) ended the call. */
    HANG_UP,

    /** Anything we don't model. */
    UNKNOWN,
}

/**
 * A decoded CONTROL-channel message. The protocol uses a `command` field
 * (e.g. `{"command":"Call"}`); some firmware revisions use `cmd`, so both are
 * accepted. Unknown keys are ignored so forward-compatible fields don't break parsing.
 */
@Serializable
data class ControlMessage(
    @SerialName("command") val command: String? = null,
    @SerialName("cmd") val cmd: String? = null,
) {
    val name: String? get() = command ?: cmd

    fun classify(): InboundCommand = when (name?.trim()?.lowercase()) {
        "call" -> InboundCommand.CALL
        "getcallinfo" -> InboundCommand.GET_CALL_INFO
        "hangup" -> InboundCommand.HANG_UP
        else -> InboundCommand.UNKNOWN
    }
}

object Commands {
    private val json = Json {
        ignoreUnknownKeys = true
        isLenient = true
    }

    /** Parses a CONTROL payload, returning null if it isn't valid JSON. */
    fun parse(payload: ByteArray): ControlMessage? = try {
        json.decodeFromString<ControlMessage>(payload.decodeToString())
    } catch (_: Exception) {
        null
    }

    // ---- Outbound commands (encode with [Frame.encode] on the CONTROL channel) ----

    /** Confirmed working: releases the door latch. Gate behind a connected call. */
    fun openDoor(): ByteArray = control("""{"command":"OpenDoor"}""")

    /**
     * Opens the door station's speaker. UNCONFIRMED: the reference client lists
     * several talk-command candidates and sends none on accept, so this is a seam
     * to confirm against the device if uplink audio turns out to be one-way. Not
     * part of the proven answer flow.
     */
    fun startTalk(): ByteArray = control("""{"command":"StartTalk"}""")

    /** Tears the call down from our side (matches the reference client). */
    fun hangUp(): ByteArray = control("""{"command":"HangUp","OtherAnswer":0}""")

    /** Tells a second caller we're already on a call. */
    fun deviceBusy(): ByteArray = control("""{"command":"deviceBusy"}""")

    /**
     * The answer handshake, confirmed against the reference client: the three
     * `Answer` variants, sent in order. The door treats the `OtherAnswer` flag
     * variants as the accept confirmation. The reference spaces them ~80 ms apart;
     * we enqueue them back-to-back on the ordered write loop. No `StartTalk` is
     * sent on accept — the reference client doesn't send one.
     */
    val ANSWER_SEQUENCE: List<String> = listOf(
        """{"command":"Answer"}""",
        """{"command":"Answer","OtherAnswer":1}""",
        """{"command":"Answer","OtherAnswer":true}""",
    )

    /** Encoded answer frames, in send order. */
    fun answerFrames(): List<ByteArray> = ANSWER_SEQUENCE.map { control(it) }

    private fun control(jsonText: String): ByteArray =
        Frame.encode(Channel.CONTROL, jsonText.encodeToByteArray())
}
