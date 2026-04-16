"""
CFE Parameter Sweep Backtest
Runs multiple CFE configurations against real L2 tick data
Tests both LONG and SHORT directions
"""
import sys, csv, itertools
from collections import deque

class CFEBacktest:
    def __init__(self, params):
        self.p = params
        # RSI state (tick-level)
        self.rsi_gains = deque(); self.rsi_losses = deque()
        self.rsi_prev_mid = 0.0; self.rsi_cur = 50.0; self.rsi_prev = 50.0
        self.rsi_trend = 0.0; self.rsi_warmed = False
        self.rsi_ema_alpha = 2.0 / (self.p['rsi_ema_n'] + 1)
        # Drift state
        self.drift_sus_dir = 0; self.drift_sus_ms = 0; self.drift_sus_start = 0
        # Recent mid for price confirm
        self.recent_mid = deque()
        # Adverse block
        self.adverse_block = False; self.last_loss_exit = 0.0
        self.last_loss_dir = 0; self.last_loss_atr = 0.0
        # Position
        self.pos_active = False; self.pos_long = False
        self.pos_entry = 0.0; self.pos_sl = 0.0; self.pos_tp = 0.0
        self.pos_ts = 0; self.pos_mfe = 0.0; self.pos_size = 0.0
        self.pos_be_done = False; self.pos_trail = False; self.pos_trail_sl = 0.0
        self.pos_partial = False
        # Stats
        self.trades = []
        self.cooldown_until = 0
        self.opp_dir_cooldown = 0; self.last_closed_dir = 0; self.last_closed_ms = 0
        self.warmup_ticks = 0

    def update_rsi(self, mid):
        if self.rsi_prev_mid == 0.0:
            self.rsi_prev_mid = mid; return
        chg = mid - self.rsi_prev_mid; self.rsi_prev_mid = mid
        self.rsi_gains.append(chg if chg > 0 else 0.0)
        self.rsi_losses.append(-chg if chg < 0 else 0.0)
        n = self.p['rsi_period']
        if len(self.rsi_gains) > n: self.rsi_gains.popleft(); self.rsi_losses.popleft()
        if len(self.rsi_gains) >= n:
            ag = sum(self.rsi_gains)/n; al = sum(self.rsi_losses)/n
            self.rsi_prev = self.rsi_cur
            self.rsi_cur = 100.0 if al == 0 else 100.0 - 100.0/(1.0+ag/al)
            slope = self.rsi_cur - self.rsi_prev
            if not self.rsi_warmed: self.rsi_trend = slope; self.rsi_warmed = True
            else: self.rsi_trend = slope*self.rsi_ema_alpha + self.rsi_trend*(1-self.rsi_ema_alpha)

    def update_drift_sus(self, drift, now_ms):
        thresh = self.p['sus_thresh']
        if drift >= thresh:
            if self.drift_sus_dir == 1: pass
            else: self.drift_sus_dir = 1; self.drift_sus_start = now_ms
        elif drift <= -thresh:
            if self.drift_sus_dir == -1: pass
            else: self.drift_sus_dir = -1; self.drift_sus_start = now_ms
        else:
            self.drift_sus_dir = 0; self.drift_sus_start = 0
        self.drift_sus_ms = (now_ms - self.drift_sus_start) if self.drift_sus_dir != 0 else 0

    def rsi_dir(self):
        if not self.rsi_warmed: return 0
        thresh = self.p['rsi_thresh']
        max_t = self.p['rsi_max']
        if self.rsi_trend > thresh and self.rsi_trend < max_t: return +1
        if self.rsi_trend < -thresh and self.rsi_trend > -max_t: return -1
        return 0

    def price_confirms(self, drift_is_long, mid):
        n = self.p['price_confirm_ticks']
        if len(self.recent_mid) >= n:
            oldest = list(self.recent_mid)[-n]
            net = mid - oldest
            if drift_is_long: return net >= self.p['price_confirm_min']
            else: return net <= -self.p['price_confirm_min']
        return True

    def on_tick(self, now_ms, bid, ask, drift, atr):
        mid = (bid+ask)*0.5; spread = ask-bid
        self.warmup_ticks += 1
        self.update_rsi(mid)
        self.update_drift_sus(drift, now_ms)
        self.recent_mid.append(mid)
        if len(self.recent_mid) > 20: self.recent_mid.popleft()

        # Manage open position
        if self.pos_active:
            move = (mid-self.pos_entry) if self.pos_long else (self.pos_entry-mid)
            if move > self.pos_mfe: self.pos_mfe = move
            eff = bid if self.pos_long else ask
            tp_dist = abs(self.pos_tp - self.pos_entry)

            # Partial exit at 50% TP
            if not self.pos_partial and tp_dist > 0 and move >= tp_dist * 0.5:
                self.pos_partial = True
                partial_pnl = move * 0.5 * self.pos_size * 100.0 * 0.5  # 50% position

            # BE
            if not self.pos_be_done and tp_dist > 0 and move >= tp_dist * 0.5:
                self.pos_sl = self.pos_entry; self.pos_be_done = True

            # Trail
            if self.p['trail'] and move >= self.p['trail_arm']:
                trail_sl = (mid - self.p['trail_dist']) if self.pos_long else (mid + self.p['trail_dist'])
                if self.pos_long and trail_sl > self.pos_sl: self.pos_sl = trail_sl
                if not self.pos_long and trail_sl < self.pos_sl: self.pos_sl = trail_sl
                self.pos_trail = True

            # TP
            if (self.pos_long and bid >= self.pos_tp) or (not self.pos_long and ask <= self.pos_tp):
                self._close(eff, 'TP', now_ms); return
            # SL
            if (self.pos_long and bid <= self.pos_sl) or (not self.pos_long and ask >= self.pos_sl):
                reason = 'TRAIL' if self.pos_trail else ('BE' if self.pos_be_done else 'SL')
                self._close(eff, reason, now_ms); return
            # Timeout
            if now_ms - self.pos_ts > self.p['max_hold_ms']:
                self._close(eff, 'TO', now_ms); return
            return

        if self.warmup_ticks < 200: return  # warmup
        if spread > self.p['max_spread']: return
        if atr < self.p['atr_min'] or atr > self.p['atr_max']: return
        if now_ms < self.cooldown_until: return

        # Opposite direction cooldown
        if self.last_closed_dir != 0 and now_ms - self.last_closed_ms < 60000:
            intended = +1 if drift > 0 else -1
            if intended != self.last_closed_dir: return

        # Adverse block
        if self.adverse_block and self.last_loss_dir != 0:
            thresh = self.last_loss_atr * 0.5
            dist = (self.last_loss_exit - mid) if self.last_loss_dir == 1 else (mid - self.last_loss_exit)
            same = (drift > 0 and self.last_loss_dir == 1) or (drift < 0 and self.last_loss_dir == -1)
            if same and dist > thresh: return
            else: self.adverse_block = False

        # RSI direction
        rsi_d = self.rsi_dir()
        if rsi_d == 0: return

        # RSI/drift agreement
        if rsi_d == +1 and drift < 0: return
        if rsi_d == -1 and drift > 0: return

        # Drift threshold
        if abs(drift) < self.p['drift_min']: return

        # Sustained drift check
        if self.drift_sus_ms < self.p['sus_min_ms']: return

        # Price confirmation
        if not self.price_confirms(rsi_d == +1, mid): return

        # Counter-spike block
        if len(self.recent_mid) >= 3:
            oldest = list(self.recent_mid)[-3]
            spike = mid - oldest
            spike_thresh = atr * 0.4
            if rsi_d == +1 and spike <= -spike_thresh: return
            if rsi_d == -1 and spike >= spike_thresh: return

        # Enter
        is_long = (rsi_d == +1)
        sl_pts = atr * self.p['sl_atr_mult']
        tp_pts = sl_pts * self.p['tp_rr']
        entry = ask if is_long else bid
        sl = entry - sl_pts if is_long else entry + sl_pts
        tp = entry + tp_pts if is_long else entry - tp_pts
        size = min(self.p['max_lot'], max(0.01, self.p['risk'] / (sl_pts * 100.0)))
        size = round(size / 0.01) * 0.01

        self.pos_active = True; self.pos_long = is_long
        self.pos_entry = entry; self.pos_sl = sl; self.pos_tp = tp
        self.pos_ts = now_ms; self.pos_mfe = 0.0; self.pos_size = size
        self.pos_be_done = False; self.pos_trail = False; self.pos_partial = False
        self.pos_trail_sl = sl

    def _close(self, ep, reason, now_ms):
        pnl_pts = (ep-self.pos_entry) if self.pos_long else (self.pos_entry-ep)
        pnl_usd = pnl_pts * self.pos_size * 100.0
        self.last_closed_dir = +1 if self.pos_long else -1
        self.last_closed_ms = now_ms
        if pnl_usd < 0:
            self.adverse_block = True
            self.last_loss_exit = ep
            self.last_loss_dir = +1 if self.pos_long else -1
            self.last_loss_atr = self.p['atr_min']
            self.cooldown_until = now_ms + self.p['cooldown_ms']
        from datetime import datetime, timezone
        ts = datetime.fromtimestamp(now_ms/1000, tz=timezone.utc).strftime('%H:%M')
        self.trades.append({
            't': ts, 's': 'L' if self.pos_long else 'S',
            'pnl': pnl_usd, 'mfe': self.pos_mfe,
            'reason': reason, 'held': (now_ms-self.pos_ts)//1000
        })
        self.pos_active = False

def run_bt(csv_file, params):
    cfe = CFEBacktest(params)
    atr_w = deque(); pb = 0.0; av = 2.0
    with open(csv_file, newline='') as f:
        for row in csv.DictReader(f):
            try:
                ms = int(float(row.get('ts_ms', 0)))
                bid = float(row['bid']); ask = float(row['ask'])
                drift = float(row.get('ewm_drift', 0.0))
                atr_pts = float(row.get('atr', 0.0))
            except: continue
            if bid <= 0 or ask <= 0 or ask < bid: continue
            if atr_pts <= 0:
                if pb > 0:
                    tr = ask-bid+abs(bid-pb); atr_w.append(tr)
                    if len(atr_w) > 200: atr_w.popleft()
                    if len(atr_w) >= 50: av = sum(atr_w)/len(atr_w)*14
                atr_pts = av
            pb = bid
            cfe.on_tick(ms, bid, ask, drift, atr_pts)
    if cfe.pos_active:
        cfe._close(cfe.pos_entry, 'END', 0)
    return cfe.trades

def score(trades):
    if not trades: return -999, 0, 0, 0
    wins = [t for t in trades if t['pnl'] > 0]
    total = sum(t['pnl'] for t in trades)
    wr = len(wins)/len(trades) if trades else 0
    exp = total/len(trades) if trades else 0
    return total, wr, len(trades), exp

if len(sys.argv) < 2: print("Usage: python cfe_sweep.py <csv>"); sys.exit(1)

# Parameter grid
param_grid = {
    'rsi_period':       [20, 30],
    'rsi_ema_n':        [8, 12],
    'rsi_thresh':       [2.0, 4.0, 6.0],
    'rsi_max':          [15.0],
    'drift_min':        [0.3, 0.5, 0.8],
    'sus_thresh':       [0.3, 0.5],
    'sus_min_ms':       [15000, 30000, 45000],
    'price_confirm_ticks': [3],
    'price_confirm_min':   [0.05],
    'sl_atr_mult':      [0.4, 0.6, 0.8],
    'tp_rr':            [1.5, 2.0, 2.5],
    'atr_min':          [1.0],
    'atr_max':          [8.0],
    'max_spread':       [0.40],
    'max_lot':          [0.10],
    'risk':             [10.0],
    'cooldown_ms':      [15000, 30000],
    'max_hold_ms':      [180000, 300000],
    'trail':            [False, True],
    'trail_arm':        [1.0],
    'trail_dist':       [0.5],
}

# Generate all combinations
keys = list(param_grid.keys())
values = list(param_grid.values())
combos = list(itertools.product(*values))
print(f"Testing {len(combos)} parameter combinations...")

results = []
for i, combo in enumerate(combos):
    params = dict(zip(keys, combo))
    trades = run_bt(sys.argv[1], params)
    total, wr, n, exp = score(trades)
    if n >= 5:  # need minimum trades
        results.append((total, wr, n, exp, params, trades))
    if (i+1) % 500 == 0:
        print(f"  {i+1}/{len(combos)} done, best so far: ${max((r[0] for r in results), default=0):+.2f}")

results.sort(key=lambda x: x[0], reverse=True)

print(f"\n{'='*65}")
print(f"CFE SWEEP RESULTS -- Top 15 configurations")
print(f"{'='*65}")
print(f"{'Rank':4s} {'PnL':8s} {'WR':6s} {'N':4s} {'Exp':8s} {'drift':6s} {'sus_ms':7s} {'sl':5s} {'tp_rr':6s} {'rsi_t':6s} {'trail':5s}")
for rank, (total, wr, n, exp, p, trades) in enumerate(results[:15], 1):
    print(f"{rank:4d} ${total:+7.2f} {100*wr:5.1f}% {n:4d} ${exp:+6.3f} "
          f"{p['drift_min']:6.1f} {p['sus_min_ms']//1000:5d}s "
          f"{p['sl_atr_mult']:5.2f} {p['tp_rr']:6.2f} "
          f"{p['rsi_thresh']:6.1f} {'Y' if p['trail'] else 'N':5s}")

# Show best config trades
if results:
    best_total, best_wr, best_n, best_exp, best_p, best_trades = results[0]
    print(f"\n{'='*65}")
    print(f"BEST CONFIG TRADES (PnL=${best_total:+.2f}, WR={100*best_wr:.1f}%, {best_n} trades)")
    print(f"Params: drift>={best_p['drift_min']} sus>={best_p['sus_min_ms']//1000}s "
          f"sl={best_p['sl_atr_mult']}xATR tp={best_p['tp_rr']}xSL "
          f"rsi_thresh={best_p['rsi_thresh']} trail={'Y' if best_p['trail'] else 'N'}")
    print(f"\n  {'T':6s} {'S':2s} {'Reason':8s} {'PnL':8s} {'MFE':6s} {'Held'}")
    long_pnl = sum(t['pnl'] for t in best_trades if t['s']=='L')
    short_pnl = sum(t['pnl'] for t in best_trades if t['s']=='S')
    long_n = len([t for t in best_trades if t['s']=='L'])
    short_n = len([t for t in best_trades if t['s']=='S'])
    for t in best_trades:
        print(f"  {t['t']:6s} {t['s']:2s} {t['reason']:8s} ${t['pnl']:+6.2f} {t['mfe']:6.3f} {t['held']}s")
    print(f"\n  LONG:  {long_n} trades ${long_pnl:+.2f}")
    print(f"  SHORT: {short_n} trades ${short_pnl:+.2f}")

    # Also show top 3 configs in detail
    print(f"\n{'='*65}")
    print("TOP 3 CONFIGS DETAIL:")
    for rank, (total, wr, n, exp, p, trades) in enumerate(results[:3], 1):
        from collections import Counter
        reasons = Counter(t['reason'] for t in trades)
        long_pnl = sum(t['pnl'] for t in trades if t['s']=='L')
        short_pnl = sum(t['pnl'] for t in trades if t['s']=='S')
        print(f"\n#{rank} PnL=${total:+.2f} WR={100*wr:.1f}% ({n} trades)")
        print(f"  drift>={p['drift_min']} sus>={p['sus_min_ms']//1000}s sl={p['sl_atr_mult']}xATR "
              f"tp_rr={p['tp_rr']} rsi_t={p['rsi_thresh']} cool={p['cooldown_ms']//1000}s "
              f"hold={p['max_hold_ms']//60000}min trail={'Y' if p['trail'] else 'N'}")
        print(f"  Reasons: {dict(reasons)}")
        print(f"  LONG: {len([t for t in trades if t['s']=='L'])} trades ${long_pnl:+.2f}")
        print(f"  SHORT: {len([t for t in trades if t['s']=='S'])} trades ${short_pnl:+.2f}")
