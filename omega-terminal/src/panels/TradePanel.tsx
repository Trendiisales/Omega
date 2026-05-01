// TradePanel — single-trade drill-down.
//
// Step 4 deliverable. Renders the timeline + economics for one closed
// trade identified by `args[0]` (the trade id, as carried through the
// router from `TRADE 12345` or from a row click in LdgPanel).
//
// Data source — client-side fallback over the ledger snapshot:
//   STEP4_OPENER §A.1.TRADE allows two implementations:
//     (a) a new C++ route `/api/v1/omega/trade/<id>`, or
//     (b) pull the row from the ledger snapshot client-side as a
//         fallback when no dedicated route exists.
//   This panel ships with (b). The fetch grabs the most recent
//   `TRADE_LOOKUP_LIMIT` rows from `/ledger` and finds the matching id
//   in memory. If the id isn't in that window, the panel surfaces a
//   "trade not found in recent window" message and offers to pull a
//   bigger window. The dedicated `/trade/<id>` route can land later
//   without touching this panel — the lookup contract just gets faster.
//
// LedgerEntry fields available today:
//   id, engine, symbol, side, entry_ts, exit_ts, entry, exit, pnl, reason
// Fields the STEP4_OPENER also mentioned (MFE / MAE / signal context)
// are NOT in `LedgerEntry`. They live on the (open-) Position row, which
// is gone by the time a trade lands in the ledger. Two paths to surface
// them later: extend LedgerEntry on the C++ side, or snapshot the
// position into the ledger when it closes. Either is engine-side work
// and out of scope for the Step-4 UI cut. Surfaced here as a labeled
// "deferred" row in the timeline so the gap is visible.

import { useCallback, useMemo } from 'react';
import { getLedger } from '@/api/omega';
import type { LedgerEntry, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const TRADE_LOOKUP_LIMIT = 5000;

export function TradePanel({ args, onNavigate }: Props) {
  const tradeId = (args[0] ?? '').trim();

  const fetcher = useCallback(
    (s: AbortSignal) =>
      getLedger({ limit: TRADE_LOOKUP_LIMIT }, { signal: s }),
    []
  );
  const { state, refetch } = usePanelData<LedgerEntry[]>(fetcher, [tradeId]);

  const trade = useMemo<LedgerEntry | null>(() => {
    if (!tradeId) return null;
    const rows = state.status === 'ok' ? state.data : (state.data ?? []);
    return rows.find((r) => r.id === tradeId) ?? null;
  }, [state, tradeId]);

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Trade drill-down"
    >
      <header className="flex items-baseline justify-between">
        <div>
          <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
            TRADE
          </div>
          <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
            Trade Drill-Down
          </h2>
          <p className="mt-2 font-mono text-xs text-amber-500/80">
            Per-trade timeline. Looked up in the most recent {TRADE_LOOKUP_LIMIT}
            ledger rows; no dedicated `/trade/&lt;id&gt;` route exists yet.
            {tradeId && (
              <>
                {' '}&middot; id:{' '}
                <span className="text-amber-300">{tradeId}</span>
              </>
            )}
          </p>
        </div>
        {onNavigate && (
          <button
            type="button"
            onClick={() => onNavigate('LDG')}
            className="rounded border border-amber-700 px-3 py-1 font-mono text-[10px] uppercase tracking-widest text-amber-300 hover:bg-amber-900/40"
          >
            ← Back to LDG
          </button>
        )}
      </header>

      {!tradeId && <NoIdHint />}

      {tradeId && state.status === 'loading' && trade === null && (
        <LoadingCard />
      )}

      {tradeId && state.status === 'err' && trade === null && (
        <ErrorCard
          message={(state.error as OmegaApiError).message}
          onRetry={refetch}
        />
      )}

      {tradeId && (state.status === 'ok' || trade !== null) && trade === null && (
        <NotFoundCard tradeId={tradeId} onRetry={refetch} />
      )}

      {trade !== null && (
        <TradeDetail trade={trade} onNavigate={onNavigate} />
      )}
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Sub-views                                                               */
/* ──────────────────────────────────────────────────────────────────────── */

function NoIdHint() {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-6 py-10 text-center">
      <p className="font-mono text-sm text-amber-400">
        No trade id supplied.
      </p>
      <p className="mt-2 font-mono text-xs text-amber-500/70">
        Type{' '}
        <span className="text-amber-300">TRADE &lt;id&gt;</span> in the
        command bar, or click a row in the LDG panel to drill in.
      </p>
    </div>
  );
}

function LoadingCard() {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-6 py-10 text-center">
      <span className="font-mono text-xs text-amber-500">
        Loading ledger snapshot…
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

function NotFoundCard({
  tradeId,
  onRetry,
}: {
  tradeId: string;
  onRetry: () => void;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-6 py-10 text-center">
      <p className="font-mono text-sm text-amber-400">
        Trade <span className="text-amber-300">{tradeId}</span> not found in the
        recent {TRADE_LOOKUP_LIMIT}-row window.
      </p>
      <p className="mt-2 font-mono text-xs text-amber-500/70">
        It may be older than the lookup window, or the id may be wrong.
      </p>
      <button
        type="button"
        onClick={onRetry}
        className="mt-4 rounded border border-amber-700 px-3 py-1 font-mono text-[10px] uppercase tracking-widest text-amber-300 hover:bg-amber-900/40"
      >
        Retry lookup
      </button>
    </div>
  );
}

function TradeDetail({
  trade,
  onNavigate,
}: {
  trade: LedgerEntry;
  onNavigate?: (target: string) => void;
}) {
  const durationMs = Math.max(0, trade.exit_ts - trade.entry_ts);
  const durationLabel = formatDuration(durationMs);
  const moveAbs = trade.exit - trade.entry;
  const movePct =
    trade.entry !== 0 ? (moveAbs / trade.entry) * 100 : 0;
  const dirSign = trade.side === 'LONG' ? 1 : -1;

  return (
    <div className="flex flex-col gap-4">
      {/* Top summary cards */}
      <div className="grid grid-cols-4 gap-4">
        <DetailCard label="Engine">
          <button
            type="button"
            onClick={() =>
              onNavigate ? onNavigate(`LDG ${trade.engine}`) : undefined
            }
            className="font-mono text-base font-bold text-amber-300 hover:text-amber-200 hover:underline"
            disabled={!onNavigate}
          >
            {trade.engine}
          </button>
        </DetailCard>
        <DetailCard label="Symbol">
          <span className="font-mono text-base text-amber-300">
            {trade.symbol}
          </span>
        </DetailCard>
        <DetailCard label="Side">
          <span
            className={`font-mono text-base ${trade.side === 'LONG' ? 'up' : 'down'}`}
          >
            {trade.side}
          </span>
        </DetailCard>
        <DetailCard label="Realized P&L">
          <span className={`font-mono text-base ${pnlClass(trade.pnl)}`}>
            {`${trade.pnl >= 0 ? '+' : ''}$${formatNum(trade.pnl, 2)}`}
          </span>
        </DetailCard>
      </div>

      {/* Timeline */}
      <div className="border border-amber-800/50 bg-black/40">
        <div className="border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Timeline
          </h3>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <tbody>
            <TimelineRow label="Entry" ts={trade.entry_ts} price={trade.entry} />
            <TimelineRow label="Exit"  ts={trade.exit_ts}  price={trade.exit}  />
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">Duration</td>
              <td className="px-3 py-2 font-mono text-amber-300">
                {durationLabel}
              </td>
              <td className="px-3 py-2 text-right text-amber-500/70">Move</td>
              <td className="px-3 py-2 text-right font-mono text-amber-300">
                {`${moveAbs >= 0 ? '+' : ''}${formatNum(moveAbs, 4)}`}{' '}
                <span
                  className={`ml-2 ${pnlClass(dirSign * moveAbs)}`}
                >
                  ({moveAbs >= 0 ? '+' : ''}
                  {movePct.toFixed(2)}%)
                </span>
              </td>
            </tr>
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">Exit reason</td>
              <td colSpan={3} className="px-3 py-2 font-mono text-amber-300">
                {trade.reason || '—'}
              </td>
            </tr>
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">MFE / MAE</td>
              <td colSpan={3} className="px-3 py-2 font-mono text-amber-500/60">
                deferred — not stored on closed-trade ledger today
              </td>
            </tr>
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">Signal context</td>
              <td colSpan={3} className="px-3 py-2 font-mono text-amber-500/60">
                deferred — engine-side accessor not yet wired
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      {/* Raw payload for debugging / copy-paste */}
      <details className="border border-amber-800/50 bg-black/40">
        <summary className="cursor-pointer select-none border-b border-amber-800/40 px-4 py-2 font-mono text-xs uppercase tracking-[0.3em] text-amber-500 hover:text-amber-300">
          Raw payload (JSON)
        </summary>
        <pre className="overflow-x-auto px-4 py-3 font-mono text-[11px] leading-5 text-amber-400">
{JSON.stringify(trade, null, 2)}
        </pre>
      </details>
    </div>
  );
}

function TimelineRow({
  label,
  ts,
  price,
}: {
  label: string;
  ts: number;
  price: number;
}) {
  return (
    <tr className="border-t border-amber-900/40">
      <td className="px-3 py-2 text-amber-500/70">{label}</td>
      <td className="px-3 py-2 font-mono text-amber-300">{formatUtcFull(ts)}</td>
      <td className="px-3 py-2 text-right text-amber-500/70">Price</td>
      <td className="px-3 py-2 text-right font-mono text-amber-300">
        {formatNum(price, 4)}
      </td>
    </tr>
  );
}

function DetailCard({
  label,
  children,
}: {
  label: string;
  children: React.ReactNode;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className="mt-1">{children}</div>
    </div>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Formatters                                                              */
/* ──────────────────────────────────────────────────────────────────────── */

function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '0';
  return n.toLocaleString('en-US', {
    minimumFractionDigits: dp,
    maximumFractionDigits: dp,
  });
}

function formatUtcFull(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm   = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd   = String(d.getUTCDate()).padStart(2, '0');
  const HH   = String(d.getUTCHours()).padStart(2, '0');
  const MM   = String(d.getUTCMinutes()).padStart(2, '0');
  const SS   = String(d.getUTCSeconds()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${HH}:${MM}:${SS}Z`;
}

function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '—';
  const sec = Math.floor(ms / 1000);
  const days = Math.floor(sec / 86400);
  const hrs  = Math.floor((sec % 86400) / 3600);
  const mins = Math.floor((sec % 3600) / 60);
  const s    = sec % 60;
  if (days > 0) return `${days}d ${hrs}h ${mins}m`;
  if (hrs  > 0) return `${hrs}h ${mins}m`;
  if (mins > 0) return `${mins}m ${s}s`;
  return `${s}s`;
}

function pnlClass(pnl: number): string {
  if (pnl > 0) return 'up';
  if (pnl < 0) return 'down';
  return 'text-amber-400';
}
