package com.smarthome.intercom.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.Socket

/**
 * Keeps intercom traffic on the home Wi-Fi and reachable while idle.
 *
 *  - Resolves the current Wi-Fi network and its IPv4 address so sockets bind to
 *    the LAN interface only (never mobile data — satisfies SEC-1 and the
 *    "don't escape to mobile data" requirement).
 *  - Holds a [WifiManager.MulticastLock] so broadcast discovery probes are
 *    delivered, and a high-perf [WifiManager.WifiLock] so Wi-Fi stays awake
 *    during a call.
 */
class NetworkBinder(context: Context) {

    private val appContext = context.applicationContext
    private val connectivity =
        appContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val wifi =
        appContext.getSystemService(Context.WIFI_SERVICE) as WifiManager

    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    /** The active Wi-Fi [Network], or null if not connected to Wi-Fi. */
    fun wifiNetwork(): Network? = connectivity.allNetworks.firstOrNull { net ->
        connectivity.getNetworkCapabilities(net)
            ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
    }

    /** This device's IPv4 address on Wi-Fi, used to bind listening sockets. */
    fun wifiIpv4Address(): Inet4Address? {
        val net = wifiNetwork() ?: return null
        val props = connectivity.getLinkProperties(net) ?: return null
        return props.linkAddresses
            .map { it.address }
            .filterIsInstance<Inet4Address>()
            .firstOrNull { !it.isLoopbackAddress && !it.isLinkLocalAddress }
    }

    fun isOnWifi(): Boolean = wifiNetwork() != null

    /**
     * Human-readable dump of every network the OS currently exposes to us, with
     * transports, internet/validation flags, and IPs. Logged on each connectivity
     * refresh so a red "not on Wi-Fi" dot can be traced to what the phone actually
     * sees (e.g. Wi-Fi present but no IPv4, or no Wi-Fi transport at all).
     */
    fun describeNetworks(): String {
        val nets = connectivity.allNetworks
        if (nets.isEmpty()) return "(no networks)"
        return nets.joinToString("; ") { net ->
            val caps = connectivity.getNetworkCapabilities(net)
            val transports = listOfNotNull(
                "WIFI".takeIf { caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true },
                "CELLULAR".takeIf { caps?.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) == true },
                "ETHERNET".takeIf { caps?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true },
                "VPN".takeIf { caps?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) == true },
            ).ifEmpty { listOf("none") }.joinToString("+")
            val internet = caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true
            val validated = caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) == true
            val ips = connectivity.getLinkProperties(net)
                ?.linkAddresses?.joinToString(",") { it.address.hostAddress ?: "?" } ?: ""
            "[$transports internet=$internet validated=$validated ips=$ips]"
        }
    }

    /** Pins a UDP socket to the Wi-Fi network so its traffic can't leak to cellular. */
    fun bindToWifi(socket: DatagramSocket) {
        wifiNetwork()?.bindSocket(socket)
    }

    /** Pins a TCP socket to the Wi-Fi network. */
    fun bindToWifi(socket: Socket) {
        wifiNetwork()?.bindSocket(socket)
    }

    @Suppress("DEPRECATION")
    fun acquireLocks() {
        if (multicastLock == null) {
            multicastLock = wifi.createMulticastLock(MULTICAST_LOCK_TAG).apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        if (wifiLock == null) {
            wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, WIFI_LOCK_TAG).apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    fun releaseLocks() {
        multicastLock?.let { if (it.isHeld) it.release() }
        wifiLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
        wifiLock = null
    }

    private companion object {
        const val MULTICAST_LOCK_TAG = "SmartIntercom:multicast"
        const val WIFI_LOCK_TAG = "SmartIntercom:wifi"
    }
}
