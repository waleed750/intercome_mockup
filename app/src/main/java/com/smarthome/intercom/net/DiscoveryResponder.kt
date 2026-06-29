package com.smarthome.intercom.net

import android.util.Log
import com.smarthome.intercom.protocol.Discovery
import com.smarthome.intercom.protocol.ScreenInfo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress

/**
 * UDP discovery responder (port 8089). Binds the well-known port, listens for
 * the door's broadcast/unicast probe, and replies to the sender with this
 * unit's [ScreenInfo] so the door lists us as an available indoor station.
 *
 * Runs continuously for the whole service lifetime. [screenInfoProvider] is
 * called per-reply so edits in Settings (and the live Wi-Fi IP) take effect
 * without restarting discovery.
 */
class DiscoveryResponder(
    private val scope: CoroutineScope,
    private val binder: NetworkBinder,
    private val screenInfoProvider: () -> ScreenInfo,
) {
    @Volatile private var socket: DatagramSocket? = null
    private var job: Job? = null

    fun start() {
        if (job?.isActive == true) return
        job = scope.launch(Dispatchers.IO) { listen() }
    }

    fun stop() {
        job?.cancel()
        job = null
        socket?.close()
        socket = null
    }

    private fun listen() {
        try {
            val sock = DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                bind(InetSocketAddress(Discovery.PORT))
            }
            // Best-effort: keep replies on Wi-Fi. Reception still works without it.
            runCatching { binder.bindToLan(sock) }
            socket = sock

            Log.d(TAG, "Discovery listening on UDP ${Discovery.PORT}")
            val rxBuf = ByteArray(RECEIVE_BUFFER)
            while (scope.isActive) {
                val packet = DatagramPacket(rxBuf, rxBuf.size)
                sock.receive(packet) // blocks until a probe arrives

                // Log every datagram so we can see whether the door's probe even
                // reaches us, and what it actually contains (token diagnosis).
                val preview = String(packet.data, 0, packet.length.coerceAtMost(256), Charsets.UTF_8)
                Log.d(TAG, "UDP from ${packet.address?.hostAddress}:${packet.port} (${packet.length}B): $preview")

                if (!Discovery.isDiscoveryRequest(packet.data, packet.length)) {
                    Log.d(TAG, "  ignored: no discovery token matched")
                    continue
                }

                val door = Discovery.doorAddrFrom(packet.data, packet.length)
                val info = screenInfoProvider()
                val reply = Discovery.buildReply(info, door)
                sock.send(DatagramPacket(reply, reply.size, packet.address, Discovery.PORT))
                Log.d(TAG, "  replied to ${packet.address?.hostAddress}:${Discovery.PORT}: ${reply.decodeToString()}")
            }
        } catch (e: Exception) {
            if (scope.isActive) Log.w(TAG, "Discovery loop ended", e)
        } finally {
            socket?.close()
            socket = null
        }
    }

    private companion object {
        const val TAG = "DiscoveryResponder"
        const val RECEIVE_BUFFER = 4096
    }
}
