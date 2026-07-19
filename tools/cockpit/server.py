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
  * Omega desk      -- VPS GUI http://45.85.3.79:7779 (companion + shadow + telemetry)

Read-only. Serves its own index.html + small JSON proxy routes so the
page can fetch local files without CORS pain. Every route is fail-safe:
a missing/garbled source returns {"error": ...}, never a 500 that blanks
the panel.

    python3 server.py            # PORT env overrides (default 8099)
    open http://127.0.0.1:8099
"""
from __future__ import annotations

import csv
import glob
import io
import json
import os
import subprocess
import time
import urllib.request
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = int(os.environ.get("PORT", 8099))
BIND = os.environ.get("OMEGA_COCKPIT_BIND", "127.0.0.1")

HOME = Path.home()
# Source files (env-overridable; defaults match the live layout 2026-06-30).
COMPANION = Path(os.environ.get("COMPANION_STATE", HOME / "stall-accountant" / "companion_state.json"))
# S-2026-07-02: ~/Crypto after the 2026-07-01 IBKRCrypto->Crypto consolidation
# (env override wins; old path kept as fallback for a not-yet-moved box).
_crypto_new = HOME / "Crypto" / "backtest" / "data" / "ibkrcrypto" / "state.json"
_crypto_old = HOME / "IBKRCrypto" / "backtest" / "data" / "ibkrcrypto" / "state.json"
CRYPTO = Path(os.environ.get("IBKRCRYPTO_STATE", _crypto_new if _crypto_new.exists() else _crypto_old))
RDAGENT = Path(os.environ.get("RDAGENT_LATEST", HOME / "Omega" / "data" / "rdagent" / "latest.json"))

# /api/<key> -> source file
ROUTES = {
    "/api/companion": COMPANION,
    "/api/crypto": CRYPTO,
    "/api/rdagent": RDAGENT,
}

# ── TRADES LOG: every closed clip + every open position across ALL symbols/engines ──
# The operator asked for "current trade/s showing and past trades in a log, for all the
# symbols". No single file has this -- each companion book writes its OWN companion_closed.csv
# (past) + companion_positions.json (open). We glob them ALL, tag by source dir, and merge.
# Read-only, fail-safe: any unreadable file is skipped, never a 500.
STALL_ROOT = Path(os.environ.get("STALL_ACCT_DIR", HOME / "stall-accountant"))
_CLOSED_COLS = ("ts", "book", "reason", "engine", "symbol", "side",
                "entry", "realized_pnl", "mfe_peak_pct", "bars_held")


def _src_label(path: Path) -> str:
    # root companion_closed.csv == the main OMEGA stall book; a subdir == that book's name.
    d = path.parent
    return "OMEGA" if d == STALL_ROOT else d.name


# ── S-2026-07-20g: crypto LIVE closes (real Binance fills) in the trades log ──
# The stall-root glob only covers Mac-local companion books; the crypto desk books
# every real SELL to josgp1 data/live_trades.csv (ledger of record since 20f), so
# without this pull the log showed NO trades all weekend while crypto clipped live.
# Read-only ssh cat, TTL-cached so page loads don't hammer the box; on failure the
# last good copy is served (fail-safe, never blanks the log).
CHIMERA_HOST = os.environ.get("CHIMERA_SSH_HOST", "chimera-direct")
CHIMERA_LIVE_LEDGER = os.environ.get("CHIMERA_LIVE_LEDGER", "~/ChimeraCrypto/data/live_trades.csv")
_CRYPTO_TTL_S = 120
_crypto_cache: dict = {"ts": 0.0, "rows": []}


def _read_crypto_live() -> list[dict]:
    now = time.time()
    if now - _crypto_cache["ts"] < _CRYPTO_TTL_S:
        return _crypto_cache["rows"]
    rows: list[dict] = []
    try:
        out = subprocess.run(["ssh", CHIMERA_HOST, f"cat {CHIMERA_LIVE_LEDGER}"],
                             capture_output=True, text=True, timeout=10)
        if out.returncode != 0 or not out.stdout.strip():
            raise RuntimeError(out.stderr.strip() or "empty")
        for row in csv.DictReader(io.StringIO(out.stdout)):
            try:
                rows.append({
                    "src": "CRYPTO-LIVE",
                    "ts": int(float(row["exit_ts_ms"]) / 1000),
                    "engine": "LiveMimic",
                    "symbol": row.get("coin", ""),
                    "side": "BUY",
                    "entry": float(row["entry_px"]),
                    "pnl": float(row["realized_usd"]),
                    "reason": row.get("reason", ""),
                    "mfe_pct": None,
                    "bars": "",
                    "book": "crypto_live",
                })
            except (KeyError, TypeError, ValueError):
                continue
        _crypto_cache["rows"] = rows
    except Exception:  # noqa: BLE001 -- box unreachable: serve last good, retry next TTL
        rows = _crypto_cache["rows"]
    _crypto_cache["ts"] = now
    return rows


def _read_trades() -> dict:
    closed, opened = [], []
    closed.extend(_read_crypto_live())
    # ---- past clips: every companion_closed.csv under the stall root ----
    for fp in sorted(glob.glob(str(STALL_ROOT / "**" / "companion_closed.csv"), recursive=True)):
        p = Path(fp)
        src = _src_label(p)
        try:
            with p.open(newline="") as f:
                for row in csv.DictReader(f):
                    if not row.get("ts"):
                        continue
                    def _f(k):
                        try:
                            return float(row.get(k) or 0)
                        except (TypeError, ValueError):
                            return None
                    closed.append({
                        "src": src,
                        "ts": int(_f("ts") or 0),
                        "engine": row.get("engine", ""),
                        "symbol": row.get("symbol", ""),
                        "side": row.get("side", ""),
                        "entry": _f("entry"),
                        "pnl": _f("realized_pnl"),
                        "reason": row.get("reason", ""),
                        "mfe_pct": _f("mfe_peak_pct"),
                        "bars": row.get("bars_held", ""),
                        "book": row.get("book", src),
                    })
        except Exception:  # noqa: BLE001 -- skip a garbled book, never blank the log
            continue
    # ---- current positions: every companion_positions.json under the stall root ----
    for fp in sorted(glob.glob(str(STALL_ROOT / "**" / "companion_positions.json"), recursive=True)):
        p = Path(fp)
        src = _src_label(p)
        try:
            d = json.loads(p.read_text())
        except Exception:  # noqa: BLE001
            continue
        for _k, v in (d.items() if isinstance(d, dict) else []):
            if not isinstance(v, dict):
                continue
            opened.append({
                "src": src,
                "engine": v.get("eng") or v.get("engine", ""),
                "symbol": v.get("sym") or v.get("symbol", ""),
                "side": v.get("side", ""),
                "entry": v.get("entry"),
                "upnl": v.get("last_upnl", v.get("mfe_usd")),
                "mfe_pct": v.get("mfe_pct"),
                "open_ts": v.get("open_ts"),
                "book": v.get("book", src),
            })
    # ---- S-2026-07-08c: the native VPS C++ books persist OPEN positions as
    # companion_positions.tsv (no header: key book eng sym side entry open_bar
    # ext_bar mfe_pct mfe_usd bars last_upnl) -- mirror-pulled from the VPS.
    for fp in sorted(glob.glob(str(STALL_ROOT / "**" / "companion_positions.tsv"), recursive=True)):
        p2 = Path(fp)
        src = _src_label(p2)
        try:
            for ln in p2.read_text().splitlines():
                t = ln.split("\t")
                if len(t) < 12 or not t[5]:
                    continue
                def _tf(x):
                    try: return float(x)
                    except (TypeError, ValueError): return None
                opened.append({
                    "src": src,
                    "engine": t[2], "symbol": t[3], "side": t[4],
                    "entry": _tf(t[5]),
                    "upnl": _tf(t[11]),
                    "mfe_pct": _tf(t[8]),
                    "open_ts": None,
                    "book": t[1] or src,
                })
        except Exception:  # noqa: BLE001
            continue
    closed.sort(key=lambda r: r["ts"], reverse=True)
    return {
        "closed": closed,
        "open": opened,
        "n_closed": len(closed),
        "n_open": len(opened),
        "note": "forward live clips. CRYPTO-LIVE rows = real Binance fills (live_trades.csv). BeFloor gold/FX desk $ are BACKTEST (bt), not forward -- $0 forward until a real move clips.",
    }

# PANIC KILL-ALL fan-out (per-book isolation, operator's choice). The cockpit is a
# different origin from each book's GUI, so the browser POSTs same-origin to the
# cockpit and the cockpit forwards to the book's real flatten endpoint -- no CORS,
# and the cockpit never holds a close path of its own (each book closes its OWN book).
# Crypto is intentionally ABSENT: its live :8090 book is the C++ ibkrcrypto_engine,
# which has no flatten command surface yet -- a button here would be false comfort.
OMEGA_DESK = os.environ.get("OMEGA_DESK_URL", "http://45.85.3.79:7779").rstrip("/")
RDAGENT_URL = os.environ.get("RDAGENT_URL", "http://127.0.0.1:7799").rstrip("/")
FLATTEN_TARGETS = {
    "omega": OMEGA_DESK + "/api/flatten",
    "rdagent": RDAGENT_URL + "/flatten-all",
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
        if route == "/api/trades":
            try:
                self._json(_read_trades())
            except Exception as e:  # noqa: BLE001 -- never blank the panel
                self._json({"error": str(e), "closed": [], "open": []}, 200)
            return
        if route in ("/api/health", "/healthz"):
            self._json({
                "ok": True,
                "sources": {k: ROUTES[k].exists() for k in ROUTES},
            })
            return
        return super().do_GET()

    def do_POST(self):  # noqa: N802
        route = self.path.split("?")[0]
        if route.startswith("/flatten/"):
            book = route[len("/flatten/"):]
            target = FLATTEN_TARGETS.get(book)
            if not target:
                self._json({"error": f"unknown book: {book}"}, 200)
                return
            try:
                req = urllib.request.Request(target, data=b"", method="POST")
                with urllib.request.urlopen(req, timeout=15) as r:
                    body = r.read()
                try:
                    self._json(json.loads(body))
                except Exception:  # noqa: BLE001 -- pass through non-JSON downstream replies
                    self._json({"ok": True, "raw": body.decode(errors="replace")[:400]})
            except Exception as e:  # noqa: BLE001 -- downstream down/unreachable
                self._json({"error": f"{book} flatten failed: {e}", "target": target}, 200)
            return
        self._json({"error": "not found"}, 404)

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
