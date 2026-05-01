// GpPanel — Graph Price (interactive chart).
//
// Step 6 deliverable. Recharts-based price chart over OpenBB
// `/equity/price/historical` via /api/v1/omega/gp. Args:
// `GP <symbol> [<interval>]`. Polling cadence: 30 s. Engine cache TTL
// 25 s, shared with /hp on identical (symbol, interval).
//
// MUST be lazy-loaded from PanelHost (React.lazy + <Suspense>) — same
// rule as CurvPanel — to keep Recharts out of the eager bundle.

import { useCallback, useMemo, useState } from 'react';
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { getGp } from '@/api/omega';
import type { BarInterval, HistoricalBar, OpenBbEnvelope } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SummaryCard,
  WarningsBanner,
  detectMock,
  formatDate,
  formatNum,
  formatPct,
  formatVolume,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const GP_POLL_MS = 30000;
const INTERVAL_OPTIONS: BarInterval[] = ['1d', '1h', '15m', '1m', '1W', '1M'];

interface ChartPoint {
  ts: number;
  date: string;
  close: number;
}

function parseInterval(s: string | undefined): BarInterval {
  if (!s) return '1d';
  const v = s.toLowerCase() as BarInterval;
  return (INTERVAL_OPTIONS as string[]).includes(v) ? (v as BarInterval) : '1d';
}

export function GpPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const initialInterval = parseInterval(args[1]);
  const [interval, setInterval] = useState<BarInterval>(initialInterval);

  const fetcher = useCallback(
    (s: AbortSignal) => getGp({ symbol, interval }, { signal: s }),
    [symbol, interval],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<HistoricalBar>>(
    fetcher,
    [symbol, interval],
    { pollMs: symbol ? GP_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="GP" exampleArgs="AAPL 1d" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const bars = env?.results ?? [];

  const chart: ChartPoint[] = useMemo(
    () =>
      bars
        .map((b) => {
          const ts = Date.parse(b.date);
          const close = b.close ?? b.adj_close;
          if (!Number.isFinite(ts) || close == null || !Number.isFinite(close)) return null;
          return { ts, date: b.date, close } as ChartPoint;
        })
        .filter((p): p is ChartPoint => p != null)
        .sort((a, b) => a.ts - b.ts),
    [bars],
  );

  const lastClose = chart.length > 0 ? chart[chart.length - 1]!.close : undefined;
  const firstClose = chart.length > 0 ? chart[0]!.close : undefined;
  const periodReturn =
    firstClose != null && lastClose != null && firstClose !== 0
      ? ((lastClose - firstClose) / firstClose) * 100
      : 0;
  const minClose = chart.length > 0 ? Math.min(...chart.map((p) => p.close)) : 0;
  const maxClose = chart.length > 0 ? Math.max(...chart.map((p) => p.close)) : 0;
  const totalVol = bars.reduce((acc, b) => acc + (b.volume ?? 0), 0);

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Graph Price"
    >
      <PanelHeader
        code="GP"
        title="Graph Price"
        subtitle={
          <>
            Price chart via OpenBB &middot; symbol{' '}
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
        <SummaryCard label="Bars" value={String(chart.length)} />
        <SummaryCard label="Last close" value={lastClose != null ? formatNum(lastClose, 2) : '—'} />
        <SummaryCard
          label="Period return"
          value={chart.length > 1 ? formatPct(periodReturn) : '—'}
          accent={periodReturn >= 0 ? 'up' : 'down'}
        />
        <SummaryCard label="Total Vol" value={totalVol > 0 ? formatVolume(totalVol) : '—'} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Chart
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {chart.length === 0
              ? '—'
              : `${formatDate(chart[0]!.date)} → ${formatDate(chart[chart.length - 1]!.date)}`}
          </span>
        </div>
        <div className="h-96 w-full p-4">
          {chart.length === 0 && state.status === 'loading' && (
            <div className="flex h-full items-center justify-center font-mono text-xs text-amber-600">
              Loading chart…
            </div>
          )}
          {chart.length === 0 && state.status === 'ok' && (
            <div className="flex h-full items-center justify-center font-mono text-xs text-amber-600">
              No bars returned for "{symbol}" at interval "{interval}".
            </div>
          )}
          {chart.length > 0 && (
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={chart} margin={{ top: 10, right: 16, left: 0, bottom: 8 }}>
                <CartesianGrid stroke="#3a2b00" strokeDasharray="3 3" />
                <XAxis
                  dataKey="ts"
                  type="number"
                  scale="time"
                  domain={['dataMin', 'dataMax']}
                  tick={{ fill: '#d97706', fontSize: 11, fontFamily: 'monospace' }}
                  stroke="#92400e"
                  tickFormatter={(v: number) => formatDate(new Date(v).toISOString())}
                />
                <YAxis
                  domain={[Math.floor(minClose * 0.99), Math.ceil(maxClose * 1.01)]}
                  tick={{ fill: '#d97706', fontSize: 11, fontFamily: 'monospace' }}
                  stroke="#92400e"
                  tickFormatter={(v: number) => formatNum(v, 2)}
                />
                <Tooltip
                  contentStyle={{
                    background: '#0a0a0a',
                    border: '1px solid #92400e',
                    fontFamily: 'monospace',
                    fontSize: 12,
                    color: '#fcd34d',
                  }}
                  labelFormatter={(v: number) => formatDate(new Date(v).toISOString())}
                  formatter={(v: number) => [formatNum(v, 2), 'close']}
                />
                <Line
                  type="monotone"
                  dataKey="close"
                  stroke="#fbbf24"
                  strokeWidth={2}
                  dot={false}
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
