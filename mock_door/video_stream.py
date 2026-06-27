"""Webcam capture and ffmpeg H.264 streaming with NAL unit framing."""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import threading
import time

from protocol import VIDEO, encode_frame

LOG = logging.getLogger("VIDEO")

WIDTH = 1920
HEIGHT = 1080
FPS = 24

# H.264 Annex-B start codes
START_CODE_3 = b'\x00\x00\x01'
START_CODE_4 = b'\x00\x00\x00\x01'


def _find_ffmpeg() -> str | None:
    path = shutil.which("ffmpeg")
    if path:
        return path
    try:
        import imageio_ffmpeg
        path = imageio_ffmpeg.get_ffmpeg_exe()
        if path and os.path.isfile(path):
            return path
    except ImportError:
        pass
    return None


def ffmpeg_available() -> bool:
    return _find_ffmpeg() is not None


def ffmpeg_warning() -> str:
    return (
        "WARNING: ffmpeg not found. Video streaming disabled.\n"
        "Run: pip install imageio-ffmpeg   (or install ffmpeg and add to PATH)"
    )


def _split_nalus(data: bytes) -> list[bytes]:
    """Split H.264 Annex-B bytestream into individual NAL units (each with start code)."""
    nalus = []
    # Find all start code positions
    positions = []
    i = 0
    while i < len(data) - 3:
        if data[i:i+4] == START_CODE_4:
            positions.append(i)
            i += 4
        elif data[i:i+3] == START_CODE_3:
            positions.append(i)
            i += 3
        else:
            i += 1

    if not positions:
        return [data] if data else []

    for j in range(len(positions)):
        start = positions[j]
        end = positions[j + 1] if j + 1 < len(positions) else len(data)
        nalu = data[start:end]
        if nalu:
            nalus.append(nalu)
    return nalus


class VideoStreamer:
    def __init__(self, send_bytes, call_stop_event: threading.Event):
        self.send_bytes = send_bytes
        self.call_stop_event = call_stop_event
        self.thread: threading.Thread | None = None
        self.process: subprocess.Popen | None = None

    def start(self):
        self.ffmpeg_path = _find_ffmpeg()
        if not self.ffmpeg_path:
            LOG.warning(ffmpeg_warning())
            return
        self.thread = threading.Thread(target=self._run, name="video", daemon=True)
        self.thread.start()

    def stop(self):
        self._stop_ffmpeg()

    def _run(self):
        try:
            import cv2
        except ImportError:
            LOG.warning("opencv-python is not installed. Video streaming disabled.")
            return

        cap = cv2.VideoCapture(0)
        if not cap.isOpened():
            LOG.warning("Webcam unavailable. Continuing without video.")
            cap.release()
            return

        cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)
        cap.set(cv2.CAP_PROP_FPS, FPS)

        cmd = [
            self.ffmpeg_path,
            "-hide_banner",
            "-loglevel", "error",
            "-f", "rawvideo",
            "-pix_fmt", "bgr24",
            "-s", f"{WIDTH}x{HEIGHT}",
            "-r", str(FPS),
            "-i", "pipe:0",
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-tune", "zerolatency",
            "-profile:v", "baseline",
            "-level", "4.0",
            "-crf", "18",
            "-maxrate", "5M",
            "-bufsize", "10M",
            "-g", str(FPS),
            "-pix_fmt", "yuv420p",
            "-bsf:v", "dump_extra",
            "-f", "h264",
            "pipe:1",
        ]

        try:
            self.process = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            reader = threading.Thread(target=self._read_ffmpeg_stdout, name="video-ffmpeg-read", daemon=True)
            reader.start()
            LOG.info("Webcam video streaming started (%dx%d @ %d fps)", WIDTH, HEIGHT, FPS)
            frame_delay = 1.0 / FPS
            while not self.call_stop_event.is_set():
                ok, frame = cap.read()
                if not ok:
                    LOG.warning("Webcam frame capture failed")
                    time.sleep(frame_delay)
                    continue
                frame = cv2.resize(frame, (WIDTH, HEIGHT))
                try:
                    self.process.stdin.write(frame.tobytes())
                    self.process.stdin.flush()
                except (BrokenPipeError, OSError, AttributeError) as exc:
                    LOG.error("ffmpeg input stopped: %s", exc)
                    break
                time.sleep(frame_delay)
        except OSError as exc:
            LOG.error("Unable to start ffmpeg: %s", exc)
        finally:
            cap.release()
            self._stop_ffmpeg()

    def _read_ffmpeg_stdout(self):
        """Read H.264 from ffmpeg and send each complete NAL unit as one VIDEO frame."""
        process = self.process
        if process is None or process.stdout is None:
            return
        buf = bytearray()
        while not self.call_stop_event.is_set():
            try:
                chunk = process.stdout.read(32768)
            except OSError as exc:
                LOG.warning("ffmpeg output read failed: %s", exc)
                return
            if not chunk:
                if process.poll() is not None:
                    break
                time.sleep(0.005)
                continue
            buf.extend(chunk)
            # Find all start code positions (00 00 00 01)
            positions = []
            i = 0
            while i <= len(buf) - 4:
                if buf[i] == 0 and buf[i+1] == 0 and buf[i+2] == 0 and buf[i+3] == 1:
                    positions.append(i)
                    i += 4
                else:
                    i += 1
            if len(positions) < 2:
                continue
            # Send all complete NALs (between consecutive start codes)
            for j in range(len(positions) - 1):
                nalu = bytes(buf[positions[j]:positions[j+1]])
                self.send_bytes(encode_frame(VIDEO, nalu))
            # Keep the last (possibly incomplete) NAL in the buffer
            buf = buf[positions[-1]:]

        # Flush remaining
        if buf:
            self.send_bytes(encode_frame(VIDEO, bytes(buf)))

    def _stop_ffmpeg(self):
        process = self.process
        if process is None:
            return
        self.process = None
        try:
            if process.stdin:
                process.stdin.close()
        except OSError:
            pass
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        if process.stderr:
            try:
                err = process.stderr.read()
                if err:
                    LOG.debug("ffmpeg stderr: %s", err.decode(errors="replace").strip())
            except OSError:
                pass
