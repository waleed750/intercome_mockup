package com.smarthome.intercom.protocol

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

/**
 * This unit's identity, returned to the door over UDP discovery so it lists us
 * as an available indoor station.
 *
 * The wire shape mirrors the proven reference client exactly: a **flat** JSON
 * object (no wrapper). The door keys on [command] and [appid] to accept the
 * reply; [alias]/[serial] are this install's cosmetic identity. [dstAddr]/
 * [dstType] address the reply back to the door and are normally overwritten with
 * the door's own address, echoed from its probe (see [Discovery.buildReply]).
 * The door learns our IP from the UDP source address, so no `ip` field is sent.
 */
@Serializable
data class ScreenInfo(
    val command: String = "cmd_reply_get_device_info",
    val appid: String = "7551000",
    val alias: String,
    @SerialName("group_ip") val groupIp: String = "239.255.74.199",
    val serial: String,
    val dstType: Int = 3,
    val dstAddr: String,
    val verify: Int = 0,
    val deviceBusy: Int = 0,
    @SerialName("camera_en") val cameraEn: Int = 0,
    @SerialName("relay0_delay") val relay0Delay: Int = 1,
)

/** The door's own address, parsed from its probe, used to address the reply back to it. */
data class DoorAddr(val addr: String, val type: Int)

/** UDP discovery on port 8089. */
object Discovery {
    const val PORT = 8089

    /**
     * Substrings the door includes in its discovery probe. We match on substring
     * rather than a strict schema because the probe shape varies by firmware and
     * we only need to recognise "is this a discovery request?".
     */
    private val REQUEST_TOKENS = listOf(
        "cmd_send_get_call_device",
        "cmd_send_get_device_info",
    )

    fun isDiscoveryRequest(payload: ByteArray, length: Int = payload.size): Boolean {
        val text = String(payload, 0, length, Charsets.UTF_8)
        return REQUEST_TOKENS.any { text.contains(it) }
    }

    private val json = Json {
        encodeDefaults = true
        ignoreUnknownKeys = true
        isLenient = true
    }

    /** The few probe fields we care about: the door's own address. */
    @Serializable
    private data class Probe(
        val localAddr: String? = null,
        val localType: Int? = null,
    )

    /**
     * Extracts the door's own address (`localAddr`/`localType`) from its probe so
     * the reply can be addressed back to it. Returns null if the probe can't be
     * parsed or carries no `localAddr`.
     */
    fun doorAddrFrom(payload: ByteArray, length: Int = payload.size): DoorAddr? =
        runCatching {
            val p = json.decodeFromString(Probe.serializer(), String(payload, 0, length, Charsets.UTF_8))
            p.localAddr?.let { DoorAddr(it, p.localType ?: 3) }
        }.getOrNull()

    /**
     * Builds the discovery reply: the flat [ScreenInfo] record the door expects.
     * When [door] is known (parsed from the probe), its address is echoed into
     * `dstAddr`/`dstType` so the reply is addressed to the door — matching the
     * reference client, whose door answers to its own `localAddr`.
     */
    fun buildReply(info: ScreenInfo, door: DoorAddr? = null): ByteArray {
        val reply = if (door != null) info.copy(dstAddr = door.addr, dstType = door.type) else info
        return json.encodeToString(ScreenInfo.serializer(), reply).encodeToByteArray()
    }
}
