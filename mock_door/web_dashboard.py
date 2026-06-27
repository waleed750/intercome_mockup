"""Web-based dashboard for the Mock Door Intercom service."""

from __future__ import annotations

import json
import logging
import sys
import threading
import traceback
from datetime import datetime

from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO

from mock_door import MockDoorService
from video_stream import ffmpeg_available, ffmpeg_warning

app = Flask(__name__)
app.config["SECRET_KEY"] = "mock-door-dev"
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

service: MockDoorService | None = None
service_lock = threading.Lock()
traffic_log: list[dict] = []
MAX_LOG = 500


class WebSocketLogHandler(logging.Handler):
    def emit(self, record):
        try:
            msg = self.format(record)
            entry = {
                "time": datetime.now().strftime("%H:%M:%S.%f")[:-3],
                "level": record.levelname,
                "source": record.name,
                "message": record.getMessage(),
                "formatted": msg,
            }
            traffic_log.append(entry)
            if len(traffic_log) > MAX_LOG:
                traffic_log.pop(0)
            socketio.emit("log", entry)

            if "DOOR UNLOCKED" in msg:
                socketio.emit("status", {"status": "DOOR UNLOCKED!", "color": "green"})
            elif "Call accepted" in msg:
                socketio.emit("status", {"status": "CONNECTED", "color": "green"})
            elif "Call started" in msg:
                socketio.emit("status", {"status": "RINGING...", "color": "yellow"})
            elif "Call closed" in msg or "Remote closed" in msg:
                socketio.emit("status", {"status": "IDLE", "color": "gray"})
            elif "busy" in msg.lower():
                socketio.emit("status", {"status": "BUSY", "color": "orange"})
            elif "failed" in msg.lower() and "TCP" in msg:
                socketio.emit("status", {"status": "FAILED", "color": "orange"})
        except Exception:
            pass


class ConsoleLogHandler(logging.StreamHandler):
    """Also print all logs to the terminal for debugging."""
    pass


def _get_or_create_service(ip: str = "") -> MockDoorService:
    global service
    with service_lock:
        if service is None:
            service = MockDoorService(ip)
            service.discovery.on_unit_discovered.append(_on_unit_discovered)
            service.start()
        elif ip:
            service.target_ip = ip
        return service


def _on_unit_discovered(unit):
    try:
        socketio.emit("unit_discovered", {
            "ip": unit.ip,
            "alias": unit.alias,
            "serial": unit.serial,
        })
    except Exception:
        pass


@app.route("/")
def index():
    has_ffmpeg = ffmpeg_available()
    return render_template("dashboard.html", has_ffmpeg=has_ffmpeg)


@app.route("/api/call", methods=["POST"])
def api_call():
    data = request.get_json(silent=True) or {}
    ip = data.get("ip", "").strip()
    svc = _get_or_create_service(ip)
    socketio.emit("status", {"status": "CALLING...", "color": "yellow"})

    def _safe_call():
        try:
            svc.call(ip)
        except Exception:
            logging.getLogger("DOOR").error("Call crashed:\n%s", traceback.format_exc())
            socketio.emit("status", {"status": "ERROR", "color": "orange"})

    threading.Thread(target=_safe_call, daemon=True).start()
    return jsonify({"ok": True})


@app.route("/api/hangup", methods=["POST"])
def api_hangup():
    def _safe_hangup():
        try:
            if service:
                service.hang_up(send_command=True)
        except Exception:
            logging.getLogger("DOOR").error("Hangup crashed:\n%s", traceback.format_exc())

    threading.Thread(target=_safe_hangup, daemon=True).start()
    socketio.emit("status", {"status": "IDLE", "color": "gray"})
    return jsonify({"ok": True})


@app.route("/api/command", methods=["POST"])
def api_command():
    data = request.get_json(silent=True) or {}
    command = data.get("command", "").strip()
    if not command:
        return jsonify({"error": "command required"}), 400
    extra = {k: v for k, v in data.items() if k != "command"}
    if service:
        try:
            service.send_command(command, **extra)
        except Exception:
            logging.getLogger("DOOR").error("Command failed:\n%s", traceback.format_exc())
    return jsonify({"ok": True})


@app.route("/api/discovery", methods=["POST"])
def api_discovery():
    try:
        svc = _get_or_create_service()
        svc.send_discovery_now()
    except Exception:
        logging.getLogger("DISCOVERY").error("Discovery crashed:\n%s", traceback.format_exc())
    return jsonify({"ok": True})


@app.route("/api/units")
def api_units():
    try:
        if service is None:
            return jsonify([])
        units = service.get_discovered_units()
        return jsonify([{"ip": u.ip, "alias": u.alias, "serial": u.serial} for u in units])
    except Exception:
        return jsonify([])


@app.route("/api/logs")
def api_logs():
    return jsonify(traffic_log[-100:])


@app.route("/api/status")
def api_status():
    try:
        in_call = service is not None and service.sock is not None
        return jsonify({
            "in_call": in_call,
            "target_ip": service.target_ip if service else "",
        })
    except Exception:
        return jsonify({"in_call": False, "target_ip": ""})


@app.errorhandler(Exception)
def handle_error(e):
    logging.getLogger("DASHBOARD").error("Unhandled error: %s\n%s", e, traceback.format_exc())
    return jsonify({"error": str(e)}), 500


def main():
    # Console handler — prints everything to terminal
    console = ConsoleLogHandler(sys.stdout)
    console.setFormatter(logging.Formatter(
        "%(asctime)s [%(name)-12s] %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    ))
    logging.getLogger().addHandler(console)

    # WebSocket handler — sends to browser
    ws_handler = WebSocketLogHandler()
    ws_handler.setFormatter(logging.Formatter("%(asctime)s [%(name)s] %(message)s", datefmt="%H:%M:%S"))
    logging.getLogger().addHandler(ws_handler)

    logging.getLogger().setLevel(logging.DEBUG)
    logging.getLogger("werkzeug").setLevel(logging.WARNING)
    logging.getLogger("engineio").setLevel(logging.WARNING)
    logging.getLogger("socketio").setLevel(logging.WARNING)

    # Auto-start service so discovery begins immediately
    _get_or_create_service()

    print()
    print("=" * 50)
    print("  Mock Door Intercom — Web Dashboard")
    print("  Open in browser: http://localhost:5000")
    print("  DEBUG MODE: all logs print to this terminal")
    print("=" * 50)
    print()

    socketio.run(app, host="0.0.0.0", port=5000, debug=False, allow_unsafe_werkzeug=True)


if __name__ == "__main__":
    main()
