// CCPanel — Command Center.
//
// Step 3 layout (top to bottom):
//   1. Equity strip across the top -- last value + sparkline-style 24h delta.
//      Pulls from /api/v1/omega/equity (interval=1h). Until B.3's
//      OmegaApiServer route returns data the strip shows the anchor only.
//   2. Engines table -- one row per engine with mode, state, last signal,
//      last P&L. 2 s polling.
//   3. Positions summary footer -- count, total exposure (sum of size *
//      entry), total unrealized P&L. 2 s polling.
//
// Args:
//   args[0] (if present) = symbol filter for the positions summary. With no
//   matches the summary shows zeros and the "filter" pill. This is the
//   minimum the panel does with args; deeper symbol-aware behaviour can land
//   later.

import { useCallback } from 'react';
import { getEngines, getEquity, getPositions } from '@/api/omega';
import type { Engine, EquityPoint, OmegaApiError, Position } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
}

const ENGINE_POLL_MS   = 2000;
const POSITION_POLL_MS = 2000;
const EQUITY_POLL_MS   = 30000;

export function CCPanel({ args }: Props) {
  const symbolFilter = (args[0] ?? '').toUpperCase();

  const enginesFetcher  = useCallback((s: AbortSignal) => getEngines({ signal: s }), []);
  const positionsFetcher = useCallback((s: AbortSignal) => getPositions({ signal: s }), []);
  const equityFetcher   = useCallback(
    (s: AbortSignal) => getEquity({ interval: '1h' }, { signal: s }),
    []
  );

  const engines   = usePanelData<Engine[]>(enginesFetcher,   [], { pollMs: ENGINE_POLL_MS });
  const positions = usePanelData<Position[]>(positionsFetcher, [], { pollMs: POSITION_POLL_MS });
  const equity    = usePanelData<EquityPoint[]>(equityFetcher,  [], { pollMs: EQUITY_POLL_MS });

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-6"
      aria-label="Command Center"
    >
      <PanelHeader args={args} symbolFilter={symbolFilter} />

      <EquityStrip state={equity.state} onRetry={equity.refetch} />

      <EngineTable state={engines.state} onRetry={engines.refetch} />

      <PositionsSummary
        state={positions.state}
        onRetry={positions.refetch}
        symbolFilter={symbolFilter}
      />
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Header                                                                  */
/* ──────────────────────────────────────────────────────────────────────── */

function PanelHeader({ args, symbolFilter }: { args: string[]; symbolFilter: string }) {
  return (
    <header>
      <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
        CC
      </div>
      <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
        Command Center
      </h2>
      <p className="mt-2 font-mono text-xs text-amber-500/80">
        Live engine status, equity strip, positions summary.
        {args.length > 0 && (
          <>
            {' '}&middot; filter: <span className="text-amber-300">{symbolFilter}</span>
          </>
        )}
      </p>
    </header>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Equity strip                                                            */
/* ──────────────────────────────────────────────────────────────────────── */

function EquityStrip({
  state,
  onRetry,
}: {
  state: ReturnType<typeof usePanelData<EquityPoint[]>>['state'];
  onRetry: () => void;
}) {
  const series = state.status === 'ok' ? state.data : (state.data ?? []);
  const last = series.length > 0 ? series[series.length - 1]! : null;
  const first = series.length > 0 ? series[0]! : null;
  const delta = last && first ? last.equity - first.equity : 0;
  const isUp = delta >= 0;

  return (
    <div className="border border-amber-800/50 bg-black/40">
      <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
        <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
          Equity
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
          interval 1h &middot; window {series.length} pts
        </span>
      </div>
      <div className="grid grid-cols-3 gap-4 px-4 py-4">
        <Cell label="Account">
          <span className="font-mono text-2xl text-amber-300">
            {last ? `$${formatNum(last.equity, 2)}` : '—'}
          </span>
        </Cell>
        <Cell label="Window Δ">
          <span
            className={`font-mono text-2xl ${isUp ? 'up' : 'down'}`}
            aria-label={isUp ? 'up' : 'down'}
          >
            {series.length === 0 ? '—' : `${isUp ? '+' : ''}$${formatNum(delta, 2)}`}
          </span>
        </Cell>
        <Cell label="Last update">
          <span className="font-mono text-sm text-amber-400">
            {last ? formatUtc(last.ts) : '—'}
          </span>
        </Cell>
      </div>
      <FetchStatusBar state={state} onRetry={onRetry} />
    </div>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Engines table                                                           */
/* ──────────────────────────────────────────────────────────────────────── */

function EngineTable({
  state,
  onRetry,
}: {
  state: ReturnType<typeof usePanelData<Engine[]>>['state'];
  onRetry: () => void;
}) {
  const rows = state.status === 'ok' ? state.data : (state.data ?? []);
  return (
    <div className="border border-amber-800/50 bg-black/40">
      <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
        <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
          Engines
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
          {rows.length} registered &middot; poll 2s
        </span>
      </div>
      <table className="w-full border-collapse font-mono text-xs">
        <thead>
          <tr className="border-b border-amber-900/60 text-left text-amber-500">
            <th className="px-3 py-2">Name</th>
            <th className="px-3 py-2">Mode</th>
            <th className="px-3 py-2">State</th>
            <th className="px-3 py-2">Enabled</th>
            <th className="px-3 py-2 text-right">Last Signal</th>
            <th className="px-3 py-2 text-right">Last P&L</th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && state.status === 'loading' && (
            <SkeletonRows cols={6} rows={6} />
          )}
          {rows.length === 0 && state.status === 'ok' && (
            <tr><td colSpan={6} className="px-3 py-6 text-center text-amber-600">
              No engines registered.
            </td></tr>
          )}
          {rows.map((e) => (
            <tr key={e.name} className="border-b border-amber-900/40">
              <td className="px-3 py-2 font-bold text-amber-300">{e.name}</td>
              <td className={`px-3 py-2 ${e.mode === 'LIVE' ? 'gold' : 'dim'}`}>{e.mode}</td>
              <td className={`px-3 py-2 ${stateClass(e.state)}`}>{e.state}</td>
              <td className="px-3 py-2 text-amber-400">{e.enabled ? 'YES' : 'no'}</td>
              <td className="px-3 py-2 text-right text-amber-400">
                {e.last_signal_ts > 0 ? formatUtc(e.last_signal_ts) : '—'}
              </td>
              <td
                className={`px-3 py-2 text-right ${pnlClass(e.last_pnl)}`}
              >
                {e.last_signal_ts > 0 ? `$${formatNum(e.last_pnl, 2)}` : '—'}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <FetchStatusBar state={state} onRetry={onRetry} />
    </div>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Positions summary                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

function PositionsSummary({
  state,
  onRetry,
  symbolFilter,
}: {
  state: ReturnType<typeof usePanelData<Position[]>>['state'];
  onRetry: () => void;
  symbolFilter: string;
}) {
  const all = state.status === 'ok' ? state.data : (state.data ?? []);
  const rows = symbolFilter ? all.filter((p) => p.symbol.toUpperCase() === symbolFilter) : all;
  const totalExposure = rows.reduce((acc, p) => acc + p.size * p.entry, 0);
  const totalUnrealized = rows.reduce((acc, p) => acc + p.unrealized_pnl, 0);
  return (
    <div className="border border-amber-800/50 bg-black/40">
      <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
        <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
          Positions
        </h3>
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
          {rows.length} open &middot; poll 2s
          {symbolFilter && (<> &middot; filter: <span className="text-amber-400">{symbolFilter}</span></>)}
        </span>
      </div>
      <div className="grid grid-cols-3 gap-4 px-4 py-4">
        <Cell label="Open count">
          <span className="font-mono text-2xl text-amber-300">{rows.length}</span>
        </Cell>
        <Cell label="Notional exposure">
          <span className="font-mono text-2xl text-amber-300">
            ${formatNum(totalExposure, 2)}
          </span>
        </Cell>
        <Cell label="Unrealized P&L">
          <span
            className={`font-mono text-2xl ${pnlClass(totalUnrealized)}`}
          >
            {rows.length === 0 ? '—' : `${totalUnrealized >= 0 ? '+' : ''}$${formatNum(totalUnrealized, 2)}`}
          </span>
        </Cell>
      </div>
      <FetchStatusBar state={state} onRetry={onRetry} />
    </div>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Shared bits                                                             */
/* ──────────────────────────────────────────────────────────────────────── */

function Cell({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div>
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className="mt-1">{children}</div>
    </div>
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
  // YYYY-MM-DD HH:MM UTC -- always UTC because everything in the engine is UTC.
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
