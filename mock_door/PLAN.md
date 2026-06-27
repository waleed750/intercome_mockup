# Mock Door Intercom Service — Implementation Plan

Build a Python service that simulates a smart home door intercom station for testing the Android intercom app. The service runs on a Windows laptop and acts as the "door" side of the intercom protocol. Place all files under `mock_door/` at the project root.

---

## Protocol Specification (reverse-engineered from the Android app)

### 1. Discovery (UDP port 8089)

- The door (this Python service) periodically broadcasts a UDP discovery probe to the LAN broadcast address on port **8089**.
- The probe is a JSON object like:

```json
{"command":"cmd_send_get_device_info","localAddr":"door-001","localType":3}
```

- The Android app listens on UDP 8089 and replies with its own JSON identity. We don't need to parse the reply — just send the probe so the app knows we exist.
- Send the probe every 3 seconds.

### 2. Call Connection (TCP port 8189)

- When the user presses a "Call" button in the Python service's terminal, the service connects via TCP to the Android app's IP address on port **8189**.
- All data over TCP uses a multiplexed binary frame protocol:
  - Frame format: `[4-byte magic][4-byte little-endian payload length][payload]`
  - Channel markers (magic = marker byte repeated 4 times):
    - `0xAA` = CONTROL channel (UTF-8 JSON commands)
    - `0xBB` = VIDEO channel (H.264 Annex-B NAL units)
    - `0xCC` = AUDIO channel (G.711 A-law, 8kHz mono, 160 bytes per 20ms frame)

### 3. Control Commands (JSON over CONTROL channel `0xAA`)

**Outbound (door → app):**

- `{"command":"Call"}` — initiate a call (visitor pressed doorbell)
- `{"command":"GetCallInfo"}` — re-query call state
- `{"command":"HangUp"}` — end the call from door side

**Inbound (app → door) — the service should parse and log these:**

- `{"command":"Answer"}` — app accepted the call (may include `"OtherAnswer":1` or `"OtherAnswer":true`)
- `{"command":"HangUp","OtherAnswer":0}` — app ended the call
- `{"command":"OpenDoor"}` — app unlocked the door (just print a log message like "DOOR UNLOCKED!")
- `{"command":"StartTalk"}` — app wants to talk
- `{"command":"deviceBusy"}` — app is busy with another call

### 4. Video Streaming (H.264 over VIDEO channel `0xBB`)

- Capture from the laptop's webcam using OpenCV.
- Encode each frame to H.264 Annex-B format using `ffmpeg` as a subprocess pipe (stdin=raw frames, stdout=H.264 stream).
- FFmpeg command:

```
ffmpeg -f rawvideo -pix_fmt bgr24 -s {width}x{height} -r 15 -i pipe:0 -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline -level 3.1 -b:v 500k -g 30 -f h264 pipe:1
```

- Read H.264 NAL units from ffmpeg stdout and send them as VIDEO frames over TCP.
- Use 15 fps, 640x480 resolution.

### 5. Audio Streaming (G.711 A-law over AUDIO channel `0xCC`)

- Capture from the laptop's microphone using `pyaudio` at 8000 Hz, mono, 16-bit PCM.
- Encode PCM to G.711 A-law (implement the ITU-T G.711 A-law encoder — it's a simple per-sample computation).
- Send 160-byte audio frames (20ms each) over the AUDIO channel.
- Also receive audio from the app on the AUDIO channel, decode A-law to PCM, and play it back through `pyaudio` output stream.

### 6. Interactive Terminal UI

The service should have a simple terminal menu:

```
=== Mock Door Intercom ===
Target IP: <entered at startup or via argument>
[d] Send discovery probe now
[c] Call the indoor unit
[h] Hang up
[q] Quit
```

- On startup, ask for the Android device's IP address (or accept it as a CLI argument).
- Discovery probes run automatically in a background thread.
- When `c` is pressed: connect TCP to the app on port 8189, send a `{"command":"Call"}` control frame, then start streaming video and audio.
- When `h` is pressed: send `{"command":"HangUp"}` and stop streaming.
- When `q` is pressed: clean shutdown.

---

## File Structure

```
mock_door/
├── requirements.txt          # opencv-python, pyaudio, numpy
├── mock_door.py              # Main entry point with terminal UI
├── protocol.py               # Frame encode/decode, channel constants
├── discovery.py              # UDP discovery probe sender
├── video_stream.py           # Webcam capture + ffmpeg H.264 encoding
├── audio_stream.py           # Mic capture, A-law encode/decode, speaker playback
└── run.bat                   # Windows batch: creates venv in mock_door\.venv, installs deps, runs
```

---

## Important Implementation Requirements

### 1. Virtual Environment

`run.bat` must create a `.venv` inside the `mock_door/` directory (not globally), install requirements into it, and run the script using that venv's Python. Example:

```bat
@echo off
cd /d "%~dp0"
if not exist .venv (
    echo Creating virtual environment...
    python -m venv .venv
)
call .venv\Scripts\activate.bat
pip install -r requirements.txt --quiet
python mock_door.py %*
```

### 2. Threading Model

Use Python `threading` module. The main thread handles the terminal menu. Background threads handle:

- Discovery probes (UDP broadcast every 3s)
- TCP read loop (parse incoming frames from app)
- Video capture + send (webcam → ffmpeg → TCP)
- Audio capture + send (mic → A-law encode → TCP)
- Audio playback (TCP → A-law decode → speaker)

### 3. Frame Parser

Implement the same streaming parser logic as the Android app — handle:

- **Split frames**: a frame spanning multiple TCP reads is buffered until complete.
- **Merged frames**: multiple frames in one read are all emitted.
- **Desync recovery**: if the read head is not on a valid magic (e.g. after a dropped byte), advance one byte at a time until re-locked onto the next known marker.

```python
class FrameParser:
    MAGIC_SIZE = 4
    LENGTH_SIZE = 4
    HEADER_SIZE = MAGIC_SIZE + LENGTH_SIZE
    MAX_PAYLOAD = 4 * 1024 * 1024

    MARKERS = {
        0xAA: "CONTROL",
        0xBB: "VIDEO",
        0xCC: "AUDIO",
    }

    def __init__(self, on_frame):
        """on_frame(channel: str, payload: bytes)"""
        self.on_frame = on_frame
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf.extend(data)
        self._drain()

    def _drain(self):
        pos = 0
        while True:
            if len(self.buf) - pos < self.MAGIC_SIZE:
                break
            marker = self.buf[pos]
            channel = self.MARKERS.get(marker)
            if channel is None or not all(self.buf[pos + i] == marker for i in range(1, 4)):
                pos += 1  # desync — skip one byte
                continue
            if len(self.buf) - pos < self.HEADER_SIZE:
                break
            length = int.from_bytes(self.buf[pos+4:pos+8], 'little')
            if length < 0 or length > self.MAX_PAYLOAD:
                pos += 1
                continue
            frame_end = pos + self.HEADER_SIZE + length
            if frame_end > len(self.buf):
                break
            payload = bytes(self.buf[pos + self.HEADER_SIZE : frame_end])
            self.on_frame(channel, payload)
            pos = frame_end
        self.buf = self.buf[pos:]
```

### 4. Frame Encoder

```python
def encode_frame(channel_marker: int, payload: bytes) -> bytes:
    magic = bytes([channel_marker]) * 4
    length = len(payload).to_bytes(4, 'little')
    return magic + length + payload

CONTROL = 0xAA
VIDEO   = 0xBB
AUDIO   = 0xCC
```

### 5. G.711 A-Law Codec

```python
SEG_END = [0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF]

def linear_to_alaw(sample: int) -> int:
    """Encode a 16-bit signed PCM sample to 8-bit A-law."""
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
    """Decode an 8-bit A-law sample to 16-bit signed PCM."""
    a = alaw_byte ^ 0x55
    t = (a & 0x0F) << 4
    seg = (a & 0x70) >> 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t += 0x108
        t <<= (seg - 1)
    return t if (a & 0x80) else -t

def pcm_to_alaw(pcm_bytes: bytes) -> bytes:
    """Encode LE PCM16 bytes to A-law bytes."""
    import struct
    samples = struct.unpack(f'<{len(pcm_bytes)//2}h', pcm_bytes)
    return bytes(linear_to_alaw(s) for s in samples)

def alaw_to_pcm(alaw_bytes: bytes) -> bytes:
    """Decode A-law bytes to LE PCM16 bytes."""
    import struct
    samples = [alaw_to_linear(b) for b in alaw_bytes]
    return struct.pack(f'<{len(samples)}h', *samples)
```

### 6. Clean Shutdown

All threads must be daemon threads or properly joined on quit. Close sockets, stop ffmpeg subprocess, stop pyaudio gracefully. Use a `threading.Event` called `stop_event` that all threads check.

### 7. Error Handling

- If webcam is unavailable, log a warning and continue without video (don't crash).
- If microphone is unavailable, log a warning and continue without audio (don't crash).
- If ffmpeg is not on PATH, log an error with install instructions and skip video.
- If TCP connection fails, log the error and return to the menu.

### 8. Windows Compatibility

- This runs on Windows 11.
- Use `msvcrt.getch()` for non-blocking key input instead of Unix `select`/`termios`.
- Use `socket.SO_BROADCAST` for UDP discovery.

### 9. Logging

Use Python `logging` module with timestamps. Log all control commands sent and received. Example:

```
2024-01-15 10:30:01 [DISCOVERY] Sent probe to 192.168.1.255:8089
2024-01-15 10:30:04 [CONTROL] >>> Call
2024-01-15 10:30:07 [CONTROL] <<< Answer
2024-01-15 10:30:07 [CONTROL] <<< Answer (OtherAnswer=1)
2024-01-15 10:30:15 [CONTROL] <<< OpenDoor
2024-01-15 10:30:15 [DOOR] DOOR UNLOCKED!
```

### 10. FFmpeg Dependency

Assume `ffmpeg` is available on PATH. Add a check at startup — if not found, print:

```
WARNING: ffmpeg not found on PATH. Video streaming disabled.
Install from https://www.gyan.dev/ffmpeg/builds/ and add to PATH.
```

---

## Call Flow Summary

```
1. Service starts → discovery probes begin (UDP broadcast every 3s)
2. User presses [c] → TCP connect to app_ip:8189
3. Send CONTROL frame: {"command":"Call"}
4. Start video thread (webcam → ffmpeg → H.264 → VIDEO frames)
5. Start audio capture thread (mic → A-law → AUDIO frames)
6. TCP read loop receives:
   - CONTROL: Answer → log "Call accepted", continue streaming
   - CONTROL: OpenDoor → log "DOOR UNLOCKED!"
   - CONTROL: HangUp → stop streaming, close TCP
   - AUDIO: A-law data → decode → play through speaker
7. User presses [h] → send HangUp, stop streaming, close TCP
8. User presses [q] → clean shutdown everything
```

---

## requirements.txt

```
opencv-python>=4.8
pyaudio>=0.2.13
numpy>=1.24
```

---

## Notes

- The Android app listens as a TCP **server** on port 8189. The door (this service) is the TCP **client** that connects to it.
- The Android app listens on UDP 8089 for discovery. The door **broadcasts** probes.
- Video frames from ffmpeg come as a continuous H.264 bytestream. Read chunks (e.g. 4096 bytes) from ffmpeg stdout and send each chunk as one VIDEO frame. The app's `FrameParser` and `VideoPipeline` handle reassembly and NAL unit boundary detection internally.
- Audio frames must be exactly 160 bytes (20ms at 8kHz) per AUDIO frame to match the app's expectation.
- The app binds to its Wi-Fi interface IP. Both devices must be on the same Wi-Fi network.
