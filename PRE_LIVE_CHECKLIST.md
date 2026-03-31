# ⚠️  PRE-LIVE CHECKLIST -- MUST COMPLETE BEFORE SWITCHING TO LIVE MODE ⚠️

This file exists because testing settings were left active.
**DO NOT switch mode=LIVE until every item below is verified.**

---

## 🔴 CRITICAL -- SETTINGS DISABLED FOR TESTING

### 1. session_watermark_pct (CURRENTLY DISABLED)
- **File:** `config/omega_config.ini` line ~79
- **Current:** `session_watermark_pct=0.0`  ← TESTING VALUE
- **Required:** `session_watermark_pct=0.27`
- **What it does:** Stops new entries if drawdown >= $121 from intra-day peak AND day is negative
- **Risk if left at 0.0:** No drawdown protection. System will keep trading through unlimited losses.

---

## 🟡 VERIFY BEFORE LIVE

### 2. Binary matches HEAD
- Run: `grep "Git hash" logs/omega_YYYY-MM-DD.log | tail -1`
- Must match: `git rev-parse HEAD` (first 7 chars)
- **If mismatch:** Run `.\REBUILD_AND_START.ps1` -- do NOT proceed until confirmed

### 3. Depth feed stable
- Check log: `grep "Depth feed ACTIVE\|INVALID_REQUEST" logs/omega_YYYY-MM-DD.log | tail -5`
- Must see: `Depth feed ACTIVE -- 19 symbols`
- Must NOT see: `INVALID_REQUEST` after depth active

### 4. L2 data flowing
- Check log: `grep "CTRADER-EVTS.*XAUUSD" logs/omega_YYYY-MM-DD.log | tail -3`
- Must show non-zero event count

### 5. mode=SHADOW confirmed before any parameter changes
- Check: `grep "mode=" config/omega_config.ini`
- Only change to LIVE when all items above pass

### 6. Account equity matches expected
- GUI: http://185.167.119.59:7779
- Verify equity matches cTrader account balance

---

## SIGN-OFF
Before going live, confirm each item above and delete this block:
```
[ ] session_watermark_pct restored to 0.27
[ ] Binary hash verified
[ ] Depth feed stable (no INVALID_REQUEST)
[ ] L2 events flowing
[ ] Mode is SHADOW until final switch
[ ] Equity confirmed
```

