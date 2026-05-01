// CryptoPanel — Crypto majors / DeFi / stablecoins.
//
// Step 6 deliverable. Sortable table over OpenBB `/crypto/price/quote`
// via `/api/v1/omega/crypto`. The engine accepts either a comma-
// separated symbol list (e.g. "BTC-USD,ETH-USD") or a region preset
// (MAJORS / DEFI / STABLE) and expands to a list before calling OpenBB.
//
// Args: `CRYPTO <symbols-or-region>`. Examples:
//   CRYPTO MAJORS
//   CRYPTO DEFI
//   CRYPTO BTC-USD,ETH-USD,SOL-USD
//
// Polling cadence: 5 s. Engine-side cache TTL 4 s.

import { useCallback, useMemo, useState } from 'react';
import { getCrypto } from '@/api/omega';
import type { CryptoQuote, OpenBbEnvelope } from '@/api/types';
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
  formatLargeNum,
  formatNum,
  formatVolume,
  formatPct,
  type SortDir,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const CRYPTO_POLL_MS = 5000;

type SortKey =
  | 'symbol'
  | 'name'
  | 'last_price'
  | 'change'
  | 'change_percent'
  | 'volume'
  | 'market_cap';

function joinSymbols(args: string[]): string {
  if (args.length === 0) return 'MAJORS';
  if (args.length === 1) return args[0]!.replace(/\s+/g, '');
  return args.map((a) => a.replace(/[, ]+/g, '')).filter(Boolean).join(',');
}

export function CryptoPanel({ args }: Props) {
  const symbols = joinSymbols(args);
  const [sortKey, setSortKey] = useState<SortKey>('market_cap');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  const fetcher = useCallback(
    (s: AbortSignal) => getCrypto({ symbols }, { signal: s }),
    [symbols],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<CryptoQuote>>(
    fetcher,
    [symbols],
    { pollMs: CRYPTO_POLL_MS },
  );

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const rows = env?.results ?? [];
  const sorted = useMemo(() => sortRows(rows, sortKey, sortDir), [rows, sortKey, sortDir]);

  const upCount = rows.filter((r) => (r.change ?? 0) > 0).length;
  const downCount = rows.filter((r) => (r.change ?? 0) < 0).length;
  const totalCap = rows.reduce((acc, r) => acc + (r.market_cap ?? 0), 0);

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
      aria-label="Crypto"
    >
      <PanelHeader
        code="CRYPTO"
        title="Crypto"
        subtitle={
          <>
            Crypto quotes via OpenBB &middot; set{' '}
            <span className="text-amber-300">{symbols}</span> &middot; poll 5s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Symbols" value={String(rows.length)} size="lg" />
        <SummaryCard label="Up" value={String(upCount)} accent="up" size="lg" />
        <SummaryCard label="Down" value={String(downCount)} accent="down" size="lg" />
        <SummaryCard label="Cap" value={totalCap === 0 ? '—' : formatLargeNum(totalCap)} size="lg" />
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
              <SortHeader<SortKey> k="symbol"         label="Symbol"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="name"           label="Name"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} />
              <SortHeader<SortKey> k="last_price"     label="Last"     sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="change"         label="Δ"        sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="change_percent" label="Δ%"       sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="volume"         label="Volume"   sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
              <SortHeader<SortKey> k="market_cap"     label="Mkt Cap"  sortKey={sortKey} sortDir={sortDir} onClick={clickHeader} align="right" />
            </tr>
          </thead>
          <tbody>
            {sorted.length === 0 && state.status === 'loading' && <SkeletonRows cols={7} rows={5} />}
            {sorted.length === 0 && state.status === 'ok' && (
              <tr>
                <td colSpan={7} className="px-3 py-6 text-center text-amber-600">
                  No quotes returned for "{symbols}".
                </td>
              </tr>
            )}
            {sorted.map((r) => (
              <tr key={r.symbol} className="border-b border-amber-900/40">
                <td className="px-3 py-2 font-bold text-amber-300">{r.symbol}</td>
                <td className="px-3 py-2 text-amber-400">{r.name ?? '—'}</td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.last_price != null ? formatNum(r.last_price, 2) : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change ?? 0)}`}>
                  {r.change != null ? `${r.change >= 0 ? '+' : ''}${formatNum(r.change, 2)}` : '—'}
                </td>
                <td className={`px-3 py-2 text-right ${dirClass(r.change_percent ?? 0)}`}>
                  {r.change_percent != null ? formatPct(r.change_percent) : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.volume != null ? formatVolume(r.volume) : '—'}
                </td>
                <td className="px-3 py-2 text-right text-amber-400">
                  {r.market_cap != null ? formatLargeNum(r.market_cap) : '—'}
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

function sortRows(rows: CryptoQuote[], key: SortKey, dir: SortDir): CryptoQuote[] {
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
