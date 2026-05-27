"""Shared utilities for daily_bracket / sunday_bracket.

Pre-flight checks, kill-switch, order-state persistence, market-data freshness
guards, and failure notification — extracted here so both bracket scripts
behave identically on the boundaries that matter (don't fire on stale data,
don't fire when account is mis-configured, don't fire if a HALT file exists,
write a recoverable state file before every order placement).
"""
from __future__ import annotations

import json
import logging
import math
import os
import socket
import sys
import urllib.error
import urllib.request
from datetime import datetime, time, timedelta, timezone
from pathlib import Path

log = logging.getLogger('bracket._common')

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / 'logs'
DATA_DIR = ROOT / 'data'
STATE_DIR = DATA_DIR / 'state'
HALT_FILE = ROOT / 'HALT'
HEARTBEAT_FILE = LOG_DIR / 'heartbeat.ndjson'

PAPER_PORTS = {4002, 7497}
LIVE_PORTS = {4001, 7496}

STALE_THRESHOLD_SEC = 30
# How recent the last paper trade must be before live mode is permitted.
LIVE_PAPER_MAX_AGE_DAYS = 30


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def ensure_dirs() -> None:
    for d in (LOG_DIR, DATA_DIR, STATE_DIR):
        d.mkdir(parents=True, exist_ok=True)


# ── HALT kill-switch ──────────────────────────────────────────────────────

def halt_active() -> bool:
    """True when C:\\Omega\\bracket-bot\\HALT exists. Operators drop this file
    to stop all scheduled runs cold without disabling tasks one-by-one."""
    return HALT_FILE.exists()


def check_halt_or_exit() -> None:
    if halt_active():
        msg = f'HALT file present at {HALT_FILE} — refusing to run'
        log.error(msg)
        notify_failure('HALT', msg)
        sys.exit(0)


# ── Paper / live port verification ────────────────────────────────────────

def verify_account_mode(port: int, live_flag: bool) -> None:
    """Refuse to run if --live was passed against a paper port (or vice
    versa). Bracket configs are validated on paper only; an accidental
    --live with the paper port number would silently mis-route."""
    if live_flag and port not in LIVE_PORTS:
        raise SystemExit(
            f'--live passed but port {port} is not a known live port '
            f'({sorted(LIVE_PORTS)}). Refusing to run.'
        )
    if not live_flag and port not in PAPER_PORTS:
        raise SystemExit(
            f'--paper (default) but port {port} is not a known paper port '
            f'({sorted(PAPER_PORTS)}). Pass --live explicitly if intended.'
        )


def assert_account_label(ib, live_flag: bool) -> None:
    """Cross-check IBKR's own account label against the --live flag. Paper
    accounts on IBKR are prefixed 'DU'; live accounts are 'U'. Catches the
    case where Gateway is logged into the wrong account on the right port."""
    try:
        accounts = ib.managedAccounts()
    except Exception as e:  # noqa: BLE001
        log.warning(f'managedAccounts() failed, skipping label check: {e}')
        return
    if not accounts:
        log.warning('no managed accounts returned, skipping label check')
        return
    acct = accounts[0]
    is_paper = acct.startswith('DU')
    if live_flag and is_paper:
        raise SystemExit(
            f'--live passed but Gateway is logged into paper account {acct}'
        )
    if not live_flag and not is_paper:
        raise SystemExit(
            f'--paper (default) but Gateway is logged into live account {acct}'
        )
    log.info(f'account label check OK: {acct} (live_flag={live_flag})')


# ── Market hours / holiday gate ───────────────────────────────────────────
# Hardcoded CME globex closures (full-day closures). The Sunday-night bracket
# specifically does NOT fire when the open is delayed for a US holiday.
# Maintain this list manually each year — the bot reads ahead but does not
# refresh it.
CME_FULL_CLOSURES_2026 = {
    '2026-01-01',  # New Year's Day
    '2026-01-19',  # MLK
    '2026-02-16',  # Presidents Day
    '2026-04-03',  # Good Friday
    '2026-05-25',  # Memorial Day
    '2026-06-19',  # Juneteenth
    '2026-07-03',  # Independence Day observed
    '2026-09-07',  # Labor Day
    '2026-11-26',  # Thanksgiving
    '2026-12-25',  # Christmas
}
CME_FULL_CLOSURES_2027 = {
    '2027-01-01',
    '2027-01-18',
    '2027-02-15',
    '2027-03-26',
    '2027-05-31',
    '2027-06-18',
    '2027-07-05',
    '2027-09-06',
    '2027-11-25',
    '2027-12-24',
}
CME_CLOSURES = CME_FULL_CLOSURES_2026 | CME_FULL_CLOSURES_2027


def is_cme_closed(dt: datetime) -> bool:
    return dt.strftime('%Y-%m-%d') in CME_CLOSURES


def assert_market_open(dt: datetime, allow_weekend: bool = False) -> None:
    """Raises SystemExit(0) if the market is closed. Exit code 0 because a
    no-fire-on-holiday is a clean no-op, not a failure."""
    if not allow_weekend and dt.weekday() >= 5:
        log.info(f'weekend (weekday={dt.weekday()}) — skipping')
        sys.exit(0)
    if is_cme_closed(dt):
        log.info(f'CME holiday {dt.date()} — skipping')
        sys.exit(0)


# ── L2 / top-of-book staleness guard ──────────────────────────────────────

def is_price_stale(ticker_time: datetime | None, threshold_sec: int = STALE_THRESHOLD_SEC) -> bool:
    """A ticker with no time field or a time more than threshold_sec behind
    wall-clock is treated as stale. Used after switching to real-time mdt —
    IBKR silently downgrades to delayed when the account lacks a sub, so a
    valid-looking float can still be 15 min old."""
    if ticker_time is None:
        return True
    if ticker_time.tzinfo is None:
        ticker_time = ticker_time.replace(tzinfo=timezone.utc)
    age = (utcnow() - ticker_time).total_seconds()
    if age > threshold_sec:
        log.warning(f'price stale: ticker_time={ticker_time.isoformat()} age={age:.1f}s threshold={threshold_sec}s')
        return True
    return False


def log_mdt_status(ib, mdt_requested: int) -> None:
    """Log the marketDataType IBKR actually returned. Catches the silent
    downgrade where reqMarketDataType(1) succeeds but the account isn't
    subscribed, so the stream that follows is delayed."""
    label = {1: 'live', 2: 'frozen', 3: 'delayed', 4: 'delayed-frozen'}.get(mdt_requested, '?')
    log.info(f'requested marketDataType={mdt_requested} ({label})')


# ── Order-state persistence ───────────────────────────────────────────────

def state_path(strategy: str) -> Path:
    ensure_dirs()
    return STATE_DIR / f'{strategy}.json'


def write_state(strategy: str, payload: dict) -> None:
    p = state_path(strategy)
    payload = {'ts': utcnow().isoformat(), 'strategy': strategy, **payload}
    tmp = p.with_suffix('.json.tmp')
    tmp.write_text(json.dumps(payload, indent=2))
    tmp.replace(p)
    log.info(f'state: wrote {p.name} stage={payload.get("stage")}')


def clear_state(strategy: str) -> None:
    p = state_path(strategy)
    if p.exists():
        p.unlink()
        log.info(f'state: cleared {p.name}')


def read_state(strategy: str) -> dict | None:
    p = state_path(strategy)
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text())
    except Exception as e:  # noqa: BLE001
        log.warning(f'state: failed to read {p.name}: {e}')
        return None


# ── Heartbeat + webhook notification ──────────────────────────────────────

def write_heartbeat(strategy: str, stage: str, **extra) -> None:
    ensure_dirs()
    rec = {'ts': utcnow().isoformat(), 'host': socket.gethostname(),
           'strategy': strategy, 'stage': stage, **extra}
    with open(HEARTBEAT_FILE, 'a') as f:
        f.write(json.dumps(rec) + '\n')


def notify_failure(strategy: str, message: str) -> None:
    """Post a one-line failure notice to the webhook in
    BRACKET_WEBHOOK_URL (Slack-compatible 'text' payload). No-op if unset
    so dev runs don't spam."""
    url = os.environ.get('BRACKET_WEBHOOK_URL')
    if not url:
        return
    payload = {'text': f'[bracket-bot {socket.gethostname()}] {strategy}: {message}'}
    try:
        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode('utf-8'),
            headers={'Content-Type': 'application/json'},
        )
        urllib.request.urlopen(req, timeout=10).read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        log.warning(f'webhook post failed: {e}')


# ── Live-promotion guard ──────────────────────────────────────────────────

def assert_live_allowed(trades_file: Path) -> None:
    """Refuse --live unless BRACKET_GO_LIVE=1 AND there is a recent paper
    trade in trades.ndjson. Prevents flipping live by re-running the deploy
    script without first validating on paper."""
    if os.environ.get('BRACKET_GO_LIVE') != '1':
        raise SystemExit(
            '--live blocked: set BRACKET_GO_LIVE=1 in the environment to confirm'
        )
    if not trades_file.exists():
        raise SystemExit(
            f'--live blocked: no trades file at {trades_file} — run paper first'
        )
    cutoff = utcnow() - timedelta(days=LIVE_PAPER_MAX_AGE_DAYS)
    recent_paper = False
    with open(trades_file) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:  # noqa: BLE001
                continue
            if not rec.get('paper'):
                continue
            try:
                ts = datetime.fromisoformat(rec['ts'])
            except Exception:  # noqa: BLE001
                continue
            if ts.tzinfo is None:
                ts = ts.replace(tzinfo=timezone.utc)
            if ts >= cutoff:
                recent_paper = True
                break
    if not recent_paper:
        raise SystemExit(
            f'--live blocked: no paper trade within last {LIVE_PAPER_MAX_AGE_DAYS} days'
        )


# ── NTP / clock drift sanity check ────────────────────────────────────────

def warn_on_clock_drift(max_drift_sec: int = 5) -> None:
    """Best-effort: compare system clock to pool.ntp.org. Logs (does not
    abort) when drift > max_drift_sec — abort would be too aggressive for
    a flaky network."""
    try:
        import struct
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(3)
        msg = b'\x1b' + 47 * b'\0'
        client.sendto(msg, ('pool.ntp.org', 123))
        data, _ = client.recvfrom(1024)
        client.close()
        ntp_time = struct.unpack('!12I', data)[10] - 2208988800
        local = utcnow().timestamp()
        drift = abs(local - ntp_time)
        if drift > max_drift_sec:
            log.warning(f'clock drift {drift:.1f}s exceeds {max_drift_sec}s (vs pool.ntp.org)')
        else:
            log.info(f'clock drift {drift:.2f}s (OK)')
    except Exception as e:  # noqa: BLE001
        log.warning(f'NTP check failed (non-fatal): {e}')
