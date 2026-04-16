"""
TSE Backtest -- runs exact TSE logic against real tick data from VPS.
Usage: python3 tse_backtest.py <l2_ticks_csv>
The L2 ticks CSV has columns: timestamp_ms, bid, ask, micro_edge, ...
"""
import sys, csv
from collections import deque

# ── TSE params (match current TickScalpEngine.hpp exactly) ──────────────────
TSE_MAX_LOT           = 0.01
TSE_RISK_DOLLARS      = 10.0
TSE_DAILY_LOSS_LIMIT  = 50.0
TSE_MAX_CONSEC_LOSSES = 3
TSE_COOLDOWN_MS       = 15000
TSE_PAUSE_MS          = 300000
TSE_MAX_SPREAD        = 0.50
TSE_COMMISSION_RT     = 0.20
TSE_ATR_MIN           = 1.0
TSE_ATR_MAX           = 8.0
TSE_P1_TICKS          = 8
TSE_P1_MIN_MOVE       = 0.50
TSE_P1_TP_MULT        = 2.5
TSE_P1_SL_MULT        = 1.5
TSE_BE_FRAC           = 0.50
TSE_P3_TRAIL_ARM      = 0.50
TSE_P3_TRAIL_DIST     = 0.30
TSE_P3_SPIKE_MULT     = 3.0
TSE_P3_WINDOW         = 8
TSE_P3_MIN_MOVE       = 0.15
TSE_P3_TP             = 1.00
TSE_P3_SL             = 0.40
TSE_RSI_PERIOD        = 14
TSE_RSI_SLOPE_ALPHA   = 2.0 / 6.0
TSE_RSI_SLOPE_THRESH  = 0.05
TSE_RSI_REVERSAL_THRESH = 0.12
TSE_REVERSAL_MIN_HOLD_MS = 3000
TSE_WARMUP_TICKS      = 60

class TSEBacktest:
    def __init__(self):
        # RSI
        self.rsi_gains = deque(); self.rsi_losses = deque()
        self.rsi_prev_mid = 0.0; self.rsi_cur = 50.0; self.rsi_prev_val = 50.0
        self.rsi_slope_ema = 0.0; self.rsi_warmed = False
        # Velocity
        self.bid_hist = deque(); self.tick_times = deque()
        self.vel_baseline = 0.0; self.ticks_win = 0; self.vel_win_ms = 0
        self.ticks_total = 0
        # Position
        self.pos_active = False; self.pos_long = False
        self.pos_entry = 0.0; self.pos_ts = 0; self.pos_mfe = 0.0
        self.pos_sl = 0.0; self.pos_tp = 0.0; self.pos_be_done = False
        self.pos_pattern = ''
        # Risk state
        self.daily_pnl = 0.0; self.last_exit_ms = 0
        self.pause_until_ms = 0; self.consec_losses = 0
        # Stats
        self.trades = []
        self.blocks = {'atr':0,'cooldown':0,'rsi':0,'warmup':0,'spread':0,'no_burst':0,'cost':0}

    def update_rsi(self, bid):
        if self.rsi_prev_mid == 0.0:
            self.rsi_prev_mid = bid; return
        chg = bid - self.rsi_prev_mid; self.rsi_prev_mid = bid
        self.rsi_gains.append(chg if chg > 0 else 0.0)
        self.rsi_losses.append(-chg if chg < 0 else 0.0)
        if len(self.rsi_gains) > TSE_RSI_PERIOD:
            self.rsi_gains.popleft(); self.rsi_losses.popleft()
        if len(self.rsi_gains) >= TSE_RSI_PERIOD:
            ag = sum(self.rsi_gains)/TSE_RSI_PERIOD
            al = sum(self.rsi_losses)/TSE_RSI_PERIOD
            self.rsi_prev_val = self.rsi_cur
            self.rsi_cur = 100.0 if al == 0 else 100.0 - 100.0/(1.0+ag/al)
            s = max(-5.0, min(5.0, self.rsi_cur - self.rsi_prev_val))
            if not self.rsi_warmed: self.rsi_slope_ema = s; self.rsi_warmed = True
            else: self.rsi_slope_ema = s*TSE_RSI_SLOPE_ALPHA + self.rsi_slope_ema*(1.0-TSE_RSI_SLOPE_ALPHA)

    def update_vel(self, now_ms):
        if self.vel_win_ms == 0: self.vel_win_ms = now_ms
        self.ticks_win += 1
        if now_ms - self.vel_win_ms >= 30000:
            self.vel_baseline = self.vel_baseline*0.7 + self.ticks_win*0.3
            self.ticks_win = 0; self.vel_win_ms = now_ms

    def ready(self, now_ms, atr):
        if atr < TSE_ATR_MIN or atr > TSE_ATR_MAX:
            self.blocks['atr'] += 1; return False
        if not (self.vel_baseline >= 10.0 and self.rsi_warmed and self.ticks_total >= TSE_WARMUP_TICKS):
            self.blocks['warmup'] += 1; return False
        if now_ms < self.pause_until_ms:
            return False
        if now_ms - self.last_exit_ms < TSE_COOLDOWN_MS:
            self.blocks['cooldown'] += 1; return False
        return True

    def try_p1(self, bid, ask, spread, atr, now_ms):
        if len(self.bid_hist) < TSE_P1_TICKS + 1: return None, 0.0
        b = list(self.bid_hist); n = len(b)
        up = dn = 0
        for i in range(n-TSE_P1_TICKS, n):
            d = b[i]-b[i-1]
            if d > 0: up += 1
            elif d < 0: dn += 1
        bu = (up >= TSE_P1_TICKS-1 and dn == 0)
        bd = (dn >= TSE_P1_TICKS-1 and up == 0)
        if not bu and not bd: self.blocks['no_burst'] += 1; return None, 0.0
        net = abs(b[n-1]-b[n-1-TSE_P1_TICKS])
        if net < TSE_P1_MIN_MOVE: return None, 0.0
        if self.rsi_warmed:
            if bu and self.rsi_slope_ema < -TSE_RSI_SLOPE_THRESH:
                self.blocks['rsi'] += 1; return None, 0.0
            if bd and self.rsi_slope_ema > TSE_RSI_SLOPE_THRESH:
                self.blocks['rsi'] += 1; return None, 0.0
        cost = spread + TSE_COMMISSION_RT
        tp_pts = net * TSE_P1_TP_MULT
        sl_pts = net * TSE_P1_SL_MULT
        if tp_pts <= cost: self.blocks['cost'] += 1; return None, 0.0
        return ('LONG' if bu else 'SHORT'), net

    def on_tick(self, now_ms, bid, ask, atr):
        self.ticks_total += 1
        mid = (bid+ask)*0.5
        spread = ask-bid
        self.bid_hist.append(bid)
        if len(self.bid_hist) > 60: self.bid_hist.popleft()
        self.tick_times.append(now_ms)
        if len(self.tick_times) > 60: self.tick_times.popleft()
        self.update_rsi(bid)
        self.update_vel(now_ms)

        if self.pos_active:
            move = (mid-self.pos_entry) if self.pos_long else (self.pos_entry-mid)
            if move > self.pos_mfe: self.pos_mfe = move
            eff = bid if self.pos_long else ask
            tp_dist = abs(self.pos_tp - self.pos_entry)

            # BE move
            if not self.pos_be_done and tp_dist > 0 and self.pos_mfe >= tp_dist * TSE_BE_FRAC:
                self.pos_sl = self.pos_entry; self.pos_be_done = True

            # RSI reversal exit
            held = now_ms - self.pos_ts
            slope_ag = (self.pos_long and self.rsi_slope_ema < -TSE_RSI_REVERSAL_THRESH) or \
                       (not self.pos_long and self.rsi_slope_ema > TSE_RSI_REVERSAL_THRESH)
            if self.rsi_warmed and held >= TSE_REVERSAL_MIN_HOLD_MS and self.pos_mfe < 0.05 and slope_ag:
                self._close(eff, 'RSI_REVERSAL', now_ms); return

            # TP
            if (self.pos_long and bid >= self.pos_tp) or (not self.pos_long and ask <= self.pos_tp):
                self._close(eff, 'TP_HIT', now_ms); return
            # SL
            sl_hit = (self.pos_long and bid <= self.pos_sl) or (not self.pos_long and ask >= self.pos_sl)
            if sl_hit:
                reason = 'BE_HIT' if self.pos_be_done else 'SL_HIT'
                self._close(eff, reason, now_ms); return
            # Timeout 5min
            if now_ms - self.pos_ts > 300000:
                self._close(eff, 'TIMEOUT', now_ms); return
            return

        if spread > TSE_MAX_SPREAD: self.blocks['spread'] += 1; return
        if not self.ready(now_ms, atr): return

        direction, net = self.try_p1(bid, ask, spread, atr, now_ms)
        if direction:
            sl_pts = net * TSE_P1_SL_MULT
            tp_pts = net * TSE_P1_TP_MULT
            self.pos_active = True; self.pos_long = (direction=='LONG')
            self.pos_entry = ask if self.pos_long else bid
            self.pos_ts = now_ms; self.pos_mfe = 0.0; self.pos_be_done = False
            self.pos_sl = self.pos_entry - sl_pts if self.pos_long else self.pos_entry + sl_pts
            self.pos_tp = self.pos_entry + tp_pts if self.pos_long else self.pos_entry - tp_pts
            self.pos_pattern = 'P1'

    def _close(self, exit_px, reason, now_ms):
        pnl_pts = (exit_px-self.pos_entry) if self.pos_long else (self.pos_entry-exit_px)
        pnl_usd = pnl_pts * TSE_MAX_LOT * 100.0
        self.daily_pnl += pnl_usd; self.last_exit_ms = now_ms
        win = pnl_usd > 0
        if win: self.consec_losses = 0
        else:
            self.consec_losses += 1
            if self.consec_losses >= TSE_MAX_CONSEC_LOSSES:
                self.pause_until_ms = now_ms + TSE_PAUSE_MS
                self.consec_losses = 0
        from datetime import datetime, timezone
        ts = datetime.fromtimestamp(now_ms/1000, tz=timezone.utc).strftime('%H:%M:%S')
        self.trades.append({
            'time': ts, 'side': 'LONG' if self.pos_long else 'SHORT',
            'entry': self.pos_entry, 'exit': exit_px, 'reason': reason,
            'pnl_pts': pnl_pts, 'pnl_usd': pnl_usd, 'mfe': self.pos_mfe,
            'held_s': (now_ms-self.pos_ts)//1000
        })
        self.pos_active = False

# ── Run ─────────────────────────────────────────────────────────────────────
if len(sys.argv) < 2:
    print("Usage: python3 tse_backtest.py <l2_ticks_csv>")
    sys.exit(1)

tse = TSEBacktest()
tick_count = 0
skipped = 0

# Compute rolling ATR from bid prices
atr_window = deque()
ATR_PERIOD = 14*14  # ~200 ticks for ATR14

with open(sys.argv[1]) as f:
    reader = csv.reader(f)
    header = next(reader)
    print(f"CSV columns: {header}")
    # Find column indices
    try:
        ts_col  = header.index('timestamp_ms') if 'timestamp_ms' in header else 0
        bid_col = header.index('bid') if 'bid' in header else 1
        ask_col = header.index('ask') if 'ask' in header else 2
    except:
        ts_col, bid_col, ask_col = 0, 1, 2

    prev_bid = 0.0
    atr_val = 2.0  # seed

    for row in reader:
        try:
            now_ms = int(float(row[ts_col]))
            bid    = float(row[bid_col])
            ask    = float(row[ask_col])
        except:
            skipped += 1; continue

        if bid <= 0 or ask <= 0 or ask < bid: skipped += 1; continue

        # Rolling ATR approx from true range
        if prev_bid > 0:
            tr = ask - bid + abs(bid - prev_bid)
            atr_window.append(tr)
            if len(atr_window) > ATR_PERIOD: atr_window.popleft()
            if len(atr_window) >= ATR_PERIOD//2:
                atr_val = sum(atr_window)/len(atr_window) * 14  # scale to pts
        prev_bid = bid

        # Only XAUUSD (gold) -- filter if symbol column exists
        tse.on_tick(now_ms, bid, ask, atr_val)
        tick_count += 1

# Force close any open position
if tse.pos_active:
    tse._close(tse.pos_entry, 'END_OF_DATA', tick_count*250)

# ── Results ──────────────────────────────────────────────────────────────────
print(f"\n{'='*60}")
print(f"TSE BACKTEST RESULTS -- {sys.argv[1].split('/')[-1]}")
print(f"{'='*60}")
print(f"Ticks processed : {tick_count:,}  (skipped: {skipped})")
print(f"Total trades    : {len(tse.trades)}")

if tse.trades:
    wins  = [t for t in tse.trades if t['pnl_usd'] > 0]
    loss  = [t for t in tse.trades if t['pnl_usd'] <= 0]
    total_pnl = sum(t['pnl_usd'] for t in tse.trades)
    wr = 100*len(wins)/len(tse.trades) if tse.trades else 0
    avg_win  = sum(t['pnl_usd'] for t in wins)/len(wins) if wins else 0
    avg_loss = sum(t['pnl_usd'] for t in loss)/len(loss) if loss else 0

    print(f"Win rate        : {wr:.1f}%  ({len(wins)}W / {len(loss)}L)")
    print(f"Total PnL       : ${total_pnl:+.2f}")
    print(f"Avg win         : ${avg_win:+.2f}")
    print(f"Avg loss        : ${avg_loss:+.2f}")
    print(f"Expectancy      : ${total_pnl/len(tse.trades):+.3f}/trade")

    print(f"\nExit reasons:")
    from collections import Counter
    for reason, count in Counter(t['reason'] for t in tse.trades).most_common():
        pnl = sum(t['pnl_usd'] for t in tse.trades if t['reason']==reason)
        print(f"  {reason:20s} {count:3d} trades  ${pnl:+.2f}")

    print(f"\nEntry blocks:")
    total_blocks = sum(tse.blocks.values())
    for k,v in sorted(tse.blocks.items(), key=lambda x:-x[1]):
        if v > 0: print(f"  {k:15s} {v:6,d}")
    print(f"  {'TOTAL':15s} {total_blocks:6,d}")

    print(f"\nAll trades:")
    print(f"  {'Time':8s} {'Side':5s} {'Entry':8s} {'Exit':8s} {'Reason':15s} {'PnL':8s} {'MFE':6s} {'Held':5s}")
    for t in tse.trades:
        print(f"  {t['time']:8s} {t['side']:5s} {t['entry']:8.2f} {t['exit']:8.2f} "
              f"{t['reason']:15s} ${t['pnl_usd']:+6.2f} {t['mfe']:6.3f} {t['held_s']:4d}s")
else:
    print("NO TRADES -- all blocked")
    print(f"\nBlock breakdown:")
    for k,v in sorted(tse.blocks.items(), key=lambda x:-x[1]):
        print(f"  {k:15s} {v:6,d}")
