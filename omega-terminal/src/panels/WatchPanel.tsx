// WatchPanel — nightly INTEL-screener hits.
//
// Step 6 deliverable. Renders the engine's WATCH registry snapshot
// returned by /api/v1/omega/watch. The actual scan runs on the
// engine-side WatchScheduler (cron-style nightly job) over the chosen
// universe (S&P 500 / NDX / ALL); this panel polls the registry every
// 60 s and surfaces last-run / next-run / scanning status alongside the
// flagged hits.
//
// Args: `WATCH <universe>`. Universe options: SP500 (default), NDX, ALL.

import { useCallback, useMemo, useState } from 'react';
import { getWatch } from '@/api/omega';
import type { WatchEnvelope, WatchHit } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  PanelHeader,
  SkeletonRows,
  SortHeader,
  SummaryCard,
  WarningsBanner,
  detectMock,
  dirClass,
  formatNum,
  formatUtc,
  formatVolume,
  formatPct,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const WATCH_POLL_MS = 60000;

const UNIVERSES = ['SP500', 'NDX', 'ALL'] as const;
type Universe = typeof UNIVERSES[number];

type SortKey = 'symbol' | 'signal' | 'score' | 'last_price' | 'change_percent' | 'volume' | 'flagged_at';

function parseUniverse(s: string | undefined): Universe {
  const u = (s ?? 'SP500').toUpperCase() as Universe;
  return (UNIVERSES as readonly string[]).includes(u) ? u : 'SP500';
}

export function WatchPanel({ args, onNavigate }: Props) {
  const initialUniverse = parseUniverse(args[0]);
  const [universe, setUniverse] = useState<Universe>(initialUniverse);
  const [sortKey, setSortKey] = useState<SortKey>('score');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getWatch({ universe }, { signal: s }),
    [universe],
  );
  const { state, refetch } = usePanelData<WatchEnvelope>(
    fetcher,
    [universe],
    { pollMs: WATCH_POLL_MS },
  );

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'engine';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const hits = env?.hits ?? [];
  const sorted = useMemo(() => sortRows(hits, sortKey, sortDir), [hits, sortKey, sortDir]);

  const lastRun = env?.last_run_ts ?? 0;
  const nextRun = env?.next_run_ts ?? 0;
  const scanning = env?.scanning ?? false;

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir(k === 'symbol' || k === 'signal' ? 'asc' : 'desc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Watch List"
    >
      <PanelHeader
        code="WATCH"
        title="Watch List"
        subtitle={
          <>
            Nightly INTEL screener &middot; universe{' '}
            <span className="text-amber-300">{universe}</span> &middot; poll 60s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="flex items-center gap-2">
        {UNIVERSES.map((u) => (
          <button
            key={u}
            type="button"
            onClick={() => setUniverse(u)}
            className={
              'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
              (universe === u
                ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
            }
          >
            {u}
          </button>
        ))}
      </div>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Hits" value={String(hits.length)} size="lg" />
        <SummaryCard
          label="Status"
          value={scanning ? 'SCANNING' : 'IDLE'}
          accent={scanning ? 'text-amber-300' : 'text-amber-400'}
          size="lg"
        />
        <SummaryCard label="Last run" value={lastRun > 0 ? formatUtc(lastRun) : '—'} />
        <SummaryCard label="Next run" value={nextRun > 0 ? formatUtc(nextRun) : '—'} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Flagged
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'hit' : 'hits'}
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="symbol"         label="Symbol"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="signal"         label="Signal"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="score"          label="Score"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="last_price"     label="Last"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="change_percent" label="Δ%"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="volume"         label="Volume"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="flagged_at"     label="Flagged" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <th className="px-3 py-2 text-left uppercase text-amber-500">Rationale</th>
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={8} rows={6} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={8} className="px-3 py-6 text-center text-amber-600">
                  {scanning ? 'Scan in progress…' : 'Nothing flagged this run.'}
                </td>
              </tr>
            )}
            {sorted.map((h, i) => {
              const flagged = h.flagged_at ? Date.parse(h.flagged_at) : 0;
              return (
                <tr
                  key={`${h.symbol}-${i}`}
                  className="cursor-pointer border-b border-amber-900/40 hover:bg-amber-950/30"
                  onClick={() => onNavigate?.(`DES ${h.symbol}`)}
                  title={`Click for DES ${h.symbol}`}
                >
                  <td className="px-3 py-2 font-bold text-amber-300">{h.symbol}</td>
                  <td className="px-3 py-2 text-amber-400">{h.signal ?? '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{h.score != null ? formatNum(h.score, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{h.last_price != null ? formatNum(h.last_price, 2) : '—'}</td>
                  <td className={`px-3 py-2 text-right ${dirClass(h.change_percent ?? 0)}`}>
                    {h.change_percent != null ? formatPct(h.change_percent) : '—'}
                  </td>
                  <td className="px-3 py-2 text-right text-amber-400">{h.volume != null ? formatVolume(h.volume) : '—'}</td>
                  <td className="px-3 py-2 text-amber-400">{flagged > 0 ? formatUtc(flagged) : '—'}</td>
                  <td className="px-3 py-2 text-amber-400">{h.rationale ?? '—'}</td>
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

function sortRows(rows: WatchHit[], key: SortKey, dir: SortDir): WatchHit[] {
  return [...rows].sort((a, b) => {
    const sign = dir === 'asc' ? 1 : -1;
    let av: unknown;
    let bv: unknown;
    if (key === 'flagged_at') {
      av = a.flagged_at ? Date.parse(a.flagged_at) : 0;
      bv = b.flagged_at ? Date.parse(b.flagged_at) : 0;
    } else {
      av = a[key];
      bv = b[key];
    }
    if (typeof av === 'number' && typeof bv === 'number') return (av - bv) * sign;
    if (av == null && bv == null) return 0;
    if (av == null) return 1 * sign;
    if (bv == null) return -1 * sign;
    return String(av).localeCompare(String(bv)) * sign;
  });
}
