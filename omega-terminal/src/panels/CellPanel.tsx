// CellPanel — per-cell drill across Tsmom / Donchian / EmaPullback / TrendRider.
//
// Step 4 deliverable, Step 7 GUI completion.
//
// Step 7 update:
//   The /api/v1/omega/cells route is now live. v1 of the route derives
//   the cell-grid by grouping the closed-trade ledger on (engine, symbol)
//   — see build_cells_json() in src/api/OmegaApiServer.cpp. When the
//   engines later self-register their internal cell registries
//   (CellSummaryRegistry mirroring OpenPositionRegistry), the route's
//   shape stays identical and this panel automatically picks up the
//   richer source.
//
// Args:
//   args[0] -> optional engine filter (case-sensitive, e.g. "HybridGold")
//   args[1] -> optional symbol filter (case-sensitive, e.g. "XAUUSD")
//
//   `CELL` with no args returns every cell.
//   `CELL HybridGold` filters to one engine.
//   `CELL HybridGold XAUUSD` filters to one cell.
//
// Sort: total_pnl desc by default. Click any column header to re-sort.
// Click a row to navigate to LDG with the engine pre-filtered.

import { useCallback, useMemo, useState } from 'react';
import { getCells } from '@/api/omega';
import type { CellRow, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

type SortKey =
  | 'engine'
  | 'symbol'
  | 'trades'
  | 'win_rate'
  | 'total_pnl'
  | 'mfe_avg'
  | 'mae_avg'
  | 'last_signal_ts';
type SortDir = 'asc' | 'desc';

export function CellPanel({ args, onNavigate }: Props) {
  // arg0 is engine; arg1 is symbol. Both optional.
  const engineFilter = (args[0] ?? '').trim();
  const symbolFilter = (args[1] ?? '').trim();

  const [sortKey, setSortKey] = useState<SortKey>('total_pnl');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) =>
      getCells(
        {
          engine: engineFilter || undefined,
          symbol: symbolFilter || undefined,
        },
        { signal: s },
      ),
    [engineFilter, symbolFilter],
  );

  const { state, refetch } = usePanelData<CellRow[]>(fetcher, [
    engineFilter,
    symbolFilter,
  ]);

  const rows = state.status === 'ok' ? state.data : (state.data ?? []);

  const sorted = useMemo<CellRow[]>(() => {
    const out = [...rows];
    out.sort((a, b) => compareCellRows(a, b, sortKey, sortDir));
    return out;
  }, [rows, sortKey, sortDir]);

  const onHeaderClick = useCallback(
    (key: SortKey) => {
      if (key === sortKey) {
        setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
      } else {
        setSortKey(key);
        // Sensible defaults: text columns asc, numeric desc.
        setSortDir(key === 'engine' || key === 'symbol' ? 'asc' : 'desc');
      }
    },
    [sortKey],
  );

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Cell grid"
    >
      <header>
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          CELL
        </div>
        <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
          Cell Grid
        </h2>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Per-cell summary derived from the closed-trade ledger. Live as
          the engines trade.
          {engineFilter && (
            <>
              {' '}&middot; engine:{' '}
              <span className="text-amber-300">{engineFilter}</span>
            </>
          )}
          {symbolFilter && (
            <>
              {' '}&middot; symbol:{' '}
              <span className="text-amber-300">{symbolFilter}</span>
            </>
          )}
        </p>
      </header>

      {state.status === 'loading' && rows.length === 0 && <LoadingCard />}

      {state.status === 'err' && rows.length === 0 && (
        <ErrorCard
          message={(state.error as OmegaApiError).message}
          onRetry={refetch}
        />
      )}

      {state.status !== 'loading' && rows.length === 0 && state.status !== 'err' && (
        <EmptyCard hasFilter={Boolean(engineFilter || symbolFilter)} />
      )}

      {sorted.length > 0 && (
        <CellTable
          rows={sorted}
          sortKey={sortKey}
          sortDir={sortDir}
          onHeaderClick={onHeaderClick}
          onNavigate={onNavigate}
        />
      )}
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Sub-views                                                               */
/* ──────────────────────────────────────────────────────────────────────── */

function LoadingCard() {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-6 py-10 text-center">
      <span className="font-mono text-xs text-amber-500">
        Loading cell grid…
      </span>
    </div>
  );
}

function ErrorCard({
  message,
  onRetry,
}: {
  message: string;
  onRetry: () => void;
}) {
  return (
    <div className="flex items-center justify-between border border-red-700/50 bg-red-950/20 px-4 py-3">
      <span className="font-mono text-xs text-down">{message}</span>
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

function EmptyCard({ hasFilter }: { hasFilter: boolean }) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-6 py-10 text-center">
      <p className="font-mono text-sm text-amber-400">
        No cells in the current ledger window.
      </p>
      <p className="mt-2 font-mono text-xs text-amber-500/70">
        {hasFilter
          ? 'No closed trades match the active engine/symbol filter. Try widening.'
          : 'The engine has not closed any trades yet — the grid populates as trades land in g_omegaLedger.'}
      </p>
    </div>
  );
}

function CellTable({
  rows,
  sortKey,
  sortDir,
  onHeaderClick,
  onNavigate,
}: {
  rows: CellRow[];
  sortKey: SortKey;
  sortDir: SortDir;
  onHeaderClick: (k: SortKey) => void;
  onNavigate?: (target: string) => void;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40">
      <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
        <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
          Cells ({rows.length})
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
          click row → LDG &lt;engine&gt;
        </span>
      </div>
      <table className="w-full border-collapse font-mono text-xs">
        <thead>
          <tr className="border-b border-amber-900/60 text-left text-amber-500">
            <Th k="engine" cur={sortKey} dir={sortDir} on={onHeaderClick}>
              Engine
            </Th>
            <Th k="symbol" cur={sortKey} dir={sortDir} on={onHeaderClick}>
              Symbol
            </Th>
            <Th
              k="trades"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Trades
            </Th>
            <Th
              k="win_rate"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Win Rate
            </Th>
            <Th
              k="total_pnl"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Total P&amp;L
            </Th>
            <Th
              k="mfe_avg"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Avg MFE
            </Th>
            <Th
              k="mae_avg"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Avg MAE
            </Th>
            <Th
              k="last_signal_ts"
              cur={sortKey}
              dir={sortDir}
              on={onHeaderClick}
              align="right"
            >
              Last Signal
            </Th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <tr
              key={r.cell}
              className="cursor-pointer border-b border-amber-900/40 hover:bg-amber-950/30"
              onClick={() => onNavigate?.(`LDG ${r.engine}`)}
            >
              <td className="px-3 py-2 text-amber-300">{r.engine}</td>
              <td className="px-3 py-2 text-amber-300">{r.symbol}</td>
              <td className="px-3 py-2 text-right text-amber-300">{r.trades}</td>
              <td className="px-3 py-2 text-right text-amber-300">
                {(r.win_rate * 100).toFixed(1)}%
              </td>
              <td
                className={`px-3 py-2 text-right ${pnlClass(r.total_pnl)}`}
              >
                {`${r.total_pnl >= 0 ? '+' : ''}$${formatNum(r.total_pnl, 2)}`}
              </td>
              <td className="px-3 py-2 text-right text-amber-300">
                {formatNum(r.mfe_avg, 4)}
              </td>
              <td className="px-3 py-2 text-right text-amber-300">
                {formatNum(r.mae_avg, 4)}
              </td>
              <td className="px-3 py-2 text-right font-mono text-amber-500/80">
                {formatRelative(r.last_signal_ts)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function Th({
  k,
  cur,
  dir,
  on,
  align,
  children,
}: {
  k: SortKey;
  cur: SortKey;
  dir: SortDir;
  on: (k: SortKey) => void;
  align?: 'left' | 'right';
  children: React.ReactNode;
}) {
  const isCur = k === cur;
  const arrow = isCur ? (dir === 'asc' ? '▲' : '▼') : '';
  return (
    <th
      className={`cursor-pointer select-none px-3 py-2 hover:text-amber-300 ${
        align === 'right' ? 'text-right' : 'text-left'
      } ${isCur ? 'text-amber-300' : ''}`}
      onClick={() => on(k)}
    >
      {children}
      {arrow && <span className="ml-1 text-[10px]">{arrow}</span>}
    </th>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Sort + formatters                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

function compareCellRows(
  a: CellRow,
  b: CellRow,
  key: SortKey,
  dir: SortDir,
): number {
  const sign = dir === 'asc' ? 1 : -1;
  switch (key) {
    case 'engine':
      return sign * a.engine.localeCompare(b.engine);
    case 'symbol':
      return sign * a.symbol.localeCompare(b.symbol);
    case 'trades':
      return sign * (a.trades - b.trades);
    case 'win_rate':
      return sign * (a.win_rate - b.win_rate);
    case 'total_pnl':
      return sign * (a.total_pnl - b.total_pnl);
    case 'mfe_avg':
      return sign * (a.mfe_avg - b.mfe_avg);
    case 'mae_avg':
      return sign * (a.mae_avg - b.mae_avg);
    case 'last_signal_ts':
      return sign * (a.last_signal_ts - b.last_signal_ts);
    default:
      return 0;
  }
}

function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '0';
  return n.toLocaleString('en-US', {
    minimumFractionDigits: dp,
    maximumFractionDigits: dp,
  });
}

function formatRelative(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const ageMs = Date.now() - unixMs;
  if (ageMs < 0) return formatUtcShort(unixMs);
  const sec = Math.floor(ageMs / 1000);
  if (sec < 60) return `${sec}s ago`;
  const min = Math.floor(sec / 60);
  if (min < 60) return `${min}m ago`;
  const hr = Math.floor(min / 60);
  if (hr < 24) return `${hr}h ago`;
  const day = Math.floor(hr / 24);
  if (day < 30) return `${day}d ago`;
  return formatUtcShort(unixMs);
}

function formatUtcShort(unixMs: number): string {
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd = String(d.getUTCDate()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd}`;
}

function pnlClass(pnl: number): string {
  if (pnl > 0) return 'up';
  if (pnl < 0) return 'down';
  return 'text-amber-400';
}
