#!/usr/bin/env python3
# ibkr_api_probe.py — ACTIVE liveness probe for the IBKR API Server (runs ON the box).
# The un-fakeable check: complete the TWS API handshake to 4001 and require a SERVER
# response. Port-listening / socket-established / exec-log all stay "green" while the
# API Server is DISCONNECTED behind a 2FA dialog — only an actual server reply proves
# IBKR is really answering. Prints UP or "DOWN <reason>"; exit 0=UP, 2=DOWN.
import socket, sys
HOST, PORT = "127.0.0.1", 4001
try:
    s = socket.create_connection((HOST, PORT), timeout=6)
except Exception as e:
    print(f"DOWN connect-failed: {e}"); sys.exit(2)
try:
    s.sendall(b"API\x00")
    msg = b"v100..176"
    s.sendall(len(msg).to_bytes(4, "big") + msg)   # v100+ handshake
    s.settimeout(6)
    data = s.recv(4096)                             # server version + conn time if API is UP
    if data and len(data) > 4:
        print("UP"); sys.exit(0)
    print("DOWN no-server-reply (API Server disconnected — 2FA/login needed)"); sys.exit(2)
except Exception as e:
    print(f"DOWN handshake-timeout: {e} (API Server disconnected — 2FA/login needed)"); sys.exit(2)
finally:
    s.close()
