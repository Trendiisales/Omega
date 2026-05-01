// IntelPanel — Intelligence Screener.
//
// Step 5 deliverable. Shows a news feed pulled from OpenBB Hub via
// `/api/v1/omega/intel`, which the engine-side OpenBbProxy backs with a
// call to OpenBB's `/news/world` endpoint.
//
// Pattern follows the multi-card layout of CCPanel:
//   - Top summary cards (count, latest article time, provider, mode).
//   - Scrollable article list below.
//   - 30 s polling cadence (news rarely updates faster than that and
//     OpenBB's free tier rate-limits aggressive callers).
//   - Args: `INTEL <screen-id>` -- the screen-id is forwarded to the
//     server but currently maps every value to /news/world. Step 6 will
//     branch into sector / earnings / macro screens.
//   - Skeleton-rows loading state + red retry banner -- identical
//     pattern to the Step-3/4 panels via the FetchStatusBar helper.
//
// OpenBB envelope handling: `getIntel` returns `OpenBbEnvelope<IntelArticle>`.
// We dereference `.results` for the article list and surface the envelope's
// `provider` field in a corner badge so it is obvious whether the panel is
// looking at real OpenBB data, the in-engine "mock" provider (when
// OMEGA_OPENBB_MOCK=1 is set on the server), or the "omega-stub" provider
// (returned by the engine for not-yet-wired regions).
//
// Mock-mode visibility: when `provider === "mock"` or `extra.mock === true`,
// the corner badge renders amber "MOCK" so verification builds are obvious
// at a glance and won't be confused for real data.

import { useCallback } from 'react';
import { getIntel } from '@/api/omega';
import type { IntelArticle, OpenBbEnvelope, OmegaApiError } from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const INTEL_POLL_MS = 30000;
const INTEL_LIMIT   = 25;

export function IntelPanel({ args }: Props) {
  const screen = (args[0] ?? 'TOP').toUpperCase();

  const fetcher = useCallback(
    (s: AbortSignal) => getIntel({ screen, limit: INTEL_LIMIT }, { signal: s }),
    [screen]
  );
  const { state, refetch } = usePanelData<OpenBbEnvelope<IntelArticle>>(
    fetcher,
    [screen],
    { pollMs: INTEL_POLL_MS }
  );

  const env = state.status === 'ok'
    ? state.data
    : (state.data ?? null);

  const articles = env?.results ?? [];
  const provider = env?.provider ?? 'unknown';
  const isMock   = provider === 'mock' ||
                   Boolean(env?.extra && (env.extra as Record<string, unknown>).mock);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;

  const latest = articles
    .map((a) => parseDateMs(a.date))
    .filter((n) => n > 0)
    .reduce((a, b) => (a > b ? a : b), 0);

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Intelligence Screener"
    >
      <header>
        <div className="flex items-center justify-between">
          <div>
            <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
              INTEL
            </div>
            <h2 className="mt-1 font-mono text-2xl font-bold text-amber-300">
              Intelligence Screener
            </h2>
          </div>
          <ProviderBadge provider={provider} isMock={isMock} />
        </div>
        <p className="mt-2 font-mono text-xs text-amber-500/80">
          OpenBB news feed &middot; screen{' '}
          <span className="text-amber-300">{screen}</span> &middot; poll 30s
        </p>
      </header>

      <div className="grid grid-cols-4 gap-4">
        <SummaryCard label="Articles" value={String(articles.length)} />
        <SummaryCard
          label="Latest"
          value={latest > 0 ? formatUtc(latest) : '—'}
        />
        <SummaryCard label="Provider" value={provider} />
        <SummaryCard
          label="Mode"
          value={isMock ? 'MOCK' : 'LIVE'}
          accent={isMock ? 'text-amber-500' : 'up'}
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
            Feed
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {articles.length} {articles.length === 1 ? 'article' : 'articles'}
          </span>
        </div>
        <div>
          {articles.length === 0 && state.status === 'loading' && (
            <SkeletonArticles rows={5} />
          )}
          {articles.length === 0 && state.status === 'ok' && (
            <div className="px-4 py-8 text-center font-mono text-xs text-amber-600">
              No articles returned for screen "{screen}".
            </div>
          )}
          {articles.map((a, i) => (
            <ArticleRow key={`${a.url ?? a.title}-${i}`} article={a} />
          ))}
        </div>
        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Article row                                                              */
/* ──────────────────────────────────────────────────────────────────────── */

function ArticleRow({ article }: { article: IntelArticle }) {
  const ts = parseDateMs(article.date);
  return (
    <article className="border-b border-amber-900/40 px-4 py-3">
      <div className="flex items-baseline justify-between gap-4">
        <h4 className="font-mono text-sm text-amber-300">
          {article.url ? (
            <a
              href={article.url}
              target="_blank"
              rel="noopener noreferrer"
              className="hover:underline"
            >
              {article.title || '(untitled)'}
            </a>
          ) : (
            article.title || '(untitled)'
          )}
        </h4>
        <span className="shrink-0 font-mono text-[10px] uppercase tracking-widest text-amber-700">
          {ts > 0 ? formatUtc(ts) : '—'}
        </span>
      </div>
      <div className="mt-1 font-mono text-[11px] text-amber-500">
        {article.source ?? '—'}
        {article.symbols && article.symbols.length > 0 && (
          <>
            {' '}&middot;{' '}
            <span className="text-amber-400">
              {article.symbols.slice(0, 6).join(', ')}
              {article.symbols.length > 6 && ` +${article.symbols.length - 6}`}
            </span>
          </>
        )}
      </div>
      {article.text && (
        <p className="mt-2 font-mono text-xs leading-relaxed text-amber-400/90">
          {truncate(article.text, 280)}
        </p>
      )}
    </article>
  );
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

function SkeletonArticles({ rows }: { rows: number }) {
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

/* ──────────────────────────────────────────────────────────────────────── */
/*  Formatting helpers                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

function parseDateMs(s: string): number {
  // OpenBB news dates are ISO-8601 strings, possibly without a Z suffix.
  // Date.parse handles both shapes; on failure we treat the value as
  // "unknown" and let the caller drop it.
  if (!s) return 0;
  const t = Date.parse(s);
  return Number.isFinite(t) ? t : 0;
}

function formatUtc(unixMs: number): string {
  if (!unixMs || unixMs <= 0) return '—';
  const d = new Date(unixMs);
  const yyyy = d.getUTCFullYear();
  const mm   = String(d.getUTCMonth() + 1).padStart(2, '0');
  const dd   = String(d.getUTCDate()).padStart(2, '0');
  const HH   = String(d.getUTCHours()).padStart(2, '0');
  const MM   = String(d.getUTCMinutes()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd} ${HH}:${MM}Z`;
}

function truncate(s: string, max: number): string {
  if (s.length <= max) return s;
  // Trim back to the last whitespace before max so we don't cut a word.
  const cut = s.slice(0, max);
  const sp  = cut.lastIndexOf(' ');
  const tail = sp > max * 0.6 ? cut.slice(0, sp) : cut;
  return tail + '…';
}
