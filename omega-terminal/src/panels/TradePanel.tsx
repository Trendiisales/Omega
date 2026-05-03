// TradePanel — single-trade drill-down.
//
// Step 4 deliverable, Step 7 GUI completion.
//
// Step 7 update:
//   - Primary data path is now /api/v1/omega/trade/<id> (TradeDetail). The
//     engine returns the full TradeRecord shape including MFE/MAE, regime,
//     L2 imbalance, ATR at entry, and the slippage/commission breakdown.
//     The "deferred" rows from the Step-4 cut are now real values.
//   - Fallback path: if /trade/<id> 404s (the trade is older than the
//     engine's in-memory ledger window) the panel transparently falls
//     back to scanning the most recent TRADE_LOOKUP_LIMIT rows from
//     /ledger and surfaces the trimmed LedgerEntry shape (no MFE/MAE in
//     that branch — same as the Step-4 behaviour).
//   - Loading + error + not-found cards keep the same chrome as before so
//     the panel looks identical when the data is absent or in flight.
//
// Renders the timeline + economics for one closed trade identified by
// `args[0]` (the trade id, as carried through the router from
// `TRADE 12345` or from a row click in LdgPanel).

import { useCallback, useMemo } from 'react';
import { getLedger, getTrade } from '@/api/omega';
import type { LedgerEntry, OmegaApiError, TradeDetail } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const TRADE_LOOKUP_LIMIT = 5000;

/**
 * The shape the panel actually renders. /trade/<id> returns TradeDetail
 * (the full superset). The ledger fallback returns a LedgerEntry which
 * is the field-trimmed subset; we widen it to TradeDetail-ish shape
 * with sentinel zeros for the missing fields and a `_partial` flag the
 * UI uses to grey out the rows that fallback can't fill.
 */
type RenderedTrade =
  | { kind: 'full'; data: TradeDetail }
  | { kind: 'partial'; data: LedgerEntry };

export function TradePanel({ args, onNavigate }: Props) {
  const tradeId = (args[0] ?? '').trim();

  // Primary fetch: /trade/<id>. Returns null when the id is empty so the
  // panel can render the NoIdHint without firing a request. Returns null
  // on 404 so the fallback effect can take over without surfacing the
  // error card. Any other failure mode (network, 5xx) is rethrown so the
  // hook surfaces the red banner.
  const primaryFetcher = useCallback(
    async (s: AbortSignal): Promise<TradeDetail | null> => {
      if (tradeId.length === 0) return null;
      try {
        return await getTrade(tradeId, { signal: s });
      } catch (e) {
        const err = e as OmegaApiError;
        if (err && err.status === 404) return null;
        throw e;
      }
    },
    [tradeId],
  );
  const primary = usePanelData<TradeDetail | null>(primaryFetcher, [tradeId]);

  // Fallback fetch: /ledger. Only fires its effect work after a primary
  // null/404; usePanelData runs unconditionally though, so we guard the
  // "not found" UI path off both states. The fallback is a single page
  // of the most recent TRADE_LOOKUP_LIMIT trades.
  const fallbackFetcher = useCallback(
    (s: AbortSignal) =>
      getLedger({ limit: TRADE_LOOKUP_LIMIT }, { signal: s }),
    [],
  );
  const fallback = usePanelData<LedgerEntry[]>(fallbackFetcher, [tradeId]);

  const rendered = useMemo<RenderedTrade | null>(() => {
    if (!tradeId) return null;
    if (primary.state.status === 'ok' && primary.state.data !== null) {
      return { kind: 'full', data: primary.state.data };
    }
    // Primary either returned null (404) or is still loading; check fallback.
    const ledgerRows =
      fallback.state.status === 'ok'
        ? fallback.state.data
        : fallback.state.data ?? [];
    const hit = ledgerRows.find((r) => r.id === tradeId);
    return hit ? { kind: 'partial', data: hit } : null;
  }, [primary.state, fallback.state, tradeId]);

  // Combined loading: still loading if neither primary nor fallback have
  // resolved AND we haven't found anything yet.
  const isLoading =
    rendered === null &&
    (primary.state.status === 'loading' || fallback.state.status === 'loading');

  // Combined error: only show the red banner if BOTH paths errored. A
  // primary error with a successful fallback means the fallback view
  // takes precedence.
  const hardError =
    rendered === null &&
    primary.state.status === 'err' &&
    fallback.state.status === 'err'
      ? (primary.state.error as OmegaApiError)
      : null;

  // Combined not-found: both paths resolved with no hit.
  const isNotFound =
    rendered === null &&
    !isLoading &&
    !hardError &&
    primary.state.status !== 'loading' &&
    fallback.state.status !== 'loading';

  const refetchAll = useCallback(() => {
    primary.refetch();
    fallback.refetch();
  }, [primary, fallback]);

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
            Per-trade timeline.
            {rendered?.kind === 'full' && (
              <>
                {' '}&middot;{' '}
                <span className="text-emerald-400">live engine detail</span>
              </>
            )}
            {rendered?.kind === 'partial' && (
              <>
                {' '}&middot;{' '}
                <span className="text-amber-300">ledger fallback</span>{' '}
                (older than engine in-memory window)
              </>
            )}
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

      {tradeId && isLoading && <LoadingCard />}

      {tradeId && hardError !== null && (
        <ErrorCard message={hardError.message} onRetry={refetchAll} />
      )}

      {tradeId && isNotFound && (
        <NotFoundCard tradeId={tradeId} onRetry={refetchAll} />
      )}

      {rendered !== null && (
        <TradeBody rendered={rendered} onNavigate={onNavigate} />
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
      <p className="font-mono text-sm text-amber-400">No trade id supplied.</p>
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
        Loading trade detail…
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
        Trade <span className="text-amber-300">{tradeId}</span> not found.
      </p>
      <p className="mt-2 font-mono text-xs text-amber-500/70">
        Engine returned 404 and the trade is not in the recent{' '}
        {TRADE_LOOKUP_LIMIT}-row ledger window. The id may be wrong, or the
        trade may be older than the engine's retention.
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

function TradeBody({
  rendered,
  onNavigate,
}: {
  rendered: RenderedTrade;
  onNavigate?: (target: string) => void;
}) {
  // Coerce both shapes into a single rendering view-model with explicit
  // "missing" markers for fallback rows.
  const vm = toViewModel(rendered);
  const durationMs = Math.max(0, vm.exit_ts - vm.entry_ts);
  const durationLabel = formatDuration(durationMs);
  const moveAbs = vm.exit - vm.entry;
  const movePct = vm.entry !== 0 ? (moveAbs / vm.entry) * 100 : 0;
  const dirSign = vm.side === 'LONG' ? 1 : -1;

  return (
    <div className="flex flex-col gap-4">
      {/* Top summary cards */}
      <div className="grid grid-cols-4 gap-4">
        <DetailCard label="Engine">
          <button
            type="button"
            onClick={() =>
              onNavigate ? onNavigate(`LDG ${vm.engine}`) : undefined
            }
            className="font-mono text-base font-bold text-amber-300 hover:text-amber-200 hover:underline"
            disabled={!onNavigate}
          >
            {vm.engine}
          </button>
        </DetailCard>
        <DetailCard label="Symbol">
          <span className="font-mono text-base text-amber-300">{vm.symbol}</span>
        </DetailCard>
        <DetailCard label="Side">
          <span
            className={`font-mono text-base ${vm.side === 'LONG' ? 'up' : 'down'}`}
          >
            {vm.side}
            {vm.shadow !== null && vm.shadow && (
              <span className="ml-2 font-mono text-[10px] uppercase tracking-widest text-amber-500/80">
                shadow
              </span>
            )}
          </span>
        </DetailCard>
        <DetailCard label="Realized P&L">
          <span className={`font-mono text-base ${pnlClass(vm.pnl)}`}>
            {`${vm.pnl >= 0 ? '+' : ''}$${formatNum(vm.pnl, 2)}`}
          </span>
          {vm.gross_pnl !== null && vm.net_pnl !== null && (
            <div className="mt-0.5 font-mono text-[10px] uppercase tracking-widest text-amber-500/70">
              gross {formatNum(vm.gross_pnl, 2)} / net {formatNum(vm.net_pnl, 2)}
            </div>
          )}
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
            <TimelineRow label="Entry" ts={vm.entry_ts} price={vm.entry} />
            <TimelineRow label="Exit" ts={vm.exit_ts} price={vm.exit} />
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">Duration</td>
              <td className="px-3 py-2 font-mono text-amber-300">
                {durationLabel}
              </td>
              <td className="px-3 py-2 text-right text-amber-500/70">Move</td>
              <td className="px-3 py-2 text-right font-mono text-amber-300">
                {`${moveAbs >= 0 ? '+' : ''}${formatNum(moveAbs, 4)}`}{' '}
                <span className={`ml-2 ${pnlClass(dirSign * moveAbs)}`}>
                  ({moveAbs >= 0 ? '+' : ''}
                  {movePct.toFixed(2)}%)
                </span>
              </td>
            </tr>
            <tr className="border-t border-amber-900/40">
              <td className="px-3 py-2 text-amber-500/70">Exit reason</td>
              <td colSpan={3} className="px-3 py-2 font-mono text-amber-300">
                {vm.reason || '—'}
              </td>
            </tr>
            <ExcursionRow label="MFE" value={vm.mfe} />
            <ExcursionRow label="MAE" value={vm.mae} />
            <DeferredOrValueRow
              label="Regime"
              value={vm.regime}
              fallbackNote="not stored on this trade"
            />
            <DeferredOrValueRow
              label="L2 imbalance"
              value={
                vm.l2_imbalance === null
                  ? null
                  : `${formatNum(vm.l2_imbalance, 3)}${
                      vm.l2_live === true
                        ? ' (live)'
                        : vm.l2_live === false
                          ? ' (synthetic)'
                          : ''
                    }`
              }
              fallbackNote="ledger fallback — not surfaced"
            />
            <DeferredOrValueRow
              label="ATR at entry"
              value={
                vm.atr_at_entry === null || vm.atr_at_entry === 0
                  ? null
                  : formatNum(vm.atr_at_entry, 4)
              }
              fallbackNote={
                vm.atr_at_entry === 0
                  ? 'engine did not populate ATR for this trade'
                  : 'ledger fallback — not surfaced'
              }
            />
          </tbody>
        </table>
      </div>

      {/* Costs (only meaningful from the full path; the ledger row trims them) */}
      {(vm.slippage_entry !== null ||
        vm.slippage_exit !== null ||
        vm.commission !== null) && (
        <div className="border border-amber-800/50 bg-black/40">
          <div className="border-b border-amber-800/40 px-4 py-2">
            <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
              Costs
            </h3>
          </div>
          <table className="w-full border-collapse font-mono text-xs">
            <tbody>
              <tr className="border-t border-amber-900/40">
                <td className="px-3 py-2 text-amber-500/70">Slippage entry</td>
                <td className="px-3 py-2 font-mono text-amber-300">
                  {vm.slippage_entry === null
                    ? '—'
                    : `$${formatNum(vm.slippage_entry, 2)}`}
                </td>
                <td className="px-3 py-2 text-right text-amber-500/70">
                  Slippage exit
                </td>
                <td className="px-3 py-2 text-right font-mono text-amber-300">
                  {vm.slippage_exit === null
                    ? '—'
                    : `$${formatNum(vm.slippage_exit, 2)}`}
                </td>
              </tr>
              <tr className="border-t border-amber-900/40">
                <td className="px-3 py-2 text-amber-500/70">Commission</td>
                <td className="px-3 py-2 font-mono text-amber-300">
                  {vm.commission === null
                    ? '—'
                    : `$${formatNum(vm.commission, 2)}`}
                </td>
                <td className="px-3 py-2 text-right text-amber-500/70">
                  Latency
                </td>
                <td className="px-3 py-2 text-right font-mono text-amber-300">
                  {vm.latency_ms === null
                    ? '—'
                    : `${formatNum(vm.latency_ms, 1)} ms`}
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      )}

      {/* Raw payload for debugging / copy-paste */}
      <details className="border border-amber-800/50 bg-black/40">
        <summary className="cursor-pointer select-none border-b border-amber-800/40 px-4 py-2 font-mono text-xs uppercase tracking-[0.3em] text-amber-500 hover:text-amber-300">
          Raw payload (JSON)
        </summary>
        <pre className="overflow-x-auto px-4 py-3 font-mono text-[11px] leading-5 text-amber-400">
{JSON.stringify(rendered.data, null, 2)}
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

function ExcursionRow({
  label,
  value,
}: {
  label: string;
  value: number | null;
}) {
  if (value === null) {
    return (
      <tr className="border-t border-amber-900/40">
        <td className="px-3 py-2 text-amber-500/70">{label}</td>
        <td colSpan={3} className="px-3 py-2 font-mono text-amber-500/60">
          ledger fallback — not surfaced
        </td>
      </tr>
    );
  }
  return (
    <tr className="border-t border-amber-900/40">
      <td className="px-3 py-2 text-amber-500/70">{label}</td>
      <td colSpan={3} className="px-3 py-2 font-mono text-amber-300">
        {formatNum(value, 4)}
      </td>
    </tr>
  );
}

function DeferredOrValueRow({
  label,
  value,
  fallbackNote,
}: {
  label: string;
  value: string | null;
  fallbackNote: string;
}) {
  return (
    <tr className="border-t border-amber-900/40">
      <td className="px-3 py-2 text-amber-500/70">{label}</td>
      <td colSpan={3} className="px-3 py-2 font-mono">
        {value === null || value.length === 0 ? (
          <span className="text-amber-500/60">{fallbackNote}</span>
        ) : (
          <span className="text-amber-300">{value}</span>
        )}
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
/*  View-model coercion + formatters                                        */
/* ──────────────────────────────────────────────────────────────────────── */

interface TradeViewModel {
  engine: string;
  symbol: string;
  side: 'LONG' | 'SHORT';
  entry_ts: number;
  exit_ts: number;
  entry: number;
  exit: number;
  pnl: number;
  reason: string;
  // Nullable fields are present on the full path, missing on fallback.
  gross_pnl: number | null;
  net_pnl: number | null;
  mfe: number | null;
  mae: number | null;
  regime: string | null;
  l2_imbalance: number | null;
  l2_live: boolean | null;
  atr_at_entry: number | null;
  slippage_entry: number | null;
  slippage_exit: number | null;
  commission: number | null;
  latency_ms: number | null;
  shadow: boolean | null;
}

function toViewModel(r: RenderedTrade): TradeViewModel {
  if (r.kind === 'full') {
    const d = r.data;
    return {
      engine: d.engine,
      symbol: d.symbol,
      side: d.side,
      entry_ts: d.entry_ts,
      exit_ts: d.exit_ts,
      entry: d.entry,
      exit: d.exit,
      pnl: d.pnl,
      reason: d.reason,
      gross_pnl: d.gross_pnl,
      net_pnl: d.net_pnl,
      mfe: d.mfe,
      mae: d.mae,
      regime: d.regime,
      l2_imbalance: d.l2_imbalance,
      l2_live: d.l2_live,
      atr_at_entry: d.atr_at_entry,
      slippage_entry: d.slippage_entry,
      slippage_exit: d.slippage_exit,
      commission: d.commission,
      latency_ms: d.latency_ms,
      shadow: d.shadow,
    };
  }
  // Fallback path: LedgerEntry. The engine trims most fields, so we
  // surface them as nulls and let the row renderers print "ledger
  // fallback — not surfaced" placeholders. This matches the pre-Step-7
  // behaviour for these specific rows so the UI is no worse than the
  // Step-4 baseline when the fallback fires.
  const d = r.data;
  return {
    engine: d.engine,
    symbol: d.symbol,
    side: d.side,
    entry_ts: d.entry_ts,
    exit_ts: d.exit_ts,
    entry: d.entry,
    exit: d.exit,
    pnl: d.pnl,
    reason: d.reason,
    gross_pnl: null,
    net_pnl: null,
    mfe: null,
    mae: null,
    regime: null,
    l2_imbalance: null,
    l2_live: null,
    atr_at_entry: null,
    slippage_entry: null,
    slippage_exit: null,
    commission: null,
    latency_ms: null,
    shadow: null,
  };
}

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
  const mm = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd = String(d.getUTCDate()).padStart(2, '0');
  const HH = String(d.getUTCHours()).padStart(2, '0');
  const MM = String(d.getUTCMinutes()).padStart(2, '0');
  const SS = String(d.getUTCSeconds()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${HH}:${MM}:${SS}Z`;
}

function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '—';
  const sec = Math.floor(ms / 1000);
  const days = Math.floor(sec / 86400);
  const hrs = Math.floor((sec % 86400) / 3600);
  const mins = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (days > 0) return `${days}d ${hrs}h ${mins}m`;
  if (hrs > 0) return `${hrs}h ${mins}m`;
  if (mins > 0) return `${mins}m ${s}s`;
  return `${s}s`;
}

function pnlClass(pnl: number): string {
  if (pnl > 0) return 'up';
  if (pnl < 0) return 'down';
  return 'text-amber-400';
}
