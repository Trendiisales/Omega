#!/usr/bin/env python3
"""DISPLAY-TRUTH RECONCILIATION SELF-TEST — RED when the desk DISPLAYS != REALITY.

Built S-2026-07-12 (operator, angry, justified: "why do you keep missing obvious
things like missing symbols and trades — if there is data not being displayed why
are you not catching it"). Every prior guard checks PLUMBING — endpoint serves,
file fresh, hash aligned, totals fold (feeds_selftest / feedpath_selftest /
mimic_pnl_completeness_gate / trade_visibility_gate). NOTHING checked CONTENT
PARITY: that what the desk actually SHOWS (engine rosters, live cells, closed
trades, config labels) matches the authoritative producer-side reality. The
operator was the reconciliation layer. This selftest closes that permanently.

CHECKS (DISPLAYED vs AUTHORITATIVE, per desk surface):
  [1] CRYPTO-ROSTER   /api/crypto_companion legs (tags+coins) == the chimera box's
                      live cell roster (current-boot [CLIP-INIT] journal lines).
                      FAIL on: missing live cell, ghost/stale cell the box no longer
                      runs (the TRX class), legacy producer shape (legs without
                      tag/cell — pre-fix emit), or desk copy diverged from the box
                      state file (relay dead while both sides look "fresh").
  [2] TRADE-RECON     closed trades that EXIST (last 24h) vs closed trades the desk
                      SERVES: chimera data/chimera_inbound.csv + Mac ibkrcrypto
                      daily/intraday crypto_inbound.csv  ->  omega-new
                      C:\\Omega\\logs\\trades\\*.csv  ->  /api/crypto_trades rows.
                      FAIL if producer > relay or relay > endpoint beyond the relay
                      grace window. Catches "trades happened but desk blind" the
                      DAY it recurs.
  [3] SYMBOL-COVERAGE every enabled trading universe appears on its desk surface:
                      chimera 8-coin grid -> companion legs; ibkrcrypto slot keys
                      (Mac state.json) -> pushed ibkrcrypto_gui states on omega-new;
                      FX/IDX ladder pairs + BIGCAP ladder/2pct universes (parsed
                      from engine_init.hpp — the WIRED truth) -> their endpoints;
                      Omega enabled engines' symbols -> desk telemetry quote tiles
                      (non-zero quote enforced in market hours only).
  [4] CONFIG-LABELS   detector windows/thresholds SHOWN in the served state ==
                      the WIRED values ([CLIP-INIT] det=1h/+2%% vs served
                      det_w/det_thr_pct per cell). WARN on mismatch (stale label,
                      not missing money).

RESULT: marker line + exit 0 GREEN / 2 RED (WARN-only stays GREEN, is printed).
Interpreter: /usr/bin/python3 (3.9-safe: no datetime.UTC, no 3.11-only APIs).
Cron: scripts/install_crashsafe_monitor_crons.sh (30 min, crash-safe wrapped).
Negative tests: DTS_INJECT=roster_missing|roster_extra|legacy_shape|
trade_undercount|symbol_missing|config_drift (comma list) mutates the fetched
data so each check can be PROVEN to fire. Never set in cron.

This is the CONTENT-parity layer ABOVE the structural gate
(tools/trade_visibility_manifest.tsv + scripts/trade_visibility_gate.sh).
"""
from __future__ import annotations

import datetime as dt
import json
import os
import re
import subprocess
import sys
import urllib.request

CHIMERA = "chimera-direct"
VPS = "omega-new"
OMEGA = "/Users/jo/Omega"
ENGINE_INIT = OMEGA + "/include/engine_init.hpp"
MAC_CRYPTO_DAILY = "/Users/jo/Crypto/backtest/data/ibkrcrypto"
MAC_CRYPTO_INTRA = "/Users/jo/Crypto/backtest/data/ibkrcrypto_intraday"

# Relay grace (seconds a close may legitimately not yet be visible downstream):
# chimera -> omega-new relay is launchd 120s (allow 15 min); ibkrcrypto scp is
# HOURLY cron (allow 75 min). Endpoint serves at most 20 rows/source (cap).
GRACE_CHIMERA_S = 15 * 60
GRACE_IBKRCRYPTO_S = 75 * 60
ENDPOINT_CAP_PER_SRC = 20

INJECT = set(x.strip() for x in os.environ.get("DTS_INJECT", "").split(",") if x.strip())


def desk_url() -> str:
    if os.environ.get("OMEGA_DESK_URL"):
        return os.environ["OMEGA_DESK_URL"]
    try:
        out = subprocess.run(["ssh", "-G", VPS], capture_output=True, text=True, timeout=5).stdout
        for ln in out.splitlines():
            if ln.startswith("hostname "):
                return "http://%s:7779" % ln.split()[1]
    except Exception:
        pass
    return "http://45.85.3.79:7779"


DESK = desk_url()


def http_json(path, timeout=12, retries=2, empty_probe=None):
    """GET DESK+path as JSON. Retries transient failures AND transient-empty payloads:
    the companion relay scp's state files onto omega-new NON-atomically, so a read that
    races the push can see the endpoint's empty default ({"ts":0,"legs":[]}) for a
    moment. empty_probe(j)->True marks such a payload as retry-worthy (finding
    S-2026-07-12: observed live — 2 of ~10 fetches hit timeout/empty-default)."""
    import time
    last = {"__err__": "unfetched"}
    for i in range(retries + 1):
        try:
            with urllib.request.urlopen(DESK + path, timeout=timeout) as r:
                j = json.load(r)
            if empty_probe is None or not empty_probe(j):
                return j
            last = j
        except Exception as e:
            last = {"__err__": str(e)}
        if i < retries:
            time.sleep(4)
    return last


# ── ONE ssh to the chimera box: authoritative roster + producer trade record ──
CHIMERA_PROBE = r'''
import json, subprocess, re, os
out = {}
try:
    out["active"] = subprocess.run(["systemctl", "is-active", "chimera"],
                                   capture_output=True, text=True, timeout=10).stdout.strip()
except Exception as e:
    out["active"] = "probe-err:%s" % e
# current-process CLIP-INIT roster: group journal lines by PID, keep the LAST pid's block
clips = {}
try:
    j = subprocess.run(["journalctl", "-u", "chimera", "--since", "-30 days", "--no-pager"],
                       capture_output=True, text=True, timeout=60).stdout
    by_pid = {}
    for ln in j.splitlines():
        m = re.search(r"chimera\[(\d+)\]: \[CLIP-INIT\] (\S+) -> det=(\d+)h/\+(\d+(?:\.\d+)?)%", ln)
        if m:
            by_pid.setdefault(m.group(1), {})[m.group(2)] = [int(m.group(3)), float(m.group(4))]
        else:
            m2 = re.search(r"chimera\[(\d+)\]: \[CLIP-INIT\]", ln)
            if m2:
                by_pid.setdefault(m2.group(1), {})
    if by_pid:
        # journal is time-ordered; the pid of the LAST CLIP-INIT line = current roster
        last_pid = None
        for ln in j.splitlines():
            m = re.search(r"chimera\[(\d+)\]: \[CLIP-INIT\]", ln)
            if m:
                last_pid = m.group(1)
        clips = by_pid.get(last_pid, {})
except Exception as e:
    out["journal_err"] = str(e)
out["clip_init"] = clips
# the producer's own emitted state (for desk-copy divergence check)
try:
    st = json.load(open("/home/jo/ChimeraCrypto/data/crypto_companion_state.json"))
    out["state_ts"] = st.get("ts", 0)
    out["state_tags"] = sorted(l.get("tag", "") for l in st.get("legs", []))
except Exception as e:
    out["state_err"] = str(e)
# producer trade record (closed shadow trades exported for the desk)
rows = []
try:
    with open("/home/jo/ChimeraCrypto/data/chimera_inbound.csv") as f:
        for ln in f:
            p = ln.strip().split(",")
            if len(p) >= 9 and p[0] != "id":
                rows.append([int(p[2]), p[3], p[4]])
except Exception:
    pass
out["inbound"] = rows[-500:]
# journal DESK_EXPORT lines (last 24h) — the export hook's own record
try:
    j24 = subprocess.run(["journalctl", "-u", "chimera", "--since", "-24 hours", "--no-pager"],
                         capture_output=True, text=True, timeout=60).stdout
    out["desk_export_24h"] = sum(1 for ln in j24.splitlines() if "[DESK_EXPORT]" in ln)
except Exception:
    out["desk_export_24h"] = -1
print(json.dumps(out))
'''


def probe_chimera():
    try:
        r = subprocess.run(["ssh", CHIMERA, "python3", "-"], input=CHIMERA_PROBE,
                           capture_output=True, text=True, timeout=90)
        if r.returncode != 0:
            return {"__err__": "ssh rc=%d %s" % (r.returncode, r.stderr.strip()[:120])}
        return json.loads(r.stdout.strip().splitlines()[-1])
    except Exception as e:
        return {"__err__": str(e)}


# ── ONE ssh to omega-new: inbound CSVs + pushed ibkrcrypto panel states ────────
VPS_FILES = [
    ("crypto_inbound", r"C:\Omega\logs\trades\crypto_inbound.csv"),
    ("crypto_intraday_inbound", r"C:\Omega\logs\trades\crypto_intraday_inbound.csv"),
    ("chimera_inbound", r"C:\Omega\logs\trades\chimera_inbound.csv"),
    ("gui_state_daily", r"C:\Omega\ibkrcrypto_gui\state.json"),
    ("gui_state_intraday", r"C:\Omega\ibkrcrypto_gui\state_intraday.json"),
]


def probe_vps():
    # cmd `type` chain with unique markers; the whole command MUST start `ssh omega-new`.
    # `echo.` before each marker forces a newline: a file WITHOUT a trailing newline
    # (json.dump output) otherwise merges the next marker onto its last line and the
    # following file silently concatenates into the previous one (found on first run).
    parts = []
    for i, (_, path) in enumerate(VPS_FILES):
        parts.append("echo. & echo @@F%d@@ & type %s 2>nul" % (i, path))
    cmd = "cmd /c \"" + " & ".join(parts) + "\""
    try:
        r = subprocess.run(["ssh", VPS, cmd], capture_output=True, text=True, timeout=60)
    except Exception as e:
        return {"__err__": str(e)}
    if r.returncode != 0 and not r.stdout:
        return {"__err__": "ssh rc=%d" % r.returncode}
    out, cur = {}, None
    for ln in r.stdout.splitlines():
        m = re.search(r"@@F(\d+)@@", ln)
        if m:
            # tolerant: text BEFORE an in-line marker still belongs to the previous file
            head = ln[:m.start()]
            if cur is not None and head:
                out[cur].append(head)
            cur = VPS_FILES[int(m.group(1))][0]
            out[cur] = []
        elif cur is not None:
            out[cur].append(ln)
    return {k: "\n".join(v) for k, v in out.items()}


def csv_close_rows(text):
    """[(exit_ts, sym, strat)] from an inbound-schema csv body."""
    rows = []
    for ln in (text or "").splitlines():
        p = ln.strip().split(",")
        if len(p) >= 9 and p[0] and p[0] != "id":
            try:
                rows.append((int(p[2]), p[3], p[4]))
            except ValueError:
                continue
    return rows


def read_file(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


# ── engine_init.hpp = the WIRED truth (rosters parsed from source, not memory) ─
def parse_cfg_array(src, decl_re):
    """Active (non-commented) quoted first-fields of a `static const X[] = {...}` block."""
    m = re.search(decl_re, src)
    if not m:
        return []
    depth, i, body = 0, m.end() - 1, ""
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                break
        body += src[i]
        i += 1
    out = []
    for ln in body.splitlines():
        s = ln.strip()
        if s.startswith("//"):
            continue
        mm = re.match(r'\{\s*"([A-Z0-9._]+)"', s)
        if mm:
            out.append(mm.group(1))
    return out


def parse_name_array(src, var):
    m = re.search(r"static const char\* %s\[\]\s*=\s*\{(.*?)\};" % var, src, re.S)
    if not m:
        return []
    names = []
    for ln in m.group(1).splitlines():
        s = ln.strip()
        if s.startswith("//"):
            continue
        names.extend(re.findall(r'"([A-Z0-9.]+)"', s))
    return names


# enabled entry-engine -> desk telemetry quote key (None = no dedicated tile; reported)
ENGINE_TELEM_KEY = [
    (r"^g_connors_ger", "ger30"), (r"^g_connors_nas", "nas"),
    (r"^g_dj30_turtle", "dj"), (r"^g_nas_turtle", "nas"), (r"^g_spx_turtle", "sp"),
    (r"^g_idx_bear_short_nas", "nas"), (r"^g_idx_bear_short_sp", "sp"),
    (r"^g_gold_", "gold"), (r"^g_xau_", "gold"), (r"^g_rider_", "gold"),
    (r"^g_mgc_", "gold"),   # MGC = micro gold future; desk gold tile is its price surface
    (r"^g_survivor", None),  # multi-symbol portfolio, no single tile
]
ENGINE_DENY = {"g_regime_adaptor", "g_aurora_gate", "g_macro_gold_gate", "g_engine_heartbeat",
               "g_risk_monitor", "g_macro_ctx", "g_adaptive_risk", "g_corr_matrix"}


def us_index_market_open(now=None):
    now = now or dt.datetime.now(dt.timezone.utc)
    wd, hr = now.weekday(), now.hour
    if wd == 5:
        return False
    if wd == 6:
        return False          # Sun UTC: reopen ~22:00 but bar/quote warm-up; skip honestly
    if wd == 4 and hr >= 22:
        return False
    return True


# Per-telemetry-key liveness window (UTC). US index/gold futures run ~24h (Sun 22:00
# reopen) -> us_index_market_open. GER40/DAX has its OWN session: the engine's boot
# heartbeat registers `session=07-22 UTC` (Ger40 live_required only 07-22 UTC), so a
# 0 quote OUTSIDE that window is HONEST, not a divergence. Applying the US 24h window
# to ger30 cried wolf every Sun22:00-Mon07:00 (false RED). Match the engine's session.
def key_market_open(key, now=None):
    now = now or dt.datetime.now(dt.timezone.utc)
    if key == "ger30":
        wd, hr = now.weekday(), now.hour
        if wd >= 5:            # Sat/Sun closed
            return False
        return 7 <= hr < 22    # GER40 feed session 07-22 UTC (matches boot heartbeat)
    return us_index_market_open(now)


def main() -> int:
    checks = []
    red = warn = 0

    def rec(state, tag, msg):
        nonlocal red, warn
        if state == "FAIL":
            red += 1
        elif state == "WARN":
            warn += 1
        checks.append("  %-4s [%s] %s" % (state, tag, msg))

    now_s = dt.datetime.now(dt.timezone.utc).timestamp()

    # ── fetch everything (each failure degrades to an explicit FAIL, never a crash) ──
    served_cc = http_json("/api/crypto_companion",
                          empty_probe=lambda j: not j.get("legs"))
    served_ct = http_json("/api/crypto_trades")
    served_fx = http_json("/api/fxladder_companion")
    served_ix = http_json("/api/idxladder_companion")
    served_sl = http_json("/api/stockladder_companion")
    served_b2 = http_json("/api/bigcap2pct_companion")
    served_tm = http_json("/api/telemetry")
    chim = probe_chimera()
    vps = probe_vps()
    einit = read_file(ENGINE_INIT)

    # ── negative-test injections (DTS_INJECT — proves each check can fire) ──────
    if "roster_missing" in INJECT and isinstance(served_cc, dict):
        served_cc["legs"] = [l for l in served_cc.get("legs", []) if l.get("sym") != "BTC"]
    if "roster_extra" in INJECT and isinstance(served_cc, dict):
        served_cc.setdefault("legs", []).append(
            {"sym": "FAKE", "tag": "FAKE-UJ9-CLIP", "cell": "UJ9", "det_w": 1, "det_thr_pct": 9.0})
    if "legacy_shape" in INJECT and isinstance(served_cc, dict):
        served_cc["legs"] = [{k: v for k, v in l.items() if k not in ("tag", "cell")}
                             for l in served_cc.get("legs", [])]
    if "trade_undercount" in INJECT and isinstance(chim, dict):
        old = int(now_s) - 3600
        chim.setdefault("inbound", []).extend([[old, "BTC", "INJ-TEST"]] * 3)
    if "symbol_missing" in INJECT and isinstance(served_fx, dict):
        served_fx["pairs"] = [p for p in served_fx.get("pairs", []) if p.get("pair") != "GBPUSD"]
    if "config_drift" in INJECT and isinstance(served_cc, dict):
        for l in served_cc.get("legs", []):
            if l.get("tag", "").startswith("BTC-"):
                l["det_thr_pct"] = float(l.get("det_thr_pct", 0)) + 7.0

    # ═══ [1] CRYPTO COMPANION ROSTER PARITY ══════════════════════════════════
    clip = chim.get("clip_init") or {}
    legs = served_cc.get("legs", []) if isinstance(served_cc, dict) else []
    if "__err__" in chim:
        rec("FAIL", "CRYPTO-ROSTER", "chimera box unreachable (%s) — authoritative roster UNVERIFIABLE" % chim["__err__"])
    elif chim.get("active") != "active":
        rec("FAIL", "CRYPTO-ROSTER", "chimera.service NOT active (%s) — no live roster to reconcile" % chim.get("active"))
    elif "__err__" in served_cc:
        rec("FAIL", "CRYPTO-ROSTER", "/api/crypto_companion unreachable: %s" % served_cc["__err__"])
    elif not legs:
        rec("FAIL", "CRYPTO-ROSTER", "desk serves ZERO companion legs while box runs %d cells" % len(clip))
    elif not all(("tag" in l and "cell" in l) for l in legs):
        rec("FAIL", "CRYPTO-ROSTER", "LEGACY producer shape — legs lack tag/cell (pre-fix emit_companion_state; "
                                     "desk cannot attribute cells; producer/relay running OLD binary or stale copy)")
    elif not clip:
        rec("WARN", "CRYPTO-ROSTER", "no [CLIP-INIT] lines recoverable from 30d journal (rotated?) — "
                                     "producer-drift not checkable this run; desk-vs-box copy check still below")
    else:
        served_tags = set(l.get("tag", "") for l in legs)
        clip_tags = set(clip.keys())
        missing = sorted(clip_tags - served_tags)
        ghost = sorted(served_tags - clip_tags)
        if missing:
            rec("FAIL", "CRYPTO-ROSTER", "live cells MISSING from desk state: %s" % ",".join(missing))
        if ghost:
            rec("FAIL", "CRYPTO-ROSTER", "desk shows cells the box does NOT run (stale/ghost, the TRX class): %s" % ",".join(ghost))
        if not missing and not ghost:
            coins = sorted(set(t.split("-")[0] for t in clip_tags))
            rec("PASS", "CRYPTO-ROSTER", "%d/%d cells match boot [CLIP-INIT] exactly (%s)" %
                (len(served_tags), len(clip_tags), "/".join(coins)))
    # desk copy divergence vs the box's own state file (relay content parity)
    if isinstance(served_cc, dict) and "__err__" not in served_cc and chim.get("state_ts"):
        lag = int(chim["state_ts"]) - int(served_cc.get("ts", 0))
        if lag > 30 * 60:
            rec("FAIL", "CRYPTO-ROSTER", "desk copy is %dmin BEHIND the box state file — relay diverged "
                                         "(box ts=%s desk ts=%s)" % (lag // 60, chim["state_ts"], served_cc.get("ts")))
        else:
            rec("PASS", "CRYPTO-ROSTER", "desk copy in sync with box state file (lag %ds <= 30min)" % max(lag, 0))

    # ═══ [2] TRADE-COUNT RECONCILIATION (closed trades, last 24h) ═════════════
    day_ago = now_s - 24 * 3600
    ep_rows = served_ct if isinstance(served_ct, list) else []
    ep_24h = {}
    for r_ in ep_rows:
        if float(r_.get("exitTs", 0)) >= day_ago:
            ep_24h[r_.get("book", "?")] = ep_24h.get(r_.get("book", "?"), 0) + 1

    def recon(label, producer_rows, vps_text, grace_s, endpoint_book, n_srcs):
        if isinstance(served_ct, dict) and "__err__" in served_ct:
            rec("FAIL", "TRADE-RECON", "%s: /api/crypto_trades unreachable: %s" % (label, served_ct["__err__"]))
            return
        a = sum(1 for (ts, _, _) in producer_rows if day_ago <= ts <= now_s - grace_s)
        a_fresh = sum(1 for (ts, _, _) in producer_rows if ts > now_s - grace_s)
        if vps_text is None:
            rec("FAIL", "TRADE-RECON", "%s: omega-new inbound csv UNREADABLE — relay leg unverifiable" % label)
            return
        b_rows = csv_close_rows(vps_text)
        b = sum(1 for (ts, _, _) in b_rows if ts >= day_ago)
        c = ep_24h.get(endpoint_book, 0)
        b_cap = min(b, ENDPOINT_CAP_PER_SRC * n_srcs)
        if a > b:
            rec("FAIL", "TRADE-RECON", "%s: %d closes exist at producer (24h, past %dmin grace) but only %d "
                "reached omega-new — TRADES INVISIBLE TO DESK" % (label, a, grace_s // 60, b))
        elif b_cap > c:
            rec("FAIL", "TRADE-RECON", "%s: %d rows landed on omega-new (24h) but endpoint serves %d — "
                "desk render starved" % (label, b_cap, c))
        else:
            extra = " (+%d in relay grace)" % a_fresh if a_fresh else ""
            rec("PASS", "TRADE-RECON", "%s: producer=%d -> omega-new=%d -> endpoint=%d (24h)%s" %
                (label, a, b, c, extra))

    chim_rows = [(int(r_[0]), r_[1], r_[2]) for r_ in chim.get("inbound", [])] if "__err__" not in chim else []
    if "__err__" in chim:
        rec("FAIL", "TRADE-RECON", "chimera: box unreachable — producer close-count unverifiable")
    else:
        recon("chimera", chim_rows, vps.get("chimera_inbound") if "__err__" not in vps else None,
              GRACE_CHIMERA_S, "chimera", 1)
        de = chim.get("desk_export_24h", -1)
        chim_24h = sum(1 for (ts, _, _) in chim_rows if ts >= day_ago)
        if de >= 0 and de != chim_24h:
            rec("WARN", "TRADE-RECON", "chimera: journal [DESK_EXPORT]=%d vs inbound csv rows=%d in 24h "
                "(export hook / csv divergence)" % (de, chim_24h))
    # S-2026-07-12 CONSOLIDATION: the Mac ibkrcrypto book was folded onto the ONE Chimera
    # system (josgp1) and DROPPED from the desk (buildCryptoTradesJson serves chimera only).
    # Reconciling it here FALSE-REDs ("desk starved") because old csv rows still exist but the
    # endpoint intentionally serves none. Retired — only the live Chimera book is reconciled.

    # ═══ [3] SYMBOL COVERAGE (enabled universe -> desk surface) ═══════════════
    # 3a chimera coin grid -> companion legs
    if clip and legs:
        want = set(t.split("-")[0] for t in clip.keys())
        got = set(l.get("sym", "") for l in legs)
        miss = sorted(want - got)
        if miss:
            rec("FAIL", "SYMBOL-COV", "chimera grid coins missing from desk panel: %s" % ",".join(miss))
        else:
            rec("PASS", "SYMBOL-COV", "chimera grid: all %d coins on the desk panel" % len(want))
    # 3a2 S-2026-07-13: TRADED-vs-TICKER parity — every symbol the chimera box has actually
    # traded (companion CLIP-INIT roster + inbound-csv close history) must have a top-bar
    # tile (ctk_<SYM>) in the SERVED desk HTML. Root cause class: TIA-TSMOM banked +7.65
    # with NO tile — the ticker list was hand-grown per-book and never reconciled.
    try:
        traded = set()
        if clip:
            traded |= set(t.split("-")[0] for t in clip.keys())
        if "__err__" not in chim:
            for r_ in chim.get("inbound", []):
                try:
                    s_ = str(r_[1]).strip().upper()
                    if s_ and s_.isalnum():
                        traded.add(s_)
                except Exception:
                    pass
        import urllib.request as _ur
        with _ur.urlopen(DESK + "/", timeout=12) as _r:
            _html = _r.read().decode("utf-8", "replace")
        # tiles are JS-built at runtime — parse the CTKS ticker array from the served page
        # (each entry ['SYM','SYMUSDT']), not literal DOM ids.
        tiles = set(re.findall(r"\['([A-Z0-9]+)','[A-Z0-9]+USDT'\]", _html))
        miss2 = sorted(t for t in traded if t and t not in tiles)
        if miss2:
            rec("FAIL", "SYMBOL-COV", "TRADED symbols with NO top-bar tile: %s" % ",".join(miss2))
        else:
            rec("PASS", "SYMBOL-COV", "ticker parity: all %d traded symbols have a top-bar tile" % len(traded))
    except Exception as e:
        rec("WARN", "SYMBOL-COV", "ticker-parity check unavailable: %s" % e)
    # 3b ibkrcrypto slot keys -> pushed panel states on omega-new
    # S-2026-07-12 CONSOLIDATION: ibkrcrypto book RETIRED (folded onto Chimera/josgp1). Its
    # pushed panel states are frozen and no longer displayed -> nothing to reconcile. Skipped.
    for label, mac_path, vps_key in ():
        try:
            mac_slots = set(s.get("key", "") for s in json.loads(read_file(mac_path)).get("slots", []))
        except Exception as e:
            rec("FAIL", "SYMBOL-COV", "%s: Mac state unreadable (%s)" % (label, e))
            continue
        if "__err__" in vps:
            rec("FAIL", "SYMBOL-COV", "%s: omega-new unreachable — pushed panel state unverifiable" % label)
            continue
        try:
            vps_slots = set(s.get("key", "") for s in json.loads(vps.get(vps_key) or "{}").get("slots", []))
        except Exception:
            rec("FAIL", "SYMBOL-COV", "%s: pushed state on omega-new missing/unparseable (%s)" % (label, vps_key))
            continue
        miss = sorted(mac_slots - vps_slots)
        if "symbol_missing" in INJECT and label == "ibkrcrypto-daily" and mac_slots:
            miss = sorted(mac_slots)[:1]
        if miss:
            rec("FAIL", "SYMBOL-COV", "%s: book slots invisible on desk copy: %s" % (label, ",".join(miss)))
        else:
            rec("PASS", "SYMBOL-COV", "%s: all %d slots present in pushed desk state" % (label, len(mac_slots)))
    # 3c/3d/3e wired rosters (engine_init.hpp) -> endpoints
    for label, wired, served, field, key in (
            ("fx-ladder", parse_cfg_array(einit, r"static const FLCfg FL\[\]\s*=\s*\{"), served_fx, "pairs", "pair"),
            ("idx-ladder", parse_cfg_array(einit, r"static const ILCfg IL\[\]\s*=\s*\{"), served_ix, "pairs", "pair"),
            ("bigcap-ladder", parse_name_array(einit, "BIGCAP_LAD"), served_sl, "names", "sym"),
            ("bigcap-2pct", parse_name_array(einit, "BC2_UNIV"), served_b2, "names", "sym")):
        if not wired:
            rec("WARN", "SYMBOL-COV", "%s: could not parse wired roster from engine_init.hpp (source drifted?)" % label)
            continue
        if not isinstance(served, dict) or "__err__" in served:
            rec("FAIL", "SYMBOL-COV", "%s: endpoint unreachable: %s" % (label, served.get("__err__", "?")))
            continue
        got = set(x.get(key, "") for x in served.get(field, []))
        miss = sorted(set(wired) - got)
        extra = sorted(got - set(wired))
        if miss:
            rec("FAIL", "SYMBOL-COV", "%s: WIRED-but-undisplayed: %s (of %d wired)" % (label, ",".join(miss), len(wired)))
        else:
            rec("PASS", "SYMBOL-COV", "%s: all %d wired symbols displayed%s" %
                (label, len(wired), " (+%d retired-history rows: %s)" % (len(extra), ",".join(extra[:5])) if extra else ""))
        if extra and not miss:
            rec("WARN", "SYMBOL-COV", "%s: endpoint carries non-wired symbols %s — retired history or stale state" %
                (label, ",".join(extra[:8])))
    # 3f Omega enabled engines -> telemetry quote tiles
    enabled = sorted(set(m.group(1) for m in re.finditer(r"(g_[A-Za-z0-9_]+)\.enabled\s*=\s*true", einit))
                     - ENGINE_DENY)
    if not enabled:
        rec("WARN", "SYMBOL-COV", "no enabled engines parsed from engine_init.hpp — parser drift")
    elif not isinstance(served_tm, dict) or "__err__" in served_tm:
        rec("FAIL", "SYMBOL-COV", "engines: /api/telemetry unreachable: %s" % served_tm.get("__err__", "?"))
    else:
        unmapped, dead_keys, dark_quotes = [], [], []
        open_now = us_index_market_open()   # for the PASS/SKIP summary wording
        any_open = False
        for g in enabled:
            key = "__none__"
            for pat, k in ENGINE_TELEM_KEY:
                if re.match(pat, g):
                    key = k
                    break
            if key == "__none__":
                unmapped.append(g)
                continue
            if key is None:
                continue
            if (key + "_bid") not in served_tm:
                dead_keys.append("%s->%s" % (g, key))
            else:
                k_open = key_market_open(key)   # per-symbol session (GER40 != US 24h)
                any_open = any_open or k_open
                if k_open and float(served_tm.get(key + "_bid", 0)) <= 0:
                    dark_quotes.append("%s->%s" % (g, key))
        open_now = open_now or any_open
        if dead_keys:
            rec("FAIL", "SYMBOL-COV", "engines: telemetry MISSING quote keys for enabled engines: %s" % ",".join(dead_keys))
        if dark_quotes:
            rec("FAIL", "SYMBOL-COV", "engines: enabled-engine symbols DARK on desk in market hours: %s" %
                ",".join(sorted(set(dark_quotes))))
        if not dead_keys and not dark_quotes:
            rec("PASS" if open_now else "SKIP", "SYMBOL-COV",
                "engines: %d enabled engines' symbols on desk tiles%s" %
                (len(enabled), "" if open_now else " (key-presence only — market closed, quotes not enforced)"))
        if unmapped:
            rec("WARN", "SYMBOL-COV", "engines with no telemetry-key mapping (extend ENGINE_TELEM_KEY): %s" %
                ",".join(unmapped))

    # ═══ [4] CONFIG-LABEL FRESHNESS (served detector labels == wired) ═════════
    if clip and legs and all("tag" in l for l in legs):
        drift = []
        for l in legs:
            want = clip.get(l.get("tag"))
            if not want:
                continue
            w_h, thr = want
            if int(l.get("det_w", -1)) != w_h or abs(float(l.get("det_thr_pct", -1)) - thr) > 1e-6:
                drift.append("%s shown det=%sh/+%s%% wired det=%dh/+%g%%" %
                             (l.get("tag"), l.get("det_w"), l.get("det_thr_pct"), w_h, thr))
        if drift:
            rec("WARN", "CONFIG-LABEL", "detector labels on desk DIFFER from wired [CLIP-INIT] values: " +
                "; ".join(drift[:6]) + (" (+%d more)" % (len(drift) - 6) if len(drift) > 6 else ""))
        else:
            rec("PASS", "CONFIG-LABEL", "all %d served detector labels match wired [CLIP-INIT] det windows/thresholds"
                % len(legs))
    else:
        rec("SKIP", "CONFIG-LABEL", "needs both boot [CLIP-INIT] roster and tagged legs (see CRYPTO-ROSTER above)")

    # ── verdict ────────────────────────────────────────────────────────────────
    verdict = ("RED — DESK DISPLAY DIVERGES FROM REALITY (%d mismatch%s)" % (red, "es" if red != 1 else "")
               ) if red else ("GREEN — displayed content matches authoritative reality" +
                              (" (%d WARN)" % warn if warn else ""))
    print("DISPLAY-TRUTH SELF-TEST " + ("RED" if red else "GREEN") + "  -- " + verdict)
    for c in checks:
        print(c)
    if INJECT:
        print("  NOTE: DTS_INJECT=%s active — this run is a NEGATIVE TEST, not system truth" % ",".join(sorted(INJECT)))
    print("-> Plumbing guards say the pipe works; THIS guard says the water is the right water.")
    print("RESULT: " + ("RED" if red else "GREEN"))
    return 2 if red else 0


if __name__ == "__main__":
    sys.exit(main())
