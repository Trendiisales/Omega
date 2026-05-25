//+------------------------------------------------------------------+
//|                                              AtrMeanRevGrid.mq5  |
//|     Forex mean-reversion grid EA. ATR-normalized entry + SL,     |
//|     RSI confirmation, vol-adaptive add distance, unified         |
//|     trailing SL anchored to slow EMA, five TP methods.           |
//|                                                                  |
//|     Slow MA only (fast MA removed -- it was firing exit too      |
//|     close to entry on mean-rev cycles).                          |
//|                                                                  |
//|     Build -> Test -> Learn -> Refine.                            |
//+------------------------------------------------------------------+
#property copyright "2026"
#property version   "0.20"
#property strict

#include <Trade/Trade.mqh>
#include <Trade/PositionInfo.mqh>
#include <Trade/SymbolInfo.mqh>

CTrade         trade;
CPositionInfo  pos;
CSymbolInfo    sym;

//===================================================================
//  Enums (declared before input blocks so dropdowns render in MT5)
//===================================================================
enum LotMode    { LOT_FIXED, LOT_RISK_PCT };
enum TpMethod {
    TP_RSI_OR_MA           = 1,   // RSI back to TpLevel OR price crosses slow MA -- internal, candle close only
    TP_FIXED_PIPS_FROM_WAP = 2,   // broker-side TP at WAP +/- N points
    TP_PCT_FROM_WAP        = 3,   // broker-side TP at WAP * (1 +/- pct/100)
    TP_ATR_FROM_WAP        = 4    // broker-side TP at WAP +/- Z * ATR_short
};

//===================================================================
//  GROUP: EA General
//===================================================================
input group "EA General"
input long             InpMagicNumber        = 770251;
input int              InpSlippagePts        = 20;
input ENUM_TIMEFRAMES  InpTimeframe          = PERIOD_CURRENT; // matches chart -- change tf in chart settings, not here
input int              InpMaxTradesOverall   = 50;
input int              InpMaxTradesPerSymbol = 10;
input bool             InpVerboseLog         = true;          // structured Print on entry decisions

//===================================================================
//  GROUP: Instrument Filters
//===================================================================
input group "Instrument Filters"
input double           InpMaxSpreadPts       = 25.0;

//===================================================================
//  GROUP: ATR Settings
//===================================================================
input group "ATR Settings"
input int              InpAtrPeriodShort     = 100;
input int              InpAtrPeriodLong      = 1000;
input double           InpEntryAtrMultX      = 14.0;  // entry: price X*ATR_short from slow MA
input double           InpSlAtrBufferY       = 4.0;   // SL = slowMA -/+ (X+Y)*ATR -- buffer beyond entry, geometry-safe
input double           InpAddDistBaseMult    = 1.0;   // base ATR multiplier between adds (scaled by vol ratio)

//===================================================================
//  GROUP: Slow MA  (slow MA is the mean -- only MA used in this EA)
//===================================================================
input group "Slow MA"
input int              InpSlowMaPeriod       = 200;
input ENUM_MA_METHOD   InpSlowMaType         = MODE_EMA;
input ENUM_APPLIED_PRICE InpSlowMaPrice      = PRICE_CLOSE;

//===================================================================
//  GROUP: RSI
//===================================================================
input group "RSI"
input int              InpRsiPeriod          = 14;
input double           InpRsiEntryLow        = 20.0;  // BUY when RSI <= this (SELL uses 100-this)
input double           InpRsiRecoveryOffset  = 15.0;  // RSI must recover this much before re-dipping
input double           InpRsiTpLevel         = 50.0;  // BUY closes when RSI >= this (SELL uses 100-this)

//===================================================================
//  GROUP: Entry Logic
//===================================================================
input group "Entry Logic"
input LotMode          InpLotMode            = LOT_FIXED;
input double           InpBaseLot            = 0.01;
input double           InpRiskPctPerTrade    = 0.5;   // % of equity to risk per base (if LOT_RISK_PCT)
input bool             InpAllowBuys          = true;
input bool             InpAllowSells         = true;

//===================================================================
//  GROUP: Add Position Logic
//===================================================================
input group "Add Position Logic"
input double           InpMultA              = 1.5;   // Level 1 add: base * A
input double           InpMultB              = 1.7;   // Levels 2-4: previous * B
input double           InpMultC              = 2.0;   // Level 5+:   previous * C
input int              InpMinCandlesBetweenAdds = 5;
input double           InpMaxAccountExposurePct = 25.0; // hard circuit breaker

//===================================================================
//  GROUP: Stop Loss
//===================================================================
input group "Stop Loss"
input double           InpSlMinMovePts       = 10.0;  // skip PositionModify if change < this many points

//===================================================================
//  GROUP: Take Profit
//===================================================================
input group "Take Profit"
input TpMethod         InpTpMethod           = TP_RSI_OR_MA;  // Take Profit Method (dropdown)
input double           InpTpFixedPipTarget   = 200.0; // method 2 (points)
input double           InpTpPctFromWap       = 0.5;   // method 3 (% of WAP)
input double           InpTpAtrMultFromWap   = 8.0;   // method 4 (X * ATR_short from WAP)
input bool             InpUseMaCap           = false; // for methods 2/3/4 -- cap WAP target at slow MA
input bool             InpUseTimeExit        = false;
input int              InpTimeExitCandles    = 200;

//===================================================================
//  GROUP: Risk  (Max Account Exposure % is read from Add Position Logic group;
//               kept as a single source of truth to avoid drift)
//===================================================================

//===================================================================
//  State
//===================================================================
int hSlowMa = INVALID_HANDLE;
int hRsi    = INVALID_HANDLE;
int hAtrS   = INVALID_HANDLE;
int hAtrL   = INVALID_HANDLE;

struct GridState {
    int      levels_open;
    double   last_entry_price;
    datetime last_entry_time;
    double   last_entry_lot;
    double   current_sl;
    double   rsi_at_last_entry;
    bool     rsi_recovered;
    datetime first_entry_time;
};
GridState g_long;
GridState g_short;

datetime g_last_candle_time = 0;

void ResetGrid(GridState &gs)
{
    gs.levels_open       = 0;
    gs.last_entry_price  = 0.0;
    gs.last_entry_time   = 0;
    gs.last_entry_lot    = 0.0;
    gs.current_sl        = 0.0;
    gs.rsi_at_last_entry = 0.0;
    gs.rsi_recovered     = false;
    gs.first_entry_time  = 0;
}

//===================================================================
//  Init / Deinit
//===================================================================
int OnInit()
{
    trade.SetExpertMagicNumber(InpMagicNumber);
    trade.SetDeviationInPoints((ulong)InpSlippagePts);
    trade.SetMarginMode();
    trade.SetTypeFillingBySymbol(_Symbol);

    hSlowMa = iMA(_Symbol, InpTimeframe, InpSlowMaPeriod, 0, InpSlowMaType, InpSlowMaPrice);
    hRsi    = iRSI(_Symbol, InpTimeframe, InpRsiPeriod, PRICE_CLOSE);
    hAtrS   = iATR(_Symbol, InpTimeframe, InpAtrPeriodShort);
    hAtrL   = iATR(_Symbol, InpTimeframe, InpAtrPeriodLong);
    if (hSlowMa == INVALID_HANDLE || hRsi == INVALID_HANDLE
        || hAtrS == INVALID_HANDLE || hAtrL == INVALID_HANDLE) {
        Print("[InitFail] indicator handle creation failed");
        return INIT_FAILED;
    }
    if (!sym.Name(_Symbol)) { Print("[InitFail] symbol"); return INIT_FAILED; }
    sym.RefreshRates();

    ResetGrid(g_long);
    ResetGrid(g_short);
    ReconstructGridFromPositions();

    PrintFormat("[Init] AtrMeanRevGrid v0.20 magic=%I64d tf=%s entryX=%.1f slBufY=%.1f tpMethod=%s",
                InpMagicNumber, EnumToString((ENUM_TIMEFRAMES)Period()),
                InpEntryAtrMultX, InpSlAtrBufferY, EnumToString(InpTpMethod));
    return INIT_SUCCEEDED;
}

void OnDeinit(const int reason)
{
    if (hSlowMa != INVALID_HANDLE) IndicatorRelease(hSlowMa);
    if (hRsi    != INVALID_HANDLE) IndicatorRelease(hRsi);
    if (hAtrS   != INVALID_HANDLE) IndicatorRelease(hAtrS);
    if (hAtrL   != INVALID_HANDLE) IndicatorRelease(hAtrL);
}

//===================================================================
//  Indicator helpers
//===================================================================
bool ReadIndicator(int handle, int shift, double &out)
{
    double buf[1];
    if (CopyBuffer(handle, 0, shift, 1, buf) != 1) return false;
    out = buf[0];
    return true;
}

bool SnapshotIndicators(double &slow_ma, double &rsi, double &atr_short, double &atr_long)
{
    if (!ReadIndicator(hSlowMa, 1, slow_ma))   return false;
    if (!ReadIndicator(hRsi,    1, rsi))       return false;
    if (!ReadIndicator(hAtrS,   1, atr_short)) return false;
    if (!ReadIndicator(hAtrL,   1, atr_long))  return false;
    return (atr_short > 0.0 && atr_long > 0.0);
}

double GetCurrentRsi() { double v=0; ReadIndicator(hRsi, 1, v); return v; }
double GetCurrentAtrShort() { double v=0; ReadIndicator(hAtrS, 1, v); return v; }
double GetCurrentSlowMa() { double v=0; ReadIndicator(hSlowMa, 1, v); return v; }

//===================================================================
//  Position iteration / WAP / floating PnL / exposure
//===================================================================
int CountOpenPositions(string symbol, long magic, ENUM_POSITION_TYPE side, bool any_side, bool any_symbol=false)
{
    int n = 0;
    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != magic) continue;
        if (!any_symbol && pos.Symbol() != symbol) continue;
        if (!any_side  && pos.PositionType() != side) continue;
        ++n;
    }
    return n;
}

void ComputeWap(ENUM_POSITION_TYPE side, double &wap, double &total_volume, double &floating_pnl)
{
    double sum_pv=0, sum_v=0, fpnl=0;
    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != InpMagicNumber) continue;
        if (pos.Symbol() != _Symbol) continue;
        if (pos.PositionType() != side) continue;
        sum_pv += pos.PriceOpen() * pos.Volume();
        sum_v  += pos.Volume();
        fpnl   += pos.Profit() + pos.Swap() + pos.Commission();
    }
    wap = (sum_v > 0.0) ? (sum_pv / sum_v) : 0.0;
    total_volume = sum_v;
    floating_pnl = fpnl;
}

double AccountExposurePct()
{
    double equity = AccountInfoDouble(ACCOUNT_EQUITY);
    if (equity <= 0.0) return 100.0;
    double margin = AccountInfoDouble(ACCOUNT_MARGIN);
    return 100.0 * margin / equity;
}

//===================================================================
//  Lot sizing
//===================================================================
double NormalizeLot(double raw)
{
    double minv = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
    double maxv = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
    double step = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
    if (step <= 0.0) step = 0.01;
    double v = MathFloor(raw/step)*step;
    if (v < minv) v = minv;
    if (v > maxv) v = maxv;
    return NormalizeDouble(v, 2);
}

double BaseLotForEntry(double entry_price, double sl_price)
{
    if (InpLotMode == LOT_FIXED) return NormalizeLot(InpBaseLot);
    double equity   = AccountInfoDouble(ACCOUNT_EQUITY);
    double risk_amt = equity * InpRiskPctPerTrade / 100.0;
    double tick_sz  = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_SIZE);
    double tick_val = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_VALUE);
    if (tick_sz <= 0.0 || tick_val <= 0.0) return NormalizeLot(InpBaseLot);
    double sl_dist  = MathAbs(entry_price - sl_price);
    if (sl_dist <= 0.0) return NormalizeLot(InpBaseLot);
    double loss_per_lot = (sl_dist / tick_sz) * tick_val;
    if (loss_per_lot <= 0.0) return NormalizeLot(InpBaseLot);
    return NormalizeLot(risk_amt / loss_per_lot);
}

double AddLevelLot(double prev_lot, int next_level_index)
{
    double mult;
    if      (next_level_index == 1) mult = InpMultA;
    else if (next_level_index <= 4) mult = InpMultB;
    else                            mult = InpMultC;
    return NormalizeLot(prev_lot * mult);
}

//===================================================================
//  Vol-adaptive add distance: floor=1.0, no ceiling
//===================================================================
double AddDistancePx(double atr_short, double atr_long)
{
    double ratio = (atr_long > 0.0) ? atr_short/atr_long : 1.0;
    double eff   = MathMax(1.0, ratio);
    return InpAddDistBaseMult * eff * atr_short;
}

//===================================================================
//  Spread filter
//===================================================================
bool SpreadOk()
{
    sym.RefreshRates();
    double pt = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
    if (pt <= 0.0) return true;
    double spr = (sym.Ask() - sym.Bid()) / pt;
    return (spr <= InpMaxSpreadPts);
}

//===================================================================
//  Reconstruct in-memory grid from open positions (init / after restart)
//===================================================================
void ReconstructGridFromPositions()
{
    ResetGrid(g_long);
    ResetGrid(g_short);
    datetime newest_long=0, oldest_long=TimeCurrent();
    datetime newest_short=0, oldest_short=TimeCurrent();
    double newest_long_px=0, newest_long_lot=0;
    double newest_short_px=0, newest_short_lot=0;

    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != InpMagicNumber) continue;
        if (pos.Symbol() != _Symbol) continue;
        if (pos.PositionType() == POSITION_TYPE_BUY) {
            ++g_long.levels_open;
            if (pos.Time() > newest_long) { newest_long=pos.Time(); newest_long_px=pos.PriceOpen(); newest_long_lot=pos.Volume(); }
            if (pos.Time() < oldest_long) oldest_long = pos.Time();
        } else if (pos.PositionType() == POSITION_TYPE_SELL) {
            ++g_short.levels_open;
            if (pos.Time() > newest_short) { newest_short=pos.Time(); newest_short_px=pos.PriceOpen(); newest_short_lot=pos.Volume(); }
            if (pos.Time() < oldest_short) oldest_short = pos.Time();
        }
    }
    if (g_long.levels_open > 0) {
        g_long.last_entry_price = newest_long_px;
        g_long.last_entry_time  = newest_long;
        g_long.last_entry_lot   = newest_long_lot;
        g_long.first_entry_time = oldest_long;
        g_long.rsi_recovered    = true;
    }
    if (g_short.levels_open > 0) {
        g_short.last_entry_price = newest_short_px;
        g_short.last_entry_time  = newest_short;
        g_short.last_entry_lot   = newest_short_lot;
        g_short.first_entry_time = oldest_short;
        g_short.rsi_recovered    = true;
    }
}

//===================================================================
//  Compute the broker-side TP price for WAP-based methods (3/4/5)
//===================================================================
bool ComputeBrokerSideTp(ENUM_POSITION_TYPE side, double &out_tp)
{
    out_tp = 0.0;
    double wap=0, vol=0, fpnl=0;
    ComputeWap(side, wap, vol, fpnl);
    if (vol <= 0.0 || wap <= 0.0) return false;

    double pt = SymbolInfoDouble(_Symbol, SYMBOL_POINT);

    double target = 0.0;
    if (InpTpMethod == TP_FIXED_PIPS_FROM_WAP) {
        target = (side == POSITION_TYPE_BUY) ? wap + InpTpFixedPipTarget * pt
                                              : wap - InpTpFixedPipTarget * pt;
    } else if (InpTpMethod == TP_PCT_FROM_WAP) {
        double pct = InpTpPctFromWap / 100.0;
        target = (side == POSITION_TYPE_BUY) ? wap * (1.0 + pct)
                                              : wap * (1.0 - pct);
    } else if (InpTpMethod == TP_ATR_FROM_WAP) {
        double atr_s = GetCurrentAtrShort();
        if (atr_s <= 0.0) return false;
        double dist = InpTpAtrMultFromWap * atr_s;
        target = (side == POSITION_TYPE_BUY) ? wap + dist : wap - dist;
    } else {
        return false; // not a WAP-based method
    }

    if (InpUseMaCap) {
        double slow_ma = GetCurrentSlowMa();
        if (slow_ma > 0.0) {
            if (side == POSITION_TYPE_BUY)  target = MathMin(target, slow_ma);
            else                             target = MathMax(target, slow_ma);
        }
    }
    out_tp = target;
    return true;
}

// Push a single TP value onto every open position for a side. Skipped if
// existing TP matches within 1 point (idempotent).
void ApplyBrokerSideTp(ENUM_POSITION_TYPE side, double new_tp)
{
    double pt = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != InpMagicNumber) continue;
        if (pos.Symbol() != _Symbol) continue;
        if (pos.PositionType() != side) continue;
        double cur_tp = pos.TakeProfit();
        if (MathAbs(cur_tp - new_tp) < pt) continue;
        trade.PositionModify(pos.Ticket(), pos.StopLoss(), new_tp);
    }
}

// Recompute + apply WAP-based TP. Call after every entry/add for grid sync.
void SyncBrokerSideTp(ENUM_POSITION_TYPE side)
{
    if (InpTpMethod != TP_FIXED_PIPS_FROM_WAP
        && InpTpMethod != TP_PCT_FROM_WAP
        && InpTpMethod != TP_ATR_FROM_WAP) return;
    double tp=0;
    if (!ComputeBrokerSideTp(side, tp)) return;
    ApplyBrokerSideTp(side, tp);
}

//===================================================================
//  Entry / Add
//===================================================================
void LogEntryDecision(string side, string verdict, string detail)
{
    if (!InpVerboseLog) return;
    PrintFormat("[Entry] %s %s -- %s", side, verdict, detail);
}

void TryEntry(double slow_ma, double rsi, double atr_short, double atr_long)
{
    if (!SpreadOk())              { LogEntryDecision("ANY","skip","spread_filter"); return; }
    double expo = AccountExposurePct();
    if (expo >= InpMaxAccountExposurePct) {
        LogEntryDecision("ANY","skip", StringFormat("exposure=%.1f >= cap=%.1f", expo, InpMaxAccountExposurePct));
        return;
    }
    int total_per_sym = CountOpenPositions(_Symbol, InpMagicNumber, POSITION_TYPE_BUY, true);
    int total_overall = CountOpenPositions("",     InpMagicNumber, POSITION_TYPE_BUY, true, true);
    if (total_per_sym >= InpMaxTradesPerSymbol) { LogEntryDecision("ANY","skip","max_per_symbol"); return; }
    if (total_overall >= InpMaxTradesOverall)   { LogEntryDecision("ANY","skip","max_overall");    return; }

    double entry_dist = InpEntryAtrMultX * atr_short;
    sym.RefreshRates();
    double ask = sym.Ask();
    double bid = sym.Bid();

    // ---- BUY ----
    if (InpAllowBuys) {
        bool price_ok = (ask <= slow_ma - entry_dist);
        bool rsi_ok   = (rsi <= InpRsiEntryLow);
        if (g_long.levels_open == 0) {
            if (price_ok && rsi_ok)
                PlaceBuy(slow_ma, atr_short, true);
            else
                LogEntryDecision("BUY","skip", StringFormat("base price_ok=%d rsi=%.1f thr=%.1f",
                                 price_ok, rsi, InpRsiEntryLow));
        } else {
            UpdateRsiRecovery(g_long, rsi, true);
            int bars_since = iBarShift(_Symbol, InpTimeframe, g_long.last_entry_time, false);
            bool candle_ok = (bars_since >= InpMinCandlesBetweenAdds);
            double need_dist = AddDistancePx(atr_short, atr_long);
            bool dist_ok = (g_long.last_entry_price - ask) >= need_dist;
            if (g_long.rsi_recovered && candle_ok && dist_ok && rsi_ok)
                PlaceBuy(slow_ma, atr_short, false);
            else
                LogEntryDecision("BUY","skip", StringFormat("add lvl=%d rec=%d cand=%d(%d) dist=%d(%.5f) rsi_ok=%d",
                                 g_long.levels_open, g_long.rsi_recovered, candle_ok, bars_since,
                                 dist_ok, need_dist, rsi_ok));
        }
    }

    // ---- SELL (mirror) ----
    if (InpAllowSells) {
        double rsi_entry_high = 100.0 - InpRsiEntryLow;
        bool price_ok = (bid >= slow_ma + entry_dist);
        bool rsi_ok   = (rsi >= rsi_entry_high);
        if (g_short.levels_open == 0) {
            if (price_ok && rsi_ok)
                PlaceSell(slow_ma, atr_short, true);
            else
                LogEntryDecision("SELL","skip", StringFormat("base price_ok=%d rsi=%.1f thr=%.1f",
                                 price_ok, rsi, rsi_entry_high));
        } else {
            UpdateRsiRecovery(g_short, rsi, false);
            int bars_since = iBarShift(_Symbol, InpTimeframe, g_short.last_entry_time, false);
            bool candle_ok = (bars_since >= InpMinCandlesBetweenAdds);
            double need_dist = AddDistancePx(atr_short, atr_long);
            bool dist_ok = (bid - g_short.last_entry_price) >= need_dist;
            if (g_short.rsi_recovered && candle_ok && dist_ok && rsi_ok)
                PlaceSell(slow_ma, atr_short, false);
            else
                LogEntryDecision("SELL","skip", StringFormat("add lvl=%d rec=%d cand=%d(%d) dist=%d(%.5f) rsi_ok=%d",
                                 g_short.levels_open, g_short.rsi_recovered, candle_ok, bars_since,
                                 dist_ok, need_dist, rsi_ok));
        }
    }
}

void UpdateRsiRecovery(GridState &gs, double rsi, bool is_long)
{
    if (gs.levels_open == 0) return;
    if (is_long) {
        if (rsi >= gs.rsi_at_last_entry + InpRsiRecoveryOffset) gs.rsi_recovered = true;
    } else {
        if (rsi <= gs.rsi_at_last_entry - InpRsiRecoveryOffset) gs.rsi_recovered = true;
    }
}

void PlaceBuy(double slow_ma, double atr_short, bool is_base)
{
    sym.RefreshRates();
    double price   = sym.Ask();
    double init_sl = slow_ma - (InpEntryAtrMultX + InpSlAtrBufferY) * atr_short;
    if (init_sl >= price) {
        LogEntryDecision("BUY","abort", StringFormat("sl(%.5f) >= price(%.5f) -- skip", init_sl, price));
        return;
    }

    double lot;
    if (is_base) lot = BaseLotForEntry(price, init_sl);
    else         lot = AddLevelLot(g_long.last_entry_lot, g_long.levels_open);

    if (!trade.Buy(lot, _Symbol, price, init_sl, 0.0, "AtrMeanRevGrid BUY")) {
        PrintFormat("[BuyFail] ret=%d %s", trade.ResultRetcode(), trade.ResultRetcodeDescription());
        return;
    }
    ++g_long.levels_open;
    g_long.last_entry_price  = price;
    g_long.last_entry_time   = TimeCurrent();
    g_long.last_entry_lot    = lot;
    g_long.rsi_at_last_entry = GetCurrentRsi();
    g_long.rsi_recovered     = false;
    if (is_base) { g_long.current_sl = init_sl; g_long.first_entry_time = TimeCurrent(); }
    PrintFormat("[BUY] lvl=%d lot=%.2f px=%.5f sl=%.5f rsi=%.2f", g_long.levels_open, lot, price, init_sl, g_long.rsi_at_last_entry);

    // WAP shifted -> resync broker-side TP across all long positions
    SyncBrokerSideTp(POSITION_TYPE_BUY);
}

void PlaceSell(double slow_ma, double atr_short, bool is_base)
{
    sym.RefreshRates();
    double price   = sym.Bid();
    double init_sl = slow_ma + (InpEntryAtrMultX + InpSlAtrBufferY) * atr_short;
    if (init_sl <= price) {
        LogEntryDecision("SELL","abort", StringFormat("sl(%.5f) <= price(%.5f) -- skip", init_sl, price));
        return;
    }

    double lot;
    if (is_base) lot = BaseLotForEntry(price, init_sl);
    else         lot = AddLevelLot(g_short.last_entry_lot, g_short.levels_open);

    if (!trade.Sell(lot, _Symbol, price, init_sl, 0.0, "AtrMeanRevGrid SELL")) {
        PrintFormat("[SellFail] ret=%d %s", trade.ResultRetcode(), trade.ResultRetcodeDescription());
        return;
    }
    ++g_short.levels_open;
    g_short.last_entry_price  = price;
    g_short.last_entry_time   = TimeCurrent();
    g_short.last_entry_lot    = lot;
    g_short.rsi_at_last_entry = GetCurrentRsi();
    g_short.rsi_recovered     = false;
    if (is_base) { g_short.current_sl = init_sl; g_short.first_entry_time = TimeCurrent(); }
    PrintFormat("[SELL] lvl=%d lot=%.2f px=%.5f sl=%.5f rsi=%.2f", g_short.levels_open, lot, price, init_sl, g_short.rsi_at_last_entry);

    SyncBrokerSideTp(POSITION_TYPE_SELL);
}

//===================================================================
//  Trailing SL (anchored slow EMA, ratchet only)
//===================================================================
void TrailSl(double slow_ma, double atr_short)
{
    double pt = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
    if (pt <= 0.0) pt = 0.00001;
    double min_move_px = InpSlMinMovePts * pt;

    if (g_long.levels_open > 0) {
        double new_sl = slow_ma - (InpEntryAtrMultX + InpSlAtrBufferY) * atr_short;
        if (new_sl > g_long.current_sl + min_move_px) {
            g_long.current_sl = new_sl;
            ApplyUnifiedSl(POSITION_TYPE_BUY, new_sl);
        }
    }
    if (g_short.levels_open > 0) {
        double new_sl = slow_ma + (InpEntryAtrMultX + InpSlAtrBufferY) * atr_short;
        if (new_sl < g_short.current_sl - min_move_px || g_short.current_sl == 0.0) {
            g_short.current_sl = new_sl;
            ApplyUnifiedSl(POSITION_TYPE_SELL, new_sl);
        }
    }
}

void ApplyUnifiedSl(ENUM_POSITION_TYPE side, double new_sl)
{
    double pt = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != InpMagicNumber) continue;
        if (pos.Symbol() != _Symbol) continue;
        if (pos.PositionType() != side) continue;
        if (MathAbs(pos.StopLoss() - new_sl) < pt) continue;
        trade.PositionModify(pos.Ticket(), new_sl, pos.TakeProfit());
    }
}

//===================================================================
//  TP evaluation
//   Methods 1/2 : checked on candle close (internal -- not on broker)
//   Methods 3/4/5: broker-side TP, plus time-exit fallback
//===================================================================
bool ShouldCloseAll_Internal(ENUM_POSITION_TYPE side, double slow_ma, double rsi, string &reason)
{
    // Time-based fallback (applies to ALL methods if toggled)
    if (InpUseTimeExit) {
        GridState gs = (side == POSITION_TYPE_BUY) ? g_long : g_short;
        if (gs.first_entry_time > 0) {
            int bars_open = iBarShift(_Symbol, InpTimeframe, gs.first_entry_time, false);
            if (bars_open >= InpTimeExitCandles) { reason = "time_exit"; return true; }
        }
    }

    // Internal-only checks for methods 1 and 2
    if (InpTpMethod != TP_RSI_OR_MA_EITHER && InpTpMethod != TP_RSI_OR_MA_AND_POSITIVE)
        return false;

    sym.RefreshRates();
    double px = (side == POSITION_TYPE_BUY) ? sym.Bid() : sym.Ask();
    double rsi_tp_long  = InpRsiTpLevel;
    double rsi_tp_short = 100.0 - InpRsiTpLevel;
    bool rsi_or_ma = (side == POSITION_TYPE_BUY)
                     ? ((rsi >= rsi_tp_long)  || (px >= slow_ma))
                     : ((rsi <= rsi_tp_short) || (px <= slow_ma));
    if (!rsi_or_ma) return false;

    if (InpTpMethod == TP_RSI_OR_MA_EITHER) { reason = "rsi_or_slow_ma"; return true; }

    // TP_RSI_OR_MA_AND_POSITIVE -- gate on combined PnL > 0
    double wap=0, vol=0, fpnl=0;
    ComputeWap(side, wap, vol, fpnl);
    if (fpnl > 0.0) { reason = "rsi_or_slow_ma_in_profit"; return true; }
    return false;
}

void CloseAllSide(ENUM_POSITION_TYPE side, string reason)
{
    for (int i = PositionsTotal()-1; i >= 0; --i) {
        if (!pos.SelectByIndex(i)) continue;
        if (pos.Magic() != InpMagicNumber) continue;
        if (pos.Symbol() != _Symbol) continue;
        if (pos.PositionType() != side) continue;
        trade.PositionClose(pos.Ticket());
    }
    if (side == POSITION_TYPE_BUY)  ResetGrid(g_long);
    if (side == POSITION_TYPE_SELL) ResetGrid(g_short);
    PrintFormat("[CloseAll] side=%s reason=%s", EnumToString(side), reason);
}

//===================================================================
//  OnTradeTransaction -- detects broker-side SL/TP fills so we
//  reset GridState even when the close didn't go through our code.
//  Bug fix: prior version left g_long/g_short "open" after broker SL
//  hit, blocking new entries.
//===================================================================
void OnTradeTransaction(const MqlTradeTransaction& trans,
                        const MqlTradeRequest&     request,
                        const MqlTradeResult&      result)
{
    if (trans.type != TRADE_TRANSACTION_DEAL_ADD) return;
    if (!HistoryDealSelect(trans.deal)) return;

    long  magic  = (long)HistoryDealGetInteger(trans.deal, DEAL_MAGIC);
    string sym_d = HistoryDealGetString(trans.deal, DEAL_SYMBOL);
    long  entry  = (long)HistoryDealGetInteger(trans.deal, DEAL_ENTRY);
    if (magic != InpMagicNumber)  return;
    if (sym_d != _Symbol)         return;
    if (entry != DEAL_ENTRY_OUT && entry != DEAL_ENTRY_INOUT) return;

    // Recompute counts from open positions; if a side is now flat, reset its grid.
    int long_open  = CountOpenPositions(_Symbol, InpMagicNumber, POSITION_TYPE_BUY,  false);
    int short_open = CountOpenPositions(_Symbol, InpMagicNumber, POSITION_TYPE_SELL, false);
    if (long_open  == 0 && g_long.levels_open  > 0) {
        PrintFormat("[GridReset] BUY closed by broker (likely SL). resetting state");
        ResetGrid(g_long);
    }
    if (short_open == 0 && g_short.levels_open > 0) {
        PrintFormat("[GridReset] SELL closed by broker (likely SL). resetting state");
        ResetGrid(g_short);
    }
    // Also resync TP if a side is still open (in case of partial close)
    if (long_open  > 0) SyncBrokerSideTp(POSITION_TYPE_BUY);
    if (short_open > 0) SyncBrokerSideTp(POSITION_TYPE_SELL);
}

//===================================================================
//  OnTick -- new candle only
//===================================================================
void OnTick()
{
    datetime cur_bar = iTime(_Symbol, InpTimeframe, 0);
    if (cur_bar == g_last_candle_time) return;
    g_last_candle_time = cur_bar;

    double slow_ma=0, rsi=0, atr_short=0, atr_long=0;
    if (!SnapshotIndicators(slow_ma, rsi, atr_short, atr_long)) return;

    TrailSl(slow_ma, atr_short);

    string r;
    if (g_long.levels_open  > 0 && ShouldCloseAll_Internal(POSITION_TYPE_BUY,  slow_ma, rsi, r))
        CloseAllSide(POSITION_TYPE_BUY,  r);
    if (g_short.levels_open > 0 && ShouldCloseAll_Internal(POSITION_TYPE_SELL, slow_ma, rsi, r))
        CloseAllSide(POSITION_TYPE_SELL, r);

    TryEntry(slow_ma, rsi, atr_short, atr_long);
}
