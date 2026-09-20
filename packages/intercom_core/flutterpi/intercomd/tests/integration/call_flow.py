#!/usr/bin/env python3
"""
End-to-end call flow against the door station simulator.

Stands in for the panel and checks the behaviours that the protocol document
says are load-bearing, and that a unit test cannot reach because they are about
timing and socket behaviour rather than pure functions:

  1. The door streams nothing until it has both the three-frame Answer
     handshake AND a StartTalk.
  2. Once streaming, audio arrives at the 20 ms cadence (~50 frames a second).
  3. The door gates its downlink when the panel's uplink goes quiet. This is
     why the panel must keep sending silence frames rather than nothing.
  4. The downlink resumes once the uplink does.
  5. OpenDoor and HangUp are understood.

Usage: call_flow.py [path-to-door-sim]
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

def free_port() -> int:
    """Ask the OS for an unused port so the test never fights a real deployment."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]
SILENCE_FRAME = bytes([0xD5]) * 160


def frame(marker: int, payload: bytes) -> bytes:
    return bytes([marker] * 4) + struct.pack("<I", len(payload)) + payload


class Panel:
    """Minimal panel: connects, speaks the protocol, counts what comes back."""

    def __init__(self, port, host="127.0.0.1"):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.audio = 0
        self.video = 0
        self.control = []
        self._stop = False
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        buf = b""
        while not self._stop:
            try:
                data = self.sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while len(buf) >= 8:
                m = buf[0]
                if not (buf[1] == m and buf[2] == m and buf[3] == m
                        and m in (0xAA, 0xBB, 0xCC)):
                    buf = buf[1:]
                    continue
                n = struct.unpack("<I", buf[4:8])[0]
                if len(buf) < 8 + n:
                    break
                payload, buf = buf[8:8 + n], buf[8 + n:]
                if m == 0xCC:
                    self.audio += 1
                elif m == 0xBB:
                    self.video += 1
                else:
                    self.control.append(payload.decode("utf-8", "replace"))

    def send_control(self, payload: bytes):
        self.sock.sendall(frame(0xAA, payload))

    def send_answer_handshake(self):
        for p in (b'{"command":"Answer"}',
                  b'{"command":"Answer","OtherAnswer":1}',
                  b'{"command":"Answer","OtherAnswer":true}'):
            self.send_control(p)

    def stream_audio(self, seconds: float):
        """Send silence frames at the 20 ms cadence, as the real panel must."""
        end = time.monotonic() + seconds
        nxt = time.monotonic()
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= nxt:
                self.sock.sendall(frame(0xCC, SILENCE_FRAME))
                nxt += 0.02
            else:
                time.sleep(0.002)

    def close(self):
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass


failures = []


def check(name: str, ok: bool, detail: str = ""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def main() -> int:
    sim_path = sys.argv[1] if len(sys.argv) > 1 else "./build/door-sim"
    if not os.path.exists(sim_path):
        print(f"door-sim not found at {sim_path}", file=sys.stderr)
        return 2

    log = open("door-sim.log", "w")
    port = free_port()
    sim = subprocess.Popen([sim_path, "--listen", "--port", str(port)],
                           stdout=log, stderr=log)
    try:
        time.sleep(0.5)
        if sim.poll() is not None:
            print(f"simulator exited immediately — could not bind port {port}",
                  file=sys.stderr)
            return 2

        panel = Panel(port)

        print("Answer handshake alone must not start the stream")
        panel.send_answer_handshake()
        panel.stream_audio(1.0)
        after_answer = panel.audio
        check("silent until StartTalk", after_answer == 0,
              f"got {after_answer} audio frames, expected 0")

        print("StartTalk starts the stream")
        panel.send_control(b'{"command":"StartTalk"}')
        panel.stream_audio(1.0)
        streamed = panel.audio - after_answer
        check("audio arrives at the 20 ms cadence", 40 <= streamed <= 60,
              f"{streamed} frames in 1 s, expected ~50")

        print("A quiet uplink gates the downlink")
        time.sleep(1.2)                    # past the door's grace period
        mark = panel.audio
        time.sleep(1.0)
        while_quiet = panel.audio - mark
        check("downlink gated while uplink is silent", while_quiet == 0,
              f"got {while_quiet} frames, expected 0")

        print("Resuming the uplink brings the downlink back")
        panel.stream_audio(1.0)
        resumed = panel.audio - mark - while_quiet
        check("downlink resumes", resumed >= 30, f"{resumed} frames")

        print("Control commands")
        panel.send_control(b'{"command":"OpenDoor"}')
        time.sleep(0.3)
        panel.send_control(b'{"command":"HangUp","OtherAnswer":0}')
        time.sleep(0.5)
        panel.close()

        log.flush()
        text = open("door-sim.log").read()
        check("OpenDoor recognised", "DOOR UNLOCKED" in text)
        check("HangUp recognised", "panel hung up" in text)
        check("no frame parser resyncs", "resync=0" in text or "resync" not in text)

    finally:
        sim.terminate()
        try:
            sim.wait(timeout=3)
        except subprocess.TimeoutExpired:
            sim.kill()
        log.close()

    print()
    if failures:
        print(f"{len(failures)} failed: {', '.join(failures)}")
        print("--- simulator log ---")
        print(open("door-sim.log").read())
        return 1
    print("all integration checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
