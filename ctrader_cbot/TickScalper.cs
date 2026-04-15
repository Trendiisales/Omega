// =============================================================================
//  TickScalper.cs -- cTrader Algo cBot
//
//  Standalone tick-pattern scalper for XAUUSD.
//  Runs entirely inside cTrader -- no external connections required.
//  Reads live ticks from cTrader's OnTick() and DOM from MarketDepth.
//
//  THREE ENTRY PATTERNS (all require cost coverage before entry):
//
//  P1 -- TICK MOMENTUM BURST
//    5+ consecutive same-direction bid moves (each tick bid > prev bid)
//    spread <= MAX_SPREAD_ENTRY
//    Net move over last 5 ticks >= MIN_BURST_MOVE
//    Entry: market in burst direction
//    TP: net_move * TP_BURST_MULT  SL: net_move * SL_BURST_MULT
//
//  P2 -- DOM ABSORPTION
//    L2 imbalance > IMB_LONG_THRESH for 5+ consecutive DOM updates (bid wall)
//    or < IMB_SHORT_THRESH for 5+ consecutive DOM updates (ask wall)
//    Price flat or moving with imbalance direction (net move >= -0.05pt)
//    Entry: market in imbalance direction
//    TP: DOM_TP_PT  SL: DOM_SL_PT (fixed, tight)
//
//  P3 -- VELOCITY SPIKE
//    Tick rate spikes > VEL_SPIKE_MULT * 30s baseline AND direction consistent
//    Net directional move in last VEL_WINDOW_TICKS ticks >= MIN_VEL_MOVE
//    Entry: market in spike direction
//    TP: VEL_TP_PT  SL: VEL_SL_PT
//
//  RISK CONTROLS:
//    Max 1 position at a time
//    Max 0.02 lots per trade (configurable, default 0.01)
//    30s cooldown after any exit
//    Session gate: 07:00-22:00 UTC only (London + NY)
//    Daily loss limit: $50 (configurable)
//    Max consecutive losses: 3 then pause 5 minutes
//
//  SHADOW MODE (default ON):
//    When shadow_mode=true: logs signals but does NOT place real orders
//    Set to false via cTrader parameter once shadow logs look correct
//
//  LOGGING:
//    All signals + entries + exits logged to Print() (visible in cTrader log)
//    Trade summary logged on each close: pattern, direction, PnL, hold time
// =============================================================================

using System;
using System.Collections.Generic;
using cAlgo.API;
using cAlgo.API.Internals;

namespace cAlgo.Robots
{
    [Robot(TimeZone = TimeZones.UTC, AccessRights = AccessRights.None)]
    public class TickScalper : Robot
    {
        // ── Parameters ───────────────────────────────────────────────────────
        [Parameter("Shadow Mode (log only, no orders)", DefaultValue = true)]
        public bool ShadowMode { get; set; }

        [Parameter("Max Lot Size", DefaultValue = 0.01, MinValue = 0.01, MaxValue = 0.05)]
        public double MaxLotSize { get; set; }

        [Parameter("Daily Loss Limit ($)", DefaultValue = 50.0, MinValue = 10.0)]
        public double DailyLossLimit { get; set; }

        [Parameter("Max Consecutive Losses", DefaultValue = 3, MinValue = 2)]
        public int MaxConsecLosses { get; set; }

        [Parameter("Cooldown After Exit (seconds)", DefaultValue = 30, MinValue = 10)]
        public int CooldownSec { get; set; }

        [Parameter("Session Start UTC Hour", DefaultValue = 7, MinValue = 0, MaxValue = 23)]
        public int SessionStartUtc { get; set; }

        [Parameter("Session End UTC Hour", DefaultValue = 22, MinValue = 1, MaxValue = 24)]
        public int SessionEndUtc { get; set; }

        [Parameter("Max Spread to Enter (pts)", DefaultValue = 0.50, MinValue = 0.10)]
        public double MaxSpreadEntry { get; set; }

        // ── Pattern 1: Tick Momentum Burst ───────────────────────────────────
        [Parameter("P1: Consecutive ticks required", DefaultValue = 5, MinValue = 3)]
        public int P1BurstTicks { get; set; }

        [Parameter("P1: Min net move (pts)", DefaultValue = 0.20, MinValue = 0.05)]
        public double P1MinBurstMove { get; set; }

        [Parameter("P1: TP multiplier (x net move)", DefaultValue = 2.0, MinValue = 1.0)]
        public double P1TpMult { get; set; }

        [Parameter("P1: SL multiplier (x net move)", DefaultValue = 1.0, MinValue = 0.5)]
        public double P1SlMult { get; set; }

        // ── Pattern 2: DOM Absorption ─────────────────────────────────────────
        [Parameter("P2: Imbalance threshold long (>0.5)", DefaultValue = 0.62, MinValue = 0.55)]
        public double P2ImbLong { get; set; }

        [Parameter("P2: Imbalance threshold short (<0.5)", DefaultValue = 0.38, MinValue = 0.10)]
        public double P2ImbShort { get; set; }

        [Parameter("P2: DOM ticks required", DefaultValue = 5, MinValue = 3)]
        public int P2DomTicks { get; set; }

        [Parameter("P2: TP (pts)", DefaultValue = 0.30, MinValue = 0.10)]
        public double P2TpPt { get; set; }

        [Parameter("P2: SL (pts)", DefaultValue = 0.20, MinValue = 0.10)]
        public double P2SlPt { get; set; }

        // ── Pattern 3: Velocity Spike ─────────────────────────────────────────
        [Parameter("P3: Spike multiplier vs baseline", DefaultValue = 3.0, MinValue = 1.5)]
        public double P3SpikeMult { get; set; }

        [Parameter("P3: Window ticks for direction", DefaultValue = 8, MinValue = 4)]
        public int P3WindowTicks { get; set; }

        [Parameter("P3: Min directional move (pts)", DefaultValue = 0.15, MinValue = 0.05)]
        public double P3MinVelMove { get; set; }

        [Parameter("P3: TP (pts)", DefaultValue = 1.00, MinValue = 0.20)]
        public double P3TpPt { get; set; }

        [Parameter("P3: SL (pts)", DefaultValue = 0.40, MinValue = 0.10)]
        public double P3SlPt { get; set; }

        // ── State ─────────────────────────────────────────────────────────────
        private MarketDepth _depth;

        // Tick history
        private readonly Queue<double> _bidHistory    = new Queue<double>();
        private readonly Queue<DateTime> _tickTimes   = new Queue<DateTime>();
        private const int MAX_TICK_HISTORY = 60;

        // DOM history
        private readonly Queue<double> _imbHistory = new Queue<double>();
        private const int MAX_IMB_HISTORY = 20;

        // Velocity baseline (ticks per 30s rolling)
        private int    _ticksLast30s   = 0;
        private double _velBaseline    = 0.0;
        private DateTime _vel30sStart  = DateTime.MinValue;

        // Trade state
        private Position  _openPos        = null;
        private DateTime  _lastExitTime   = DateTime.MinValue;
        private double    _dailyPnl       = 0.0;
        private DateTime  _dailyResetDate = DateTime.MinValue;
        private int       _consecLosses   = 0;
        private DateTime  _pauseUntil     = DateTime.MinValue;
        private int       _totalTrades    = 0;
        private int       _totalWins      = 0;
        private string    _lastPattern    = "";
        private DateTime  _entryTime      = DateTime.MinValue;

        // ── Startup ───────────────────────────────────────────────────────────
        protected override void OnStart()
        {
            Print("[TS] TickScalper starting. Symbol=" + Symbol.Name
                + " Shadow=" + ShadowMode
                + " MaxLot=" + MaxLotSize
                + " DailyLimit=$" + DailyLossLimit);

            _depth = MarketData.GetMarketDepth(Symbol.Name);
            _depth.Updated += OnDepthUpdated;
            _vel30sStart = Server.Time;

            Print("[TS] DOM feed attached. Session=" + SessionStartUtc + ":00-" + SessionEndUtc + ":00 UTC");
            if (ShadowMode)
                Print("[TS] *** SHADOW MODE -- signals logged, NO orders placed ***");
        }

        // ── Tick handler ──────────────────────────────────────────────────────
        protected override void OnTick()
        {
            var now   = Server.Time;
            var bid   = Symbol.Bid;
            var ask   = Symbol.Ask;
            var spread = ask - bid;

            // Daily PnL reset
            DailyReset(now);

            // Velocity baseline update (count ticks per 30s)
            _ticksLast30s++;
            if ((now - _vel30sStart).TotalSeconds >= 30.0)
            {
                _velBaseline = _velBaseline * 0.7 + _ticksLast30s * 0.3; // EWM
                _ticksLast30s = 0;
                _vel30sStart = now;
            }

            // Maintain bid history
            _bidHistory.Enqueue(bid);
            _tickTimes.Enqueue(now);
            while (_bidHistory.Count > MAX_TICK_HISTORY) { _bidHistory.Dequeue(); _tickTimes.Dequeue(); }

            // Manage open position (live) or shadow
            if (_openPos != null)
            {
                ManagePosition(bid, ask, now);
                return;
            }
            if (_shadowActive)
            {
                CheckShadowHit(bid, ask, now);
                return;
            }

            // Guards
            if (!InSession(now))              return;
            if (_dailyPnl <= -DailyLossLimit) { LogThrottle("[TS] Daily loss limit hit -- stopped", ref _dailyLimitLog, now); return; }
            if (now < _pauseUntil)            { LogThrottle("[TS] Consecutive loss pause active", ref _pauseLog, now); return; }
            if ((now - _lastExitTime).TotalSeconds < CooldownSec) return;
            if (spread > MaxSpreadEntry)      return;

            // Try patterns in priority order
            if (TryP1Burst(bid, ask, spread, now)) return;
            if (TryP3Velocity(bid, ask, spread, now)) return;
            // P2 triggered from OnDepthUpdated

            // Shadow hit check -- runs every tick when shadow position open
            CheckShadowHit(bid, ask, now);
        }

        // ── DOM handler ───────────────────────────────────────────────────────
        private void OnDepthUpdated()
        {
            if (_depth.BidEntries.Count == 0 || _depth.AskEntries.Count == 0) return;

            // Compute level-count imbalance (same as Omega -- size=0 on BlackBull)
            int bids = Math.Min(_depth.BidEntries.Count, 5);
            int asks = Math.Min(_depth.AskEntries.Count, 5);
            double imb = (bids + asks) > 0 ? (double)bids / (bids + asks) : 0.5;

            _imbHistory.Enqueue(imb);
            while (_imbHistory.Count > MAX_IMB_HISTORY) _imbHistory.Dequeue();

            // P2 gate: only try entry if no position + in session + cooldown clear
            var now = Server.Time;
            if (_openPos != null) return;
            if (!InSession(now)) return;
            if (_dailyPnl <= -DailyLossLimit) return;
            if (now < _pauseUntil) return;
            if ((now - _lastExitTime).TotalSeconds < CooldownSec) return;
            if (Symbol.Spread > MaxSpreadEntry) return;

            TryP2Absorption(Symbol.Bid, Symbol.Ask, imb, now);
        }

        // ── Pattern 1: Tick Momentum Burst ───────────────────────────────────
        private bool TryP1Burst(double bid, double ask, double spread, DateTime now)
        {
            if (_bidHistory.Count < P1BurstTicks + 1) return false;

            var bids = new List<double>(_bidHistory);
            int n = bids.Count;

            // Check last P1BurstTicks are all moving same direction
            int upCount = 0, dnCount = 0;
            for (int i = n - P1BurstTicks; i < n; i++)
            {
                double delta = bids[i] - bids[i - 1];
                if (delta > 0) upCount++;
                else if (delta < 0) dnCount++;
            }

            bool burstUp = (upCount >= P1BurstTicks - 1 && dnCount == 0);
            bool burstDn = (dnCount >= P1BurstTicks - 1 && upCount == 0);
            if (!burstUp && !burstDn) return false;

            // Net move
            double netMove = Math.Abs(bids[n - 1] - bids[n - 1 - P1BurstTicks]);
            if (netMove < P1MinBurstMove) return false;

            // Enter
            bool isLong = burstUp;
            double tp = netMove * P1TpMult;
            double sl = netMove * P1SlMult;
            Enter("P1-BURST", isLong, bid, ask, tp, sl, now);
            return true;
        }

        // ── Pattern 2: DOM Absorption ─────────────────────────────────────────
        private void TryP2Absorption(double bid, double ask, double currentImb, DateTime now)
        {
            if (_imbHistory.Count < P2DomTicks) return;

            var imbs = new List<double>(_imbHistory);
            int n = imbs.Count;

            // Check last P2DomTicks all above/below threshold
            int longCount = 0, shortCount = 0;
            for (int i = n - P2DomTicks; i < n; i++)
            {
                if (imbs[i] > P2ImbLong)  longCount++;
                if (imbs[i] < P2ImbShort) shortCount++;
            }

            bool domLong  = (longCount  >= P2DomTicks);
            bool domShort = (shortCount >= P2DomTicks);
            if (!domLong && !domShort) return;

            // Price not moving against imbalance (flat or with it)
            if (_bidHistory.Count >= 3)
            {
                var bids = new List<double>(_bidHistory);
                int nb = bids.Count;
                double recentMove = bids[nb - 1] - bids[nb - 3];
                if (domLong  && recentMove < -0.05) return; // price dropping into bid wall -- skip
                if (domShort && recentMove >  0.05) return; // price rising into ask wall -- skip
            }

            Enter("P2-DOM", domLong, bid, ask, P2TpPt, P2SlPt, now);
        }

        // ── Pattern 3: Velocity Spike ─────────────────────────────────────────
        private bool TryP3Velocity(double bid, double ask, double spread, DateTime now)
        {
            if (_velBaseline < 2.0) return false; // not enough baseline yet
            if (_ticksLast30s < _velBaseline * P3SpikeMult) return false; // no spike

            if (_bidHistory.Count < P3WindowTicks + 1) return false;
            var bids = new List<double>(_bidHistory);
            int n = bids.Count;

            // Net directional move in last P3WindowTicks
            double netMove = bids[n - 1] - bids[n - 1 - P3WindowTicks];
            if (Math.Abs(netMove) < P3MinVelMove) return false;

            bool isLong = (netMove > 0);
            Enter("P3-VEL", isLong, bid, ask, P3TpPt, P3SlPt, now);
            return true;
        }

        // ── Enter ─────────────────────────────────────────────────────────────
        private void Enter(string pattern, bool isLong, double bid, double ask,
                           double tpPts, double slPts, DateTime now)
        {
            double entryPx = isLong ? ask : bid;
            double tpPx    = isLong ? entryPx + tpPts : entryPx - tpPts;
            double slPx    = isLong ? entryPx - slPts : entryPx + slPts;
            double lot     = MaxLotSize;

            // Cost coverage check: expected profit must exceed round-trip cost
            double costPts = Symbol.Spread + 0.20; // spread + commission estimate
            if (tpPts <= costPts)
            {
                Print("[TS-NOCOST] " + pattern + " " + (isLong ? "LONG" : "SHORT")
                    + " tp=" + tpPts.ToString("F2") + " <= cost=" + costPts.ToString("F2") + " -- skip");
                return;
            }

            Print("[TS-SIGNAL] " + pattern + " " + (isLong ? "LONG" : "SHORT")
                + " entry=" + entryPx.ToString("F2")
                + " tp=" + tpPx.ToString("F2") + " sl=" + slPx.ToString("F2")
                + " lot=" + lot.ToString("F2")
                + " spread=" + Symbol.Spread.ToString("F2")
                + (ShadowMode ? " [SHADOW]" : ""));

            if (ShadowMode)
            {
                // Shadow: simulate a position for tracking
                _lastPattern = pattern;
                _entryTime = now;
                // Record as shadow open -- we track in _openPos via a fake position
                // We use a marker: place order with label "SHADOW" and 0 volume if possible,
                // otherwise just track state locally
                _shadowActive   = true;
                _shadowIsLong   = isLong;
                _shadowEntry    = entryPx;
                _shadowTp       = tpPx;
                _shadowSl       = slPx;
                _shadowPattern  = pattern;
                _shadowEntryTs  = now;
                return;
            }

            // Live: place market order
            var result = isLong
                ? ExecuteMarketOrder(TradeType.Buy,  Symbol.Name, lot, pattern, slPts / Symbol.PipSize, tpPts / Symbol.PipSize)
                : ExecuteMarketOrder(TradeType.Sell, Symbol.Name, lot, pattern, slPts / Symbol.PipSize, tpPts / Symbol.PipSize);

            if (result.IsSuccessful)
            {
                _openPos     = result.Position;
                _lastPattern = pattern;
                _entryTime   = now;
                ++_totalTrades;
                Print("[TS-ENTRY] " + pattern + " " + (isLong ? "LONG" : "SHORT")
                    + " @ " + result.Position.EntryPrice.ToString("F2")
                    + " id=" + result.Position.Id);
            }
            else
            {
                Print("[TS-FAIL] Order failed: " + result.Error);
            }
        }

        // ── Shadow state ──────────────────────────────────────────────────────
        private bool     _shadowActive  = false;
        private bool     _shadowIsLong  = false;
        private double   _shadowEntry   = 0.0;
        private double   _shadowTp      = 0.0;
        private double   _shadowSl      = 0.0;
        private string   _shadowPattern = "";
        private DateTime _shadowEntryTs = DateTime.MinValue;

        // ── Manage open position (live) ───────────────────────────────────────
        private void ManagePosition(double bid, double ask, DateTime now)
        {
            if (_openPos == null) return;

            // Check if closed externally (SL/TP hit by broker)
            bool stillOpen = false;
            foreach (var p in Positions)
            {
                if (p.Id == _openPos.Id) { stillOpen = true; break; }
            }
            if (!stillOpen)
            {
                // Position closed -- find result in history
                double pnl = 0.0;
                foreach (var h in History)
                {
                    if (h.PositionId == _openPos.Id) { pnl = h.NetProfit; break; }
                }
                OnPositionClosed(_lastPattern, _openPos.TradeType == TradeType.Buy, pnl,
                                 (now - _entryTime).TotalSeconds, now);
                _openPos = null;
            }
        }

        // ── OnTick shadow management ──────────────────────────────────────────
        // Shadow hit checks run inside OnTick after the bid/ask are updated
        // We run this check at end of OnTick when shadowActive
        private void CheckShadowHit(double bid, double ask, DateTime now)
        {
            if (!_shadowActive) return;

            double mid = (bid + ask) * 0.5;
            bool tpHit = _shadowIsLong ? bid >= _shadowTp : ask <= _shadowTp;
            bool slHit = _shadowIsLong ? bid <= _shadowSl : ask >= _shadowSl;

            if (tpHit || slHit)
            {
                double exitPx = _shadowIsLong ? bid : ask;
                double pnl    = _shadowIsLong ? (exitPx - _shadowEntry) * MaxLotSize * 100.0
                                              : (_shadowEntry - exitPx) * MaxLotSize * 100.0;
                string reason = tpHit ? "TP" : "SL";
                Print("[TS-SHADOW-EXIT] " + _shadowPattern + " " + (_shadowIsLong ? "LONG" : "SHORT")
                    + " @ " + exitPx.ToString("F2")
                    + " " + reason
                    + " pnl=$" + pnl.ToString("F2")
                    + " held=" + (now - _shadowEntryTs).TotalSeconds.ToString("F0") + "s");
                OnPositionClosed(_shadowPattern, _shadowIsLong, pnl,
                                 (now - _shadowEntryTs).TotalSeconds, now);
                _shadowActive = false;
            }

            // Also check timeout (2 min)
            if ((now - _shadowEntryTs).TotalSeconds > 120.0)
            {
                double exitPx = _shadowIsLong ? bid : ask;
                double pnl    = _shadowIsLong ? (exitPx - _shadowEntry) * MaxLotSize * 100.0
                                              : (_shadowEntry - exitPx) * MaxLotSize * 100.0;
                Print("[TS-SHADOW-TIMEOUT] " + _shadowPattern
                    + " pnl=$" + pnl.ToString("F2"));
                OnPositionClosed(_shadowPattern, _shadowIsLong, pnl, 120.0, now);
                _shadowActive = false;
            }
        }

        // Override OnTick to also check shadow
        // Already called from OnTick -- add CheckShadowHit at end
        // We hook by overriding the method and calling CheckShadowHit after base logic
        // Done at end of OnTick above

        // ── Position closed callback ──────────────────────────────────────────
        private void OnPositionClosed(string pattern, bool wasLong, double pnl, double holdSec, DateTime now)
        {
            _dailyPnl    += pnl;
            _lastExitTime = now;
            ++_totalTrades;

            bool win = pnl > 0;
            if (win) { ++_totalWins; _consecLosses = 0; }
            else     { ++_consecLosses; }

            Print("[TS-CLOSE] " + pattern + " " + (wasLong ? "LONG" : "SHORT")
                + " pnl=$" + pnl.ToString("F2")
                + " held=" + holdSec.ToString("F0") + "s"
                + " daily=$" + _dailyPnl.ToString("F2")
                + " W/T=" + _totalWins + "/" + _totalTrades
                + " consec_loss=" + _consecLosses);

            if (_consecLosses >= MaxConsecLosses)
            {
                _pauseUntil = now.AddMinutes(5);
                Print("[TS] " + _consecLosses + " consecutive losses -- pausing 5 minutes");
                _consecLosses = 0;
            }
        }

        // ── Helpers ───────────────────────────────────────────────────────────
        private bool InSession(DateTime utcNow)
        {
            int h = utcNow.Hour;
            return h >= SessionStartUtc && h < SessionEndUtc;
        }

        private void DailyReset(DateTime now)
        {
            if (now.Date != _dailyResetDate.Date)
            {
                if (_dailyResetDate != DateTime.MinValue)
                    Print("[TS-DAILY] Reset. Yesterday PnL=$" + _dailyPnl.ToString("F2")
                        + " Trades=" + _totalTrades + " WR=" +
                        ((_totalTrades > 0) ? (100.0 * _totalWins / _totalTrades).ToString("F0") : "0") + "%");
                _dailyPnl       = 0.0;
                _dailyResetDate = now;
            }
        }

        // Log throttle helpers (prevent log spam)
        private DateTime _dailyLimitLog = DateTime.MinValue;
        private DateTime _pauseLog      = DateTime.MinValue;

        private void LogThrottle(string msg, ref DateTime last, DateTime now)
        {
            if ((now - last).TotalSeconds >= 30.0)
            {
                Print(msg);
                last = now;
            }
        }

        // ── OnTick shadow check (added at end of OnTick) ──────────────────────
        // We need to call CheckShadowHit from OnTick. Since C# doesn't allow
        // partial methods easily in cTrader, we restructure OnTick to call it.
        // The OnTick above returns early on various guards -- we insert the call
        // via position close event instead.

        protected override void OnPositionClosed(PositionClosedEventArgs args)
        {
            if (_openPos != null && args.Position.Id == _openPos.Id)
            {
                double pnl = args.Position.NetProfit;
                OnPositionClosed(_lastPattern, args.Position.TradeType == TradeType.Buy,
                                 pnl, (Server.Time - _entryTime).TotalSeconds, Server.Time);
                _openPos = null;
            }
        }

        protected override void OnStop()
        {
            Print("[TS] Stopped. Total trades=" + _totalTrades
                + " Wins=" + _totalWins
                + " WR=" + ((_totalTrades > 0) ? (100.0 * _totalWins / _totalTrades).ToString("F0") : "0") + "%"
                + " DailyPnL=$" + _dailyPnl.ToString("F2"));
        }
    }
}
