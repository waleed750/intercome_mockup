"""Binary frame protocol used by the Android intercom app."""

from __future__ import annotations

CONTROL = 0xAA
VIDEO = 0xBB
AUDIO = 0xCC

CHANNEL_NAMES = {
    CONTROL: "CONTROL",
    VIDEO: "VIDEO",
    AUDIO: "AUDIO",
}


def encode_frame(channel_marker: int, payload: bytes) -> bytes:
    if channel_marker not in CHANNEL_NAMES:
        raise ValueError(f"unknown channel marker: {channel_marker!r}")
    magic = bytes([channel_marker]) * 4
    length = len(payload).to_bytes(4, "little")
    return magic + length + payload


class FrameParser:
    MAGIC_SIZE = 4
    LENGTH_SIZE = 4
    HEADER_SIZE = MAGIC_SIZE + LENGTH_SIZE
    MAX_PAYLOAD = 4 * 1024 * 1024

    MARKERS = CHANNEL_NAMES

    def __init__(self, on_frame):
        """Create a parser that calls on_frame(channel: str, payload: bytes)."""
        self.on_frame = on_frame
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf.extend(data)
        self._drain()

    def reset(self):
        self.buf.clear()

    def _drain(self):
        pos = 0
        while True:
            if len(self.buf) - pos < self.MAGIC_SIZE:
                break
            marker = self.buf[pos]
            channel = self.MARKERS.get(marker)
            if channel is None or not all(self.buf[pos + i] == marker for i in range(1, 4)):
                pos += 1
                continue
            if len(self.buf) - pos < self.HEADER_SIZE:
                break
            length = int.from_bytes(self.buf[pos + 4 : pos + 8], "little")
            if length > self.MAX_PAYLOAD:
                pos += 1
                continue
            frame_end = pos + self.HEADER_SIZE + length
            if frame_end > len(self.buf):
                break
            payload = bytes(self.buf[pos + self.HEADER_SIZE : frame_end])
            self.on_frame(channel, payload)
            pos = frame_end
        self.buf = self.buf[pos:]
