// scanner/score.ts — share-viability scoring + decision layer.
//
// This is a TypeScript port of the decision logic from the MarketPulse
// Streamlit scanner (`marketpulse_compact_viewer.py`). Two differences from
// the original:
//
//   1. The Python app received the composite scores (combined / undervalued
//      / explosion / momentum) and the `reasons` / `risk_flags` / `classification`
//      text columns PRE-COMPUTED in a CSV produced by a separate upstream
//      scorer that is not part of Omega. Here `computeScores()` reconstructs
//      those columns from the raw indicator fields the panel assembles from
//      Yahoo data, so the panel is self-contained.
//
//   2. Everything is a pure function over a `ScanRow`. No pandas, no Streamlit.
//
// The classification / decision / plan / live-VWAP functions below are a
// faithful line-for-line port of the Python so behaviour matches the tool the
// user already trusts.

/* ============================================================ */
/* Row shape                                                     */
/* ============================================================ */

export interface ScanRow {
  symbol: string;
  name?: string;

  // Raw fields assembled from Yahoo routes.
  price: number;
  change_pct: number;
  volume: number;
  dollar_volume: number;
  market_cap?: number;
  shares_outstanding?: number;
  relative_volume?: number;
  rsi14?: number;
  atr_pct?: number;
  sma20?: number;
  sma50?: number;
  pct_below_52w_high?: number;

  // Fundamentals (Yahoo quoteSummary via /key). The `sec_` prefix is kept
  // for column-name parity with the MarketPulse CSV.
  sec_pe?: number;
  sec_ps?: number;
  sec_pb?: number;
  sec_roe_pct?: number;
  sec_net_margin_pct?: number;
  sec_debt_equity?: number;

  // Reconstructed composite columns.
  momentum_score: number;
  explosion_score: number;
  undervalued_score: number;
  combined_score: number;
  classification: string;
  reasons: string;
  risk_flags: string;

  // Derived decision layer.
  liquidity_state?: string;
  volume_state?: string;
  trend_state?: string;
  extension_state?: string;
  value_state?: string;
  data_gaps?: string;
  action?: string;
  bias?: string;
  decision?: string;
  quality?: number;
  entry_plan?: string;
  invalidation?: string;
  risk_plan?: string;
  target_plan?: string;
  position_type?: string;
  action_rank?: number;
  decision_rank?: number;

  // Live intraday VWAP layer.
  live_price?: number | null;
  live_vwap?: number | null;
  live_open?: number | null;
  live_high?: number | null;
  live_volume?: number | null;
  vwap_gap_pct?: number | null;
  open_gap_pct?: number | null;
  hod_gap_pct?: number | null;
  live_decision?: string;
  live_reason?: string;
  vwap_state?: string;
  open_state?: string;
  hod_state?: string;
  trigger_price?: number | null;
  stop_price?: number | null;
  target_1?: number | null;
  target_2?: number | null;
  live_rank?: number;
}

/* ============================================================ */
/* Numeric helpers (mirror the Python to_number / optional)      */
/* ============================================================ */

export function toNum(v: number | undefined | null, def = 0): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : def;
}

function opt(v: number | undefined | null): number | null {
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}

function clamp(n: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, n));
}

function round2(n: number): number {
  return Math.round(n * 100) / 100;
}

/** Linear ramp: value in [a,b] maps to [0,span]; clamped outside. */
function ramp(value: number, a: number, b: number, span: number): number {
  if (b === a) return 0;
  const t = (value - a) / (b - a);
  return clamp(t, 0, 1) * span;
}

/* ============================================================ */
/* Score + classification reconstruction                         */
/* ============================================================ */

/**
 * Reconstruct the momentum / explosion / undervalued / combined scores and
 * the reasons / risk_flags / classification text from raw indicator fields.
 *
 * Formulas are transparent and intentionally close to the spirit of the
 * MarketPulse rules + the Omega WatchScheduler "v1 momentum + relative-volume"
 * rule. They are deliberately conservative so the downstream decision layer
 * (ported verbatim) behaves the same way it did against the CSV scores.
 */
export function computeScores(row: ScanRow): void {
  const change = toNum(row.change_pct);
  const relvol = toNum(row.relative_volume);
  const rsi = toNum(row.rsi14, 50);
  const atr = toNum(row.atr_pct);
  const dollarVol = toNum(row.dollar_volume);
  const belowHigh = opt(row.pct_below_52w_high);

  const price = toNum(row.price);
  const sma20 = opt(row.sma20);
  const sma50 = opt(row.sma50);
  const aboveSma20 = sma20 !== null && price > sma20;
  const aboveSma50 = sma50 !== null && price > sma50;

  // ---- reasons text (parsed by trend_state / classify_action) ----
  const reasons: string[] = [];
  if (aboveSma20) reasons.push('above SMA20');
  if (aboveSma50) reasons.push('above SMA50');
  if (relvol >= 1.2) reasons.push('relative volume expansion');
  if (belowHigh !== null && belowHigh <= 10) reasons.push('near 52w high');
  row.reasons = reasons.join('; ');

  // ---- momentum ----
  let momentum = 0;
  momentum += ramp(change, 0, 15, 40); // daily thrust
  momentum += ramp(relvol, 1, 3, 25); // participation
  if (rsi >= 50 && rsi <= 72) momentum += 20;
  else if (rsi > 72 && rsi <= 80) momentum += 10;
  else if (rsi < 40) momentum -= 10;
  if (aboveSma20) momentum += 8;
  if (aboveSma50) momentum += 7;
  row.momentum_score = round2(clamp(momentum, 0, 100));

  // ---- explosion ----
  let explosion = 0;
  explosion += ramp(relvol, 1.5, 6, 40); // volume spike
  explosion += ramp(atr, 3, 15, 25); // range expansion
  explosion += ramp(change, 2, 30, 25); // move size
  if (belowHigh !== null && belowHigh <= 10) explosion += 10; // breakout proximity
  row.explosion_score = round2(clamp(explosion, 0, 100));

  // ---- undervalued ----
  const pe = opt(row.sec_pe);
  const ps = opt(row.sec_ps);
  const pb = opt(row.sec_pb);
  const roe = opt(row.sec_roe_pct);
  const margin = opt(row.sec_net_margin_pct);
  const de = opt(row.sec_debt_equity);
  let value = 0;
  if (pe !== null && pe > 0) value += pe < 10 ? 25 : pe < 20 ? 15 : pe < 30 ? 5 : 0;
  if (ps !== null && ps > 0) value += ps < 1 ? 15 : ps < 3 ? 8 : 0;
  if (pb !== null && pb > 0) value += pb < 1.5 ? 15 : pb < 3 ? 8 : 0;
  if (roe !== null) value += roe > 15 ? 15 : roe > 8 ? 8 : 0;
  if (margin !== null) value += margin > 15 ? 10 : margin > 5 ? 5 : 0;
  if (de !== null) value += de < 0.5 ? 10 : de < 1 ? 5 : de > 2 ? -10 : 0;
  row.undervalued_score = round2(clamp(value, 0, 100));

  // ---- combined ----
  let combined =
    row.momentum_score * 0.3 + row.explosion_score * 0.3 + row.undervalued_score * 0.25;
  if (dollarVol >= 10_000_000) combined += 15;
  else if (dollarVol >= 3_000_000) combined += 8;
  row.combined_score = round2(clamp(combined, 0, 100));

  // ---- risk flags + classification ----
  const risks: string[] = [];
  if (margin !== null && margin < 0) risks.push('negative ttm income');
  if (de !== null && de > 2) risks.push('high debt');
  if (price > 0 && price < 1) risks.push('sub-dollar');
  if (dollarVol > 0 && dollarVol < 3_000_000) risks.push('thin liquidity');
  row.risk_flags = risks.join('; ');
  row.classification = risks.some((r) =>
    ['negative ttm income', 'high debt', 'sub-dollar'].includes(r)
  )
    ? 'HIGH RISK'
    : 'STANDARD';
}

/* ============================================================ */
/* State descriptors (ports of the Python *_state functions)     */
/* ============================================================ */

export function liquidityState(row: ScanRow): string {
  const dv = toNum(row.dollar_volume);
  const vol = toNum(row.volume);
  const price = toNum(row.price);
  if (dv >= 50_000_000) return 'Excellent liquidity';
  if (dv >= 10_000_000) return 'Good liquidity';
  if (dv >= 3_000_000) return 'Tradable but thinner';
  if (dv > 0) return 'Low liquidity';
  if (vol > 0 && price > 0) return 'Liquidity unknown';
  return 'No liquidity data';
}

export function volumeState(row: ScanRow): string {
  const rv = toNum(row.relative_volume);
  const vol = toNum(row.volume);
  if (rv >= 5) return 'Extreme relative volume';
  if (rv >= 2) return 'High relative volume';
  if (rv >= 1.2) return 'Above-normal volume';
  if (rv > 0) return 'Normal/weak volume';
  if (vol > 0) return 'Relative volume missing';
  return 'Volume missing';
}

export function trendState(row: ScanRow): string {
  const price = toNum(row.price);
  const reasons = (row.reasons ?? '').toLowerCase();
  const a20 = reasons.includes('above sma20');
  const a50 = reasons.includes('above sma50');
  if (a20 && a50) return 'Above SMA20 and SMA50';
  if (a20) return 'Above SMA20';
  if (a50) return 'Above SMA50';
  if (price > 0) return 'Trend not confirmed';
  return 'Trend data missing';
}

export function extensionState(row: ScanRow): string {
  const rsi = opt(row.rsi14);
  const change = opt(row.change_pct);
  const atr = opt(row.atr_pct);
  const flags: string[] = [];
  if (rsi !== null) {
    if (rsi >= 85) flags.push('RSI very extended');
    else if (rsi >= 75) flags.push('RSI stretched');
    else if (rsi >= 50 && rsi <= 72) flags.push('RSI strong');
    else if (rsi < 35) flags.push('RSI weak/oversold');
  } else flags.push('RSI missing');
  if (change !== null) {
    if (change >= 35) flags.push('daily move extreme');
    else if (change >= 12) flags.push('daily move stretched');
    else if (change >= 3) flags.push('daily move constructive');
    else if (change < 0) flags.push('red on day');
  } else flags.push('daily change missing');
  if (atr !== null) {
    if (atr >= 18) flags.push('ATR very high');
    else if (atr >= 8) flags.push('ATR high');
    else if (atr >= 3) flags.push('ATR tradable');
  } else flags.push('ATR missing');
  return flags.join('; ');
}

export function valueState(row: ScanRow): string {
  const value = toNum(row.undervalued_score);
  const pe = opt(row.sec_pe);
  const ps = opt(row.sec_ps);
  const pb = opt(row.sec_pb);
  const roe = opt(row.sec_roe_pct);
  const margin = opt(row.sec_net_margin_pct);
  const de = opt(row.sec_debt_equity);
  const items: string[] = [];
  if (value >= 70) items.push('strong value score');
  else if (value >= 50) items.push('some value support');
  else if (value > 0) items.push('weak value support');
  else items.push('no value score');
  if (pe !== null) items.push(`PE ${pe.toFixed(1)}`);
  if (ps !== null) items.push(`PS ${ps.toFixed(1)}`);
  if (pb !== null) items.push(`PB ${pb.toFixed(1)}`);
  if (roe !== null) items.push(`ROE ${roe.toFixed(1)}%`);
  if (margin !== null) items.push(`net margin ${margin.toFixed(1)}%`);
  if (de !== null) items.push(`D/E ${de.toFixed(2)}`);
  return items.join('; ');
}

/* ============================================================ */
/* Action classification (port of classify_action)              */
/* ============================================================ */

export function classifyAction(row: ScanRow): string {
  const classification = row.classification ?? '';
  const combined = toNum(row.combined_score);
  const value = toNum(row.undervalued_score);
  const explosion = toNum(row.explosion_score);
  const momentum = toNum(row.momentum_score);
  const change = toNum(row.change_pct);
  const relvol = toNum(row.relative_volume);
  const dollarVol = toNum(row.dollar_volume);
  const rsi = toNum(row.rsi14, 50);
  const atr = toNum(row.atr_pct);
  const risks = (row.risk_flags ?? '').toLowerCase();
  const reasons = (row.reasons ?? '').toLowerCase();

  const weakLiquidity = dollarVol > 0 && dollarVol < 3_000_000;
  const strongLiquidity = dollarVol >= 3_000_000;
  const goodLiquidity = dollarVol >= 5_000_000;
  const activeVolume = relvol >= 1.7;
  const veryActiveVolume = relvol >= 2.0;
  const extended = rsi >= 82 || change >= 35 || atr >= 18;
  const positiveMove = change >= 2;
  const aboveTrend = reasons.includes('above sma20') || reasons.includes('above sma50');
  const highRiskText =
    risks.includes('negative ttm income') ||
    risks.includes('high debt') ||
    risks.includes('sub-dollar');

  if (weakLiquidity) return 'LOW LIQUIDITY';
  if (extended && explosion >= 45) return 'AVOID EXTENDED';

  if (classification === 'HIGH RISK' || highRiskText) {
    if (
      explosion >= 60 &&
      momentum >= 55 &&
      goodLiquidity &&
      veryActiveVolume &&
      positiveMove &&
      !extended
    )
      return 'EXPLOSION READY';
    return 'HIGH RISK';
  }

  if (
    explosion >= 65 &&
    momentum >= 55 &&
    goodLiquidity &&
    veryActiveVolume &&
    positiveMove &&
    rsi >= 45 &&
    rsi <= 78
  )
    return 'TRADE WATCH';

  if (explosion >= 55 && activeVolume && positiveMove && strongLiquidity && rsi <= 82)
    return 'EXPLOSION READY';

  if (
    momentum >= 60 &&
    relvol >= 1.2 &&
    change >= 0.5 &&
    goodLiquidity &&
    rsi <= 78 &&
    aboveTrend
  )
    return 'MOMENTUM CONTINUATION';

  if (value >= 60 && momentum >= 45) return 'VALUE + MOMENTUM';
  if (value >= 60) return 'UNDERVALUED WATCH';
  if (explosion >= 50 || momentum >= 50) return 'WAIT FOR PULLBACK';
  if (combined >= 45 && (explosion >= 45 || momentum >= 45 || value >= 50))
    return 'WAIT FOR PULLBACK';
  return 'IGNORE';
}

export function tradeBias(row: ScanRow): string {
  const action = row.action ?? '';
  const change = toNum(row.change_pct);
  const relvol = toNum(row.relative_volume);
  if (action === 'TRADE WATCH' || action === 'EXPLOSION READY') {
    if (relvol >= 2 && change >= 5) return 'Breakout / high-volume continuation';
    return 'Explosion watch / needs level confirmation';
  }
  if (action === 'MOMENTUM CONTINUATION') return 'Trend continuation / pullback entry';
  if (action === 'VALUE + MOMENTUM') return 'Value plus active momentum';
  if (action === 'UNDERVALUED WATCH') return 'Fundamental/value research';
  if (action === 'WAIT FOR PULLBACK' || action === 'AVOID EXTENDED')
    return 'Wait / avoid chasing';
  if (action === 'LOW LIQUIDITY') return 'Avoid unless liquidity improves';
  if (action === 'HIGH RISK') return 'Avoid unless catalyst is exceptional';
  return 'No clear bias';
}

export function decision(row: ScanRow): string {
  const action = row.action ?? '';
  const dollarVol = toNum(row.dollar_volume);
  const relvol = toNum(row.relative_volume);
  const rsi = toNum(row.rsi14, 50);
  // Port fix: the Python used `change == 0` to flag missing data, which
  // mislabels a stock that is legitimately flat on the day. We treat the
  // field as missing only when it is genuinely absent.
  const changeMissing = !Number.isFinite(row.change_pct as number);
  if (action === 'TRADE WATCH') return 'ACTIONABLE WATCH';
  if (action === 'EXPLOSION READY' || action === 'MOMENTUM CONTINUATION' || action === 'VALUE + MOMENTUM')
    return 'CONFIRM ON CHART';
  if (action === 'UNDERVALUED WATCH') return 'VALUE RESEARCH';
  if (action === 'WAIT FOR PULLBACK') return 'WAIT FOR PULLBACK';
  if (action === 'LOW LIQUIDITY' || action === 'AVOID EXTENDED' || action === 'HIGH RISK' || action === 'IGNORE')
    return 'AVOID NOW';
  if (dollarVol <= 0 || relvol <= 0 || rsi <= 0 || changeMissing) return 'INSUFFICIENT DATA';
  return 'CONFIRM ON CHART';
}

export function qualityScore(row: ScanRow): number {
  const action = row.action ?? '';
  const combined = toNum(row.combined_score);
  const value = toNum(row.undervalued_score);
  const explosion = toNum(row.explosion_score);
  const momentum = toNum(row.momentum_score);
  const dollarVol = toNum(row.dollar_volume);
  const relvol = toNum(row.relative_volume);
  const rsi = toNum(row.rsi14, 50);
  const change = toNum(row.change_pct);
  const atr = toNum(row.atr_pct);
  let q = combined * 0.25 + value * 0.15 + explosion * 0.25 + momentum * 0.2;
  if (dollarVol >= 10_000_000) q += 8;
  else if (dollarVol >= 3_000_000) q += 4;
  else q -= 10;
  if (relvol >= 2) q += 7;
  else if (relvol >= 1.2) q += 3;
  if (rsi >= 45 && rsi <= 75) q += 5;
  else if (rsi >= 82) q -= 10;
  if (change >= 2 && change <= 20) q += 5;
  else if (change >= 35) q -= 10;
  if (atr >= 18) q -= 8;
  if (action === 'TRADE WATCH') q += 10;
  if (action === 'HIGH RISK' || action === 'LOW LIQUIDITY' || action === 'AVOID EXTENDED') q -= 15;
  return round2(clamp(q, 0, 100));
}

/* ---- plan strings (ports) ---- */

export function entryPlan(row: ScanRow): string {
  const action = row.action ?? '';
  const price = toNum(row.price);
  const atr = toNum(row.atr_pct);
  if (price <= 0) return 'No price data';
  const pullback = price * (1 - clamp(atr / 200, 0.015, 0.06));
  const breakout = price * 1.01;
  if (action === 'TRADE WATCH')
    return `Watch break/hold above ${breakout.toFixed(2)}; prefer pullback hold near ${pullback.toFixed(2)}; require VWAP support`;
  if (action === 'EXPLOSION READY')
    return `Buy only if above VWAP/open and breaks ${breakout.toFixed(2)}; otherwise wait`;
  if (action === 'MOMENTUM CONTINUATION')
    return `Do not chase; look for controlled pullback toward ${pullback.toFixed(2)} then continuation`;
  if (action === 'VALUE + MOMENTUM')
    return 'Confirm trend continuation; starter only if price holds above VWAP and trend';
  if (action === 'UNDERVALUED WATCH')
    return 'Research fundamentals first; wait for volume expansion or technical breakout';
  if (action === 'WAIT FOR PULLBACK') return `Wait for reset; reassess near ${pullback.toFixed(2)}`;
  if (action === 'AVOID EXTENDED')
    return 'Avoid chasing; reassess after consolidation or VWAP retest';
  if (action === 'LOW LIQUIDITY') return 'Avoid until dollar volume improves';
  return 'No actionable entry';
}

export function invalidation(row: ScanRow): string {
  const action = row.action ?? '';
  const price = toNum(row.price);
  const atr = toNum(row.atr_pct);
  if (price <= 0) return 'No price data';
  const stopPct = clamp(atr / 120, 0.025, 0.1);
  const invalid = price * (1 - stopPct);
  if (
    action === 'TRADE WATCH' ||
    action === 'EXPLOSION READY' ||
    action === 'MOMENTUM CONTINUATION' ||
    action === 'VALUE + MOMENTUM'
  )
    return `Invalid below ${invalid.toFixed(2)}, below VWAP/open, or relative volume fades below 1.2`;
  if (action === 'UNDERVALUED WATCH')
    return 'Invalid if valuation thesis breaks, filings worsen, or price loses major support';
  if (action === 'WAIT FOR PULLBACK') return 'Invalid if pullback becomes high-volume breakdown';
  return 'Avoid signal already active';
}

export function riskPlan(row: ScanRow): string {
  const price = toNum(row.price);
  const atr = toNum(row.atr_pct);
  const dollarVol = toNum(row.dollar_volume);
  if (price <= 0) return 'No price data';
  const riskPct = clamp(atr > 0 ? atr / 1.2 : 4, 2.5, 10);
  const stop = price * (1 - riskPct / 100);
  if (dollarVol < 3_000_000 && dollarVol > 0)
    return `Thin liquidity; reduce size. Example risk ${riskPct.toFixed(1)}% stop near ${stop.toFixed(2)}`;
  return `Small defined risk. Example ${riskPct.toFixed(1)}% risk zone, stop near ${stop.toFixed(2)}`;
}

export function targetPlan(row: ScanRow): string {
  const price = toNum(row.price);
  const atr = toNum(row.atr_pct);
  if (price <= 0) return 'No price data';
  const riskPct = clamp(atr > 0 ? atr / 1.2 : 4, 2.5, 10);
  const t1 = price * (1 + riskPct / 100);
  const t2 = price * (1 + (riskPct * 2) / 100);
  return `1R ${t1.toFixed(2)}; 2R ${t2.toFixed(2)}; trail after 1R/new high`;
}

export function positionType(row: ScanRow): string {
  const action = row.action ?? '';
  const atr = toNum(row.atr_pct);
  const dollarVol = toNum(row.dollar_volume);
  if (action === 'TRADE WATCH' || action === 'EXPLOSION READY')
    return atr >= 8 ? 'Day trade / very short swing' : 'Day trade / breakout watch';
  if (action === 'MOMENTUM CONTINUATION') return 'Swing continuation watch';
  if (action === 'VALUE + MOMENTUM' || action === 'UNDERVALUED WATCH')
    return 'Swing / investment research';
  if (dollarVol < 3_000_000 && dollarVol > 0) return 'Avoid size';
  return 'No trade';
}

/* ============================================================ */
/* Priority maps + decorate                                     */
/* ============================================================ */

export const ACTION_PRIORITY: Record<string, number> = {
  'TRADE WATCH': 1,
  'EXPLOSION READY': 2,
  'MOMENTUM CONTINUATION': 3,
  'VALUE + MOMENTUM': 4,
  'UNDERVALUED WATCH': 5,
  'WAIT FOR PULLBACK': 6,
  'LOW LIQUIDITY': 7,
  'AVOID EXTENDED': 8,
  'HIGH RISK': 9,
  IGNORE: 10,
};

export const DECISION_PRIORITY: Record<string, number> = {
  'ACTIONABLE WATCH': 1,
  'CONFIRM ON CHART': 2,
  'WAIT FOR PULLBACK': 3,
  'VALUE RESEARCH': 4,
  'AVOID NOW': 5,
  'INSUFFICIENT DATA': 6,
};

export const LIVE_PRIORITY: Record<string, number> = {
  'BUY TRIGGER': 1,
  'BUY WATCH': 2,
  'WAIT FOR PULLBACK': 3,
  'WAIT FOR VWAP RECLAIM': 4,
  'AVOID EXTENDED': 5,
  'AVOID WEAK': 6,
  'NO LIVE DATA': 7,
};

/** Run scores + the full decision layer over a row, mutating it in place. */
export function decorate(row: ScanRow): ScanRow {
  computeScores(row);
  row.liquidity_state = liquidityState(row);
  row.volume_state = volumeState(row);
  row.trend_state = trendState(row);
  row.extension_state = extensionState(row);
  row.value_state = valueState(row);
  row.action = classifyAction(row);
  row.bias = tradeBias(row);
  row.decision = decision(row);
  row.quality = qualityScore(row);
  row.entry_plan = entryPlan(row);
  row.invalidation = invalidation(row);
  row.risk_plan = riskPlan(row);
  row.target_plan = targetPlan(row);
  row.position_type = positionType(row);
  row.action_rank = ACTION_PRIORITY[row.action] ?? 99;
  row.decision_rank = DECISION_PRIORITY[row.decision] ?? 99;
  if (row.live_decision === undefined) {
    row.live_decision = 'NO LIVE DATA';
    row.live_rank = LIVE_PRIORITY['NO LIVE DATA'];
  }
  return row;
}

/* ============================================================ */
/* Live VWAP decision (port of decide_live)                     */
/* ============================================================ */

/**
 * Apply the live intraday VWAP layer to a row that already has its
 * `live_price` / `live_vwap` / `live_open` / `live_high` and the gap fields
 * populated (or all null when no intraday data was fetched). Mutates in place.
 */
export function decideLive(row: ScanRow): void {
  const action = row.action ?? '';
  const explosion = toNum(row.explosion_score);
  const momentum = toNum(row.momentum_score);
  const relvol = toNum(row.relative_volume);
  const rsi = toNum(row.rsi14, 50);
  const atr = toNum(row.atr_pct);
  const livePrice = opt(row.live_price ?? undefined);
  const liveVwap = opt(row.live_vwap ?? undefined);
  const liveOpen = opt(row.live_open ?? undefined);
  const liveHigh = opt(row.live_high ?? undefined);
  const vwapGap = toNum(row.vwap_gap_pct ?? undefined);
  const hodGap = toNum(row.hod_gap_pct ?? undefined);

  const setLive = (
    live_decision: string,
    live_reason: string,
    states: { vwap: string; open: string; hod: string },
    levels: { trigger: number | null; stop: number | null; t1: number | null; t2: number | null }
  ) => {
    row.live_decision = live_decision;
    row.live_reason = live_reason;
    row.vwap_state = states.vwap;
    row.open_state = states.open;
    row.hod_state = states.hod;
    row.trigger_price = levels.trigger;
    row.stop_price = levels.stop;
    row.target_1 = levels.t1;
    row.target_2 = levels.t2;
    row.live_rank = LIVE_PRIORITY[live_decision] ?? 99;
  };

  if (livePrice === null || liveVwap === null || liveOpen === null || liveHigh === null) {
    setLive(
      'NO LIVE DATA',
      'No intraday data available',
      { vwap: 'VWAP missing', open: 'Open missing', hod: 'HOD missing' },
      { trigger: null, stop: null, t1: null, t2: null }
    );
    return;
  }

  const aboveVwap = livePrice > liveVwap;
  const aboveOpen = livePrice > liveOpen;
  const nearHod = hodGap <= 1.5;
  const tooExtendedVwap = vwapGap >= 8;
  const tooExtendedHod = hodGap <= 0.25 && vwapGap >= 5;
  const weak = !aboveVwap || !aboveOpen;

  const vwapStateStr = aboveVwap ? 'Above VWAP' : 'Below VWAP';
  const openStateStr = aboveOpen ? 'Above open' : 'Below open';
  const hodStateStr = nearHod
    ? 'Near high of day'
    : hodGap <= 4
      ? 'Constructive pullback'
      : 'Far from high of day';

  const riskPct = clamp(atr > 0 ? atr / 1.25 : 3.5, 2, 9);
  const stopCandidates = [liveVwap * 0.995, liveOpen * 0.995, livePrice * (1 - riskPct / 100)].filter(
    (x) => x > 0
  );
  // Port fix: guard against an empty candidate list (Python `min([])` threw).
  const stopPrice = stopCandidates.length > 0 ? Math.min(...stopCandidates) : livePrice * 0.97;
  const triggerPrice = Math.max(liveHigh * 1.0025, livePrice * 1.004);
  const riskAmount = Math.max(0.01, livePrice - stopPrice);
  const t1 = livePrice + riskAmount;
  const t2 = livePrice + riskAmount * 2;
  const states = { vwap: vwapStateStr, open: openStateStr, hod: hodStateStr };
  const levels = { trigger: triggerPrice, stop: stopPrice, t1, t2 };

  if (weak) {
    setLive(
      'WAIT FOR VWAP RECLAIM',
      'Do not buy while below VWAP/open; wait for reclaim and hold',
      states,
      levels
    );
    return;
  }
  if (tooExtendedVwap || tooExtendedHod || rsi >= 84) {
    setLive(
      'AVOID EXTENDED',
      'Above VWAP/open but too extended; wait for controlled pullback',
      states,
      levels
    );
    return;
  }
  if (
    (action === 'TRADE WATCH' || action === 'EXPLOSION READY') &&
    explosion >= 55 &&
    momentum >= 35 &&
    aboveVwap &&
    aboveOpen &&
    nearHod &&
    relvol >= 1.7
  ) {
    setLive(
      'BUY TRIGGER',
      'Above VWAP/open, near high of day, active relative volume, explosion setup active',
      states,
      levels
    );
    return;
  }
  if (
    (action === 'TRADE WATCH' ||
      action === 'EXPLOSION READY' ||
      action === 'MOMENTUM CONTINUATION' ||
      action === 'VALUE + MOMENTUM') &&
    aboveVwap &&
    aboveOpen &&
    relvol >= 1.2
  ) {
    setLive(
      'BUY WATCH',
      'Above VWAP/open with active volume; needs breakout or pullback-hold confirmation',
      states,
      levels
    );
    return;
  }
  if (aboveVwap && aboveOpen) {
    setLive(
      'WAIT FOR PULLBACK',
      'Price is constructive but trigger is not strong enough yet',
      states,
      levels
    );
    return;
  }
  setLive('AVOID WEAK', 'Live structure is weak', states, levels);
}

/* ============================================================ */
/* Mode presets (port of AUTO_MODES, trimmed to the live set)    */
/* ============================================================ */

export interface ScanMode {
  name: string;
  live: string[];
  decisions: string[];
  minExplosion: number;
  minMomentum: number;
  minValue: number;
  minRelVolume: number;
  minChange: number;
  maxChange: number;
  minDollarVolume: number;
  maxRsi: number;
  sortCol: keyof ScanRow | 'live_rank' | 'action_rank' | 'decision_rank';
  ascending: boolean;
}

export const SCAN_MODES: ScanMode[] = [
  {
    // DEFAULT landing mode — explosion + breakout is the primary use case.
    // Tuned slightly looser than the old "Explosion trades" preset so the
    // best setups are surfaced even on a quieter tape, sorted strongest
    // explosion first. The panel's Setup filter defaults to the
    // breakout/explosion action family on top of this.
    name: 'Explosion + Breakout',
    live: [],
    decisions: [],
    minExplosion: 0,
    minMomentum: 0,
    minValue: 0,
    minRelVolume: 0,
    minChange: -100,
    maxChange: 500,
    minDollarVolume: 1_000_000,
    maxRsi: 100,
    sortCol: 'explosion_score',
    ascending: false,
  },
  {
    name: 'Live buy dashboard',
    live: ['BUY TRIGGER', 'BUY WATCH', 'WAIT FOR PULLBACK', 'WAIT FOR VWAP RECLAIM'],
    decisions: [],
    minExplosion: 0,
    minMomentum: 0,
    minValue: 0,
    minRelVolume: 0,
    minChange: -100,
    maxChange: 500,
    minDollarVolume: 0,
    maxRsi: 100,
    sortCol: 'live_rank',
    ascending: true,
  },
  {
    name: 'Momentum continuation',
    live: [],
    decisions: ['ACTIONABLE WATCH', 'CONFIRM ON CHART', 'WAIT FOR PULLBACK'],
    minExplosion: 0,
    minMomentum: 55,
    minValue: 0,
    minRelVolume: 1.2,
    minChange: 0.5,
    maxChange: 25,
    minDollarVolume: 5_000_000,
    maxRsi: 78,
    sortCol: 'momentum_score',
    ascending: false,
  },
  {
    name: 'Undervalued research',
    live: [],
    decisions: ['VALUE RESEARCH', 'ACTIONABLE WATCH', 'CONFIRM ON CHART'],
    minExplosion: 0,
    minMomentum: 0,
    minValue: 50,
    minRelVolume: 0,
    minChange: -100,
    maxChange: 500,
    minDollarVolume: 0,
    maxRsi: 100,
    sortCol: 'undervalued_score',
    ascending: false,
  },
  {
    name: 'Show everything',
    live: [],
    decisions: [],
    minExplosion: 0,
    minMomentum: 0,
    minValue: 0,
    minRelVolume: 0,
    minChange: -100,
    maxChange: 500,
    minDollarVolume: 0,
    maxRsi: 100,
    sortCol: 'quality',
    ascending: false,
  },
];
