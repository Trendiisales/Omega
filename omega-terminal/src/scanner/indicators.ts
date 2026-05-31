// scanner/indicators.ts — technical indicators computed client-side from
// the historical bars the existing Yahoo-backed routes already return.
//
// The MarketPulse Streamlit scanner consumed a pre-scored CSV whose
// columns (rsi14, atr_pct, relative_volume, pct_below_52w_high, …) were
// produced by an upstream Python scorer. That upstream scorer is NOT part
// of the Omega terminal, so the MPS panel reconstructs those columns here
// from raw OHLCV bars (`/hp` route → HistoricalBar[]) before running the
// ported decision layer in score.ts.
//
// Everything is dependency-free and pure: pass an ascending-by-date bar
// array, get numbers back. `null` means "not enough data to compute".

import type { HistoricalBar } from '@/api/types';

/** A cleaned OHLCV bar with guaranteed-finite numbers. */
export interface CleanBar {
  date: string;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

function fin(n: number | undefined): number | null {
  return typeof n === 'number' && Number.isFinite(n) ? n : null;
}

/**
 * Drop bars with a non-finite close and coerce the rest. Sorted ascending
 * by date so the most recent bar is last. Yahoo returns ascending already,
 * but we sort defensively because providers differ.
 */
export function cleanBars(bars: HistoricalBar[]): CleanBar[] {
  const out: CleanBar[] = [];
  for (const b of bars) {
    const close = fin(b.close);
    if (close === null) continue;
    const open = fin(b.open) ?? close;
    const high = fin(b.high) ?? Math.max(open, close);
    const low = fin(b.low) ?? Math.min(open, close);
    const volume = fin(b.volume) ?? 0;
    out.push({ date: b.date, open, high, low, close, volume });
  }
  out.sort((a, b) => (a.date < b.date ? -1 : a.date > b.date ? 1 : 0));
  return out;
}

/** Simple moving average of the last `period` closes. */
export function sma(bars: CleanBar[], period: number): number | null {
  if (bars.length < period) return null;
  let sum = 0;
  for (let i = bars.length - period; i < bars.length; i++) sum += bars[i]!.close;
  return sum / period;
}

/**
 * Wilder's RSI over `period` (default 14). Returns 0..100, or null when
 * there are fewer than period+1 bars.
 */
export function rsi(bars: CleanBar[], period = 14): number | null {
  if (bars.length < period + 1) return null;
  let gain = 0;
  let loss = 0;
  // Seed with the first `period` deltas.
  for (let i = 1; i <= period; i++) {
    const d = bars[i]!.close - bars[i - 1]!.close;
    if (d >= 0) gain += d;
    else loss -= d;
  }
  let avgGain = gain / period;
  let avgLoss = loss / period;
  // Wilder smoothing for the remainder.
  for (let i = period + 1; i < bars.length; i++) {
    const d = bars[i]!.close - bars[i - 1]!.close;
    const g = d > 0 ? d : 0;
    const l = d < 0 ? -d : 0;
    avgGain = (avgGain * (period - 1) + g) / period;
    avgLoss = (avgLoss * (period - 1) + l) / period;
  }
  if (avgLoss === 0) return 100;
  const rs = avgGain / avgLoss;
  return 100 - 100 / (1 + rs);
}

/**
 * Average True Range over `period` (default 14) as a PERCENT of the last
 * close — this matches the MarketPulse `atr_pct` column. Uses Wilder
 * smoothing on true range. Null when there are too few bars.
 */
export function atrPct(bars: CleanBar[], period = 14): number | null {
  if (bars.length < period + 1) return null;
  const tr: number[] = [];
  for (let i = 1; i < bars.length; i++) {
    const h = bars[i]!.high;
    const l = bars[i]!.low;
    const pc = bars[i - 1]!.close;
    tr.push(Math.max(h - l, Math.abs(h - pc), Math.abs(l - pc)));
  }
  if (tr.length < period) return null;
  // Seed with simple average of the first `period` TRs, then Wilder smooth.
  let atr = 0;
  for (let i = 0; i < period; i++) atr += tr[i]!;
  atr /= period;
  for (let i = period; i < tr.length; i++) atr = (atr * (period - 1) + tr[i]!) / period;
  const lastClose = bars[bars.length - 1]!.close;
  if (lastClose <= 0) return null;
  return (atr / lastClose) * 100;
}

/**
 * Average daily volume over the trailing `period` bars, EXCLUDING the most
 * recent (today's) bar so relative volume compares today vs the baseline.
 */
export function avgVolume(bars: CleanBar[], period = 20): number | null {
  if (bars.length < period + 1) return null;
  let sum = 0;
  const end = bars.length - 1; // exclude today
  for (let i = end - period; i < end; i++) sum += bars[i]!.volume;
  return sum / period;
}

/** Highest high over the trailing `period` bars (default ~252 = 52 weeks). */
export function highestHigh(bars: CleanBar[], period = 252): number | null {
  if (bars.length === 0) return null;
  const start = Math.max(0, bars.length - period);
  let hi = -Infinity;
  for (let i = start; i < bars.length; i++) hi = Math.max(hi, bars[i]!.high);
  return Number.isFinite(hi) ? hi : null;
}

/**
 * Cumulative intraday VWAP from a single day's intraday bars (e.g. 5m).
 * Typical price = (H+L+C)/3, volume-weighted. Falls back to the last close
 * if total volume is zero. Null on empty input.
 */
export function intradayVwap(bars: CleanBar[]): number | null {
  if (bars.length === 0) return null;
  let pv = 0;
  let vol = 0;
  for (const b of bars) {
    const typical = (b.high + b.low + b.close) / 3;
    pv += typical * b.volume;
    vol += b.volume;
  }
  if (vol <= 0) return bars[bars.length - 1]!.close;
  return pv / vol;
}

/**
 * Keep only the bars belonging to the most recent calendar day present in
 * an intraday series. Used to scope VWAP / open / high-of-day to today.
 * Date strings from Yahoo intraday are ISO datetimes ("2026-05-31T19:30:00").
 */
export function lastDayBars(bars: CleanBar[]): CleanBar[] {
  if (bars.length === 0) return bars;
  const lastDay = bars[bars.length - 1]!.date.slice(0, 10);
  return bars.filter((b) => b.date.slice(0, 10) === lastDay);
}
