// FxcPanel — FX Cross rates.
//
// Step 6 deliverable. Sortable table over OpenBB `/currency/price/quote`
// via `/api/v1/omega/fxc`. The engine accepts either a literal pair or
// one of the region presets (MAJORS / EUR / ASIA / EM) and expands to a
// comma-separated symbol list before calling OpenBB.
//
// Args: `FXC <pair-or-region>`. Examples:
//   FXC EUR/USD
//   FXC EURUSD
//   FXC MAJORS
//   FXC EM
//
// Polling cadence: 5 s. Engine-side cache TTL 4 s.

import { useCallback, useMemo, useState } from 'react';
import { getFxc } from '@/api/omega';
import type { FxQuote, OpenBbEnvelope } from '@/api/types';
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
  formatVolume,
  formatPct,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const FXC_POLL_MS = 5000;

type SortKey =
  | 'symbol'
  | 'name'
  | 'last_price'
  | 'bid'
  | 'ask'
  | 'change'
  | 'change_percent'
  | 'volume';

export function FxcPanel({ args }: Props) {
  const pair = (args[0] ?? 'MAJORS').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('change_percent');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getFxc({ pair }, { signal: s }),
    [pair],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<FxQuote>>(
    fetcher,
    [pair],
    { pollMs: FXC_POLL_MS },
  );

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const rows = env?.results ?? [];
  const sorted = useMemo(() => sortRows(rows, sortKey, sortDir), [rows, sortKey, sortDir]);

  const upCount = rows.filter((r) => (r.change ?? 0) > 0).length;
  const downCount = rows.filter((r) => (r.change ?? 0) < 0).length;
  const avgPct =
    rows.length === 0
      ? 0
      : rows.reduce((acc, r) => acc + (r.change_percent ?? 0), 0) / rows.length;

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir(k === 'symbol' || k === 'name' ? 'asc' : 'desc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="FX Cross"
    >
      <PanelHeader
        code="FXC"
        title="FX Cross"
        subtitle={
          <>
            Currency cross rates via OpenBB &middot; pair{' '}
            <span className="text-amber-300">{pair}</span> &middot; poll 5s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Pairs" value={String(rows.length)} size="lg" />
        <SummaryCard
          label="Avg Δ%"
          value={rows.length === 0 ? '—' : formatPct(avgPct)}
          accent={avgPct >= 0 ? 'up' : 'down'}
          size="lg"
        />
        <SummaryCard label="Up" value={String(upCount)} accent="up" size="lg" />
        <SummaryCard label="Down" value={String(downCount)} accent="down" size="lg" />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Quotes
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'row' : 'rows'} &middot; poll 5s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="symbol"         label="Pair"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="name"           label="Name"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="last_price"     label="Last"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="bid"            label="Bid"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="ask"            label="Ask"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="change"         label="Δ"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="change_percent" label="Δ%"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="volume"         label="Volume" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={8} rows={5} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={8} className="px-3 py-6 text-center text-amber-600">
                  No quotes returned for "{pair}".
                </td>
              </tr>
            )}
            {sorted.map((r) => (
              <tr key={r.symbol} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{r.symbol}</td>
                <td className="px-3 py-2 text-amber-400">{r.name ?? '—'}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.last_price != null ? formatNum(r.last_price, 5) : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.bid != null ? formatNum(r.bid, 5) : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.ask != null ? formatNum(r.ask, 5) : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change ?? 0)}`}>
                  {r.change != null ? `${r.change >= 0 ? '+' : ''}${formatNum(r.change, 5)}` : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change_percent ?? 0)}`}>
                  {r.change_percent != null ? formatPct(r.change_percent) : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.volume != null ? formatVolume(r.volume) : '—'}
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

function sortRows(rows: FxQuote[], key: SortKey, dir: SortDir): FxQuote[] {
  return [...rows].sort((a, b) => {
    const sign = dir === 'asc' ? 1 : -1;
    const av = a[key];
    const bv = b[key];
    if (typeof av === 'number' && typeof bv === 'number') return (av - bv) * sign;
    if (av == null && bv == null) return 0;
    if (av == null) return 1 * sign;
    if (bv == null) return -1 * sign;
    return String(av).localeCompare(String(bv)) * sign;
  });
}
