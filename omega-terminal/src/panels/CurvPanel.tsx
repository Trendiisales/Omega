// CurvPanel — Yield Curve.
//
// Step 5 deliverable. First panel that uses Recharts. Shows a US Treasury
// yield curve pulled from OpenBB Hub via `/api/v1/omega/curv`, which the
// engine-side OpenBbProxy backs with
// `/fixedincome/government/treasury_rates?provider=federal_reserve`.
//
// Layout:
//   - Top header + provider/mock badge.
//   - Summary cards: most-recent date, region, # tenors, max-min spread.
//   - Recharts <LineChart> with maturity (years) on the X axis and
//     interest rate (%) on the Y axis. Single line; tooltip on hover.
//   - 60 s polling cadence (yields are end-of-day data; faster polling
//     just wastes the OpenBB free-tier quota).
//
// Args: `CURV US|EU|JP`. Default region is "US". The engine route is
// fully wired for US via the federal_reserve provider; EU/JP currently
// return a 200 with an empty `results` array and a `warnings` entry --
// the panel surfaces that as an amber notice and does not error out.
//
// Maturity parsing: OpenBB's federal_reserve provider returns maturity
// strings like "month_3" / "year_10". We translate those into a numeric
// X coordinate measured in years so the curve sorts correctly along the
// X axis even when the API returns rows in alphabetic order.

import { useCallback, useMemo } from 'react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { getCurv } from '@/api/omega';
import type { CurvPoint, OpenBbEnvelope, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const CURV_POLL_MS = 60000;

interface ChartPoint {
  /** Maturity in years (sortable numeric key). */
  years: number;
  /** Original maturity label for the tooltip. */
  label: string;
  /** Interest rate as a percentage. */
  rate: number;
}

export function CurvPanel({ args }: Props) {
  const region = (args[0] ?? 'US').toUpperCase();

  const fetcher = useCallback(
    (s: AbortSignal) => getCurv({ region }, { signal: s }),
    [region]
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<CurvPoint>>(
    fetcher,
    [region],
    { pollMs: CURV_POLL_MS }
  );

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock   = provider === 'mock' ||
                   Boolean(env?.extra && (env.extra as Record<string, unknown>).mock);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;

  // Reduce the OpenBB rows into one chart point per maturity, taking the
  // most-recent date when multiple are present. The federal_reserve
  // provider tends to return one row per (date, maturity); CURV asks for
  // a single curve so we keep only the latest date.
  const { chart, latestDate } = useMemo(
    () => reduceToCurve(env?.results ?? []),
    [env?.results]
  );

  const minRate = chart.length === 0 ? 0 : Math.min(...chart.map((p) => p.rate));
  const maxRate = chart.length === 0 ? 0 : Math.max(...chart.map((p) => p.rate));
  const spread  = chart.length === 0 ? 0 : maxRate - minRate;

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Yield Curve"
    >
      <header>
        <div className="flex items-center justify-between">
          <div>
            <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
              CURV
            </div>
            <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
              Yield Curve
            </h2>
          </div>
          <ProviderBadge provider={provider} isMock={isMock} />
        </div>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          Government rates by maturity &middot; region{' '}
          <span className="text-amber-300">{region}</span> &middot; poll 60s
        </p>
      </header>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="As of" value={latestDate || '—'} />
        <SummaryCard label="Region" value={region} />
        <SummaryCard label="Tenors" value={String(chart.length)} />
        <SummaryCard
          label="Spread (max-min)"
          value={chart.length === 0 ? '—' : `${spread.toFixed(2)}%`}
        />
      </div>

      {warnings.length > 0 && (
        <div className="border border-amber-700/60 bg-amber-950/30 px-4 py-2 font-mono text-xs text-amber-400">
          {warnings.map((w, i) => (
            <div key={i}>warning: {w.message ?? '(no detail)'}</div>
          ))}
        </div>
      )}

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Curve
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {chart.length} {chart.length === 1 ? 'point' : 'points'}
          </span>
        </div>
        <div className="h-80 w-full p-4">
          {chart.length === 0 && state.status === 'loading' && (
            <div className="flex h-full items-center justify-center font-mono text-xs text-amber-600">
              Loading curve…
            </div>
          )}
          {chart.length === 0 && state.status === 'ok' && (
            <div className="flex h-full items-center justify-center font-mono text-xs text-amber-600">
              No curve data for region "{region}".
            </div>
          )}
          {chart.length > 0 && (
            <ResponsiveContainer width="100%" height="100%">
              <LineChart
                data={chart}
                margin={{ top: 10, right: 16, left: 0, bottom: 8 }}
              >
                <CartesianGrid stroke="#3a2b00" strokeDasharray="3 3" />
                <XAxis
                  dataKey="years"
                  type="number"
                  scale="linear"
                  domain={['dataMin', 'dataMax']}
                  ticks={pickXTicks(chart)}
                  tick={{ fill: '#d97706', fontSize: 11, fontFamily: 'monospace' }}
                  stroke="#92400e"
                  tickFormatter={(v: number) => formatYears(v)}
                  label={{
                    value: 'Maturity (years)',
                    position: 'insideBottom',
                    offset: -2,
                    fill: '#92400e',
                    fontSize: 10,
                    fontFamily: 'monospace',
                  }}
                />
                <YAxis
                  tick={{ fill: '#d97706', fontSize: 11, fontFamily: 'monospace' }}
                  stroke="#92400e"
                  tickFormatter={(v: number) => `${v.toFixed(2)}%`}
                  domain={['dataMin - 0.25', 'dataMax + 0.25']}
                />
                <Tooltip
                  contentStyle={{
                    background: '#0a0a0a',
                    border: '1px solid #92400e',
                    fontFamily: 'monospace',
                    fontSize: 12,
                    color: '#fcd34d',
                  }}
                  labelFormatter={(v: number, payload) => {
                    const label = payload?.[0]?.payload?.label as string | undefined;
                    return label ?? formatYears(v);
                  }}
                  formatter={(v: number) => [`${v.toFixed(3)}%`, 'rate']}
                />
                <Line
                  type="monotone"
                  dataKey="rate"
                  stroke="#fbbf24"
                  strokeWidth={2}
                  dot={{ fill: '#fbbf24', r: 3 }}
                  activeDot={{ r: 5 }}
                  isAnimationActive={false}
                />
              </LineChart>
            </ResponsiveContainer>
          )}
        </div>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Curve reduction                                                          */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * Take an array of OpenBB CurvPoints and produce a clean per-maturity
 * series suitable for Recharts. Behaviour:
 *
 *   - The curve is anchored at the most-recent date present in `rows`.
 *   - For each maturity at that date, emit one ChartPoint with `years`
 *     parsed from the OpenBB maturity string (e.g. "month_3" -> 0.25).
 *   - Maturities we cannot parse are dropped silently.
 *   - Output is sorted ascending by `years`.
 */
function reduceToCurve(rows: CurvPoint[]): { chart: ChartPoint[]; latestDate: string } {
  if (rows.length === 0) return { chart: [], latestDate: '' };

  let latestDate = '';
  for (const r of rows) {
    if (typeof r.date === 'string' && r.date > latestDate) {
      latestDate = r.date;
    }
  }

  const points: ChartPoint[] = [];
  const seen = new Set<string>();
  for (const r of rows) {
    if (r.date !== latestDate) continue;
    if (typeof r.maturity !== 'string') continue;
    if (seen.has(r.maturity)) continue;
    seen.add(r.maturity);
    const years = maturityToYears(r.maturity);
    if (years === null) continue;
    if (typeof r.rate !== 'number' || !Number.isFinite(r.rate)) continue;
    points.push({ years, label: r.maturity, rate: r.rate });
  }
  points.sort((a, b) => a.years - b.years);
  return { chart: points, latestDate };
}

/**
 * Convert OpenBB federal_reserve maturity strings into a numeric year
 * value. Returns null on failure.
 *
 *   "month_1"  ->  1/12
 *   "month_3"  ->  3/12
 *   "year_2"   ->  2
 *   "year_30"  ->  30
 *
 * We accept loose variants ("year_10y", "10y", "3m") for robustness in
 * case a different OpenBB provider gets wired into CURV later.
 */
function maturityToYears(s: string): number | null {
  const lower = s.toLowerCase();
  // month_<n>
  let m = lower.match(/^month_(\d+)$/);
  if (m) return parseInt(m[1]!, 10) / 12;
  // year_<n>
  m = lower.match(/^year_(\d+)$/);
  if (m) return parseInt(m[1]!, 10);
  // <n>m / <n>y
  m = lower.match(/^(\d+)\s*m$/);
  if (m) return parseInt(m[1]!, 10) / 12;
  m = lower.match(/^(\d+)\s*y$/);
  if (m) return parseInt(m[1]!, 10);
  return null;
}

/**
 * Pick a small set of ticks for the X axis that hit the obvious
 * round-number maturities. Recharts will linearly map between them.
 */
function pickXTicks(points: ChartPoint[]): number[] {
  if (points.length === 0) return [];
  const max = points[points.length - 1]!.years;
  const candidates = [0.25, 0.5, 1, 2, 3, 5, 7, 10, 20, 30];
  const out = candidates.filter((y) => y <= max + 0.5);
  if (out.length === 0) return [points[0]!.years, max];
  return out;
}

function formatYears(years: number): string {
  if (years < 1) {
    const months = Math.round(years * 12);
    return `${months}m`;
  }
  if (Math.abs(years - Math.round(years)) < 0.05) {
    return `${Math.round(years)}y`;
  }
  return `${years.toFixed(1)}y`;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Shared bits                                                              */
/* ──────────────────────────────────────────────────────────────────────── */

function SummaryCard({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: string;
}) {
  return (
    <div className="border border-amber-800/50 bg-black/40 px-4 py-3">
      <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
        {label}
      </div>
      <div className={`mt-1 font-mono text-lg ${accent ?? 'text-amber-300'}`}>
        {value}
      </div>
    </div>
  );
}

function ProviderBadge({ provider, isMock }: { provider: string; isMock: boolean }) {
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

function FetchStatusBar<T>({
  state,
  onRetry,
}: {
  state: ReturnType<typeof usePanelData<T>>['state'];
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
