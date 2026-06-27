"""Microphone capture, G.711 A-law conversion, and speaker playback using sounddevice."""

from __future__ import annotations

import logging
import queue
import struct
import threading

from protocol import AUDIO, encode_frame

LOG = logging.getLogger("AUDIO")

SAMPLE_RATE = 8000
CHANNELS = 1
SAMPLES_PER_FRAME = 160
PCM_FRAME_BYTES = SAMPLES_PER_FRAME * 2
ALAW_FRAME_BYTES = 160

SEG_END = [0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF]


def linear_to_alaw(sample: int) -> int:
    if sample >= 0:
        mask = 0xD5
    else:
        mask = 0x55
        sample = -sample - 1
    sample = sample >> 3
    if sample > 0xFFF:
        sample = 0xFFF
    seg = 0
    for threshold in SEG_END:
        if sample <= threshold:
            break
        seg += 1
    if seg >= 8:
        return 0x7F ^ mask
    if seg < 2:
        val = (seg << 4) | ((sample >> 1) & 0x0F)
    else:
        val = (seg << 4) | ((sample >> seg) & 0x0F)
    return val ^ mask


def alaw_to_linear(alaw_byte: int) -> int:
    a = alaw_byte ^ 0x55
    t = (a & 0x0F) << 4
    seg = (a & 0x70) >> 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t += 0x108
        t <<= seg - 1
    return t if (a & 0x80) else -t


def pcm_to_alaw(pcm_bytes: bytes) -> bytes:
    if len(pcm_bytes) % 2:
        pcm_bytes = pcm_bytes[:-1]
    samples = struct.unpack(f"<{len(pcm_bytes) // 2}h", pcm_bytes)
    return bytes(linear_to_alaw(s) for s in samples)


def alaw_to_pcm(alaw_bytes: bytes) -> bytes:
    samples = [alaw_to_linear(b) for b in alaw_bytes]
    return struct.pack(f"<{len(samples)}h", *samples)


class AudioStreamer:
    def __init__(self, send_bytes, call_stop_event: threading.Event):
        self.send_bytes = send_bytes
        self.call_stop_event = call_stop_event
        self.playback_queue: queue.Queue[bytes] = queue.Queue(maxsize=80)
        self.threads: list[threading.Thread] = []

    def start(self):
        self.threads = [
            threading.Thread(target=self._capture_loop, name="audio-capture", daemon=True),
            threading.Thread(target=self._playback_loop, name="audio-playback", daemon=True),
        ]
        for thread in self.threads:
            thread.start()

    def stop(self):
        self._drain_playback_queue()

    def enqueue_playback(self, alaw_payload: bytes):
        if self.call_stop_event.is_set():
            return
        try:
            self.playback_queue.put_nowait(alaw_payload)
        except queue.Full:
            pass

    def _capture_loop(self):
        try:
            import sounddevice as sd
            import numpy as np
        except ImportError:
            LOG.warning("sounddevice not installed. Microphone disabled.")
            return

        try:
            LOG.info("Microphone streaming started")
            with sd.RawInputStream(
                samplerate=SAMPLE_RATE,
                channels=CHANNELS,
                dtype="int16",
                blocksize=SAMPLES_PER_FRAME,
            ) as stream:
                while not self.call_stop_event.is_set():
                    data, overflowed = stream.read(SAMPLES_PER_FRAME)
                    pcm = bytes(data)
                    alaw = pcm_to_alaw(pcm)
                    if len(alaw) == ALAW_FRAME_BYTES:
                        self.send_bytes(encode_frame(AUDIO, alaw))
        except Exception as exc:
            LOG.warning("Microphone unavailable: %s", exc)

    def _playback_loop(self):
        try:
            import sounddevice as sd
        except ImportError:
            LOG.warning("sounddevice not installed. Speaker disabled.")
            return

        try:
            LOG.info("Speaker playback started")
            with sd.RawOutputStream(
                samplerate=SAMPLE_RATE,
                channels=CHANNELS,
                dtype="int16",
                blocksize=SAMPLES_PER_FRAME,
            ) as stream:
                while not self.call_stop_event.is_set():
                    try:
                        alaw = self.playback_queue.get(timeout=0.1)
                    except queue.Empty:
                        continue
                    pcm = alaw_to_pcm(alaw)
                    stream.write(pcm)
        except Exception as exc:
            LOG.warning("Speaker unavailable: %s", exc)

    def _drain_playback_queue(self):
        while True:
            try:
                self.playback_queue.get_nowait()
            except queue.Empty:
                return
