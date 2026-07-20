#!/usr/bin/env python3
"""
Standalone sidecar for the RD-Agent research panel.

Serves gui/index.html and routes /latest.json -> ~/Omega/data/rdagent/latest.json.
Deliberately NOT part of OmegaApiServer (that file is core/immutable per the
Omega repo rules) — this is a separate read-only process on its own port.

    python serve.py [--port 7799] [--data ~/Omega/data/rdagent/latest.json]
Open: http://127.0.0.1:7799
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

GUI_DIR = Path(__file__).resolve().parent
TOOLS = GUI_DIR.parent
QLIB_PY = "/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python"
DATA_DIR = Path.home() / "Omega" / "data" / "rdagent"
CLOSE_CANDIDATES = [DATA_DIR / "sp500_long_close.csv", DATA_DIR / "sp500_close.csv"]  # long first -> matches execute_basket.py so shown price == filled price

_PRICE_CACHE: dict[str, float] = {}
_PRICE_TS = 0.0


# S-2026-06-25 STALE-PRICE GUARD: a close is only shown if its date is within
# STALE_DAYS of the file's NEWEST date. Without this, latest_closes() returned the
# last non-empty cell per column ignoring date -> dead columns (INTC/NFLX/ADBE/DELL/
# AAPL stopped populating mid-2024) surfaced TWO-YEAR-OLD prices as "live", and the
# paper-trade button would fill at them. Stale names are dropped -> GUI shows "—".
STALE_DAYS = 5  # allow a long weekend; > this behind the newest date = drop, never show


def latest_closes() -> dict[str, float]:
    """Most-recent close per instrument (freshness-gated), cached 30 min. Fail-safe -> {} on error."""
    global _PRICE_CACHE, _PRICE_TS
    import csv as _csv, time as _time, datetime as _dt
    if _PRICE_CACHE and (_time.time() - _PRICE_TS) < 1800:
        return _PRICE_CACHE
    path = next((p for p in CLOSE_CANDIDATES if p.exists()), None)
    if not path:
        return _PRICE_CACHE
    try:
        last_date: dict[str, str] = {}   # sym -> Date of its last non-empty cell
        last_val: dict[str, float] = {}  # sym -> that value
        newest = ""                      # newest Date in the file (ISO sorts lexically)
        with open(path, newline="") as fh:
            r = _csv.reader(fh)
            syms = next(r)[1:]
            for row in r:
                if not row:
                    continue
                d = row[0]
                if d > newest:
                    newest = d
                for i, sym in enumerate(syms, start=1):
                    if i < len(row) and row[i] not in ("", "nan", "NaN"):
                        try: last_val[sym] = float(row[i]); last_date[sym] = d
                        except ValueError: pass
        try: newest_d = _dt.date.fromisoformat(newest)
        except ValueError: newest_d = None
        closes: dict[str, float] = {}
        dropped = 0
        for sym, v in last_val.items():
            fresh = True
            if newest_d is not None:
                try: fresh = (newest_d - _dt.date.fromisoformat(last_date[sym])).days <= STALE_DAYS
                except ValueError: fresh = True
            if fresh: closes[sym] = v
            else: dropped += 1
        if dropped:
            print(f"[stale-price-guard] dropped {dropped} names with closes >{STALE_DAYS}d behind {newest}", flush=True)
        _PRICE_CACHE, _PRICE_TS = closes, _time.time()
    except Exception:  # noqa: BLE001
        return _PRICE_CACHE
    return _PRICE_CACHE


class Handler(SimpleHTTPRequestHandler):
    data_path = str(Path.home() / "Omega" / "data" / "rdagent" / "latest.json")

    def end_headers(self):  # noqa: N802
        # Never let the browser cache the panel shell/JS. A committed banner-logic
        # fix MUST reach an already-open tab on reload, not sit behind a stale
        # cache. 2026-07-20: the operator's tab held the pre-10:18 calendar-day
        # banner JS and flagged Friday's (current) basket as "3 trading-days
        # behind" over a normal weekend — a false STALE. latest.json was already
        # no-store; the HTML shell was not. Add no-store idempotently for all GETs.
        if not any(h.lower().startswith(b"cache-control:")
                   for h in getattr(self, "_headers_buffer", [])):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):  # noqa: N802
        if self.path.split("?")[0] in ("/latest.json", "/data/latest.json"):
            p = Path(self.data_path)
            if not p.exists():
                self.send_error(404, "no latest.json — run export_signals.py first")
                return
            try:
                meta = json.loads(p.read_text())
                closes = latest_closes()
                if closes:
                    for item in meta.get("signal", {}).get("basket", []):
                        px = closes.get(item.get("instrument"))
                        if px is not None:
                            item["price"] = round(px, 2)
                body = json.dumps(meta).encode()
            except Exception:  # noqa: BLE001 -- never break the panel over enrichment
                body = p.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()

    def do_POST(self):  # noqa: N802
        route = self.path.split("?")[0]
        if route == "/trade-basket":
            self._trade_basket()
            return
        if route in ("/flatten-all", "/api/flatten"):
            self._flatten_all()
            return
        if route != "/promote":
            self.send_error(404)
            return
        # Run the cost-aware promote gate on whichever run the panel currently shows.
        try:
            meta = json.loads(Path(self.data_path).read_text())
            run_dir = meta["source"]["run_dir"]
            universe = meta["source"]["universe"].upper()
            provider = (
                str(Path.home() / ".qlib" / "qlib_data" / "omega_data")
                if universe in {"BIGCAP", "OMEGA-BIGCAP", "OMEGA"}
                else str(Path.home() / ".qlib" / "qlib_data" / "cn_data")
            )
            region = "us" if "omega" in provider or "omega_data" in provider else "cn"
            proc = subprocess.run(
                [QLIB_PY, str(TOOLS / "promote_faithful_bt.py"),
                 "--mlruns", run_dir, "--provider", provider, "--region", region],
                capture_output=True, text=True, timeout=300,
            )
            verdict = Path.home() / "Omega" / "data" / "rdagent" / "verdict.json"
            body = verdict.read_bytes() if proc.returncode == 0 and verdict.exists() else json.dumps(
                {"verdict": "ERROR", "note": proc.stderr.strip()[-400:] or "gate failed"}
            ).encode()
        except Exception as e:  # noqa: BLE001
            body = json.dumps({"verdict": "ERROR", "note": str(e)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _flatten_all(self):
        # PANIC KILL-ALL for THIS book (the day-mover paper basket): liquidate every
        # open position to cash via execute_basket.py --flatten. Paper/shadow only --
        # real money is NOT reachable here (live path stays gated behind --i-confirm +
        # the audited Omega live executor), same as _trade_basket.
        try:
            proc = subprocess.run(
                [sys.executable, str(TOOLS / "execute_basket.py"), "--flatten", "--mode", "shadow"],
                capture_output=True, text=True, timeout=60,
            )
            out = proc.stdout.strip().splitlines()
            body = (out[-1] if out and out[-1].startswith("{") else json.dumps(
                {"error": proc.stderr.strip()[-400:] or "flatten failed"})).encode()
        except Exception as e:  # noqa: BLE001
            body = json.dumps({"error": str(e)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _trade_basket(self):
        # Paper-trade today's BUY basket (top-5, long-only, equal-weight, shadow).
        # Real money is NOT reachable from here -- execute_basket.py --mode live is
        # gated behind --i-confirm + IB gateway and is never invoked by this route.
        try:
            proc = subprocess.run(
                [sys.executable, str(TOOLS / "execute_basket.py"),
                 "--topk", "5", "--capital", "10000", "--mode", "shadow"],
                capture_output=True, text=True, timeout=60,
            )
            out = proc.stdout.strip().splitlines()
            body = (out[-1] if out and out[-1].startswith("{") else json.dumps(
                {"error": proc.stderr.strip()[-400:] or "executor failed"})).encode()
        except Exception as e:  # noqa: BLE001
            body = json.dumps({"error": str(e)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7799)
    ap.add_argument("--data", default=str(Path.home() / "Omega" / "data" / "rdagent" / "latest.json"))
    a = ap.parse_args()
    Handler.data_path = str(Path(a.data).expanduser())
    handler = partial(Handler, directory=str(GUI_DIR))
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), handler)
    print(f"RD-Agent research panel: http://127.0.0.1:{a.port}")
    print(f"  serving {GUI_DIR}")
    print(f"  data:   {Handler.data_path}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
