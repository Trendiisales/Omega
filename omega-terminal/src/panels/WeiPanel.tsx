// WeiPanel — World Equity Indices.
//
// Step 5 deliverable. Sortable table of regional index ETF quotes pulled
// from OpenBB Hub via `/api/v1/omega/wei`. Pattern follows LdgPanel:
// sortable column headers, skeleton-rows loading state, red retry banner
// on error, region-pill in the header.
//
// Args: `WEI <region>`. Recognised regions:
//
//   "US"    SPY, QQQ, DIA, IWM, VTI                    (default)
//   "EU"    VGK, EZU, FEZ, EWG, EWU
//   "ASIA"  EWJ, FXI, EWY, EWT, EWA
//   "WORLD" VT, ACWI, VEA, VWO, EFA
//
// Anything else is forwarded as a literal comma-separated symbol list, so
// `WEI AAPL,MSFT,GOOGL` works from the command bar for an ad-hoc tape.
//
// 5 s polling cadence -- the OpenBB free-tier yfinance provider can sustain
// that without throttling, and the panel feels live without being noisy.

import { useCallback, useMemo, useState } from 'react';
import { getWei } from '@/api/omega';
import type { OpenBbEnvelope, WeiQuote, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const WEI_POLL_MS = 5000;

type SortKey =
  | 'symbol'
  | 'name'
  | 'last_price'
  | 'change'
  | 'change_percent'
  | 'volume';
type SortDir = 'asc' | 'desc';

export function WeiPanel({ args }: Props) {
  const region = (args[0] ?? 'US').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('change_percent');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getWei({ region }, { signal: s }),
    [region]
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<WeiQuote>>(
    fetcher,
    [region],
    { pollMs: WEI_POLL_MS }
  );

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock   = provider === 'mock' ||
                   Boolean(env?.extra && (env.extra as Record<string, unknown>).mock);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;

  const rows = env?.results ?? [];
  const sorted = useMemo(() => sortRows(rows, sortKey, sortDir), [rows, sortKey, sortDir]);

  const upCount   = rows.filter((r) => (r.change ?? 0) > 0).length;
  const downCount = rows.filter((r) => (r.change ?? 0) < 0).length;
  const avgPct    = rows.length === 0
    ? 0
    : rows.reduce((acc, r) => acc + (r.change_percent ?? 0), 0) / rows.length;

  function clickHeader(k: SortKey) {
    if (k === sortKey) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(k);
      const isNum = k !== 'symbol' && k !== 'name';
      setSortDir(isNum ? 'desc' : 'asc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="World Equity Indices"
    >
      <header>
        <div className="flex items-center justify-between">
          <div>
            <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
              WEI
            </div>
            <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
              World Equity Indices
            </h2>
          </div>
          <ProviderBadge provider={provider} isMock={isMock} />
        </div>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Index-ETF quotes via OpenBB &middot; region{' '}
          <span className="text-amber-300">{region}</span> &middot; poll 5s
        </p>
      </header>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Symbols" value={String(rows.length)} />
        <SummaryCard
          label="Avg Δ%"
          value={rows.length === 0 ? '—' : `${avgPct >= 0 ? '+' : ''}${avgPct.toFixed(2)}%`}
          accent={avgPct >= 0 ? 'up' : 'down'}
        />
        <SummaryCard label="Up" value={String(upCount)} accent="up" />
        <SummaryCard label="Down" value={String(downCount)} accent="down" />
      </div>

      {warnings.length > 0 && (
        <div className="border border-amber-700/60 bg-amber-950/30 px-4 py-2 font-mono text-xs text-amber-400">
          {warnings.map((w, i) => (
            <div key={i}>warning: {w.message ?? '(no detail)'}</div>
          ))}
        </div>
      )}

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Quotes
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'row' : 'rows'} &middot; poll 5s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader k="symbol"         label="Symbol" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="name"           label="Name"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="last_price"     label="Last"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="change"         label="Δ"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="change_percent" label="Δ%"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="volume"         label="Volume" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && (
              <SkeletonRows cols={6} rows={5} />
            )}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr><td colSpan={6} className="px-3 py-6 text-center text-amber-600">
                No quotes returned for region "{region}".
              </td></tr>
            )}
            {sorted.map((r) => (
              <tr key={r.symbol} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{r.symbol}</td>
                <td className="px-3 py-2 text-amber-400">{r.name ?? '—'}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.last_price != null ? formatNum(r.last_price, 2) : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change ?? 0)}`}>
                  {r.change != null
                    ? `${r.change >= 0 ? '+' : ''}${formatNum(r.change, 2)}`
                    : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change_percent ?? 0)}`}>
                  {r.change_percent != null
                    ? `${r.change_percent >= 0 ? '+' : ''}${r.change_percent.toFixed(2)}%`
                    : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.volume != null ? formatVolume(r.volume) : '—'}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Sorting                                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

function sortRows(rows: WeiQuote[], key: SortKey, dir: SortDir): WeiQuote[] {
  return [...rows].sort((a, b) => {
    const sign = dir === 'asc' ? 1 : -1;
    const av = a[key];
    const bv = b[key];
    if (typeof av === 'number' && typeof bv === 'number') {
      return (av - bv) * sign;
    }
    if (av == null && bv == null) return 0;
    if (av == null) return  1 * sign;
    if (bv == null) return -1 * sign;
    return String(av).localeCompare(String(bv)) * sign;
  });
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Shared bits                                                              */
/* ──────────────────────────────────────────────────────────────────────── */

function SummaryCard({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: string;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className={`mt-1 font-mono text-2xl ${accent ?? 'text-amber-300'}`}>
        {value}
      </div>
    </div>
  );
}

function ProviderBadge({ provider, isMock }: { provider: string; isMock: boolean }) {
  const cls = isMock
    ? 'border-amber-600 text-amber-500'
    : 'border-amber-800 text-amber-400';
  return (
    <span
      className={`rounded border px-2 py-1 font-mono text-[10px] uppercase tracking-widest ${cls}`}
      title={isMock ? 'OMEGA_OPENBB_MOCK=1 active' : 'live OpenBB provider'}
    >
      {isMock ? 'MOCK' : provider}
    </span>
  );
}

function SortHeader({
  k,
  label,
  sortKey,
  sortDir,
  onClick,
  align,
}: {
  k: SortKey;
  label: string;
  sortKey: SortKey;
  sortDir: SortDir;
  onClick: (k: SortKey) => void;
  align?: 'left' | 'right';
}) {
  const active = sortKey === k;
  const arrow = active ? (sortDir === 'asc' ? '▲' : '▼') : '';
  const align_cls = align === 'right' ? 'text-right' : 'text-left';
  return (
    <th
      onClick={() => onClick(k)}
      className={`px-3 py-2 cursor-pointer select-none uppercase ${align_cls} ${
        active ? 'text-amber-300' : 'text-amber-500'
      } hover:text-amber-300`}
    >
      {label} <span className="ml-1 text-[10px]">{arrow}</span>
    </th>
  );
}

function SkeletonRows({ cols, rows }: { cols: number; rows: number }) {
  return (
    <>
      {Array.from({ length: rows }).map((_, i) => (
        <tr key={`sk-${i}`} className="border-b border-amber-900/40">
          {Array.from({ length: cols }).map((__, j) => (
            <td key={j} className="px-3 py-2">
              <span className="inline-block h-3 w-16 animate-pulse rounded bg-amber-900/30" />
            </td>
          ))}
        </tr>
      ))}
    </>
  );
}

function FetchStatusBar<T>({
  state,
  onRetry,
}: {
  state: ReturnType<typeof usePanelData<T>>['state'];
  onRetry: () => void;
}) {
  if (state.status !== 'err') return null;
  return (
    <div className="flex items-center justify-between border-t border-red-700/50 bg-red-950/20 px-4 py-2">
      <span className="font-mono text-xs text-down">
        {(state.error as OmegaApiError).message}
      </span>
      <button
        type="button"
        onClick={onRetry}
        className="rounded border border-amber-700 px-3 py-1 font-mono text-[10px] uppercase tracking-widest text-amber-300 hover:bg-amber-900/40"
      >
        Retry
      </button>
    </div>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Formatting helpers                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '—';
  return n.toLocaleString('en-US', {
    minimumFractionDigits: dp,
    maximumFractionDigits: dp,
  });
}

function formatVolume(n: number): string {
  if (!Number.isFinite(n)) return '—';
  if (n >= 1e9) return `${(n / 1e9).toFixed(2)}B`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}K`;
  return String(Math.round(n));
}

function dirClass(v: number): string {
  if (v > 0) return 'up';
  if (v < 0) return 'down';
  return 'text-amber-400';
}
