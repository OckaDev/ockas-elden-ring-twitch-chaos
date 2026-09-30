#!/usr/bin/env python3
"""Listen for EldenChaos UDP packets so you can test Streamer.bot without the game."""

from __future__ import annotations

import json
import socket

# Bind all interfaces so Streamer.bot BroadcastUdp is received
HOST = "0.0.0.0"
PORT = 22666


def main() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, PORT))
    print(f"[mock] Listening on udp://0.0.0.0:{PORT}")
    print("[mock] Redeem a channel point or Test Action in Streamer.bot...\n")

    while True:
        data, addr = sock.recvfrom(65535)
        text = data.decode("utf-8", errors="replace")
        print(f"--- from {addr[0]}:{addr[1]} ---")
        try:
            parsed = json.loads(text)
            print(json.dumps(parsed, indent=2))
            cmd = parsed.get("cmd")
            if cmd == "heal":
                print("=> would HEAL player HP")
            elif cmd == "spawn":
                print(f"=> would SPAWN {parsed.get('count')} {parsed.get('enemy')}")
            elif cmd == "stat":
                delta = parsed.get("delta")
                sign = f"+{delta}" if isinstance(delta, int) and delta > 0 else str(delta)
                print(f"=> would apply {sign} {parsed.get('stat')}")
            print(f"announce: {parsed.get('message')}")
        except json.JSONDecodeError:
            print(text)
        print()


if __name__ == "__main__":
    main()
