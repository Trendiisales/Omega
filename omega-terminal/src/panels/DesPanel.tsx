// DesPanel — Company description / profile.
//
// Step 6 deliverable. Single-symbol profile pulled via OpenBB
// /equity/profile through /api/v1/omega/des. Args: `DES <symbol>`.
// Polling cadence: 5 minutes (engine-side cache TTL 250 s).

import { useCallback } from 'react';
import { getDes } from '@/api/omega';
import type { CompanyProfile, OpenBbEnvelope } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SummaryCard,
  WarningsBanner,
  detectMock,
  formatDate,
  formatLargeNum,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const DES_POLL_MS = 5 * 60 * 1000;

export function DesPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();

  const fetcher = useCallback(
    (s: AbortSignal) => getDes({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<CompanyProfile>>(
    fetcher,
    [symbol],
    { pollMs: symbol ? DES_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="DES" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const profile = env?.results?.[0];

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Company Description"
    >
      <PanelHeader
        code="DES"
        title="Description"
        subtitle={
          <>
            Company profile via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 5m
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <WarningsBanner warnings={warnings} />

      {profile ? (
        <>
          <div className="border border-amber-800/50 bg-black/40 p-6">
            <div className="font-mono text-[10px] uppercase tracking-widest text-amber-600">
              {profile.symbol ?? symbol} &middot; {profile.exchange ?? 'exchange unknown'}
            </div>
            <h3 className="mt-2 font-mono text-2xl font-bold text-amber-300">
              {profile.name ?? '(no name)'}
            </h3>
            {profile.website && (
              <a
                href={profile.website}
                target="_blank"
                rel="noopener noreferrer"
                className="mt-1 inline-block font-mono text-xs text-amber-500 hover:underline"
              >
                {profile.website}
              </a>
            )}
            {profile.description && (
              <p className="mt-4 max-w-3xl font-mono text-sm leading-relaxed text-amber-400/90">
                {profile.description}
              </p>
            )}
          </div>

          <div className="grid grid-cols-4 gap-4">
            <SummaryCard label="Sector" value={profile.sector ?? '—'} />
            <SummaryCard label="Industry" value={profile.industry ?? '—'} />
            <SummaryCard label="CEO" value={profile.ceo ?? '—'} />
            <SummaryCard
              label="Employees"
              value={profile.employees != null ? profile.employees.toLocaleString('en-US') : '—'}
            />
          </div>
          <div className="grid grid-cols-4 gap-4">
            <SummaryCard label="HQ City" value={profile.hq_city ?? '—'} />
            <SummaryCard label="HQ State" value={profile.hq_state ?? '—'} />
            <SummaryCard label="HQ Country" value={profile.hq_country ?? '—'} />
            <SummaryCard label="Currency" value={profile.currency ?? '—'} />
          </div>
          <div className="grid grid-cols-4 gap-4">
            <SummaryCard label="IPO Date" value={formatDate(profile.ipo_date)} />
            <SummaryCard
              label="Market Cap"
              value={profile.market_cap != null ? `$${formatLargeNum(profile.market_cap)}` : '—'}
            />
            <SummaryCard label="Provider" value={provider} />
            <SummaryCard
              label="Mode"
              value={isMock ? 'MOCK' : 'LIVE'}
              accent={isMock ? 'text-amber-500' : 'up'}
            />
          </div>
        </>
      ) : (
        <div className="flex-1 border border-amber-800/50 bg-black/40 p-8 text-center font-mono text-xs text-amber-600">
          {state.status === 'loading' ? 'Loading profile…' : `No profile returned for "${symbol}".`}
        </div>
      )}

      <div>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}
