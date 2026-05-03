// DvdPanel — Dividends.
//
// Step 6 deliverable. Sortable table over OpenBB
// `/equity/fundamental/dividends` via `/api/v1/omega/dvd`. Args: `DVD
// <symbol>`. Polling cadence: 5 minutes (engine-side cache TTL 250 s) —
// dividend data is announced quarterly.

import { useCallback, useMemo, useState } from 'react';
import { getDvd } from '@/api/omega';
import type { Dividend, OpenBbEnvelope } from '@/api/types';
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
  formatDate,
  formatNum,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const DVD_POLL_MS = 5 * 60 * 1000;

type SortKey = 'ex_dividend_date' | 'amount' | 'record_date' | 'payment_date';

function pickExDate(d: Dividend): string | undefined {
  return d.ex_dividend_date ?? d.date;
}

export function DvdPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const [sortKey, setSortKey] = useState<SortKey>('ex_dividend_date');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getDvd({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<Dividend>>(
    fetcher,
    [symbol],
    { pollMs: symbol ? DVD_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="DVD" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const rows = env?.results ?? [];
  const sorted = useMemo(() => sortRows(rows, sortKey, sortDir), [rows, sortKey, sortDir]);

  const lastDate = sorted[0] ? pickExDate(sorted[0]) : undefined;
  const lastAmount = sorted[0]?.amount;
  const ttm = computeTtm(rows);
  const count = rows.length;

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir('desc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Dividends"
    >
      <PanelHeader
        code="DVD"
        title="Dividends"
        subtitle={
          <>
            OpenBB dividends &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 5m
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Records" value={String(count)} />
        <SummaryCard label="Last ex-date" value={lastDate ? formatDate(lastDate) : '—'} />
        <SummaryCard
          label="Last amount"
          value={lastAmount != null ? `$${formatNum(lastAmount, 4)}` : '—'}
        />
        <SummaryCard label="TTM total" value={ttm > 0 ? `$${formatNum(ttm, 4)}` : '—'} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            History
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'row' : 'rows'}
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="ex_dividend_date" label="Ex-date"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="amount"           label="Amount"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="record_date"      label="Record"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="payment_date"     label="Pay"         sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <th className="px-3 py-2 text-left uppercase text-amber-500">Declared</th>
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={5} rows={5} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={5} className="px-3 py-6 text-center text-amber-600">
                  No dividend history for "{symbol}".
                </td>
              </tr>
            )}
            {sorted.map((d, i) => (
              <tr key={`${pickExDate(d) ?? 'x'}-${i}`} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{formatDate(pickExDate(d))}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {d.amount != null ? `$${formatNum(d.amount, 4)}` : '—'}
                </td>
                <td className="px-3 py-2 text-amber-400">{formatDate(d.record_date)}</td>
                <td className="px-3 py-2 text-amber-400">{formatDate(d.payment_date)}</td>
                <td className="px-3 py-2 text-amber-400">{formatDate(d.declaration_date)}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

function sortRows(rows: Dividend[], key: SortKey, dir: SortDir): Dividend[] {
  return [...rows].sort((a, b) => {
    const sign = dir === 'asc' ? 1 : -1;
    let av: unknown;
    let bv: unknown;
    if (key === 'ex_dividend_date') {
      av = pickExDate(a);
      bv = pickExDate(b);
    } else {
      av = (a as unknown as Record<string, unknown>)[key];
      bv = (b as unknown as Record<string, unknown>)[key];
    }
    if (typeof av === 'number' && typeof bv === 'number') return (av - bv) * sign;
    if (av == null && bv == null) return 0;
    if (av == null) return 1 * sign;
    if (bv == null) return -1 * sign;
    return String(av).localeCompare(String(bv)) * sign;
  });
}

function computeTtm(rows: Dividend[]): number {
  const cutoff = Date.now() - 365 * 24 * 60 * 60 * 1000;
  return rows.reduce((acc, d) => {
    const dateStr = pickExDate(d);
    if (!dateStr) return acc;
    const t = Date.parse(dateStr);
    if (!Number.isFinite(t) || t < cutoff) return acc;
    return acc + (d.amount ?? 0);
  }, 0);
}
