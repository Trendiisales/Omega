// EePanel — Earnings Estimates.
//
// Step 6 deliverable. Multi-card consensus row + sortable surprise
// table from the merged EeEnvelope returned by /api/v1/omega/ee. The
// engine fans two OpenBB calls
// (`/equity/estimates/{consensus,surprise}`) into one envelope.
//
// Args: `EE <symbol>`. Polling cadence: 5 min, engine cache TTL 250 s.

import { useCallback, useMemo, useState } from 'react';
import { getEe } from '@/api/omega';
import type { EeEnvelope, EpsConsensus, EpsSurprise } from '@/api/types';
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
  formatLargeNum,
  formatNum,
  formatPct,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const EE_POLL_MS = 5 * 60 * 1000;

type SortKey = 'date' | 'eps_actual' | 'eps_estimate' | 'eps_surprise' | 'surprise_percent';

export function EePanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('date');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getEe({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<EeEnvelope>(
    fetcher,
    [symbol],
    { pollMs: symbol ? EE_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="EE" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const consensus: EpsConsensus | undefined = env?.consensus?.[0];
  const surprises = env?.surprise ?? [];

  const sortedSurprises = useMemo(
    () => sortSurprises(surprises, sortKey, sortDir),
    [surprises, sortKey, sortDir],
  );

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
      aria-label="Earnings Estimates"
    >
      <PanelHeader
        code="EE"
        title="Earnings Estimates"
        subtitle={
          <>
            Consensus &amp; surprise via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 5m
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <WarningsBanner warnings={warnings} />

      <div>
        <h3 className="mb-2 font-mono text-[10px] uppercase tracking-[0.3em] text-amber-500">
          Consensus &middot; {consensus?.fiscal_period ?? '—'}{' '}
          {consensus?.fiscal_year ?? ''}
        </h3>
        <div className="grid grid-cols-4 gap-4">
          <SummaryCard label="EPS avg"   value={consensus?.eps_avg != null ? formatNum(consensus.eps_avg, 2) : '—'} />
          <SummaryCard label="EPS high"  value={consensus?.eps_high != null ? formatNum(consensus.eps_high, 2) : '—'} accent="up" />
          <SummaryCard label="EPS low"   value={consensus?.eps_low != null ? formatNum(consensus.eps_low, 2) : '—'} accent="down" />
          <SummaryCard label="# analysts" value={consensus?.number_of_analysts != null ? String(consensus.number_of_analysts) : '—'} />
        </div>
        <div className="mt-4 grid grid-cols-3 gap-4">
          <SummaryCard label="Revenue avg"  value={consensus?.revenue_avg != null ? `$${formatLargeNum(consensus.revenue_avg)}` : '—'} />
          <SummaryCard label="Revenue high" value={consensus?.revenue_high != null ? `$${formatLargeNum(consensus.revenue_high)}` : '—'} accent="up" />
          <SummaryCard label="Revenue low"  value={consensus?.revenue_low != null ? `$${formatLargeNum(consensus.revenue_low)}` : '—'} accent="down" />
        </div>
      </div>

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Surprise history
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sortedSurprises.length} {sortedSurprises.length === 1 ? 'row' : 'rows'}
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="date"             label="Date"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <th className="px-3 py-2 text-left uppercase text-amber-500">Period</th>
              <SortHeader<SortKey> k="eps_actual"       label="Actual"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="eps_estimate"     label="Estimate" sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="eps_surprise"     label="Δ"        sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="surprise_percent" label="Δ%"       sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sortedSurprises.length === 0 && state.status === 'loading' && <SkeletonRows cols={6} rows={5} />}
            {sortedSurprises.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={6} className="px-3 py-6 text-center text-amber-600">
                  No surprise history for "{symbol}".
                </td>
              </tr>
            )}
            {sortedSurprises.map((s, i) => (
              <tr key={`${s.date ?? ''}-${i}`} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{formatDate(s.date)}</td>
                <td className="px-3 py-2 text-amber-400">
                  {s.fiscal_period ?? '—'} {s.fiscal_year ?? ''}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">{s.eps_actual != null ? formatNum(s.eps_actual, 2) : '—'}</td>
                <td className="px-3 py-2 text-right text-amber-400">{s.eps_estimate != null ? formatNum(s.eps_estimate, 2) : '—'}</td>
                <td className={`px-3 py-2 text-right ${dirClass(s.eps_surprise ?? 0)}`}>
                  {s.eps_surprise != null ? `${s.eps_surprise >= 0 ? '+' : ''}${formatNum(s.eps_surprise, 2)}` : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(s.surprise_percent ?? 0)}`}>
                  {s.surprise_percent != null ? formatPct(s.surprise_percent) : '—'}
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

function sortSurprises(rows: EpsSurprise[], key: SortKey, dir: SortDir): EpsSurprise[] {
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
