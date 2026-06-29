"""UDP discovery probe broadcaster + reply listener for auto-detecting indoor units."""

from __future__ import annotations

import ipaddress
import json
import logging
import socket
import threading
from dataclasses import dataclass

DISCOVERY_PORT = 8089
DISCOVERY_INTERVAL_SECONDS = 3.0

LOG = logging.getLogger("DISCOVERY")


def _get_all_broadcast_addresses() -> list[str]:
    """Get broadcast addresses for all active IPv4 interfaces (Windows, no deps)."""
    addrs = set()
    addrs.add("255.255.255.255")
    try:
        # Use socket to get all local IPs, then compute /24 broadcast as fallback
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if ip.startswith("127."):
                continue
            # Assume /24 as most common subnet — covers home/office networks
            try:
                net = ipaddress.IPv4Network(f"{ip}/24", strict=False)
                addrs.add(str(net.broadcast_address))
            except ValueError:
                pass
    except Exception:
        pass
    return list(addrs)


@dataclass
class IndoorUnit:
    ip: str
    udp_ip: str
    reported_ip: str
    alias: str
    serial: str
    dst_addr: str
    raw: dict


class DiscoveryBroadcaster:
    def __init__(
        self,
        stop_event: threading.Event,
        local_addr: str = "door-001",
        broadcast_ip: str = "255.255.255.255",
        port: int = DISCOVERY_PORT,
    ):
        self.stop_event = stop_event
        self.local_addr = local_addr
        self.broadcast_ip = broadcast_ip
        self.port = port
        self.thread: threading.Thread | None = None
        self.listener_thread: threading.Thread | None = None

        self.discovered_units: dict[str, IndoorUnit] = {}
        self.units_lock = threading.Lock()
        self.on_unit_discovered: list = []

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.thread = threading.Thread(target=self._broadcast_loop, name="discovery-broadcast", daemon=True)
        self.thread.start()
        self.listener_thread = threading.Thread(target=self._listen_loop, name="discovery-listen", daemon=True)
        self.listener_thread.start()

    def send_once(self):
        payload = json.dumps(
            {
                "command": "cmd_send_get_device_info",
                "localAddr": self.local_addr,
                "localType": 3,
            },
            separators=(",", ":"),
        ).encode("utf-8")

        targets = _get_all_broadcast_addresses()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            for addr in targets:
                try:
                    sock.sendto(payload, (addr, self.port))
                except OSError:
                    pass
        LOG.info("Sent probe to %s on port %s", ", ".join(targets), self.port)

    def get_units(self) -> list[IndoorUnit]:
        with self.units_lock:
            return list(self.discovered_units.values())

    def _broadcast_loop(self):
        while not self.stop_event.is_set():
            try:
                self.send_once()
            except OSError as exc:
                LOG.warning("Discovery probe failed: %s", exc)
            self.stop_event.wait(DISCOVERY_INTERVAL_SECONDS)

    def _listen_loop(self):
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except (AttributeError, OSError):
                pass  # SO_REUSEPORT not available on Windows
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("0.0.0.0", self.port))
            sock.settimeout(1.0)
            LOG.info("Listening for discovery replies on UDP %s", self.port)

            while not self.stop_event.is_set():
                try:
                    data, addr = sock.recvfrom(4096)
                except socket.timeout:
                    continue
                except OSError as exc:
                    LOG.debug("Listener recv error: %s", exc)
                    break

                LOG.debug("UDP packet from %s:%s (%d bytes): %s",
                          addr[0], addr[1], len(data),
                          data[:200].decode("utf-8", errors="replace"))

                try:
                    msg = json.loads(data.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    LOG.debug("Non-JSON UDP from %s: %s", addr[0], exc)
                    continue

                command = msg.get("command", "")
                LOG.debug("Parsed command='%s' from %s", command, addr[0])

                # Ignore our own probes
                if command in ("cmd_send_get_device_info", "cmd_send_get_call_device"):
                    continue

                # Accept discovery replies from indoor units
                if command == "cmd_reply_get_device_info":
                    reported_ip = str(msg.get("localIp") or msg.get("ip") or "").strip()
                    unit_ip = reported_ip if _is_usable_ipv4(reported_ip) else addr[0]
                    unit = IndoorUnit(
                        ip=unit_ip,
                        udp_ip=addr[0],
                        reported_ip=reported_ip,
                        alias=msg.get("alias", "Unknown"),
                        serial=msg.get("serial", ""),
                        dst_addr=msg.get("dstAddr", ""),
                        raw=msg,
                    )
                    with self.units_lock:
                        is_new = unit.ip not in self.discovered_units
                        self.discovered_units[unit.ip] = unit

                    if is_new:
                        if unit.reported_ip and unit.reported_ip != unit.udp_ip:
                            LOG.info(
                                "NEW indoor unit found: %s (%s) at %s (UDP source %s)",
                                unit.alias,
                                unit.serial,
                                unit.ip,
                                unit.udp_ip,
                            )
                        else:
                            LOG.info("NEW indoor unit found: %s (%s) at %s", unit.alias, unit.serial, unit.ip)
                    else:
                        LOG.debug("Indoor unit refreshed: %s at %s", unit.alias, unit.ip)

                    for callback in self.on_unit_discovered:
                        try:
                            callback(unit)
                        except Exception:
                            pass
                else:
                    LOG.debug("Ignoring unknown command '%s' from %s", command, addr[0])

        except OSError as exc:
            LOG.error("Discovery listener failed to start: %s", exc)
        finally:
            if sock:
                sock.close()


def _is_usable_ipv4(value: str) -> bool:
    try:
        ip = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        return False
    return not (ip.is_loopback or ip.is_link_local or ip.is_multicast or ip.is_unspecified)
