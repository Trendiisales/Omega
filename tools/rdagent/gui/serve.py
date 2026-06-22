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


class Handler(SimpleHTTPRequestHandler):
    data_path = str(Path.home() / "Omega" / "data" / "rdagent" / "latest.json")

    def do_GET(self):  # noqa: N802
        if self.path.split("?")[0] in ("/latest.json", "/data/latest.json"):
            p = Path(self.data_path)
            if not p.exists():
                self.send_error(404, "no latest.json — run export_signals.py first")
                return
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

    def _trade_basket(self):
        # Paper-trade today's BUY basket (top-5, long-only, equal-weight, shadow).
        # Real money is NOT reachable from here -- execute_basket.py --mode live is
        # gated behind --i-confirm + IB gateway and is never invoked by this route.
        try:
            proc = subprocess.run(
                [sys.executable, str(TOOLS / "execute_basket.py"),
                 "--topk", "5", "--capital", "100000", "--mode", "shadow"],
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
