#!/usr/bin/env python3
"""
Impersonate the panel and play a loud tone at the door station.

Purpose: decide whether the daemon is involved at all in "the door does not
play what the panel sends".

A packet capture already shows the panel transmitting clear speech at the
door's own network port — 211 distinct A-law values, peaks to 15616, levels
around -25 dBFS. So either the door is not playing what it receives, or it is
not accepting us as an indoor unit it will play audio from.

This script removes the daemon from the question entirely. It speaks the wire
protocol directly, in about a hundred lines, and sends a tone loud enough that
nobody could mistake it for a level problem.

  ./panel_tone.py 192.168.100.193

  --seconds N     how long to talk for (default 45). Longer than the
                  25-35 s at which this door has hung up on its own in
                  both recorded calls, so that hangup lands inside the
                  run and is timestamped rather than coinciding with
                  the end of it.
  --hz N          tone frequency (default 1000)
  --level N       0.0 to 1.0, default 0.9 — deliberately loud
  --answer-getcallinfo
                  also answer getCallInfo the moment it arrives, rather than
                  only once connected. The protocol document says the latter;
                  it has already been wrong once about this door.

IMPORTANT: leave the daemon RUNNING while this script runs.

The first time this was used the daemon was stopped, which also stopped the
panel answering the door's discovery on UDP 8089. If the door drops an indoor
unit from its directory when it stops responding, the tone was calling as an
unregistered peer and the silence said nothing about the tone itself. This
script only dials outbound to the door's 8189, so it does not contend with the
daemon's listener at all and the two coexist happily.

If the door stays silent for this, the daemon is ruled out and the fault is the
door's configuration or how it has registered us.
"""
import argparse
import math
import socket
import struct
import sys
import threading
import time

CALL_PORT = 8189
FRAME_SAMPLES = 160          # 20 ms at 8 kHz
FRAME_MS = 0.020

ANSWER_SEQUENCE = [
    b'{"command":"Answer"}',
    b'{"command":"Answer","OtherAnswer":1}',
    b'{"command":"Answer","OtherAnswer":true}',
]
START_TALK = b'{"command":"StartTalk"}'
HANG_UP = b'{"command":"HangUp","OtherAnswer":0}'


def frame(marker: int, payload: bytes) -> bytes:
    return bytes([marker] * 4) + struct.pack("<I", len(payload)) + payload


def linear_to_alaw(pcm: int) -> int:
    """ITU-T G.711 A-law, same as the daemon's encoder."""
    value = pcm >> 3
    if value >= 0:
        mask = 0xD5
    else:
        mask = 0x55
        value = -value - 1

    seg_end = (0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF)
    seg = next((i for i, end in enumerate(seg_end) if value <= end), 8)
    if seg >= 8:
        return 0x7F ^ mask

    out = seg << 4
    out |= (value >> 1) & 0x0F if seg < 2 else (value >> seg) & 0x0F
    return out ^ mask


def tone_frames(hz: float, level: float, count: int):
    """A continuous tone, as A-law frames, phase-correct across frames."""
    phase = 0.0
    step = 2.0 * math.pi * hz / 8000.0
    for _ in range(count):
        samples = bytearray()
        for _ in range(FRAME_SAMPLES):
            samples.append(linear_to_alaw(int(math.sin(phase) * level * 32767)))
            phase += step
            if phase > 2 * math.pi:
                phase -= 2 * math.pi
        yield bytes(samples)


def reader(sock, state, answer_early):
    """Log everything the door says, and optionally answer getCallInfo early."""
    buf = b""
    while not state["stop"]:
        try:
            data = sock.recv(65536)
        except OSError:
            break
        if not data:
            print("  door closed the connection")
            state["stop"] = True
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

            if m == 0xAA:
                text = payload.decode("utf-8", "replace")
                print(f"  [{time.time() - state['t0']:6.2f}] <- {text}")
                lowered = text.lower()
                if "getcallinfo" in lowered and answer_early:
                    print("           -> answering getCallInfo immediately")
                    for cmd in ANSWER_SEQUENCE:
                        sock.sendall(frame(0xAA, cmd))
                if "hangup" in lowered:
                    state["stop"] = True
            elif m == 0xCC:
                state["audio_in"] += 1
            elif m == 0xBB:
                state["video_in"] += 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("door", help="door station address, e.g. 192.168.100.193")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--hz", type=float, default=1000.0)
    ap.add_argument("--level", type=float, default=0.9)
    ap.add_argument("--answer-getcallinfo", action="store_true")
    args = ap.parse_args()

    print(f"dialling {args.door}:{CALL_PORT}")
    sock = socket.create_connection((args.door, CALL_PORT), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    state = {"stop": False, "t0": time.time(), "audio_in": 0, "video_in": 0}
    threading.Thread(target=reader, args=(sock, state, args.answer_getcallinfo),
                     daemon=True).start()

    print("sending the Answer handshake (all three spellings)")
    for cmd in ANSWER_SEQUENCE:
        sock.sendall(frame(0xAA, cmd))

    # One second, exactly as the daemon does. The door streams nothing until it
    # has both the handshake and a StartTalk; the delay was established by
    # capture against real hardware.
    time.sleep(1.0)
    print("sending StartTalk")
    sock.sendall(frame(0xAA, START_TALK))

    total = int(args.seconds / FRAME_MS)
    print(f"playing a {args.hz:.0f} Hz tone at level {args.level} "
          f"for {args.seconds}s ({total} frames)")
    print("LISTEN AT THE DOOR NOW")

    next_at = time.monotonic()
    sent = 0
    for payload in tone_frames(args.hz, args.level, total):
        if state["stop"]:
            break
        sock.sendall(frame(0xCC, payload))
        sent += 1
        next_at += FRAME_MS
        delay = next_at - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        else:
            next_at = time.monotonic()

    print(f"\nsent {sent} audio frames at 50/s")
    print(f"received from the door: {state['audio_in']} audio, "
          f"{state['video_in']} video frames")

    if not state["stop"]:
        sock.sendall(frame(0xAA, HANG_UP))
    state["stop"] = True
    sock.close()

    print("\nIf nothing was heard at the door, the daemon is ruled out: this")
    print("script shares no code with it. The fault is then the door's own")
    print("configuration, or how it has registered this panel.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
