#!/usr/bin/env python3
# CRYPTO-DESK-TRADE GUARD (S-2026-07-16) — structural, self-healing.
#
# OPERATOR RULE (hard, end of story): there are NO negative crypto trades on the desk.
# Crypto is long-only spot + every companion is floored (feedback-mimic-be-floor-mandatory:
# "can't close neg after arming"; feedback-no-immediate-entry-upjump-mimic-only: a mimic
# books nothing until BE is covered). So this guard drops EVERY net<0 row in the desk feed
# regardless of engine/reason. A negative row is one of:
#   - a stale row from a RETIRED cell (e.g. INJ-UJ55W24-SWEET, no longer in the roster),
#   - a gap-through / bad-tick artifact on a jump_floor cell,
#   - a genuine floor violation (bug to fix at the engine).
# In every case it must NOT reach the :7779 last-15 panel. This filter runs inside the 120s
# relay (refresh_crypto_companion.sh) on the file pulled from josgp1 BEFORE the push to
# omega-new, so the class is self-healing: even if josgp1 re-books a negative, the desk never
# shows it. Every drop is LOGGED (not silent) so the shadow record stays auditable and a real
# floor violation surfaces in the relay log.
#
# Why here and not only at the source: prior resets cleared the omega COPY but the josgp1
# append-only SOURCE kept its rows and the relay re-pushed them within 2 min ("came back
# after the fix"). A relay-level guard + a complete-reset script (reset_crypto_desk_trades.sh)
# close that recurrence permanently.
import sys

# Companion-clip exit reasons (UpJumpLadderCompanion.hpp). All must be net>=0 on a floored
# cell. ENGINE_EXIT is intentionally EXCLUDED — it is shared with slot/parent flat-closes.
CLIP_REASONS = {
    "FLOOR_TRAIL_STOP", "FLOOR_TRAIL_CLIP", "BE_TRAIL_CLIP", "STALL_CLIP",
    "REVERSAL_CLIP", "REVERSAL_CUT", "PREBE_CUT", "PREBE_STOP",
}


def main() -> int:
    if len(sys.argv) < 2:
        return 0
    path = sys.argv[1]
    try:
        lines = open(path).read().splitlines()
    except OSError:
        return 0
    if not lines:
        return 0
    hdr = lines[0]
    cols = hdr.split(",")
    try:
        i_net = cols.index("net_usd")
        i_reason = cols.index("reason")
        i_strat = cols.index("strat")
    except ValueError:
        return 0  # unknown schema -> pass through untouched (fail-open, never lose data blind)

    kept = [hdr]
    dropped = []
    for ln in lines[1:]:
        f = ln.split(",")
        if len(f) <= max(i_net, i_reason, i_strat):
            kept.append(ln)
            continue
        try:
            net = float(f[i_net])
        except ValueError:
            kept.append(ln)
            continue
        # OPERATOR RULE (S-2026-07-16, hard): there are NO negative crypto trades on the desk,
        # end of story. Drop EVERY net<0 row regardless of engine/reason (crypto is long-only
        # spot + floored). CLIP_REASONS retained only to tag WHY in the log.
        #
        # EXCEPTION (S-2026-07-18r, operator order "put the correct loss number in the PnL"):
        # rows whose reason starts with "LIVE-" are REAL Binance cash fills (live mimic mirror /
        # operator-ordered flatten), NOT shadow-book rows. The 07-16 rule was written for floored
        # SHADOW books where a negative row is always a stale/artifact/bug row; a real live
        # fill's realized loss is cash truth and MUST reach the desk. Never drop it.
        if net < 0 and not f[i_reason].startswith("LIVE-"):
            dropped.append((f[i_strat], f[i_reason], net))
            continue
        kept.append(ln)

    if dropped:
        with open(path, "w") as out:
            out.write("\n".join(kept) + "\n")
        for strat, reason, net in dropped:
            kind = "companion clip" if reason in CLIP_REASONS else "trade"
            print(f"[DESK-TRADE-GUARD] DROPPED negative crypto {kind}: {strat} {reason} "
                  f"net=${net:.2f} (operator rule: no negative crypto trades on the desk)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
