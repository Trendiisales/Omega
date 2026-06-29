#!/usr/bin/env python3
"""
Omega Mac cockpit -- single-pane aggregator on :8099.

Rebuilt 2026-06-30 after a host "reset" deleted /Users/jo/omega-cockpit
(server.py + index.html) AND /Users/jo/omega-supervisor with NO backup
(never git-tracked -- Mac runtime). The data it displays all survived,
so this rebuild is wired straight to the live source files + the two
sibling GUIs.

Surfaces, in one page:
  * Companion book  -- ~/stall-accountant/companion_state.json
                       (the UNIFIED omega+crypto realized-clip roll-up:
                        by_book CRYPTO/OMEGA, by_reason, per_engine, open_detail)
  * Crypto book     -- IBKRCrypto state.json (also the full GUI on :8090)
  * RD-Agent        -- ~/Omega/data/rdagent/latest.json (full GUI on :7799)
  * Omega desk      -- VPS GUI http://185.167.119.59:7779 (companion + shadow + telemetry)

Read-only. Serves its own index.html + small JSON proxy routes so the
page can fetch local files without CORS pain. Every route is fail-safe:
a missing/garbled source returns {"error": ...}, never a 500 that blanks
the panel.

    python3 server.py            # PORT env overrides (default 8099)
    open http://127.0.0.1:8099
"""
from __future__ import annotations

import json
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = int(os.environ.get("PORT", 8099))
BIND = os.environ.get("OMEGA_COCKPIT_BIND", "127.0.0.1")

HOME = Path.home()
# Source files (env-overridable; defaults match the live layout 2026-06-30).
COMPANION = Path(os.environ.get("COMPANION_STATE", HOME / "stall-accountant" / "companion_state.json"))
CRYPTO = Path(os.environ.get("IBKRCRYPTO_STATE", HOME / "IBKRCrypto" / "backtest" / "data" / "ibkrcrypto" / "state.json"))
RDAGENT = Path(os.environ.get("RDAGENT_LATEST", HOME / "Omega" / "data" / "rdagent" / "latest.json"))

# /api/<key> -> source file
ROUTES = {
    "/api/companion": COMPANION,
    "/api/crypto": CRYPTO,
    "/api/rdagent": RDAGENT,
}


class Handler(SimpleHTTPRequestHandler):
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        route = self.path.split("?")[0]
        if route in ROUTES:
            p = ROUTES[route]
            try:
                self._json(json.loads(p.read_text()))
            except FileNotFoundError:
                self._json({"error": f"missing: {p}"}, 200)
            except Exception as e:  # noqa: BLE001 -- never blank the panel
                self._json({"error": str(e), "path": str(p)}, 200)
            return
        if route in ("/api/health", "/healthz"):
            self._json({
                "ok": True,
                "sources": {k: ROUTES[k].exists() for k in ROUTES},
            })
            return
        return super().do_GET()

    def log_message(self, *a):  # quieter logs
        pass


def main() -> None:
    handler = partial(Handler, directory=str(HERE))
    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer((BIND, PORT), handler)
    print(f"[omega-cockpit] http://{BIND}:{PORT}  (serving {HERE})", flush=True)
    for k, p in ROUTES.items():
        print(f"  {k:16s} -> {p}  {'OK' if p.exists() else 'MISSING'}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
