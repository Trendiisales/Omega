#!/usr/bin/env python3
"""Pre-flight verification before flipping a scheduled task to --live.

Runs the same check the bracket scripts run at startup. Use this BEFORE
editing register_tasks.ps1 to swap --paper for --live — if the script
exits non-zero here, the scheduled tasks will also refuse to run live.

  python scripts\\live_guard.py
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from live._common import DATA_DIR, assert_live_allowed

TRADES_FILE = DATA_DIR / 'trades.ndjson'


def main() -> int:
    flag = os.environ.get('BRACKET_GO_LIVE')
    print(f'BRACKET_GO_LIVE={flag!r}')
    print(f'trades file: {TRADES_FILE} (exists={TRADES_FILE.exists()})')
    try:
        assert_live_allowed(TRADES_FILE)
    except SystemExit as e:
        print(f'BLOCK: {e}', file=sys.stderr)
        return 1
    print('OK: --live would be allowed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
