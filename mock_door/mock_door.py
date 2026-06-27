from __future__ import annotations

import argparse
import json
import logging
import msvcrt
import socket
import threading
import time

from audio_stream import AudioStreamer
from discovery import DiscoveryBroadcaster
from protocol import AUDIO, CONTROL, FrameParser, encode_frame
from video_stream import VideoStreamer, ffmpeg_available, ffmpeg_warning

TCP_PORT = 8189

LOG = logging.getLogger("DOOR")
CONTROL_LOG = logging.getLogger("CONTROL")


class MockDoorService:
    def __init__(self, target_ip: str = ""):
        self.target_ip = target_ip
        self.stop_event = threading.Event()
        self.call_stop_event = threading.Event()
        self.discovery = DiscoveryBroadcaster(self.stop_event)
        self.sock: socket.socket | None = None
        self.sock_lock = threading.Lock()
        self.call_lock = threading.Lock()  # serializes call/hangup
        self.read_thread: threading.Thread | None = None
        self.video: VideoStreamer | None = None
        self.audio: AudioStreamer | None = None

    def start(self):
        self.discovery.start()

    def shutdown(self):
        self.stop_event.set()
        self.hang_up(send_command=True)

    def get_discovered_units(self):
        return self.discovery.get_units()

    def call(self, target_ip: str = ""):
        with self.call_lock:
            self._do_call(target_ip)

    def _do_call(self, target_ip: str = ""):
        if target_ip:
            self.target_ip = target_ip

        # Auto-detect: if no IP specified, use first discovered unit
        if not self.target_ip:
            units = self.discovery.get_units()
            if units:
                self.target_ip = units[0].ip
                LOG.info("Auto-detected indoor unit at %s (%s)", self.target_ip, units[0].alias)
            else:
                LOG.error("No indoor units discovered yet. Waiting for discovery replies...")
                return

        if self.sock is not None:
            LOG.info("Call already active")
            return

        # Clean up any leftover state from a previous call
        self._cleanup()

        try:
            LOG.info("Connecting to %s:%s ...", self.target_ip, TCP_PORT)
            sock = socket.create_connection((self.target_ip, TCP_PORT), timeout=10)
            sock.settimeout(0.5)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            import struct as _struct
            # SO_LINGER: graceful close, wait up to 5s for pending data
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, _struct.pack('hh', 1, 5))
        except OSError as exc:
            LOG.error("TCP connection to %s:%s failed: %s", self.target_ip, TCP_PORT, exc)
            return

        self.call_stop_event = threading.Event()
        self.sock = sock
        self.media_started = False

        self.read_thread = threading.Thread(target=self._read_loop, name="tcp-read", daemon=True)
        self.read_thread.start()

        self._send_control("Call")
        LOG.info("Call started to %s (waiting for Answer before starting media)", self.target_ip)

    def hang_up(self, send_command: bool = True):
        with self.call_lock:
            self._do_hang_up(send_command)

    def _do_hang_up(self, send_command: bool = True):
        # Grab and clear the socket in one lock acquisition
        with self.sock_lock:
            sock = self.sock
            self.sock = None

        if sock is None:
            return

        # Send HangUp before closing
        if send_command:
            try:
                payload = json.dumps({"command": "HangUp"}, separators=(",", ":")).encode("utf-8")
                sock.sendall(encode_frame(CONTROL, payload))
                CONTROL_LOG.info(">>> HangUp")
            except OSError:
                pass

        # Signal all threads to stop
        self.call_stop_event.set()

        # Close socket (unblocks the read loop)
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

        # Stop media
        v, a = self.video, self.audio
        self.video = None
        self.audio = None
        if v is not None:
            try:
                v.stop()
            except Exception:
                pass
        if a is not None:
            try:
                a.stop()
            except Exception:
                pass

        # Wait for read thread
        rt = self.read_thread
        self.read_thread = None
        if rt is not None:
            rt.join(timeout=3)

        LOG.info("Call closed")

    def _cleanup(self):
        """Reset all call state so a fresh call can start cleanly."""
        self.call_stop_event.set()

        v, a = self.video, self.audio
        self.video = None
        self.audio = None
        if v:
            try:
                v.stop()
            except Exception:
                pass
        if a:
            try:
                a.stop()
            except Exception:
                pass

        with self.sock_lock:
            sock = self.sock
            self.sock = None
        if sock:
            try:
                sock.close()
            except OSError:
                pass

        rt = self.read_thread
        self.read_thread = None
        if rt:
            rt.join(timeout=2)

    def send_command(self, command: str, **extra):
        """Send any control command to the app."""
        if self.sock is None:
            LOG.warning("Cannot send '%s': no active call", command)
            return
        self._send_control(command, **extra)

    def send_discovery_now(self):
        try:
            self.discovery.send_once()
        except OSError as exc:
            logging.getLogger("DISCOVERY").warning("Discovery probe failed: %s", exc)

    def _send_control(self, command: str, **extra):
        payload_obj = {"command": command, **extra}
        payload = json.dumps(payload_obj, separators=(",", ":")).encode("utf-8")
        self._send_raw(encode_frame(CONTROL, payload))
        CONTROL_LOG.info(">>> %s", command)

    def _send_raw(self, data: bytes):
        with self.sock_lock:
            sock = self.sock
            if sock is None:
                return
            try:
                sock.sendall(data)
            except OSError as exc:
                LOG.debug("Socket send failed: %s", exc)

    def _read_loop(self):
        LOG.debug("Read loop started")
        parser = FrameParser(self._on_frame)
        while not self.call_stop_event.is_set():
            with self.sock_lock:
                sock = self.sock
            if sock is None:
                LOG.debug("Read loop: sock is None, exiting")
                return
            try:
                data = sock.recv(8192)
            except socket.timeout:
                continue
            except OSError as exc:
                LOG.warning("Read loop socket error: %s", exc)
                break
            if not data:
                LOG.debug("Read loop: recv returned empty (remote closed)")
                break
            LOG.debug("Read loop: received %d bytes", len(data))
            parser.feed(data)
        if not self.call_stop_event.is_set():
            LOG.info("Remote closed TCP connection")
            self.call_stop_event.set()
        LOG.debug("Read loop ended")

    def _on_frame(self, channel: str, payload: bytes):
        if channel == "CONTROL":
            self._handle_control(payload)
        elif channel == "AUDIO" and self.audio is not None:
            self.audio.enqueue_playback(payload)

    def _handle_control(self, payload: bytes):
        try:
            message = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            CONTROL_LOG.warning("<<< invalid JSON: %s", exc)
            return

        command = message.get("command", "<missing>")
        other_answer = message.get("OtherAnswer")
        if other_answer is None:
            CONTROL_LOG.info("<<< %s", command)
        else:
            CONTROL_LOG.info("<<< %s (OtherAnswer=%s)", command, int(bool(other_answer)) if isinstance(other_answer, bool) else other_answer)

        if command == "Answer":
            LOG.info("Call accepted")
            self._start_media()
        elif command == "OpenDoor":
            LOG.info("DOOR UNLOCKED!")
        elif command == "HangUp":
            LOG.info("Remote hung up")
            # Don't call hang_up from read thread — it deadlocks on call_lock.
            # Just signal stop; the socket close will end the read loop naturally.
            self.call_stop_event.set()
        elif command == "StartTalk":
            LOG.info("Talk started by indoor unit")
        elif command == "deviceBusy":
            LOG.info("Indoor unit is busy")

    def _start_media(self):
        """Start video/audio streams after the app answers."""
        if self.media_started:
            return
        self.media_started = True
        # Run in a separate thread so it never blocks the read loop
        threading.Thread(target=self._do_start_media, daemon=True, name="media-init").start()

    def _do_start_media(self):
        try:
            self.video = VideoStreamer(self._send_raw, self.call_stop_event)
            self.video.start()
            LOG.info("Video stream started")
        except Exception as exc:
            LOG.error("Failed to start video: %s", exc)
        try:
            self.audio = AudioStreamer(self._send_raw, self.call_stop_event)
            self.audio.start()
            LOG.info("Audio stream started")
        except Exception as exc:
            LOG.error("Failed to start audio: %s", exc)


def configure_logging():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def read_key() -> str | None:
    if not msvcrt.kbhit():
        return None
    raw = msvcrt.getch()
    try:
        return raw.decode("utf-8").lower()
    except UnicodeDecodeError:
        return None


def print_menu(target_ip: str):
    print()
    print("=== Mock Door Intercom ===")
    print(f"Target IP: {target_ip}")
    print("[d] Send discovery probe now")
    print("[c] Call the indoor unit")
    print("[h] Hang up")
    print("[q] Quit")
    print("> ", end="", flush=True)


def parse_args():
    parser = argparse.ArgumentParser(description="Mock smart home door intercom station")
    parser.add_argument("target_ip", nargs="?", help="Android device IP address")
    parser.add_argument("--cli", action="store_true", help="Use terminal UI instead of GUI")
    return parser.parse_args()


def main_cli(target_ip: str):
    if not ffmpeg_available():
        print(ffmpeg_warning())

    service = MockDoorService(target_ip)
    service.start()
    print_menu(target_ip)
    try:
        while not service.stop_event.is_set():
            key = read_key()
            if key is None:
                time.sleep(0.05)
                continue
            print(key)
            if key == "d":
                service.send_discovery_now()
            elif key == "c":
                service.call()
            elif key == "h":
                service.hang_up(send_command=True)
            elif key == "q":
                break
            print_menu(target_ip)
    except KeyboardInterrupt:
        print()
    finally:
        service.shutdown()


def main_gui():
    from gui import MockDoorGUI
    app = MockDoorGUI(service_factory=lambda ip: MockDoorService(ip))
    app.run()


def main():
    configure_logging()
    args = parse_args()

    if args.cli:
        target_ip = args.target_ip or input("Android device IP address: ").strip()
        if not target_ip:
            raise SystemExit("Target IP is required")
        main_cli(target_ip)
    else:
        main_gui()


if __name__ == "__main__":
    main()
