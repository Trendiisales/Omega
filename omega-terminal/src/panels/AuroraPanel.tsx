// AuroraPanel — Aurora order-flow liquidity heatshelves.
//
// Renders the footprint liquidity map served by /api/v1/omega/aurora
// (written by ibkr/aurora_snapshot.py from the recorded MGC/NQ tape). One
// card per instrument: spot price, session bias, POC, then the ranked
// SUPPLY shelves (above price) and DEMAND shelves (below) as a ladder.
//
// Args: `AUR [SYM]` — optional symbol filter (MGC | NQ). No arg = all.
//
// Colours mirror the Pine indicator: demand/absorption = emerald,
// supply/absorption = rose, initiative = amber/sky. Footprint volume is
// the REAL CME futures tape (spot gold/indices have none).

import { useMemo } from 'react';
import { getAurora } from '@/api/omega';
import type { AuroraAll, AuroraSnap, AuroraLevel } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const AUR_POLL_MS = 5000; // snapshotter rewrites every ~minute; 5s UI poll is plenty

function fmt(n: number | null | undefined, dp = 2): string {
  return n === null || n === undefined || Number.isNaN(n) ? '—' : n.toFixed(dp);
}

function levelColor(l: AuroraLevel): string {
  if (l.kind === 'absorption') return l.side === 'demand' ? 'text-emerald-400' : 'text-rose-400';
  return l.side === 'demand' ? 'text-sky-400' : 'text-amber-400';
}

function ShelfRow({ l }: { l: AuroraLevel }) {
  const dsign = l.delta >= 0 ? '+' : '−';
  return (
    <div className="flex items-center justify-between gap-2 py-0.5 font-mono text-xs">
      <span className={`w-20 ${levelColor(l)}`}>{fmt(l.mid)}</span>
      <span className="w-14 text-neutral-400">{fmt(l.dist_atr, 2)}A</span>
      <span className="w-16 text-neutral-500">{l.kind === 'absorption' ? 'ABS' : 'INIT'}</span>
      <span className="w-16 text-neutral-400">v{fmt(l.vol, 0)}</span>
      <span className="w-16 text-neutral-400">Δ{dsign}{fmt(Math.abs(l.delta), 0)}</span>
      <span className="w-10 text-neutral-500">{l.touches > 0 ? `×${l.touches}` : ''}</span>
      <span className="w-12 text-fuchsia-400">{l.flipped ? '⟲FLIP' : ''}</span>
    </div>
  );
}

function SymbolCard({ snap }: { snap: AuroraSnap }) {
  const meta = snap._meta ?? {};
  const bias = snap.bias === 'buyers' ? 'text-emerald-400' : 'text-rose-400';
  const sup = [...(snap.key_supply ?? [])].sort((a, b) => a.mid - b.mid); // nearest above first downward
  const dem = [...(snap.key_demand ?? [])].sort((a, b) => b.mid - a.mid);

  return (
    <div className="rounded border border-neutral-700 bg-neutral-900/60 p-3">
      <div className="mb-2 flex items-baseline justify-between">
        <div className="flex items-baseline gap-3">
          <span className="text-lg font-semibold text-neutral-100">{snap.sym}</span>
          <span className="font-mono text-sm text-neutral-300">{fmt(snap.price)}</span>
          <span className={`text-xs ${bias}`}>{snap.bias === 'buyers' ? '▲ BUYERS' : '▼ SELLERS'}</span>
        </div>
        <div className="text-right text-[11px] text-neutral-500">
          <div>POC {fmt(snap.poc)} · ATR {fmt(snap.atr)}</div>
          <div>
            {meta.trades ?? '?'} trades · {snap.n_shelves ?? 0} shelves · {meta.delta_via ?? '?'}
          </div>
        </div>
      </div>

      {snap.error ? (
        <div className="rounded bg-rose-950/40 px-2 py-1 text-xs text-rose-300">{snap.error}</div>
      ) : (
        <>
          <div className="mb-1 text-[10px] uppercase tracking-wide text-rose-400/80">Supply ↑</div>
          {sup.length === 0 ? (
            <div className="py-0.5 text-xs text-neutral-600">— none in range —</div>
          ) : (
            sup.map((l, i) => <ShelfRow key={`s${i}`} l={l} />)
          )}

          <div className="my-1 border-t border-dashed border-neutral-700/70" />

          <div className="mb-1 text-[10px] uppercase tracking-wide text-emerald-400/80">Demand ↓</div>
          {dem.length === 0 ? (
            <div className="py-0.5 text-xs text-neutral-600">— none in range —</div>
          ) : (
            dem.map((l, i) => <ShelfRow key={`d${i}`} l={l} />)
          )}
        </>
      )}
    </div>
  );
}

export function AuroraPanel({ args }: Props) {
  const filter = (args[0] ?? '').toUpperCase();

  const { state, refetch } = usePanelData<AuroraAll>(
    (s: AbortSignal) => getAurora({ signal: s }),
    [],
    { pollMs: AUR_POLL_MS },
  );

  const data = state.status === 'ok' ? state.data : (state.data ?? null);

  const snaps = useMemo(() => {
    if (!data?.snaps) return [];
    const syms = data.symbols?.length ? data.symbols : Object.keys(data.snaps);
    return syms
      .filter((s) => !filter || s === filter)
      .map((s) => data.snaps[s])
      .filter((s): s is AuroraSnap => Boolean(s));
  }, [data, filter]);

  return (
    <div className="flex h-full flex-col gap-2 p-3 text-neutral-200">
      <div className="flex items-center justify-between">
        <div>
          <span className="text-base font-semibold">AURORA · Order-Flow Liquidity</span>
          <span className="ml-2 text-xs text-neutral-500">MGC gold · NQ nasdaq (real CME tape)</span>
        </div>
        <div className="flex items-center gap-3 text-xs text-neutral-500">
          {data?.stale && <span className="text-amber-400">⚠ waiting for tape</span>}
          {state.status === 'err' && <span className="text-rose-400">fetch error</span>}
          <button onClick={() => refetch()} className="rounded border border-neutral-700 px-2 py-0.5 hover:bg-neutral-800">
            refresh
          </button>
        </div>
      </div>

      {state.status === 'loading' && !data && (
        <div className="text-sm text-neutral-500">loading liquidity map…</div>
      )}

      {data?.note && snaps.length === 0 && (
        <div className="rounded border border-neutral-700 bg-neutral-900/60 p-3 text-sm text-neutral-400">
          {data.note}
        </div>
      )}

      <div className="grid grid-cols-1 gap-3 overflow-auto md:grid-cols-2">
        {snaps.map((s) => (
          <SymbolCard key={s.sym} snap={s} />
        ))}
      </div>
    </div>
  );
}
