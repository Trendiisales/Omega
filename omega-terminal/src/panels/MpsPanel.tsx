// MpsPanel — MarketPulse Share Scanner.
//
// Native Omega port of the user's MarketPulse Streamlit scanner. Instead of
// a Python/yfinance sidecar reading a pre-scored CSV, this panel assembles a
// scored share universe entirely from the terminal's existing Yahoo-backed
// routes (/mov, /hp, /key) and runs the ported decision + live-VWAP layer
// (src/scanner/*). No new backend route, no new data feed, no API key.
//
// Args: `MPS [limit]` — optional first arg overrides the deep-enrich cap.
//   e.g. "MPS 40".
//
// The scan is a one-shot heavy operation (many per-symbol calls), so unlike
// the polling Step-5/6 panels there is no auto-poll: the user triggers a
// rescan with the Rescan button (or by changing limit / universe / live).

import { useCallback, useMemo, useState } from 'react';
import { usePanelData } from '@/hooks/usePanelData';
import {
  ProviderBadge,
  SortHeader,
  SkeletonRows,
  FetchStatusBar,
  SummaryCard,
  formatNum,
  formatVolume,
  formatLargeNum,
  dirClass,
} from '@/panels/_shared/PanelChrome';
import { buildScan, type ScanResult } from '@/scanner/buildScan';
import { SCAN_MODES, type ScanRow, type ScanMode } from '@/scanner/score';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

type SortDir = 'asc' | 'desc';
type SortKey =
  | 'live_rank'
  | 'quality'
  | 'combined_score'
  | 'momentum_score'
  | 'explosion_score'
  | 'undervalued_score'
  | 'change_pct'
  | 'relative_volume'
  | 'dollar_volume'
  | 'rsi14'
  | 'symbol';

const LIVE_TONE: Record<string, string> = {
  'BUY TRIGGER': 'text-emerald-400',
  'BUY WATCH': 'text-emerald-300',
  'WAIT FOR PULLBACK': 'text-amber-300',
  'WAIT FOR VWAP RECLAIM': 'text-amber-400',
  'AVOID EXTENDED': 'text-red-400',
  'AVOID WEAK': 'text-red-500',
  'NO LIVE DATA': 'text-amber-700',
};

export function MpsPanel({ args, onNavigate }: Props) {
  const argLimit = Number.parseInt(args[0] ?? '', 10);
  const [limit, setLimit] = useState<number>(
    Number.isFinite(argLimit) && argLimit > 0 ? Math.min(argLimit, 100) : 25
  );
  const [includeLosers, setIncludeLosers] = useState(false);
  const [live, setLive] = useState(true);
  const [modeIdx, setModeIdx] = useState(0);
  const [keyword, setKeyword] = useState('');
  const [nonce, setNonce] = useState(0);

  const mode: ScanMode = SCAN_MODES[modeIdx]!;
  const [sortKey, setSortKey] = useState<SortKey>('live_rank');
  const [sortDir, setSortDir] = useState<SortDir>('asc');

  const fetcher = useCallback(
    (s: AbortSignal) =>
      buildScan({ limit, includeLosers, live, concurrency: 4 }, { signal: s, timeoutMs: 60000 }),
    [limit, includeLosers, live, nonce]
  );
  const { state, refetch } = usePanelData<ScanResult>(fetcher, [limit, includeLosers, live, nonce]);

  const result = state.status === 'ok' ? state.data : state.data ?? null;
  const allRows = result?.rows ?? [];
  const provider = result?.provider ?? 'yahoo';
  const warnings = result?.warnings ?? [];

  // Apply the selected mode preset + keyword filter.
  const filtered = useMemo(() => {
    let rows = applyMode(allRows, mode);
    const kw = keyword.trim().toLowerCase();
    if (kw) {
      rows = rows.filter((r) =>
        `${r.symbol} ${r.name ?? ''} ${r.action ?? ''} ${r.live_decision ?? ''} ${r.bias ?? ''}`
          .toLowerCase()
          .includes(kw)
      );
    }
    return sortRows(rows, sortKey, sortDir);
  }, [allRows, mode, keyword, sortKey, sortDir]);

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir(k === 'symbol' || k === 'live_rank' ? 'asc' : 'desc');
    }
  }

  function selectMode(i: number) {
    setModeIdx(i);
    const m = SCAN_MODES[i]!;
    setSortKey(m.sortCol as SortKey);
    setSortDir(m.ascending ? 'asc' : 'desc');
  }

  const counts = useMemo(() => countDecisions(filtered), [filtered]);
  const avgQuality =
    filtered.length > 0
      ? filtered.reduce((a, r) => a + (r.quality ?? 0), 0) / filtered.length
      : 0;

  return (
    <section className="flex h-full w-full flex-col overflow-hidden p-6 gap-4" aria-label="Share Scanner">
      <header>
        <div className="flex items-center justify-between">
          <div>
            <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">MPS</div>
            <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">Share Scanner</h2>
          </div>
          <ProviderBadge provider={provider} isMock={provider === 'mock'} />
        </div>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          MarketPulse viability scan over Yahoo movers &middot; scores + decision + live VWAP &middot;{' '}
          <span className="text-amber-300">{allRows.length}</span> enriched &middot; deep-enrich cap{' '}
          <span className="text-amber-300">{limit}</span>
        </p>
      </header>

      {/* Mode pills */}
      <div className="flex flex-wrap items-center gap-2">
        {SCAN_MODES.map((m, i) => (
          <button
            key={m.name}
            type="button"
            onClick={() => selectMode(i)}
            className={
              'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
              (i === modeIdx
                ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
            }
          >
            {m.name}
          </button>
        ))}
      </div>

      {/* Controls */}
      <div className="flex flex-wrap items-center gap-3 font-mono text-[11px] text-amber-400">
        <label className="flex items-center gap-1">
          Cap
          <input
            type="number"
            min={1}
            max={100}
            value={limit}
            onChange={(e) => setLimit(clampInt(e.target.value, 1, 100, 25))}
            className="w-16 rounded border border-amber-800 bg-black px-2 py-1 text-amber-200"
          />
        </label>
        <label className="flex items-center gap-1.5">
          <input type="checkbox" checked={includeLosers} onChange={(e) => setIncludeLosers(e.target.checked)} />
          Include losers
        </label>
        <label className="flex items-center gap-1.5">
          <input type="checkbox" checked={live} onChange={(e) => setLive(e.target.checked)} />
          Live VWAP
        </label>
        <input
          type="text"
          placeholder="filter…"
          value={keyword}
          onChange={(e) => setKeyword(e.target.value)}
          className="w-40 rounded border border-amber-800 bg-black px-2 py-1 text-amber-200"
        />
        <button
          type="button"
          onClick={() => {
            setNonce((n) => n + 1);
            refetch();
          }}
          disabled={state.status === 'loading'}
          className="rounded border border-amber-600 bg-amber-900/40 px-3 py-1 uppercase tracking-widest text-amber-200 hover:bg-amber-800/60 disabled:opacity-50"
        >
          {state.status === 'loading' ? 'Scanning…' : 'Rescan'}
        </button>
        <button
          type="button"
          onClick={() => downloadCsv(filtered)}
          disabled={filtered.length === 0}
          className="rounded border border-amber-800 px-3 py-1 uppercase tracking-widest text-amber-400 hover:bg-amber-950/40 disabled:opacity-40"
        >
          CSV
        </button>
      </div>

      {/* Summary */}
      <div className="grid grid-cols-6 gap-3">
        <SummaryCard label="Rows" value={String(filtered.length)} />
        <SummaryCard label="Buy Trigger" value={String(counts.buyTrigger)} accent="text-emerald-400" />
        <SummaryCard label="Buy Watch" value={String(counts.buyWatch)} accent="text-emerald-300" />
        <SummaryCard label="Actionable" value={String(counts.actionable)} />
        <SummaryCard label="Explosion" value={String(counts.explosion)} />
        <SummaryCard label="Avg Quality" value={avgQuality.toFixed(1)} />
      </div>

      {warnings.length > 0 && (
        <div className="border border-amber-700/60 bg-amber-950/30 px-4 py-2 font-mono text-[11px] text-amber-400">
          {warnings.slice(0, 3).map((w, i) => (
            <div key={i}>warning: {w}</div>
          ))}
        </div>
      )}

      {/* Table */}
      <div className="flex-1 overflow-auto border border-amber-800/50 bg-black/40">
        <table className="w-full border-collapse font-mono text-[11px]">
          <thead className="sticky top-0 bg-black">
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="live_rank" label="Live" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="symbol" label="Sym" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <th className="px-3 py-2 uppercase text-amber-500">Action</th>
              <th className="px-3 py-2 uppercase text-amber-500">Decision</th>
              <SortHeader<SortKey> k="quality" label="Qual" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="combined_score" label="Score" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="momentum_score" label="Mom" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="explosion_score" label="Expl" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="undervalued_score" label="Val" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <th className="px-3 py-2 text-right uppercase text-amber-500">Price</th>
              <SortHeader<SortKey> k="change_pct" label="Δ%" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="relative_volume" label="RVol" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="dollar_volume" label="$Vol" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="rsi14" label="RSI" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <th className="px-3 py-2 text-right uppercase text-amber-500">ATR%</th>
              <th className="px-3 py-2 text-right uppercase text-amber-500">PE</th>
              <th className="px-3 py-2 text-right uppercase text-amber-500">Trig</th>
              <th className="px-3 py-2 text-right uppercase text-amber-500">Stop</th>
              <th className="px-3 py-2 text-right uppercase text-amber-500">T1</th>
              <th className="px-3 py-2 text-right uppercase text-amber-500">T2</th>
              <th className="px-3 py-2 uppercase text-amber-500">Bias</th>
            </tr>
          </thead>
          <tbody>
            {filtered.length === 0 && state.status === 'loading' && <SkeletonRows cols={21} rows={10} />}
            {filtered.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={21} className="px-3 py-6 text-center text-amber-600">
                  No rows match this mode. Try “Show everything” or raise the cap, then Rescan.
                </td>
              </tr>
            )}
            {filtered.map((r) => (
              <tr
                key={r.symbol}
                className="border-b border-amber-900/40 hover:bg-amber-950/30 cursor-pointer"
                onClick={() => onNavigate?.(`GP ${r.symbol}`)}
                title="Open price chart (GP)"
              >
                <td className={`px-3 py-1.5 ${LIVE_TONE[r.live_decision ?? ''] ?? 'text-amber-400'}`}>
                  {r.live_decision ?? '—'}
                </td>
                <td className="px-3 py-1.5 font-bold text-amber-300">{r.symbol}</td>
                <td className="px-3 py-1.5 text-amber-400">{r.action ?? '—'}</td>
                <td className="px-3 py-1.5 text-amber-400">{r.decision ?? '—'}</td>
                <td className="px-3 py-1.5 text-right text-amber-300">{fmt(r.quality, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.combined_score, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.momentum_score, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.explosion_score, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.undervalued_score, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.price, 2)}</td>
                <td className={`px-3 py-1.5 text-right ${dirClass(r.change_pct ?? 0)}`}>
                  {Number.isFinite(r.change_pct) ? `${r.change_pct >= 0 ? '+' : ''}${r.change_pct.toFixed(2)}%` : '—'}
                </td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.relative_volume, 2)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">
                  {r.dollar_volume ? formatVolume(r.dollar_volume) : '—'}
                </td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.rsi14, 0)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.atr_pct, 1)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.sec_pe, 1)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.trigger_price, 2)}</td>
                <td className="px-3 py-1.5 text-right text-amber-400">{fmt(r.stop_price, 2)}</td>
                <td className="px-3 py-1.5 text-right text-emerald-300">{fmt(r.target_1, 2)}</td>
                <td className="px-3 py-1.5 text-right text-emerald-300">{fmt(r.target_2, 2)}</td>
                <td className="px-3 py-1.5 text-amber-400/80">{r.bias ?? '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>

      <p className="font-mono text-[10px] text-amber-700">
        Row click → GP chart. Live VWAP from Yahoo 5m bars (~15 min delayed) — planning signal, not an order.
        Scores reconstructed from Yahoo data, not the MarketPulse upstream CSV. {' '}
        <span className="text-amber-600">{formatLargeNum(allRows.reduce((a, r) => a + (r.dollar_volume ?? 0), 0))} total $vol scanned.</span>
      </p>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────── helpers ── */

function fmt(n: number | null | undefined, dp: number): string {
  return typeof n === 'number' && Number.isFinite(n) ? formatNum(n, dp) : '—';
}

function clampInt(s: string, lo: number, hi: number, def: number): number {
  const v = Number.parseInt(s, 10);
  if (!Number.isFinite(v)) return def;
  return Math.max(lo, Math.min(hi, v));
}

function applyMode(rows: ScanRow[], m: ScanMode): ScanRow[] {
  return rows.filter((r) => {
    if (m.live.length > 0 && !m.live.includes(r.live_decision ?? '')) return false;
    if (m.decisions.length > 0 && !m.decisions.includes(r.decision ?? '')) return false;
    if ((r.explosion_score ?? 0) < m.minExplosion) return false;
    if ((r.momentum_score ?? 0) < m.minMomentum) return false;
    if ((r.undervalued_score ?? 0) < m.minValue) return false;
    if ((r.relative_volume ?? 0) < m.minRelVolume) return false;
    const chg = r.change_pct;
    if (Number.isFinite(chg)) {
      if (chg < m.minChange || chg > m.maxChange) return false;
    }
    if ((r.dollar_volume ?? 0) < m.minDollarVolume) return false;
    if ((r.rsi14 ?? 0) > m.maxRsi) return false;
    return true;
  });
}

function sortRows(rows: ScanRow[], key: SortKey, dir: SortDir): ScanRow[] {
  const sign = dir === 'asc' ? 1 : -1;
  return [...rows].sort((a, b) => {
    if (key === 'symbol') return a.symbol.localeCompare(b.symbol) * sign;
    const av = a[key];
    const bv = b[key];
    const an = typeof av === 'number' && Number.isFinite(av) ? av : sign > 0 ? Infinity : -Infinity;
    const bn = typeof bv === 'number' && Number.isFinite(bv) ? bv : sign > 0 ? Infinity : -Infinity;
    return (an - bn) * sign;
  });
}

function countDecisions(rows: ScanRow[]) {
  let buyTrigger = 0;
  let buyWatch = 0;
  let actionable = 0;
  let explosion = 0;
  for (const r of rows) {
    if (r.live_decision === 'BUY TRIGGER') buyTrigger++;
    if (r.live_decision === 'BUY WATCH') buyWatch++;
    if (r.decision === 'ACTIONABLE WATCH') actionable++;
    if (r.action === 'EXPLOSION READY' || r.action === 'TRADE WATCH') explosion++;
  }
  return { buyTrigger, buyWatch, actionable, explosion };
}

const CSV_COLS: Array<keyof ScanRow> = [
  'live_decision', 'symbol', 'name', 'action', 'decision', 'quality',
  'combined_score', 'momentum_score', 'explosion_score', 'undervalued_score',
  'price', 'change_pct', 'relative_volume', 'dollar_volume', 'rsi14', 'atr_pct',
  'sec_pe', 'sec_ps', 'sec_pb', 'sec_roe_pct', 'sec_net_margin_pct', 'sec_debt_equity',
  'trigger_price', 'stop_price', 'target_1', 'target_2', 'bias', 'entry_plan',
  'invalidation', 'risk_plan', 'target_plan', 'reasons', 'risk_flags',
];

function downloadCsv(rows: ScanRow[]) {
  const esc = (v: unknown) => {
    const s = v == null ? '' : String(v);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  const head = CSV_COLS.join(',');
  const body = rows.map((r) => CSV_COLS.map((c) => esc(r[c])).join(',')).join('\n');
  const blob = new Blob([`${head}\n${body}`], { type: 'text/csv' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = 'mps_scan.csv';
  a.click();
  URL.revokeObjectURL(url);
}
