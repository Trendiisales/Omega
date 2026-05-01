// PosPanel — open positions.
//
// Step 3 layout:
//   - Full table of /api/v1/omega/positions: Engine | Symbol | Side | Size |
//     Entry | Current | Unrealized | MFE | MAE. Sortable headers.
//   - Args filter: `POS HBG` filters by case-insensitive substring on Engine
//     OR Symbol -- so `POS XAUUSD` and `POS HBG` both work for the gold
//     bracket. The filter chip in the header makes the active filter visible.
//   - 2 s polling.
//
// Step 3 ships only the HybridGold position source on the engine side.
// Other engines (Tsmom/Donchian/EmaPullback/TrendRider/HBI) will populate
// this table once their snapshotters are added in a follow-up session.

import { useCallback, useMemo, useState } from 'react';
import { getPositions } from '@/api/omega';
import type { OmegaApiError, Position } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
}

const POSITION_POLL_MS = 2000;

type SortKey = 'engine' | 'symbol' | 'side' | 'size' | 'entry' | 'current' | 'unrealized_pnl' | 'mfe' | 'mae';
type SortDir = 'asc' | 'desc';

export function PosPanel({ args }: Props) {
  const filter = (args[0] ?? '').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('unrealized_pnl');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback((s: AbortSignal) => getPositions({ signal: s }), []);
  const { state, refetch } = usePanelData<Position[]>(fetcher, [], { pollMs: POSITION_POLL_MS });

  const all = state.status === 'ok' ? state.data : (state.data ?? []);
  const visible = useMemo(() => {
    const filtered = filter
      ? all.filter(
          (p) =>
            p.engine.toUpperCase().includes(filter) ||
            p.symbol.toUpperCase().includes(filter)
        )
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
      // Numerics default to desc, identifiers default to asc.
      const isNum = k === 'size' || k === 'entry' || k === 'current' ||
                    k === 'unrealized_pnl' || k === 'mfe' || k === 'mae';
      setSortDir(isNum ? 'desc' : 'asc');
    }
  }

  const totalUnrl = visible.reduce((acc, p) => acc + p.unrealized_pnl, 0);
  const totalNotional = visible.reduce((acc, p) => acc + p.size * p.entry, 0);

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Open positions"
    >
      <header>
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          POS
        </div>
        <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
          Open Positions
        </h2>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Live open-position table across all engines that publish a snapshotter.
          Step 3 ships the HybridGold source; other engines join in a follow-up.
          {filter && (
            <>
              {' '}&middot; filter: <span className="text-amber-300">{filter}</span>
              {' '}({visible.length}/{all.length})
            </>
          )}
        </p>
      </header>

      <div className="grid grid-cols-3 gap-4">
        <SummaryCard label="Open count" value={String(visible.length)} />
        <SummaryCard
          label="Notional exposure"
          value={`$${formatNum(totalNotional, 2)}`}
        />
        <SummaryCard
          label="Total unrealized P&L"
          value={visible.length === 0 ? '—' : `${totalUnrl >= 0 ? '+' : ''}$${formatNum(totalUnrl, 2)}`}
          accent={pnlClass(totalUnrl)}
        />
      </div>

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Positions
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {visible.length} {visible.length === 1 ? 'row' : 'rows'} &middot; poll 2s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader k="engine"         label="Engine"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="symbol"         label="Symbol"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="side"           label="Side"        sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader k="size"           label="Size"        sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="entry"          label="Entry"       sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="current"        label="Current"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="unrealized_pnl" label="Unrealized"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="mfe"            label="MFE"         sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader k="mae"            label="MAE"         sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {visible.length === 0 && state.status === 'loading' && (
              <SkeletonRows cols={9} rows={4} />
            )}
            {visible.length === 0 && state.status === 'ok' && (
              <tr><td colSpan={9} className="px-3 py-6 text-center text-amber-600">
                {filter
                  ? `No open positions match "${filter}".`
                  : 'No open positions.'}
              </td></tr>
            )}
            {visible.map((p, idx) => (
              <tr key={`${p.engine}-${p.symbol}-${idx}`} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{p.engine}</td>
                <td className="px-3 py-2 text-amber-400">{p.symbol}</td>
                <td className={`px-3 py-2 ${p.side === 'LONG' ? 'up' : 'down'}`}>
                  {p.side}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">{formatNum(p.size, 4)}</td>
                <td className="px-3 py-2 text-right text-amber-400">{formatNum(p.entry, 4)}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {p.current > 0 ? formatNum(p.current, 4) : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${pnlClass(p.unrealized_pnl)}`}>
                  {`${p.unrealized_pnl >= 0 ? '+' : ''}$${formatNum(p.unrealized_pnl, 2)}`}
                </td>
                <td className="px-3 py-2 text-right up">
                  {`$${formatNum(p.mfe, 2)}`}
                </td>
                <td className="px-3 py-2 text-right down">
                  {`$${formatNum(p.mae, 2)}`}
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
  return n.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp });
}

function pnlClass(pnl: number): string {
  if (pnl > 0)  return 'up';
  if (pnl < 0)  return 'down';
  return 'text-amber-400';
}
