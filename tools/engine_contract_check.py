#!/usr/bin/env python3
# engine_contract_check.py -- pre-deploy engine wiring sanity check.
#
# Why this exists
# ---------------
# 2026-05-26: Hours wasted diagnosing GoldScalpPyramid bracket window stuck at
# range=0.00. Root cause: HybridBracketGold engine deliberately disabled via
# globals.hpp::g_disable_bracket_gold=true since 2026-04-30 audit, but its
# diagnostic emitter in tick_gold.hpp:1418 still fires every 10s showing
# "can_enter=1 can_arm=1" -- a misleading green-light for a dead engine.
#
# This script catches that class of inconsistency BEFORE deploy by static-
# analysing the engine wiring across engine_init.hpp / globals.hpp / tick_*.hpp
# and reporting mismatches.
#
# Checks (each runs even if earlier ones fail):
#   1. DISABLED+DIAG -- engine has g_disable_*=true but emits diagnostic.
#   2. ENABLED+NO_PULSE -- engine declared enabled=true (and not gated by
#      g_disable_*) but never pulsed in tick_*.hpp.
#   3. PULSED+NO_REGISTER -- engine pulses to heartbeat but is never
#      registered in init_engines() (heartbeat won't track misses).
#   4. WARMUP_MISSING -- engine declares warmup_csv_path that doesn't exist
#      on disk.
#   5. SHADOW_STALE -- engine has shadow_mode=true and enabled=true but trade
#      ledger shows no trades in last 14 days (skipped if --no-runtime).
#
# Usage:
#   python3 tools/engine_contract_check.py
#   python3 tools/engine_contract_check.py --strict      # nonzero exit on WARN
#   python3 tools/engine_contract_check.py --no-runtime  # static-only
#
# Output: stderr human report + stdout machine-readable summary (one line per
#         finding: LEVEL\tCHECK\tENGINE\tDETAIL).

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Hand-rolled aliases for engine handles whose register_engine name doesn't
# textually contain the handle slug (e.g. "XauTrendFollow1h" vs xau_tf_1h).
# Extend as engines are added. Keys are engine handles, values are the
# pulse/register name used in code.
PULSE_NAME_ALIAS = {
    "xau_tf_1h":           "XauTrendFollow1h",
    "xau_tf_2h":           "XauTrendFollow2h",
    "xau_tf_4h":           "XauTrendFollow4h",
    "xau_tf_d1":           "XauTrendFollowD1",
    "xau_d55_gated_m30":   "XauDonchian55GatedM30",
    "c1_retuned":          "C1Retuned",
    "ema_pullback":        "EmaPullback",
    "tsmom_v2":            "TsmomV2",
    "pdhl_rev":            "PdhlRev",
    "amr_eurusd":          "AmrEurusd",
    "amr_gbpusd":          "AmrGbpusd",
    "amr_ger40":           "AmrGer40",
    "amr_nas100":          "AmrNas100",
    "amr_us500":           "AmrUs500",
    "eur_gbp_pairs":       "EurGbpPairs",
    "eurusd_turtle_h4":    "EurusdTurtleH4",
    "gbpusd_turtle_h4":    "GbpusdTurtleH4",
    "vwap_rev_eurusd":     "VwapRevEurusd",
    "vwap_rev_ger40":      "VwapRevGer40",
    "ger40_london_brk":    "Ger40LondonBrk",
    "ger40_turtle_h4":     "Ger40TurtleH4",
    "minimal_h4_ger40":    "MinimalH4Ger40",
    "minimal_h4_us30":     "MinimalH4US30",
    "us30_ensemble":       "Us30Ensemble",
    "us30_3bar_mom_h1":    "Us30_3BarMomH1",
    "orb_dj30":            "OrbDj30",
    "orb_nas100":          "OrbNas100",
    "nas_bbrev_long_h1":   "NasBbRevLongH1",
}

# Header-search globs. Tick handlers live in tick_*.hpp; pulse calls only
# appear in those, plus on_tick.hpp for indices.
TICK_FILES = [REPO_ROOT / "include" / "tick_gold.hpp",
              REPO_ROOT / "include" / "tick_fx.hpp",
              REPO_ROOT / "include" / "tick_indices.hpp",
              REPO_ROOT / "include" / "on_tick.hpp"]
ENGINE_INIT = REPO_ROOT / "include" / "engine_init.hpp"
GLOBALS = REPO_ROOT / "include" / "globals.hpp"

# Regexes
RX_ENABLED = re.compile(r"^\s*g_(\w+)\.enabled\s*=\s*(true|false)\s*;")
RX_SHADOW = re.compile(r"^\s*g_(\w+)\.shadow_mode\s*=\s*(true|false)\s*;")
RX_WARMUP = re.compile(r"^\s*g_(\w+)\.warmup_csv_path\s*=\s*\"([^\"]+)\"")
RX_DISABLE = re.compile(r"^\s*static\s+bool\s+g_disable_(\w+)\s*=\s*(true|false)\s*;")
RX_REGISTER = re.compile(r"g_engine_heartbeat\.register_engine\(\s*\"([^\"]+)\"")
RX_PULSE = re.compile(r"g_engine_heartbeat\.pulse\(\s*\"([^\"]+)\"\s*\)")
RX_DIAG_REF = re.compile(r"g_(\w+)\.(phase|bracket_high|bracket_low|has_open_position\(\))")


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def scan_engines() -> dict:
    """Build {name: {'enabled': bool|None, 'shadow': bool|None, 'warmup': path|None}}."""
    out: dict[str, dict] = defaultdict(dict)
    txt = read(ENGINE_INIT)
    for line in txt.splitlines():
        if m := RX_ENABLED.match(line):
            out[m.group(1)]["enabled"] = (m.group(2) == "true")
        if m := RX_SHADOW.match(line):
            out[m.group(1)]["shadow"] = (m.group(2) == "true")
        if m := RX_WARMUP.match(line):
            out[m.group(1)]["warmup"] = m.group(2)
    return out


def scan_disable_flags() -> dict[str, bool]:
    out: dict[str, bool] = {}
    for line in read(GLOBALS).splitlines():
        if m := RX_DISABLE.match(line):
            out[m.group(1)] = (m.group(2) == "true")
    return out


def scan_pulses() -> set[str]:
    s: set[str] = set()
    for f in TICK_FILES:
        if not f.exists():
            continue
        for m in RX_PULSE.finditer(read(f)):
            s.add(m.group(1))
    return s


def scan_registers() -> set[str]:
    s: set[str] = set()
    for f in [ENGINE_INIT, *TICK_FILES]:
        if not f.exists():
            continue
        for m in RX_REGISTER.finditer(read(f)):
            s.add(m.group(1))
    return s


def scan_diag_refs() -> dict[str, list[tuple[Path, int]]]:
    """Find places that reference engine internals in stdout/cout lines."""
    out: dict[str, list[tuple[Path, int]]] = defaultdict(list)
    for f in TICK_FILES:
        if not f.exists():
            continue
        lines = read(f).splitlines()
        # crude: look for std::cout or printf within ~30 lines of g_<name>.phase ref
        for i, line in enumerate(lines):
            if "GOLD-BRK-DIAG" in line or "DIAG]" in line:
                # find the engine identifier referenced in next 40 lines
                for j in range(i, min(i + 40, len(lines))):
                    for m in RX_DIAG_REF.finditer(lines[j]):
                        out[m.group(1)].append((f, i + 1))
                        break
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="nonzero exit if any WARN findings")
    ap.add_argument("--no-runtime", action="store_true",
                    help="skip checks that need ledger / runtime state")
    args = ap.parse_args()

    engines = scan_engines()
    disable_flags = scan_disable_flags()
    pulses = scan_pulses()
    registers = scan_registers()
    diag_refs = scan_diag_refs()

    findings: list[tuple[str, str, str, str]] = []

    # CHECK 1 -- DISABLED+DIAG
    # An engine flagged true in g_disable_* AND still referenced by a diag
    # emitter is the HybridBracketGold class of bug. Dedupe (engine, file:line).
    seen_diag = set()
    for flag, is_disabled in disable_flags.items():
        if not is_disabled:
            continue
        for engine, sites in diag_refs.items():
            if flag.endswith(engine) or engine.endswith(flag) or flag == engine:
                for f, ln in sites:
                    key = (engine, str(f), ln)
                    if key in seen_diag:
                        continue
                    seen_diag.add(key)
                    findings.append((
                        "WARN", "DISABLED+DIAG",
                        f"g_{engine}",
                        f"g_disable_{flag}=true but diag emits at "
                        f"{f.relative_to(REPO_ROOT)}:{ln}"
                    ))

    # CHECK 2 -- ENABLED+NO_PULSE
    # Engine declared enabled=true and not gated by g_disable_* must pulse
    # the heartbeat or it will never be tracked. Matching is loose --
    # pulse() calls use friendlier names (e.g. "GoldScalpPyramid") that
    # don't textually match snake_case engine handles ("gold_scalp_pyramid").
    # Normalise both sides: lowercase + strip underscores.
    def norm(s: str) -> str:
        return s.lower().replace("_", "")

    pulses_norm = {norm(p) for p in pulses}
    for engine, cfg in engines.items():
        if cfg.get("enabled") is not True:
            continue
        # If a matching disable_flag is true, the engine is effectively dead
        # and this check doesn't apply.
        gated_off = any(
            disable_flags.get(flag, False)
            and (flag.endswith(engine) or engine.endswith(flag) or flag == engine)
            for flag in disable_flags
        )
        if gated_off:
            continue
        en = norm(engine)
        alias_norm = norm(PULSE_NAME_ALIAS[engine]) if engine in PULSE_NAME_ALIAS else None
        matched = (
            any(en in p or p in en for p in pulses_norm)
            or (alias_norm is not None and alias_norm in pulses_norm)
        )
        if not matched:
            findings.append((
                "WARN", "ENABLED+NO_PULSE",
                f"g_{engine}",
                "enabled=true but no g_engine_heartbeat.pulse() call mentions "
                "this engine; heartbeat miss-detector will not see silences"
            ))

    # CHECK 3 -- PULSED+NO_REGISTER
    # If pulse("X") exists but register_engine("X") doesn't, the heartbeat
    # call is a no-op (pulse() bails on unknown name).
    for p in pulses:
        if p not in registers:
            findings.append((
                "WARN", "PULSED+NO_REGISTER",
                p,
                "pulse() called but engine never registered with heartbeat -- "
                "pulse is a no-op"
            ))

    # CHECK 4 -- WARMUP_MISSING
    for engine, cfg in engines.items():
        warmup = cfg.get("warmup")
        if not warmup:
            continue
        # warmup paths in code are forward-slash relative; resolve from repo root
        # also tolerate the Windows-style C:\Omega\... prefix used in some logs
        candidate = warmup
        if candidate.startswith("C:"):
            continue  # production path, can't check from this host
        p = REPO_ROOT / candidate
        if not p.exists():
            findings.append((
                "FAIL", "WARMUP_MISSING",
                f"g_{engine}",
                f"warmup_csv_path=\"{warmup}\" but file does not exist"
            ))

    # CHECK 5 -- SHADOW_STALE (runtime only)
    # Requires reading the trade ledger to count fires per engine in the last
    # 14 days. Skipped if --no-runtime. Stub for now -- ledger schema can be
    # plumbed in when the ledger location is finalised across hosts.
    if not args.no_runtime:
        # TODO: parse C:/Omega/logs/closed_trades_*.csv (or equivalent), build
        # {engine_name: last_fire_ts}; flag shadow+enabled engines with no
        # trades in 14 days. Left as a runtime-side hook so this script stays
        # standalone (no scp/ssh dependency).
        pass

    # Report
    level_order = {"FAIL": 0, "WARN": 1, "INFO": 2}
    findings.sort(key=lambda x: (level_order.get(x[0], 99), x[1], x[2]))

    n_fail = sum(1 for f in findings if f[0] == "FAIL")
    n_warn = sum(1 for f in findings if f[0] == "WARN")

    print(f"\nEngine contract check -- {len(engines)} engines scanned, "
          f"{len(pulses)} pulse calls, {len(registers)} register calls, "
          f"{sum(1 for v in disable_flags.values() if v)} disable flags ACTIVE\n",
          file=sys.stderr)

    if not findings:
        print("[OK] No contract violations detected.", file=sys.stderr)
    else:
        # Human report on stderr
        cur = None
        for lvl, check, engine, detail in findings:
            if lvl != cur:
                print(f"\n--- {lvl} ---", file=sys.stderr)
                cur = lvl
            print(f"  [{check}] {engine}: {detail}", file=sys.stderr)

        # Machine summary on stdout
        for lvl, check, engine, detail in findings:
            print(f"{lvl}\t{check}\t{engine}\t{detail}")

    print(f"\nTotal: FAIL={n_fail} WARN={n_warn}", file=sys.stderr)

    if n_fail > 0:
        return 2
    if n_warn > 0 and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
