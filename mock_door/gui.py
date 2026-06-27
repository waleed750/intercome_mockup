"""Tkinter GUI for the Mock Door Intercom service."""

from __future__ import annotations

import logging
import queue
import threading
import tkinter as tk
from tkinter import ttk, scrolledtext
from typing import Optional

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False

try:
    from PIL import Image, ImageTk
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


class QueueLogHandler(logging.Handler):
    """Logging handler that pushes formatted records into a thread-safe queue."""

    def __init__(self, log_queue: queue.Queue):
        super().__init__()
        self.log_queue = log_queue

    def emit(self, record):
        try:
            msg = self.format(record)
            self.log_queue.put_nowait(msg)
        except Exception:
            pass


class MockDoorGUI:
    CAMERA_UPDATE_MS = 66  # ~15 fps preview
    LOG_POLL_MS = 100

    def __init__(self, service_factory):
        """service_factory(target_ip: str) -> MockDoorService"""
        self.service_factory = service_factory
        self.service = None
        self.log_queue: queue.Queue[str] = queue.Queue(maxsize=500)
        self.cap: Optional[object] = None
        self.camera_running = False

        self._build_window()
        self._install_log_handler()
        self._poll_logs()

    # ------------------------------------------------------------------ build
    def _build_window(self):
        self.root = tk.Tk()
        self.root.title("Mock Door Intercom")
        self.root.configure(bg="#1e1e2e")
        self.root.minsize(900, 620)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Title.TLabel", font=("Segoe UI", 16, "bold"),
                        foreground="#cdd6f4", background="#1e1e2e")
        style.configure("Status.TLabel", font=("Segoe UI", 11),
                        foreground="#a6adc8", background="#1e1e2e")
        style.configure("Big.TButton", font=("Segoe UI", 11), padding=8)
        style.configure("Call.TButton", font=("Segoe UI", 12, "bold"), padding=10)
        style.configure("Hangup.TButton", font=("Segoe UI", 12, "bold"), padding=10)
        style.configure("Panel.TFrame", background="#313244")
        style.configure("Dark.TFrame", background="#1e1e2e")
        style.configure("Dark.TLabel", foreground="#cdd6f4", background="#1e1e2e")
        style.configure("Dark.TEntry", fieldbackground="#45475a", foreground="#cdd6f4")

        # --- top bar: IP + connect ---
        top = ttk.Frame(self.root, style="Dark.TFrame")
        top.pack(fill=tk.X, padx=12, pady=(12, 4))

        ttk.Label(top, text="Mock Door Intercom", style="Title.TLabel").pack(side=tk.LEFT)

        self.status_var = tk.StringVar(value="IDLE")
        self.status_label = ttk.Label(top, textvariable=self.status_var, style="Status.TLabel")
        self.status_label.pack(side=tk.RIGHT, padx=(8, 0))
        ttk.Label(top, text="Status:", style="Dark.TLabel").pack(side=tk.RIGHT)

        # --- IP row ---
        ip_frame = ttk.Frame(self.root, style="Dark.TFrame")
        ip_frame.pack(fill=tk.X, padx=12, pady=4)

        ttk.Label(ip_frame, text="Target IP:", style="Dark.TLabel").pack(side=tk.LEFT)
        self.ip_var = tk.StringVar(value="192.168.1.")
        self.ip_entry = ttk.Entry(ip_frame, textvariable=self.ip_var, width=20,
                                  font=("Consolas", 11))
        self.ip_entry.pack(side=tk.LEFT, padx=(6, 10))
        self.ip_entry.bind("<Return>", lambda e: self._on_call())

        # --- button row ---
        btn_frame = ttk.Frame(self.root, style="Dark.TFrame")
        btn_frame.pack(fill=tk.X, padx=12, pady=6)

        self.btn_call = ttk.Button(btn_frame, text="Call", style="Call.TButton",
                                   command=self._on_call)
        self.btn_call.pack(side=tk.LEFT, padx=(0, 6))

        self.btn_hangup = ttk.Button(btn_frame, text="Hang Up", style="Hangup.TButton",
                                     command=self._on_hangup, state=tk.DISABLED)
        self.btn_hangup.pack(side=tk.LEFT, padx=(0, 6))

        self.btn_discovery = ttk.Button(btn_frame, text="Send Discovery", style="Big.TButton",
                                        command=self._on_discovery)
        self.btn_discovery.pack(side=tk.LEFT, padx=(0, 6))

        self.btn_camera = ttk.Button(btn_frame, text="Preview Camera", style="Big.TButton",
                                     command=self._toggle_camera)
        self.btn_camera.pack(side=tk.LEFT, padx=(0, 6))

        # --- main area: camera left, log right ---
        main = ttk.Frame(self.root, style="Dark.TFrame")
        main.pack(fill=tk.BOTH, expand=True, padx=12, pady=(4, 12))
        main.columnconfigure(0, weight=1)
        main.columnconfigure(1, weight=1)
        main.rowconfigure(0, weight=1)

        # camera panel
        cam_frame = ttk.Frame(main, style="Panel.TFrame")
        cam_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 6))

        self.cam_label = tk.Label(cam_frame, text="Camera Preview\n(click 'Preview Camera')",
                                  bg="#313244", fg="#6c7086",
                                  font=("Segoe UI", 10), anchor="center")
        self.cam_label.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

        # log panel
        log_frame = ttk.Frame(main, style="Panel.TFrame")
        log_frame.grid(row=0, column=1, sticky="nsew", padx=(6, 0))

        self.log_text = scrolledtext.ScrolledText(
            log_frame, wrap=tk.WORD, state=tk.DISABLED,
            bg="#313244", fg="#cdd6f4", insertbackground="#cdd6f4",
            font=("Consolas", 9), relief=tk.FLAT, borderwidth=4,
        )
        self.log_text.pack(fill=tk.BOTH, expand=True)

    # ------------------------------------------------------------------ logging
    def _install_log_handler(self):
        handler = QueueLogHandler(self.log_queue)
        handler.setFormatter(logging.Formatter("%(asctime)s [%(name)s] %(message)s",
                                               datefmt="%H:%M:%S"))
        logging.getLogger().addHandler(handler)
        logging.getLogger().setLevel(logging.INFO)

    def _poll_logs(self):
        while True:
            try:
                msg = self.log_queue.get_nowait()
            except queue.Empty:
                break
            self.log_text.configure(state=tk.NORMAL)
            self.log_text.insert(tk.END, msg + "\n")
            self.log_text.see(tk.END)
            self.log_text.configure(state=tk.DISABLED)

            if "DOOR UNLOCKED" in msg:
                self._set_status("DOOR UNLOCKED!")
            elif "Call accepted" in msg:
                self._set_status("CONNECTED")
            elif "Call closed" in msg:
                self._set_status("IDLE")
                self._update_buttons(in_call=False)
            elif "busy" in msg.lower():
                self._set_status("BUSY")
            elif "Remote closed" in msg:
                self._set_status("IDLE")
                self._update_buttons(in_call=False)

        self.root.after(self.LOG_POLL_MS, self._poll_logs)

    # ------------------------------------------------------------------ camera
    def _toggle_camera(self):
        if self.camera_running:
            self._stop_camera()
        else:
            self._start_camera()

    def _start_camera(self):
        if not HAS_CV2:
            self._log_to_panel("opencv-python not installed — camera preview unavailable")
            return
        if not HAS_PIL:
            self._log_to_panel("Pillow not installed — camera preview unavailable. pip install Pillow")
            return
        self.cap = cv2.VideoCapture(0)
        if not self.cap.isOpened():
            self._log_to_panel("Could not open webcam")
            self.cap = None
            return
        self.camera_running = True
        self.btn_camera.configure(text="Stop Camera")
        self._update_camera()

    def _stop_camera(self):
        self.camera_running = False
        self.btn_camera.configure(text="Preview Camera")
        if self.cap is not None:
            self.cap.release()
            self.cap = None
        self.cam_label.configure(image="", text="Camera Preview\n(click 'Preview Camera')")

    def _update_camera(self):
        if not self.camera_running or self.cap is None:
            return
        ok, frame = self.cap.read()
        if ok:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            # fit to label size
            lw = max(self.cam_label.winfo_width(), 320)
            lh = max(self.cam_label.winfo_height(), 240)
            h, w = frame.shape[:2]
            scale = min(lw / w, lh / h)
            frame = cv2.resize(frame, (int(w * scale), int(h * scale)))
            img = Image.fromarray(frame)
            imgtk = ImageTk.PhotoImage(image=img)
            self.cam_label.imgtk = imgtk  # prevent GC
            self.cam_label.configure(image=imgtk, text="")
        self.root.after(self.CAMERA_UPDATE_MS, self._update_camera)

    # ------------------------------------------------------------------ actions
    def _on_call(self):
        ip = self.ip_var.get().strip()
        if not ip:
            self._log_to_panel("Please enter the Android device IP address")
            return
        if self.service is None:
            self.service = self.service_factory(ip)
            self.service.start()
        else:
            self.service.target_ip = ip
        self._set_status("CALLING...")
        self._update_buttons(in_call=True)
        threading.Thread(target=self._do_call, daemon=True).start()

    def _do_call(self):
        self.service.call()

    def _on_hangup(self):
        if self.service:
            self.service.hang_up(send_command=True)
        self._set_status("IDLE")
        self._update_buttons(in_call=False)

    def _on_discovery(self):
        if self.service is None:
            ip = self.ip_var.get().strip() or "0.0.0.0"
            self.service = self.service_factory(ip)
            self.service.start()
        self.service.send_discovery_now()

    # ------------------------------------------------------------------ helpers
    def _set_status(self, text: str):
        self.status_var.set(text)

    def _update_buttons(self, in_call: bool):
        self.btn_call.configure(state=tk.DISABLED if in_call else tk.NORMAL)
        self.btn_hangup.configure(state=tk.NORMAL if in_call else tk.DISABLED)

    def _log_to_panel(self, msg: str):
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    def _on_close(self):
        self._stop_camera()
        if self.service:
            self.service.shutdown()
        self.root.destroy()

    def run(self):
        self.root.mainloop()
