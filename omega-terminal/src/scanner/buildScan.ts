// scanner/buildScan.ts — assembles a scored share universe from the
// existing Yahoo-backed Omega routes. No new backend route, no new feed.
//
// Pipeline:
//   1. Seed the universe from /mov (active + gainers [+ losers]). One call
//      each; gives symbol + price + %change + volume cheaply.
//   2. Rank the seed by a cheap first-pass proxy (|%change| × volume) and
//      keep the top `limit` so we only spend per-symbol calls where it
//      matters — mirrors the MarketPulse `live_limit` cap.
//   3. For each kept symbol, fetch in parallel (bounded):
//        /hp 1d (1y)  -> RSI / ATR% / SMA20 / SMA50 / 52w-high / avg-volume
//        /key         -> fundamentals (PE/PS/PB/ROE/margin/D-E/market cap)
//        /hp 5m       -> intraday VWAP / open / high-of-day (live layer)
//   4. decorate() every row (scores + decision layer), then decideLive().
//
// Per-symbol failures degrade gracefully: a missing /key just leaves the
// fundamentals undefined; a missing 5m series leaves the live layer at
// "NO LIVE DATA". One bad symbol never sinks the scan.

import { getMov, getHp, getKey, type OmegaCallOptions } from '@/api/omega';
import type { MovRow, HistoricalBar } from '@/api/types';
import {
  avgVolume,
  atrPct,
  cleanBars,
  highestHigh,
  intradayVwap,
  lastDayBars,
  rsi,
  sma,
} from './indicators';
import { decorate, decideLive, type ScanRow } from './score';

export interface BuildScanOptions {
  /** Max symbols to deep-enrich. Mirrors MarketPulse live_limit. */
  limit: number;
  /** Include the day's losers in the seed universe. */
  includeLosers: boolean;
  /** Fetch the 5m intraday series for the live VWAP layer. */
  live: boolean;
  /** Bounded concurrency for per-symbol enrichment. */
  concurrency?: number;
}

export interface ScanResult {
  rows: ScanRow[];
  provider: string;
  warnings: string[];
}

function num(v: unknown): number | undefined {
  return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}

/** Run `tasks` with at most `n` in flight at once, preserving order. */
async function pool<T, R>(items: T[], n: number, fn: (item: T, i: number) => Promise<R>): Promise<R[]> {
  const out: R[] = new Array(items.length);
  let next = 0;
  const workers = Array.from({ length: Math.min(n, items.length) }, async () => {
    for (;;) {
      const i = next++;
      if (i >= items.length) return;
      out[i] = await fn(items[i]!, i);
    }
  });
  await Promise.all(workers);
  return out;
}

function isoYearAgo(): string {
  const d = new Date();
  d.setFullYear(d.getFullYear() - 1);
  return d.toISOString().slice(0, 10);
}

/** Seed universe: dedupe across the requested discovery lists. */
async function seedUniverse(
  includeLosers: boolean,
  opts: OmegaCallOptions
): Promise<{ rows: MovRow[]; provider: string; warnings: string[] }> {
  const universes = includeLosers ? ['active', 'gainers', 'losers'] : ['active', 'gainers'];
  const envs = await Promise.all(
    universes.map((u) => getMov({ universe: u }, opts).catch(() => null))
  );
  const seen = new Set<string>();
  const rows: MovRow[] = [];
  const warnings: string[] = [];
  let provider = 'yahoo';
  for (const env of envs) {
    if (!env) continue;
    provider = env.provider ?? provider;
    for (const w of env.warnings ?? []) if (w?.message) warnings.push(w.message);
    for (const r of env.results ?? []) {
      const sym = (r.symbol ?? '').toUpperCase();
      if (!sym || seen.has(sym)) continue;
      seen.add(sym);
      rows.push(r);
    }
  }
  return { rows, provider, warnings };
}

function firstPassRank(r: MovRow): number {
  const pct = Math.abs(num(r.percent_change) ?? num(r.change_percent) ?? 0);
  const vol = num(r.volume) ?? 0;
  return (pct + 1) * Math.log10(vol + 10);
}

/** Build one ScanRow's raw fields from the daily + key + intraday data. */
function assembleRow(
  mov: MovRow,
  daily: HistoricalBar[] | null,
  key: { market_cap?: number; pe?: number; ps?: number; pb?: number; roe?: number; margin?: number; de?: number } | null,
  intraday: HistoricalBar[] | null,
  live: boolean
): ScanRow {
  const bars = daily ? cleanBars(daily) : [];
  const last = bars.length > 0 ? bars[bars.length - 1]! : null;

  const price = num(mov.price) ?? last?.close ?? 0;
  const change_pct = num(mov.percent_change) ?? num(mov.change_percent) ?? NaN;
  const volume = num(mov.volume) ?? last?.volume ?? 0;
  const dollar_volume = price * volume;

  const avgVol = avgVolume(bars, 20);
  const relative_volume = avgVol && avgVol > 0 ? volume / avgVol : undefined;
  const hi52 = highestHigh(bars, 252);
  const pct_below_52w_high =
    hi52 && hi52 > 0 ? ((hi52 - price) / hi52) * 100 : undefined;

  const row: ScanRow = {
    symbol: (mov.symbol ?? '').toUpperCase(),
    name: mov.name,
    price,
    change_pct,
    volume,
    dollar_volume,
    market_cap: key?.market_cap,
    shares_outstanding:
      key?.market_cap && price > 0 ? key.market_cap / price : undefined,
    relative_volume,
    rsi14: rsi(bars, 14) ?? undefined,
    atr_pct: atrPct(bars, 14) ?? undefined,
    sma20: sma(bars, 20) ?? undefined,
    sma50: sma(bars, 50) ?? undefined,
    pct_below_52w_high,
    sec_pe: key?.pe,
    sec_ps: key?.ps,
    sec_pb: key?.pb,
    sec_roe_pct: key?.roe,
    sec_net_margin_pct: key?.margin,
    sec_debt_equity: key?.de,
    // Filled by computeScores in decorate().
    momentum_score: 0,
    explosion_score: 0,
    undervalued_score: 0,
    combined_score: 0,
    classification: '',
    reasons: '',
    risk_flags: '',
  };

  if (live && intraday) {
    const day = lastDayBars(cleanBars(intraday));
    if (day.length > 0) {
      const live_open = day[0]!.open;
      const live_high = Math.max(...day.map((b) => b.high));
      const live_price = day[day.length - 1]!.close;
      const live_volume = day.reduce((a, b) => a + b.volume, 0);
      const live_vwap = intradayVwap(day) ?? live_price;
      row.live_open = live_open;
      row.live_high = live_high;
      row.live_price = live_price;
      row.live_volume = live_volume;
      row.live_vwap = live_vwap;
      row.vwap_gap_pct = live_vwap > 0 ? ((live_price - live_vwap) / live_vwap) * 100 : 0;
      row.open_gap_pct = live_open > 0 ? ((live_price - live_open) / live_open) * 100 : 0;
      row.hod_gap_pct = live_high > 0 ? ((live_high - live_price) / live_high) * 100 : 0;
    }
  }

  return row;
}

/**
 * Pull the merged Key Stats envelope and flatten the fields the scanner
 * needs. Yahoo returns ROE / margin as fractions, so ×100 to percent.
 */
async function fetchKey(symbol: string, opts: OmegaCallOptions) {
  try {
    const env = await getKey({ symbol }, opts);
    const k = env.key_metrics?.[0] ?? {};
    const roe = num(k.return_on_equity);
    const margin = num(k.profit_margin);
    return {
      market_cap: num(k.market_cap),
      pe: num(k.pe_ratio),
      ps: num(k.price_to_sales),
      pb: num(k.price_to_book),
      roe: roe !== undefined ? roe * 100 : undefined,
      margin: margin !== undefined ? margin * 100 : undefined,
      de: num(k.debt_to_equity),
    };
  } catch {
    return null;
  }
}

async function fetchDaily(symbol: string, opts: OmegaCallOptions): Promise<HistoricalBar[] | null> {
  try {
    const env = await getHp({ symbol, interval: '1d', start_date: isoYearAgo() }, opts);
    return env.results ?? null;
  } catch {
    return null;
  }
}

async function fetchIntraday(symbol: string, opts: OmegaCallOptions): Promise<HistoricalBar[] | null> {
  try {
    const env = await getHp({ symbol, interval: '5m' }, opts);
    return env.results ?? null;
  } catch {
    return null;
  }
}

/** Top-level entry point used by the MPS panel. */
export async function buildScan(
  o: BuildScanOptions,
  opts: OmegaCallOptions = {}
): Promise<ScanResult> {
  const seed = await seedUniverse(o.includeLosers, opts);
  const ranked = [...seed.rows].sort((a, b) => firstPassRank(b) - firstPassRank(a));
  const kept = ranked.slice(0, Math.max(1, o.limit));

  const rows = await pool(kept, o.concurrency ?? 4, async (mov) => {
    const sym = (mov.symbol ?? '').toUpperCase();
    const [daily, key, intraday] = await Promise.all([
      fetchDaily(sym, opts),
      fetchKey(sym, opts),
      o.live ? fetchIntraday(sym, opts) : Promise.resolve(null),
    ]);
    const row = assembleRow(mov, daily, key, intraday, o.live);
    decorate(row);
    decideLive(row);
    return row;
  });

  return { rows, provider: seed.provider, warnings: seed.warnings };
}
