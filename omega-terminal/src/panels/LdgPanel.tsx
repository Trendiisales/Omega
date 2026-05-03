// LdgPanel — Trade Ledger.
//
// Step 4 deliverable. Filterable, sortable closed-trade table backed by
// `/api/v1/omega/ledger`. Pattern follows EngPanel/PosPanel:
//   - Sortable column headers (click to toggle asc/desc).
//   - Args filter overloads the first positional arg:
//       * If it matches an engine name (case-insensitive substring on
//         the row's `engine`), the table filters by engine.
//       * Else it's treated as a symbol filter (substring on `symbol`).
//     Both `LDG HybridBracketGold` and `LDG XAUUSD` work that way.
//   - 30 s polling (the ledger updates only on closed trades, so polling
//     much faster than that is wasteful).
//   - Row click navigates the workspace to `TRADE <id>` for drill-down.
//   - Skeleton-rows loading state + red retry banner on OmegaApiError —
//     identical pattern to the Step-3 panels.
//
// Engine-side context: `OmegaApiServer.cpp::build_ledger_json` already
// supports an `engine=<name>` query parameter. We could push the engine
// filter server-side instead of client-side, but pulling the full ledger
// once per 30 s and filtering in the browser keeps the panel snappy when
// the user toggles between filters and matches the way EngPanel/PosPanel
// behave (single fetch, client-side filter / sort).
//
// Server-side filters that ARE worth using: `from`, `to`, and `limit` —
// the panel currently passes `limit: 1000` so the JSON stays small even
// once thousands of trades accrue. Date-range filtering UX is deferred
// to a follow-up.

import { useCallback, useMemo, useState } from 'react';
import { getLedger } from '@/api/omega';
import type { LedgerEntry, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const LEDGER_POLL_MS = 30000;
const LEDGER_LIMIT   = 1000;

type SortKey =
  | 'id'
  | 'engine'
  | 'symbol'
  | 'side'
  | 'entry_ts'
  | 'exit_ts'
  | 'entry'
  | 'exit'
  | 'pnl'
  | 'reason';
type SortDir = 'asc' | 'desc';

export function LdgPanel({ args, onNavigate }: Props) {
  const filter = (args[0] ?? '').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('exit_ts');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getLedger({ limit: LEDGER_LIMIT }, { signal: s }),
    []
  );
  const { state, refetch } = usePanelData<LedgerEntry[]>(fetcher, [], {
    pollMs: LEDGER_POLL_MS,
  });

  const all = state.status === 'ok' ? state.data : (state.data ?? []);

  // First-arg overload: try engine match first, fall back to symbol match.
  // We don't preflight the engine list -- a substring match against any row
  // settles which interpretation wins.
  const visible = useMemo(() => {
    if (!filter) {
      return sortRows(all, sortKey, sortDir);
    }
    const byEngine = all.filter((r) => r.engine.toUpperCase().includes(filter));
    if (byEngine.length > 0) return sortRows(byEngine, sortKey, sortDir);
    const bySymbol = all.filter((r) => r.symbol.toUpperCase().includes(filter));
    return sortRows(bySymbol, sortKey, sortDir);
  }, [all, filter, sortKey, sortDir]);

  function clickHeader(k: SortKey) {
    if (k === sortKey) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(k);
      const isNum =
        k === 'entry_ts' ||
        k === 'exit_ts' ||
        k === 'entry' ||
        k === 'exit' ||
        k === 'pnl';
      setSortDir(isNum ? 'desc' : 'asc');
    }
  }

  const totalPnl = visible.reduce((acc, r) => acc + r.pnl, 0);
  const wins = visible.filter((r) => r.pnl > 0).length;
  const losses = visible.filter((r) => r.pnl < 0).length;
  const winRate =
    visible.length === 0 ? 0 : (wins / visible.length) * 100;

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Trade Ledger"
    >
      <header>
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          LDG
        </div>
        <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
          Trade Ledger
        </h2>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Closed-trade ledger across all engines. Click a column header to
          sort. Click a row to drill into the trade.
          {filter && (
            <>
              {' '}&middot; filter: <span className="text-amber-300">{filter}</span>
              {' '}({visible.length}/{all.length})
            </>
          )}
        </p>
      </header>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Trades" value={String(visible.length)} />
        <SummaryCard
          label="Realized P&L"
          value={
            visible.length === 0
              ? '—'
              : `${totalPnl >= 0 ? '+' : ''}$${formatNum(totalPnl, 2)}`
          }
          accent={pnlClass(totalPnl)}
        />
        <SummaryCard
          label="Win rate"
          value={visible.length === 0 ? '—' : `${winRate.toFixed(1)}%`}
        />
        <SummaryCard
          label="Wins / losses"
          value={`${wins} / ${losses}`}
        />
      </div>

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Closed trades
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {visible.length} {visible.length === 1 ? 'row' : 'rows'} &middot; poll 30s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader k="id"       label="ID"       sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="engine"   label="Engine"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="symbol"   label="Symbol"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="side"     label="Side"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="entry_ts" label="Entry @"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="exit_ts"  label="Exit @"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="entry"    label="Entry"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="exit"     label="Exit"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="pnl"      label="P&L"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="reason"   label="Reason"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
            </tr>
          </thead>
          <tbody>
            {visible.length === 0 && state.status === 'loading' && (
              <SkeletonRows cols={10} rows={6} />
            )}
            {visible.length === 0 && state.status === 'ok' && (
              <tr><td colSpan={10} className="px-3 py-6 text-center text-amber-600">
                {filter
                  ? `No trades match "${filter}".`
                  : 'No trades in the ledger yet.'}
              </td></tr>
            )}
            {visible.map((r) => (
              <tr
                key={r.id}
                onClick={() => {
                  if (onNavigate) onNavigate(`TRADE ${r.id}`);
                }}
                className="cursor-pointer border-b border-amber-900/40 transition-colors hover:bg-amber-950/40"
              >
                <td className="px-3 py-2 text-amber-400">{r.id}</td>
                <td className="px-3 py-2 font-bold text-amber-300">{r.engine}</td>
                <td className="px-3 py-2 text-amber-400">{r.symbol}</td>
                <td className={`px-3 py-2 ${r.side === 'LONG' ? 'up' : 'down'}`}>{r.side}</td>
                <td className="px-3 py-2 text-right text-amber-400">{formatUtc(r.entry_ts)}</td>
                <td className="px-3 py-2 text-right text-amber-400">{formatUtc(r.exit_ts)}</td>
                <td className="px-3 py-2 text-right text-amber-400">{formatNum(r.entry, 4)}</td>
                <td className="px-3 py-2 text-right text-amber-400">{formatNum(r.exit, 4)}</td>
                <td className={`px-3 py-2 text-right ${pnlClass(r.pnl)}`}>
                  {`${r.pnl >= 0 ? '+' : ''}$${formatNum(r.pnl, 2)}`}
                </td>
                <td className="px-3 py-2 text-amber-500/80">{r.reason || '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

function sortRows(
  rows: LedgerEntry[],
  sortKey: SortKey,
  sortDir: SortDir
): LedgerEntry[] {
  return [...rows].sort((a, b) => {
    const dir = sortDir === 'asc' ? 1 : -1;
    const av = a[sortKey];
    const bv = b[sortKey];
    if (typeof av === 'number' && typeof bv === 'number') {
      return (av - bv) * dir;
    }
    return String(av).localeCompare(String(bv)) * dir;
  });
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

function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '0';
  return n.toLocaleString('en-US', {
    minimumFractionDigits: dp,
    maximumFractionDigits: dp,
  });
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

function pnlClass(pnl: number): string {
  if (pnl > 0)  return 'up';
  if (pnl < 0)  return 'down';
  return 'text-amber-400';
}
