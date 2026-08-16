#!/usr/bin/env bash
# Sanity-checks the exact GStreamer pipeline shapes used by
# syncn_intercom_video.c / syncn_intercom_audio.c, without needing a built
# flutter-pi binary or the mock_door protocol handshake -- just GStreamer
# itself, which is why this can run on any desktop Linux box.
#
# It does NOT test: flutter-pi's texture registry integration, the real
# door unit's actual encoder output (only a local x264enc-generated Annex-B
# stream, matching the "one NAL per buffer" shape mock_door/video_stream.py
# produces), or the real TCP protocol handshake. It DOES test: whether the
# caps/parse/decode chain this plugin builds actually accepts and decodes
# Annex-B H.264 with alignment=nal, and A-law audio at 8kHz mono -- the part
# most likely to break from a caps/format mistake.
#
# Usage: ./gst_pipeline_smoketest.sh [--headless]
#   --headless   use fakesink/fakesink instead of an on-screen sink
#                (use this over SSH / when DISPLAY isn't set)

set -euo pipefail

SINK_VIDEO="autovideosink"
SINK_AUDIO="autoaudiosink"
if [[ "${1:-}" == "--headless" || -z "${DISPLAY:-}" ]]; then
  echo "No DISPLAY (or --headless passed) -- using fakesink, checking for pipeline errors only."
  SINK_VIDEO="fakesink"
  SINK_AUDIO="fakesink"
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "== 1/3: generating a short Annex-B H.264 test clip (x264enc, baseline profile, byte-stream) =="
gst-launch-1.0 -q \
  videotestsrc num-buffers=75 pattern=ball ! \
  video/x-raw,width=640,height=480,framerate=25/1 ! \
  x264enc tune=zerolatency speed-preset=veryfast key-int-max=25 ! \
  video/x-h264,profile=baseline ! \
  h264parse config-interval=-1 ! \
  video/x-h264,stream-format=byte-stream,alignment=nal ! \
  filesink location="$WORKDIR/test.h264"

echo "== 2/3: decoding it through the SAME caps/parse/decode chain syncn_intercom_video.c uses =="
gst-launch-1.0 \
  filesrc location="$WORKDIR/test.h264" ! \
  video/x-h264,stream-format=byte-stream,alignment=nal ! \
  h264parse config-interval=-1 ! avdec_h264 ! videoconvert ! \
  video/x-raw,format=RGBA ! \
  "$SINK_VIDEO"
echo "video pipeline: OK (no GStreamer pipeline errors)"

echo "== 3/3: generating + decoding an A-law tone through the SAME chain syncn_intercom_audio.c uses =="
gst-launch-1.0 \
  audiotestsrc wave=sine freq=440 num-buffers=200 ! \
  audio/x-raw,rate=8000,channels=1,format=S16LE ! \
  alawenc ! \
  audio/x-alaw,rate=8000,channels=1 ! \
  alawdec ! audioconvert ! audioresample ! volume name=playvol volume=0.75 ! \
  "$SINK_AUDIO"
echo "audio pipeline: OK (no GStreamer pipeline errors)"

echo
echo "All pipeline shapes used by the flutter-pi plugin decode successfully in isolation."
echo "This does NOT prove flutter-pi's texture upload or the real door unit's encoder output work --"
echo "see flutterpi/README.md's 'What's verified vs. not' section for what still needs on-device testing."
