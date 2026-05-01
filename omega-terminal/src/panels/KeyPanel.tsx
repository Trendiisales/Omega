// KeyPanel — Key Stats / valuation multiples.
//
// Step 6 deliverable. Multi-card grid over the merged KeyEnvelope
// (key_metrics + multiples) returned by /api/v1/omega/key. The engine
// fans two OpenBB calls (`/equity/fundamental/{key_metrics,multiples}`)
// and stitches them into one top-level object.
//
// Args: `KEY <symbol>`. Polling cadence: 5 minutes (engine cache TTL
// 250 s).

import { useCallback } from 'react';
import { getKey } from '@/api/omega';
import type { KeyEnvelope, KeyMetricsRow, MultiplesRow } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SummaryCard,
  WarningsBanner,
  detectMock,
  formatLargeNum,
  formatNum,
  formatPct,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const KEY_POLL_MS = 5 * 60 * 1000;

function pct(v: number | undefined): string {
  if (v == null || !Number.isFinite(v)) return '—';
  // Some providers return decimals (0.32 = 32 %), others integers (32 = 32 %).
  // Heuristic: if abs < 5 and we expect a percentage, scale by 100.
  return formatPct(Math.abs(v) < 5 ? v * 100 : v);
}

function ratio(v: number | undefined, dp = 2): string {
  if (v == null || !Number.isFinite(v)) return '—';
  return formatNum(v, dp);
}

function dollars(v: number | undefined): string {
  if (v == null || !Number.isFinite(v)) return '—';
  return `$${formatLargeNum(v)}`;
}

export function KeyPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();

  const fetcher = useCallback(
    (s: AbortSignal) => getKey({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<KeyEnvelope>(
    fetcher,
    [symbol],
    { pollMs: symbol ? KEY_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="KEY" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const km: KeyMetricsRow = env?.key_metrics?.[0] ?? {};
  const mp: MultiplesRow = env?.multiples?.[0] ?? {};

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Key Stats"
    >
      <PanelHeader
        code="KEY"
        title="Key Stats"
        subtitle={
          <>
            Key ratios &amp; multiples via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 5m
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <WarningsBanner warnings={warnings} />

      <Section title="Valuation">
        <SummaryCard label="Market Cap"      value={dollars(km.market_cap)} />
        <SummaryCard label="Enterprise Val"  value={dollars(km.enterprise_value)} />
        <SummaryCard label="P/E (TTM)"       value={ratio(mp.pe_ratio_ttm ?? km.pe_ratio)} />
        <SummaryCard label="Forward P/E"     value={ratio(km.forward_pe)} />
        <SummaryCard label="PEG"             value={ratio(km.peg_ratio)} />
        <SummaryCard label="P/B"             value={ratio(km.price_to_book ?? mp.price_to_book_quarterly)} />
        <SummaryCard label="P/S"             value={ratio(km.price_to_sales ?? mp.price_to_sales_ttm)} />
        <SummaryCard label="EV / EBITDA"     value={ratio(km.ev_to_ebitda ?? mp.ev_to_ebitda_ttm)} />
      </Section>

      <Section title="Yield & payout">
        <SummaryCard label="Dividend Yield"  value={pct(km.dividend_yield)} accent={(km.dividend_yield ?? 0) > 0 ? 'up' : 'text-amber-300'} />
        <SummaryCard label="Payout Ratio"    value={pct(km.payout_ratio)} />
        <SummaryCard label="Earnings Yield"  value={pct(mp.earnings_yield_ttm)} />
        <SummaryCard label="FCF Yield"       value={pct(mp.free_cash_flow_yield_ttm)} />
      </Section>

      <Section title="Profitability">
        <SummaryCard label="ROE"             value={pct(km.return_on_equity)} />
        <SummaryCard label="ROA"             value={pct(km.return_on_assets)} />
        <SummaryCard label="Profit Margin"   value={pct(km.profit_margin)} />
        <SummaryCard label="Op. Margin"      value={pct(km.operating_margin)} />
      </Section>

      <Section title="Risk & liquidity">
        <SummaryCard label="Beta"            value={ratio(km.beta)} />
        <SummaryCard label="Debt/Equity"     value={ratio(km.debt_to_equity)} />
        <SummaryCard label="Current Ratio"   value={ratio(km.current_ratio)} />
        <SummaryCard label="Quick Ratio"     value={ratio(km.quick_ratio)} />
      </Section>

      <FetchStatusBar state={state} onRetry={refetch} />
    </section>
  );
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h3 className="mb-2 font-mono text-[10px] uppercase tracking-[0.3em] text-amber-500">
        {title}
      </h3>
      <div className="grid grid-cols-4 gap-4">{children}</div>
    </div>
  );
}
