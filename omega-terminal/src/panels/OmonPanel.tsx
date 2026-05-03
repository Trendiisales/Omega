// OmonPanel — Options Monitor.
//
// Step 6 deliverable. Sortable option-chain table over OpenBB
// `/derivatives/options/chains` via /api/v1/omega/omon. Per
// STEP6_OPENER, the chain summary header (ATM IV, term-structure
// snapshot, put/call ratio) is computed client-side from .results so
// the engine stays free of third-party JSON libs.
//
// Args: `OMON <symbol> [<expiry>]`. The optional second arg filters the
// chain to a single expiry (ISO date). Without it, the panel exposes a
// pill row of all expiries it sees in the response.
//
// Polling cadence: 5 s. Engine cache TTL 4 s.

import { useCallback, useMemo, useState } from 'react';
import { getOmon } from '@/api/omega';
import type { OpenBbEnvelope, OptionsRow } from '@/api/types';
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
  formatPct,
  formatVolume,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const OMON_POLL_MS = 5000;

type SortKey =
  | 'strike'
  | 'expiration'
  | 'option_type'
  | 'bid'
  | 'ask'
  | 'last_trade_price'
  | 'implied_volatility'
  | 'open_interest'
  | 'volume';

function rowExpiry(r: OptionsRow): string | undefined {
  return r.expiration ?? (r as Record<string, unknown>).expiry as string | undefined;
}

function rowType(r: OptionsRow): 'call' | 'put' | undefined {
  const t = (r.option_type ?? '').toLowerCase();
  if (t.startsWith('c')) return 'call';
  if (t.startsWith('p')) return 'put';
  return undefined;
}

export function OmonPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const initialExpiry = args[1] ?? '';
  const [expiry, setExpiry] = useState<string>(initialExpiry);
  const [typeFilter, setTypeFilter] = useState<'all' | 'call' | 'put'>('all');
  const [sortKey, setSortKey] = useState<SortKey>('strike');
  const [sortDir, setSortDir] = useState<SortDir>('asc');

  const fetcher = useCallback(
    (s: AbortSignal) => getOmon({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<OptionsRow>>(
    fetcher,
    [symbol],
    { pollMs: symbol ? OMON_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="OMON" exampleArgs="AAPL [2026-06-20]" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const allRows = env?.results ?? [];

  // Distinct expiries seen in the chain — sorted ascending.
  const expiries = useMemo(() => {
    const set = new Set<string>();
    for (const r of allRows) {
      const e = rowExpiry(r);
      if (e) set.add(e.slice(0, 10));
    }
    return Array.from(set).sort();
  }, [allRows]);

  // Active filter set: expiry first (engine can serve all expiries; we
  // filter client-side so the same envelope drives multiple expiry tabs
  // without an extra round trip), then type.
  const filtered = useMemo(() => {
    return allRows.filter((r) => {
      if (expiry) {
        const e = rowExpiry(r) ?? '';
        if (!e.startsWith(expiry)) return false;
      }
      if (typeFilter !== 'all' && rowType(r) !== typeFilter) return false;
      return true;
    });
  }, [allRows, expiry, typeFilter]);

  const sorted = useMemo(() => sortRows(filtered, sortKey, sortDir), [filtered, sortKey, sortDir]);

  // Chain summary — client-side per the opener.
  const calls = filtered.filter((r) => rowType(r) === 'call');
  const puts = filtered.filter((r) => rowType(r) === 'put');
  const callOi = calls.reduce((acc, r) => acc + (r.open_interest ?? 0), 0);
  const putOi = puts.reduce((acc, r) => acc + (r.open_interest ?? 0), 0);
  const pcRatio = callOi > 0 ? putOi / callOi : 0;

  // ATM IV: the IV at the strike with smallest |delta - 0.5| where delta
  // is available; otherwise we fall back to the median IV.
  const atmCandidate = filtered
    .filter((r) => r.implied_volatility != null && Number.isFinite(r.implied_volatility!))
    .map((r) => {
      const d = r.delta;
      const score = d != null ? Math.abs(Math.abs(d) - 0.5) : 1;
      return { iv: r.implied_volatility!, score };
    })
    .sort((a, b) => a.score - b.score)[0];

  function clickHeader(k: SortKey) {
    if (k === sortKey) setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    else {
      setSortKey(k);
      setSortDir(k === 'option_type' || k === 'expiration' ? 'asc' : 'asc');
    }
  }

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Options Monitor"
    >
      <PanelHeader
        code="OMON"
        title="Options Monitor"
        subtitle={
          <>
            Options chain via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span>
            {expiry && (
              <>
                {' '}&middot; expiry <span className="text-amber-300">{expiry}</span>
              </>
            )}{' '}
            &middot; poll 5s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Contracts" value={String(filtered.length)} />
        <SummaryCard
          label="ATM IV"
          value={atmCandidate ? formatPct(atmCandidate.iv * (Math.abs(atmCandidate.iv) < 5 ? 100 : 1)) : '—'}
        />
        <SummaryCard label="Call OI" value={callOi > 0 ? formatVolume(callOi) : '—'} accent="up" />
        <SummaryCard
          label="P/C ratio"
          value={pcRatio > 0 ? formatNum(pcRatio, 2) : '—'}
          accent={pcRatio > 1 ? 'down' : 'up'}
        />
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <span className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
          Type:
        </span>
        {(['all', 'call', 'put'] as const).map((t) => (
          <button
            key={t}
            type="button"
            onClick={() => setTypeFilter(t)}
            className={
              'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
              (typeFilter === t
                ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
            }
          >
            {t}
          </button>
        ))}
        {expiries.length > 0 && (
          <>
            <span className="ml-4 font-mono text-[10px] uppercase tracking-widest text-amber-600">
              Expiry:
            </span>
            <button
              type="button"
              onClick={() => setExpiry('')}
              className={
                'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
                (expiry === ''
                  ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                  : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
              }
            >
              ALL
            </button>
            {expiries.slice(0, 8).map((e) => (
              <button
                key={e}
                type="button"
                onClick={() => setExpiry(e)}
                className={
                  'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
                  (expiry === e
                    ? 'border-amber-300 bg-amber-900/40 text-amber-200'
                    : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
                }
              >
                {e}
              </button>
            ))}
            {expiries.length > 8 && (
              <span className="font-mono text-[10px] text-amber-700">
                +{expiries.length - 8} more
              </span>
            )}
          </>
        )}
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Chain
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {sorted.length} {sorted.length === 1 ? 'contract' : 'contracts'} &middot; poll 5s
          </span>
        </div>
        <table className="w-full border-collapse font-mono text-xs">
          <thead>
            <tr className="border-b border-amber-900/60 text-left text-amber-500">
              <SortHeader<SortKey> k="expiration"          label="Expiry"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="option_type"         label="Type"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="strike"              label="Strike"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="bid"                 label="Bid"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="ask"                 label="Ask"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="last_trade_price"    label="Last"    sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="implied_volatility"  label="IV"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="open_interest"       label="OI"      sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="volume"              label="Volume"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={9} rows={8} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={9} className="px-3 py-6 text-center text-amber-600">
                  No contracts in chain for "{symbol}".
                </td>
              </tr>
            )}
            {sorted.map((r, i) => {
              const t = rowType(r);
              const typeColor = t === 'call' ? 'up' : t === 'put' ? 'down' : 'text-amber-400';
              const ivPct =
                r.implied_volatility != null
                  ? Math.abs(r.implied_volatility) < 5
                    ? r.implied_volatility * 100
                    : r.implied_volatility
                  : undefined;
              return (
                <tr key={`${r.contract_symbol ?? i}`} className="border-b border-amber-900/40">
                  <td className="px-3 py-2 text-amber-400">{formatDate(rowExpiry(r))}</td>
                  <td className={`px-3 py-2 ${typeColor}`}>{t ?? '—'}</td>
                  <td className="px-3 py-2 text-right font-bold text-amber-300">
                    {r.strike != null ? formatNum(r.strike, 2) : '—'}
                  </td>
                  <td className="px-3 py-2 text-right text-amber-400">{r.bid != null ? formatNum(r.bid, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{r.ask != null ? formatNum(r.ask, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{r.last_trade_price != null ? formatNum(r.last_trade_price, 2) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{ivPct != null ? `${ivPct.toFixed(2)}%` : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{r.open_interest != null ? formatVolume(r.open_interest) : '—'}</td>
                  <td className="px-3 py-2 text-right text-amber-400">{r.volume != null ? formatVolume(r.volume) : '—'}</td>
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

function sortRows(rows: OptionsRow[], key: SortKey, dir: SortDir): OptionsRow[] {
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
