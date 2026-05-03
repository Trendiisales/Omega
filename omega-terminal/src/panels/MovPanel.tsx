// MovPanel — Movers (top gainers / losers / unusually active).
//
// Step 5 deliverable. Sortable table of the day's most-active /
// most-gaining / most-losing equities pulled from OpenBB Hub via
// `/api/v1/omega/mov`. Pattern follows LdgPanel (sortable headers, red
// retry banner, skeleton rows) with a one-extra-touch universe-pill row
// of three buttons: ACTIVE / GAINERS / LOSERS, click to switch tabs
// without leaving the panel.
//
// Args: `MOV <universe>`. Universe values:
//
//   "active"   (default) -> /equity/discovery/active
//   "gainers"            -> /equity/discovery/gainers
//   "losers"             -> /equity/discovery/losers
//
// Anything else is coerced to "active" by the engine route.
//
// Polling cadence: 1 s. This is the most-frequent poll in the Step-5 set
// because movers genuinely move tick-to-tick. The engine-side OpenBbProxy
// caches each response for 750 ms, so two UI polls within the same
// second hit the cache and only one call per second leaves the box --
// safe for OpenBB free-tier rate limits even with multiple tabs open.

import { useCallback, useMemo, useState } from 'react';
import { getMov } from '@/api/omega';
import type { OpenBbEnvelope, MovRow, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const MOV_POLL_MS = 1000;

type Universe = 'active' | 'gainers' | 'losers';

type SortKey = 'symbol' | 'name' | 'price' | 'change' | 'percent' | 'volume';
type SortDir = 'asc' | 'desc';

export function MovPanel({ args }: Props) {
  // First arg is the args-overload universe; we ALSO let users click the
  // pill row to flip universes locally without re-typing in the command bar.
  const initialUniverse: Universe = parseUniverse(args[0]);
  const [universe, setUniverse] = useState<Universe>(initialUniverse);
  const [sortKey, setSortKey] = useState<SortKey>('percent');
  const [sortDir, setSortDir] = useState<SortDir>(
    initialUniverse === 'losers' ? 'asc' : 'desc'
  );

  const fetcher = useCallback(
    (s: AbortSignal) => getMov({ universe }, { signal: s }),
    [universe]
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<MovRow>>(
    fetcher,
    [universe],
    { pollMs: MOV_POLL_MS }
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
  const totalVol  = rows.reduce((acc, r) => acc + (r.volume ?? 0), 0);

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
      aria-label="Movers"
    >
      <header>
        <div className="flex items-center justify-between">
          <div>
            <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
              MOV
            </div>
            <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
              Movers
            </h2>
          </div>
          <ProviderBadge provider={provider} isMock={isMock} />
        </div>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Top gainers / losers / unusually-active &middot; universe{' '}
          <span className="text-amber-300">{universe}</span> &middot; poll 1s
        </p>
      </header>

      <div className="flex items-center gap-2">
        <UniversePill current={universe} value="active"  onClick={setUniverse} label="ACTIVE" />
        <UniversePill current={universe} value="gainers" onClick={setUniverse} label="GAINERS" />
        <UniversePill current={universe} value="losers"  onClick={setUniverse} label="LOSERS" />
      </div>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Symbols" value={String(rows.length)} />
        <SummaryCard label="Up"      value={String(upCount)}   accent="up" />
        <SummaryCard label="Down"    value={String(downCount)} accent="down" />
        <SummaryCard label="Volume"  value={formatVolume(totalVol)} />
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
            {universe}
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'row' : 'rows'} &middot; poll 1s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader k="symbol"  label="Symbol" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="name"    label="Name"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="price"   label="Price"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="change"  label="Δ"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="percent" label="Δ%"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="volume"  label="Volume" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && (
              <SkeletonRows cols={6} rows={8} />
            )}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr><td colSpan={6} className="px-3 py-6 text-center text-amber-600">
                No rows returned for universe "{universe}".
              </td></tr>
            )}
            {sorted.map((r) => {
              const pct = r.percent_change ?? r.change_percent ?? 0;
              return (
                <tr key={r.symbol} className="border-b border-amber-900/40">
                  <td className="px-3 py-2 font-bold text-amber-300">{r.symbol}</td>
                  <td className="px-3 py-2 text-amber-400">{r.name ?? '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">
                    {r.price != null ? formatNum(r.price, 2) : '—'}
                  </td>
                  <td className={`px-3 py-2 text-right ${dirClass(r.change ?? 0)}`}>
                    {r.change != null
                      ? `${r.change >= 0 ? '+' : ''}${formatNum(r.change, 2)}`
                      : '—'}
                  </td>
                  <td className={`px-3 py-2 text-right ${dirClass(pct)}`}>
                    {pct !== 0 || r.percent_change != null || r.change_percent != null
                      ? `${pct >= 0 ? '+' : ''}${pct.toFixed(2)}%`
                      : '—'}
                  </td>
                  <td className="px-3 py-2 text-right text-amber-400">
                    {r.volume != null ? formatVolume(r.volume) : '—'}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Helpers                                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

function parseUniverse(s: string | undefined): Universe {
  const v = (s ?? '').toLowerCase();
  if (v === 'gainers' || v === 'losers') return v;
  return 'active';
}

function sortRows(rows: MovRow[], key: SortKey, dir: SortDir): MovRow[] {
  return [...rows].sort((a, b) => {
    const sign = dir === 'asc' ? 1 : -1;
    let av: unknown;
    let bv: unknown;
    if (key === 'percent') {
      av = a.percent_change ?? a.change_percent;
      bv = b.percent_change ?? b.change_percent;
    } else {
      av = a[key as keyof MovRow];
      bv = b[key as keyof MovRow];
    }
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
/*  UI bits                                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

function UniversePill({
  current,
  value,
  onClick,
  label,
}: {
  current: Universe;
  value: Universe;
  onClick: (u: Universe) => void;
  label: string;
}) {
  const active = current === value;
  return (
    <button
      type="button"
      onClick={() => onClick(value)}
      className={
        'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
        (active
          ? 'border-amber-300 bg-amber-900/40 text-amber-200'
          : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
      }
    >
      {label}
    </button>
  );
}

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
