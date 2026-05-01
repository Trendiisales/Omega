// Shared panel chrome — extracted for Step-6 panels.
//
// Step-5 panels (IntelPanel/CurvPanel/WeiPanel/MovPanel) each inline a
// private copy of every helper (SortHeader, ProviderBadge, FetchStatusBar,
// SkeletonRows, SummaryCard, formatNum, formatVolume, dirClass, formatUtc,
// parseDateMs, truncate). That was fine when there were 4 panels; with 13
// more landing in Step 6, 13× inlining would push the eager bundle past
// the +60 kB raw / +15 kB gzipped budget set by STEP6_OPENER.md.
//
// Step-6 panels import these helpers from here. The Step-5 panels are NOT
// refactored — they keep their inline copies untouched per the user's
// "never modify core code unless instructed clearly" rule.
//
// Visual contract is byte-identical to the Step-5 inline versions; this
// file is a deduplication, not a redesign.

import type { ReactNode } from 'react';
import type { OmegaApiError } from '@/api/types';
import type { PanelData } from '@/hooks/usePanelData';

/* ============================================================ */
/* Provider / mock badge                                        */
/* ============================================================ */

export function ProviderBadge({
  provider,
  isMock,
}: {
  provider: string;
  isMock: boolean;
}) {
  const cls = isMock
    ? 'border-amber-600 text-amber-500'
    : 'border-amber-800 text-amber-400';
  return (
    <span
      className={`rounded border px-2 py-1 font-mono text-[10px] uppercase tracking-widest ${cls}`}
      title={isMock ? 'OMEGA_OPENBB_MOCK=1 active' : 'live OpenBB provider'}
    >
      {isMock ? 'MOCK' : provider}
    </span>
  );
}

/* ============================================================ */
/* Summary card                                                 */
/* ============================================================ */

export function SummaryCard({
  label,
  value,
  accent,
  size = 'md',
}: {
  label: string;
  value: string;
  accent?: string;
  /**
   * Visual size of the value text. "md" matches CCPanel/IntelPanel
   * (text-lg). "lg" matches WeiPanel/MovPanel (text-2xl).
   */
  size?: 'md' | 'lg';
}) {
  const sizeCls = size === 'lg' ? 'text-2xl' : 'text-lg';
  return (
    <div className="border border-amber-800/50 bg-black/40 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className={`mt-1 font-mono ${sizeCls} ${accent ?? 'text-amber-300'}`}>
        {value}
      </div>
    </div>
  );
}

/* ============================================================ */
/* Sortable table header                                        */
/* ============================================================ */

export type SortDir = 'asc' | 'desc';

export function SortHeader<K extends string>({
  k,
  label,
  sortKey,
  sortDir,
  onClick,
  align,
}: {
  k: K;
  label: string;
  sortKey: K;
  sortDir: SortDir;
  onClick: (k: K) => void;
  align?: 'left' | 'right';
}) {
  const active = sortKey === k;
  const arrow = active ? (sortDir === 'asc' ? '▲' : '▼') : '';
  const align_cls = align === 'right' ? 'text-right' : 'text-left';
  return (
    <th
      onClick={() => onClick(k)}
      className={`px-3 py-2 cursor-pointer select-none uppercase ${align_cls} ${
        active ? 'text-amber-300' : 'text-amber-500'
      } hover:text-amber-300`}
    >
      {label} <span className="ml-1 text-[10px]">{arrow}</span>
    </th>
  );
}

/* ============================================================ */
/* Skeleton rows (loading state)                                */
/* ============================================================ */

export function SkeletonRows({
  cols,
  rows,
}: {
  cols: number;
  rows: number;
}) {
  return (
    <>
      {Array.from({ length: rows }).map((_, i) => (
        <tr key={`sk-${i}`} className="border-b border-amber-900/40">
          {Array.from({ length: cols }).map((__, j) => (
            <td key={j} className="px-3 py-2">
              <span className="inline-block h-3 w-16 animate-pulse rounded bg-amber-900/30" />
            </td>
          ))}
        </tr>
      ))}
    </>
  );
}

/* ============================================================ */
/* Skeleton article rows (for feed-style panels: NI)            */
/* ============================================================ */

export function SkeletonArticles({ rows }: { rows: number }) {
  return (
    <>
      {Array.from({ length: rows }).map((_, i) => (
        <div key={`sk-${i}`} className="border-b border-amber-900/40 px-4 py-3">
          <span className="inline-block h-3 w-2/3 animate-pulse rounded bg-amber-900/30" />
          <div className="mt-2">
            <span className="inline-block h-2 w-1/4 animate-pulse rounded bg-amber-900/20" />
          </div>
          <div className="mt-2">
            <span className="inline-block h-2 w-full animate-pulse rounded bg-amber-900/20" />
          </div>
        </div>
      ))}
    </>
  );
}

/* ============================================================ */
/* Fetch status bar (red retry banner on err)                   */
/* ============================================================ */

export function FetchStatusBar<T>({
  state,
  onRetry,
}: {
  state: PanelData<T>;
  onRetry: () => void;
}) {
  if (state.status !== 'err') return null;
  return (
    <div className="flex items-center justify-between border-t border-red-700/50 bg-red-950/20 px-4 py-2">
      <span className="font-mono text-xs text-down">
        {(state.error as OmegaApiError).message}
      </span>
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

/* ============================================================ */
/* Panel header (title + provider badge + subtitle)             */
/* ============================================================ */

export function PanelHeader({
  code,
  title,
  subtitle,
  provider,
  isMock,
}: {
  code: string;
  title: string;
  subtitle: ReactNode;
  provider: string;
  isMock: boolean;
}) {
  return (
    <header>
      <div className="flex items-center justify-between">
        <div>
          <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
            {code}
          </div>
          <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
            {title}
          </h2>
        </div>
        <ProviderBadge provider={provider} isMock={isMock} />
      </div>
      <p className="mt-2 font-mono text-xs text-amber-500/80">{subtitle}</p>
    </header>
  );
}

/* ============================================================ */
/* Warnings banner                                              */
/* ============================================================ */

export function WarningsBanner({
  warnings,
}: {
  warnings: Array<{ message?: string }>;
}) {
  if (!warnings || warnings.length === 0) return null;
  return (
    <div className="border border-amber-700/60 bg-amber-950/30 px-4 py-2 font-mono text-xs text-amber-400">
      {warnings.map((w, i) => (
        <div key={i}>warning: {w.message ?? '(no detail)'}</div>
      ))}
    </div>
  );
}

/* ============================================================ */
/* Missing-symbol prompt (for panels that require a symbol arg) */
/* ============================================================ */

export function MissingSymbolPrompt({
  code,
  exampleArgs,
}: {
  code: string;
  exampleArgs: string;
}) {
  return (
    <section
      className="flex h-full w-full items-center justify-center p-8"
      aria-label={`${code} missing symbol`}
    >
      <div className="max-w-xl text-center">
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          {code}
        </div>
        <h2 className="mt-2 font-mono text-3xl font-bold text-amber-300">
          Symbol required
        </h2>
        <p className="mt-4 font-mono text-sm text-amber-500/80">
          Type <span className="text-amber-300">{code} {exampleArgs}</span> in
          the command bar to load this panel.
        </p>
      </div>
    </section>
  );
}

/* ============================================================ */
/* Mock-mode helper                                             */
/* ============================================================ */

/**
 * Detect whether the OpenBbEnvelope-shaped object came from mock mode.
 * The engine signals mock either via `provider === "mock"` or via
 * `extra.mock === true`; this helper centralises that check.
 */
export function detectMock(env: {
  provider?: string;
  extra?: Record<string, unknown>;
} | null | undefined): boolean {
  if (!env) return false;
  if (env.provider === 'mock') return true;
  if (env.extra && (env.extra as Record<string, unknown>).mock === true) return true;
  return false;
}

/* ============================================================ */
/* Formatting helpers                                           */
/* ============================================================ */

export function formatNum(n: number, dp: number): string {
  if (!Number.isFinite(n)) return '—';
  return n.toLocaleString('en-US', {
    minimumFractionDigits: dp,
    maximumFractionDigits: dp,
  });
}

export function formatVolume(n: number): string {
  if (!Number.isFinite(n)) return '—';
  if (n >= 1e9) return `${(n / 1e9).toFixed(2)}B`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}K`;
  return String(Math.round(n));
}

export function formatLargeNum(n: number): string {
  // Same as formatVolume but extends to T (trillions) for FA/KEY-style
  // line items where market cap and balance-sheet totals matter.
  if (!Number.isFinite(n)) return '—';
  const sign = n < 0 ? '-' : '';
  const v = Math.abs(n);
  if (v >= 1e12) return `${sign}${(v / 1e12).toFixed(2)}T`;
  if (v >= 1e9)  return `${sign}${(v / 1e9).toFixed(2)}B`;
  if (v >= 1e6)  return `${sign}${(v / 1e6).toFixed(2)}M`;
  if (v >= 1e3)  return `${sign}${(v / 1e3).toFixed(1)}K`;
  return `${sign}${Math.round(v)}`;
}

export function formatPct(n: number, dp = 2): string {
  if (!Number.isFinite(n)) return '—';
  return `${n >= 0 ? '+' : ''}${n.toFixed(dp)}%`;
}

export function dirClass(v: number): string {
  if (v > 0) return 'up';
  if (v < 0) return 'down';
  return 'text-amber-400';
}

export function parseDateMs(s: string | undefined): number {
  if (!s) return 0;
  const t = Date.parse(s);
  return Number.isFinite(t) ? t : 0;
}

export function formatUtc(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm   = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd   = String(d.getUTCDate()).padStart(2, '0');
  const HH   = String(d.getUTCHours()).padStart(2, '0');
  const MM   = String(d.getUTCMinutes()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${HH}:${MM}Z`;
}

export function formatDate(s: string | undefined): string {
  if (!s) return '—';
  // OpenBB returns dates as ISO strings; render only the date portion.
  // Tolerate both "2026-05-01" and "2026-05-01T12:00:00Z".
  const dateOnly = s.length >= 10 ? s.slice(0, 10) : s;
  return dateOnly;
}

export function truncate(s: string, max: number): string {
  if (s.length <= max) return s;
  const cut = s.slice(0, max);
  const sp  = cut.lastIndexOf(' ');
  const tail = sp > max * 0.6 ? cut.slice(0, sp) : cut;
  return tail + '…';
}
