"""Simulates the Android app's TCP server to test the mock door locally."""

import json
import socket
import struct
import threading
import time
import sys

CONTROL = 0xAA
HEADER_SIZE = 8

def encode_frame(marker, payload):
    magic = bytes([marker]) * 4
    length = len(payload).to_bytes(4, 'little')
    return magic + length + payload

def parse_frames(data):
    frames = []
    pos = 0
    while pos + HEADER_SIZE <= len(data):
        marker = data[pos]
        if not all(data[pos+i] == marker for i in range(4)):
            pos += 1
            continue
        length = int.from_bytes(data[pos+4:pos+8], 'little')
        if pos + HEADER_SIZE + length > len(data):
            break
        payload = data[pos+HEADER_SIZE:pos+HEADER_SIZE+length]
        frames.append((marker, payload))
        pos += HEADER_SIZE + length
    return frames

def handle_client(conn, addr):
    print(f"[APP] Door connected from {addr}")
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                print("[APP] Door disconnected (empty read)")
                break
            frames = parse_frames(data)
            for marker, payload in frames:
                if marker == CONTROL:
                    try:
                        msg = json.loads(payload.decode())
                        cmd = msg.get("command", "?")
                        print(f"[APP] <<< {cmd}")

                        if cmd == "Call":
                            print("[APP] Ringing... auto-answering in 2s")
                            time.sleep(2)
                            answers = [
                                '{"command":"Answer"}',
                                '{"command":"Answer","OtherAnswer":1}',
                                '{"command":"Answer","OtherAnswer":true}',
                            ]
                            for a in answers:
                                frame = encode_frame(CONTROL, a.encode())
                                conn.sendall(frame)
                                print(f"[APP] >>> {a}")
                            print("[APP] Call connected. Keeping alive...")

                        elif cmd == "HangUp":
                            print("[APP] Door hung up")
                            return
                        elif cmd == "OpenDoor":
                            print("[APP] DOOR UNLOCKED!")
                    except Exception as e:
                        print(f"[APP] Parse error: {e}")
                else:
                    names = {0xBB: "VIDEO", 0xCC: "AUDIO"}
                    print(f"[APP] Received {names.get(marker, hex(marker))} frame ({len(payload)} bytes)")
    except ConnectionResetError:
        print("[APP] Connection reset by door!")
    except Exception as e:
        print(f"[APP] Error: {e}")
    finally:
        conn.close()
        print("[APP] Connection closed")

def main():
    port = 8189
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"[APP] Fake Android app listening on port {port}")
    print(f"[APP] Use IP 127.0.0.1 in the mock door dashboard")
    print()
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    main()
