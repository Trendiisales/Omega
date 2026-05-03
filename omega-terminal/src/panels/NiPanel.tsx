// NiPanel — Per-symbol news.
//
// Step 6 deliverable. Article feed identical in shape to IntelPanel but
// filtered to a single symbol. Engine forwards to OpenBB
// /news/company?symbol=<sym> via /api/v1/omega/ni.
//
// Args: `NI <symbol>`. Polling cadence: 30 s. Engine cache TTL 25 s.

import { useCallback } from 'react';
import { getNi } from '@/api/omega';
import type { IntelArticle, OpenBbEnvelope } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SkeletonArticles,
  SummaryCard,
  WarningsBanner,
  detectMock,
  formatUtc,
  parseDateMs,
  truncate,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const NI_POLL_MS = 30000;
const NI_LIMIT = 25;

export function NiPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();

  const fetcher = useCallback(
    (s: AbortSignal) => getNi({ symbol, limit: NI_LIMIT }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<IntelArticle>>(
    fetcher,
    [symbol],
    { pollMs: symbol ? NI_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="NI" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const articles = env?.results ?? [];
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;

  const latest = articles
    .map((a) => parseDateMs(a.date))
    .filter((n) => n > 0)
    .reduce((a, b) => (a > b ? a : b), 0);

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="News"
    >
      <PanelHeader
        code="NI"
        title="News"
        subtitle={
          <>
            Per-symbol news feed via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 30s
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Articles" value={String(articles.length)} />
        <SummaryCard label="Latest" value={latest > 0 ? formatUtc(latest) : '—'} />
        <SummaryCard label="Provider" value={provider} />
        <SummaryCard label="Mode" value={isMock ? 'MOCK' : 'LIVE'} accent={isMock ? 'text-amber-500' : 'up'} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            Feed
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {articles.length} {articles.length === 1 ? 'article' : 'articles'}
          </span>
        </div>
        <div>
          {articles.length === 0 && state.status === 'loading' && <SkeletonArticles rows={5} />}
          {articles.length === 0 && state.status === 'ok' && (
            <div className="px-4 py-8 text-center font-mono text-xs text-amber-600">
              No articles returned for "{symbol}".
            </div>
          )}
          {articles.map((a, i) => {
            const ts = parseDateMs(a.date);
            return (
              <article key={`${a.url ?? a.title}-${i}`} className="border-b border-amber-900/40 px-4 py-3">
                <div className="flex items-baseline justify-between gap-4">
                  <h4 className="font-mono text-sm text-amber-300">
                    {a.url ? (
                      <a
                        href={a.url}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="hover:underline"
                      >
                        {a.title || '(untitled)'}
                      </a>
                    ) : (
                      a.title || '(untitled)'
                    )}
                  </h4>
                  <span className="shrink-0 font-mono text-[10px] uppercase tracking-widest text-amber-700">
                    {ts > 0 ? formatUtc(ts) : '—'}
                  </span>
                </div>
                <div className="mt-1 font-mono text-[11px] text-amber-500">
                  {a.source ?? '—'}
                  {a.symbols && a.symbols.length > 0 && (
                    <>
                      {' '}&middot;{' '}
                      <span className="text-amber-400">
                        {a.symbols.slice(0, 6).join(', ')}
                        {a.symbols.length > 6 && ` +${a.symbols.length - 6}`}
                      </span>
                    </>
                  )}
                </div>
                {a.text && (
                  <p className="mt-2 font-mono text-xs leading-relaxed text-amber-400/90">
                    {truncate(a.text, 280)}
                  </p>
                )}
              </article>
            );
          })}
        </div>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}
