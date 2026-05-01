// EngPanel — Engines drill-down.
//
// Step 3 layout:
//   - Full table of /api/v1/omega/engines: Name | Mode | State | Enabled |
//     Last Signal | Last P&L. Sortable header (click to toggle asc/desc).
//   - Default sort is by Last Signal descending so the most recently active
//     engine bubbles to the top.
//   - Args filter: `ENG <substring>` filters the visible rows by case-
//     insensitive substring match on Name. Useful for "ENG HBG" or "ENG Tsmom".
//   - 2 s polling.
//
// Future Step 4 hook: row click should navigate the workspace to
// `LDG <engine>` once LDG panel is live. Today the click highlights the row
// and prints the action to console as a no-op stub.

import { useCallback, useMemo, useState } from 'react';
import { getEngines } from '@/api/omega';
import type { Engine, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
}

const ENGINE_POLL_MS = 2000;

type SortKey = 'name' | 'mode' | 'state' | 'enabled' | 'last_signal_ts' | 'last_pnl';
type SortDir = 'asc' | 'desc';

export function EngPanel({ args }: Props) {
  const filter = (args[0] ?? '').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('last_signal_ts');
  const [sortDir, setSortDir] = useState<SortDir>('desc');
  const [highlight, setHighlight] = useState<string | null>(null);

  const fetcher = useCallback((s: AbortSignal) => getEngines({ signal: s }), []);
  const { state, refetch } = usePanelData<Engine[]>(fetcher, [], { pollMs: ENGINE_POLL_MS });

  const all = state.status === 'ok' ? state.data : (state.data ?? []);
  const visible = useMemo(() => {
    const filtered = filter
      ? all.filter((e) => e.name.toUpperCase().includes(filter))
      : all;
    return [...filtered].sort((a, b) => {
      const dir = sortDir === 'asc' ? 1 : -1;
      const av = a[sortKey];
      const bv = b[sortKey];
      if (typeof av === 'number' && typeof bv === 'number') {
        return (av - bv) * dir;
      }
      return String(av).localeCompare(String(bv)) * dir;
    });
  }, [all, filter, sortKey, sortDir]);

  function clickHeader(k: SortKey) {
    if (k === sortKey) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(k);
      setSortDir(k === 'last_signal_ts' || k === 'last_pnl' ? 'desc' : 'asc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Engines drill-down"
    >
      <header>
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          ENG
        </div>
        <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
          Engines
        </h2>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Per-engine status, last signal, and P&amp;L attribution. Click a column
          header to sort. Click a row to highlight (drill-down lands in step 4).
          {filter && (
            <>
              {' '}&middot; filter: <span className="text-amber-300">{filter}</span>
              {' '}({visible.length}/{all.length} engines)
            </>
          )}
        </p>
      </header>

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Registered engines
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {visible.length} {visible.length === 1 ? 'row' : 'rows'} &middot; poll 2s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader k="name"           label="Name"         sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="mode"           label="Mode"         sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="state"          label="State"        sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="enabled"        label="Enabled"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="last_signal_ts" label="Last Signal"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="last_pnl"       label="Last P&L"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {visible.length === 0 && state.status === 'loading' && (
              <SkeletonRows cols={6} rows={8} />
            )}
            {visible.length === 0 && state.status === 'ok' && (
              <tr><td colSpan={6} className="px-3 py-6 text-center text-amber-600">
                {filter
                  ? `No engines match "${filter}".`
                  : 'No engines registered.'}
              </td></tr>
            )}
            {visible.map((e) => (
              <tr
                key={e.name}
                onClick={() => {
                  setHighlight(e.name);
                  // Step 4 will navigate to LDG <engine>; for now, no-op.
                  // eslint-disable-next-line no-console
                  console.log(`[ENG] row clicked: ${e.name} (drill-down lands in step 4)`);
                }}
                className={`cursor-pointer border-b border-amber-900/40 transition-colors hover:bg-amber-950/40 ${
                  highlight === e.name ? 'bg-amber-900/40' : ''
                }`}
              >
                <td className="px-3 py-2 font-bold text-amber-300">{e.name}</td>
                <td className={`px-3 py-2 ${e.mode === 'LIVE' ? 'gold' : 'dim'}`}>{e.mode}</td>
                <td className={`px-3 py-2 ${stateClass(e.state)}`}>{e.state}</td>
                <td className="px-3 py-2 text-amber-400">{e.enabled ? 'YES' : 'no'}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {e.last_signal_ts > 0 ? formatUtc(e.last_signal_ts) : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${pnlClass(e.last_pnl)}`}>
                  {e.last_signal_ts > 0 ? `$${formatNum(e.last_pnl, 2)}` : '—'}
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
              <span className="inline-block h-3 w-20 animate-pulse rounded bg-amber-900/30" />
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

function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '0';
  return n.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp });
}

function formatUtc(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm   = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd   = String(d.getUTCDate()).padStart(2, '0');
  const HH   = String(d.getUTCHours()).padStart(2, '0');
  const MM   = String(d.getUTCMinutes()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${HH}:${MM}Z`;
}

function stateClass(state: string): string {
  if (state === 'RUNNING') return 'up';
  if (state === 'ERR')     return 'down';
  return 'dim';
}

function pnlClass(pnl: number): string {
  if (pnl > 0)  return 'up';
  if (pnl < 0)  return 'down';
  return 'text-amber-400';
}
