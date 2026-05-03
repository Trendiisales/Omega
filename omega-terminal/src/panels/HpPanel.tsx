// HpPanel — Historical Price (OHLCV table).
//
// Step 6 deliverable. Sortable OHLCV table over OpenBB
// `/equity/price/historical` via /api/v1/omega/hp. Shares the engine-
// side cache slot with /gp on identical (symbol, interval, start_date,
// end_date) — chart rendering is purely client-side, so the engine
// doesn't need a `chart=true` distinguisher.
//
// Args: `HP <symbol> [<interval>]`. Examples:
//   HP AAPL
//   HP AAPL 1h
//   HP AAPL 1W
//
// Polling cadence: 30 s. Engine cache TTL 25 s.

import { useCallback, useMemo, useState } from 'react';
import { getHp } from '@/api/omega';
import type { BarInterval, HistoricalBar, OpenBbEnvelope } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SkeletonRows,
  SortHeader,
  SummaryCard,
  WarningsBanner,
  detectMock,
  dirClass,
  formatDate,
  formatNum,
  formatVolume,
  formatPct,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const HP_POLL_MS = 30000;
const INTERVAL_OPTIONS: BarInterval[] = ['1d', '1h', '15m', '1m', '1W', '1M'];

type SortKey = 'date' | 'open' | 'high' | 'low' | 'close' | 'volume';

function parseInterval(s: string | undefined): BarInterval {
  if (!s) return '1d';
  const v = s.toLowerCase() as BarInterval;
  return (INTERVAL_OPTIONS as string[]).includes(v) ? (v as BarInterval) : '1d';
}

export function HpPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const initialInterval = parseInterval(args[1]);
  const [interval, setInterval] = useState<BarInterval>(initialInterval);
  const [sortKey, setSortKey] = useState<SortKey>('date');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getHp({ symbol, interval }, { signal: s }),
    [symbol, interval],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<HistoricalBar>>(
    fetcher,
    [symbol, interval],
    { pollMs: symbol ? HP_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="HP" exampleArgs="AAPL 1d" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const bars = env?.results ?? [];
  const sorted = useMemo(() => sortRows(bars, sortKey, sortDir), [bars, sortKey, sortDir]);

  const last = sorted[0];
  const first = sorted[sorted.length - 1];
  const lastClose = last?.close;
  const periodReturn =
    first?.close != null && last?.close != null && first.close !== 0
      ? ((last.close - first.close) / first.close) * 100
      : 0;
  const totalVol = bars.reduce((acc, b) => acc + (b.volume ?? 0), 0);

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir(k === 'date' ? 'desc' : 'desc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Historical Price"
    >
      <PanelHeader
        code="HP"
        title="Historical Price"
        subtitle={
          <>
            OHLCV via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; interval{' '}
            <span className="text-amber-300">{interval}</span> &middot; poll 30s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="flex items-center gap-2">
        {INTERVAL_OPTIONS.map((iv) => (
          <button
            key={iv}
            type="button"
            onClick={() => setInterval(iv)}
            className={
              'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
              (interval === iv
                ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
            }
          >
            {iv}
          </button>
        ))}
      </div>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Bars" value={String(bars.length)} />
        <SummaryCard label="Last close" value={lastClose != null ? formatNum(lastClose, 2) : '—'} />
        <SummaryCard
          label="Period return"
          value={bars.length > 1 ? formatPct(periodReturn) : '—'}
          accent={periodReturn >= 0 ? 'up' : 'down'}
        />
        <SummaryCard label="Total Vol" value={totalVol > 0 ? formatVolume(totalVol) : '—'} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            OHLCV
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'bar' : 'bars'} &middot; poll 30s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="date"   label="Date"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="open"   label="Open"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="high"   label="High"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="low"    label="Low"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="close"  label="Close"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="volume" label="Volume" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <th className="px-3 py-2 text-right uppercase text-amber-500">Δ%</th>
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={7} rows={8} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={7} className="px-3 py-6 text-center text-amber-600">
                  No bars returned for "{symbol}" at interval "{interval}".
                </td>
              </tr>
            )}
            {sorted.map((b, i) => {
              const intraDayPct =
                b.open != null && b.close != null && b.open !== 0
                  ? ((b.close - b.open) / b.open) * 100
                  : 0;
              return (
                <tr key={`${b.date}-${i}`} className="border-b border-amber-900/40">
                  <td className="px-3 py-2 font-bold text-amber-300">{formatDate(b.date)}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{b.open != null ? formatNum(b.open, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{b.high != null ? formatNum(b.high, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{b.low != null ? formatNum(b.low, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{b.close != null ? formatNum(b.close, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{b.volume != null ? formatVolume(b.volume) : '—'}</td>
                  <td className={`px-3 py-2 text-right ${dirClass(intraDayPct)}`}>
                    {b.open != null && b.close != null ? formatPct(intraDayPct) : '—'}
                  </td>
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

function sortRows(rows: HistoricalBar[], key: SortKey, dir: SortDir): HistoricalBar[] {
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
