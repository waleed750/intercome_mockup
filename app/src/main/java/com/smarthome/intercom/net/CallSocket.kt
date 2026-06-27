package com.smarthome.intercom.net

import android.util.Log
import com.smarthome.intercom.protocol.Channel
import com.smarthome.intercom.protocol.FrameParser
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel as MailChannel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.BufferedOutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * One active door connection. Owns the read loop (bytes -> [FrameParser] ->
 * [onFrame]) and a single writer coroutine draining an outbox, so audio frames
 * (~50/s) and control commands can be enqueued from any thread without locking.
 */
class CallConnection(
    private val socket: Socket,
    private val scope: CoroutineScope,
    private val onFrame: (Channel, ByteArray) -> Unit,
    private val onClosed: () -> Unit,
) {
    val remoteAddress: InetAddress? = socket.inetAddress

    private val parser = FrameParser(onFrame = onFrame)
    private val outbox = MailChannel<ByteArray>(capacity = OUTBOX_CAPACITY)
    private var readJob: Job? = null
    private var writeJob: Job? = null
    @Volatile private var closed = false

    fun start() {
        socket.tcpNoDelay = true
        socket.keepAlive = true
        readJob = scope.launch(Dispatchers.IO) { readLoop() }
        writeJob = scope.launch(Dispatchers.IO) { writeLoop() }
    }

    /**
     * Queues a fully-encoded frame for sending. Returns false if the outbox is
     * full (only happens under severe backpressure) — fine to drop for audio,
     * and control commands are far too infrequent to fill it.
     */
    fun enqueue(frame: ByteArray): Boolean = outbox.trySend(frame).isSuccess

    fun close() {
        if (closed) return
        closed = true
        outbox.close()
        readJob?.cancel()
        writeJob?.cancel()
        runCatching { socket.close() }
    }

    private fun readLoop() {
        try {
            val input = socket.getInputStream()
            val buf = ByteArray(READ_BUFFER)
            while (scope.isActive && !closed) {
                val n = input.read(buf)
                if (n < 0) break // door closed the connection
                if (n > 0) parser.offer(buf, 0, n)
            }
        } catch (e: Exception) {
            if (!closed) Log.d(TAG, "read loop ended: ${e.message}")
        } finally {
            notifyClosed()
        }
    }

    private suspend fun writeLoop() {
        try {
            val out = BufferedOutputStream(socket.getOutputStream())
            for (frame in outbox) {
                out.write(frame)
                out.flush()
            }
        } catch (e: Exception) {
            if (!closed) Log.d(TAG, "write loop ended: ${e.message}")
        } finally {
            notifyClosed()
        }
    }

    private fun notifyClosed() {
        if (closed) return
        closed = true
        runCatching { socket.close() }
        onClosed()
    }

    private companion object {
        const val TAG = "CallConnection"
        const val READ_BUFFER = 16 * 1024
        const val OUTBOX_CAPACITY = 256
    }
}

/**
 * TCP server on port 8189. The door is the client: it connects when a visitor
 * calls. We bind to the Wi-Fi address (never a public interface — SEC-1) and
 * hand each accepted socket to [onAccepted], which decides whether to take the
 * call or reject it as busy.
 */
class CallServer(
    private val scope: CoroutineScope,
    private val port: Int = DEFAULT_PORT,
    private val bindAddressProvider: () -> InetAddress?,
    private val onAccepted: (Socket) -> Unit,
) {
    @Volatile private var server: ServerSocket? = null
    private var job: Job? = null

    fun start() {
        if (job?.isActive == true) return
        job = scope.launch(Dispatchers.IO) { acceptLoop() }
    }

    fun stop() {
        job?.cancel()
        job = null
        runCatching { server?.close() }
        server = null
    }

    private fun acceptLoop() {
        try {
            val bindAddress = bindAddressProvider()
            val srv = ServerSocket().apply {
                reuseAddress = true
                bind(InetSocketAddress(bindAddress, port), BACKLOG)
            }
            server = srv
            Log.d(TAG, "Listening on ${bindAddress?.hostAddress ?: "*"}:$port")
            while (scope.isActive) {
                val socket = srv.accept()
                onAccepted(socket)
            }
        } catch (e: Exception) {
            if (scope.isActive) Log.w(TAG, "accept loop ended", e)
        } finally {
            runCatching { server?.close() }
            server = null
        }
    }

    private companion object {
        const val TAG = "CallServer"
        const val DEFAULT_PORT = 8189
        const val BACKLOG = 4
    }
}
