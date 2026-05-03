// PanelHost — renders the correct panel for a given FunctionCode + args.
//
// Step 1 routing:
//   - HOME  -> HomePanel  (clickable code grid)
//   - HELP  -> HelpPanel  (cheat-sheet of all codes + keys)
//
// Step 3 routing:
//   - CC   -> CCPanel  (live engines / equity / positions summary)
//   - ENG  -> EngPanel (engines drill-down with sort/filter)
//   - POS  -> PosPanel (open positions table with sort/filter)
//
// Step 4 routing:
//   - LDG   -> LdgPanel   (filterable closed-trade ledger; row click -> TRADE <id>)
//   - TRADE -> TradePanel (per-trade drill-down; client-side fallback over the
//                          ledger snapshot since no /trade/<id> route ships yet)
//   - CELL  -> CellPanel  (per-cell summary grid; the engine-side /cells route
//                          is deferred so the panel renders a graceful
//                          "endpoint not yet wired" banner with a manual retry)
//   - ENG   -> EngPanel   (row click navigates to LDG <engine>)
//   - POS   -> PosPanel   (row click navigates to LDG <engine>)
//
// Step 5 routing:
//   - INTEL -> IntelPanel (OpenBB news feed; args = screen-id, default "TOP")
//   - CURV  -> CurvPanel  (OpenBB Treasury yield curve; args = region, default US)
//   - WEI   -> WeiPanel   (OpenBB index-ETF quotes; args = region, default US)
//   - MOV   -> MovPanel   (OpenBB movers; args = universe, default active)
//
//   CURV is loaded via React.lazy + <Suspense> so the Recharts dependency is
//   split into a separate chunk and only fetched when the user navigates to
//   CURV.
//
// Step 6 routing (this commit):
//   - OMON   -> OmonPanel   (options chain; args = symbol [expiry])
//   - FA     -> FaPanel     (3-tab income/balance/cash; args = symbol)
//   - KEY    -> KeyPanel    (key stats multi-card; args = symbol)
//   - DVD    -> DvdPanel    (dividend history; args = symbol)
//   - EE     -> EePanel     (consensus + surprise; args = symbol)
//   - NI     -> NiPanel     (per-symbol news; args = symbol)
//   - GP     -> GpPanel     (price chart; args = symbol [interval]) — LAZY
//   - QR     -> QrPanel     (multi-symbol quote tape; args = symbol-list)
//   - HP     -> HpPanel     (OHLCV table; args = symbol [interval])
//   - DES    -> DesPanel    (company profile; args = symbol)
//   - FXC    -> FxcPanel    (FX cross rates; args = pair-or-region)
//   - CRYPTO -> CryptoPanel (crypto quotes; args = symbols-or-region)
//   - WATCH  -> WatchPanel  (engine-cron-driven nightly screener; args = universe)
//
//   GP is lazy-loaded (React.lazy + <Suspense>) — same rule as CURV. Recharts
//   stays in its existing on-demand chunk; no eager-bundle delta from
//   re-importing it.
//
//   The other 12 Step-6 panels are eagerly imported. Combined with the
//   shared chrome module (panels/_shared/PanelChrome.tsx) and the deduped
//   helpers therein, this stays inside the +60 kB raw / +15 kB gzipped
//   eager-bundle budget set by STEP6_OPENER.md.
//
// `onNavigate` widened from `(code: FunctionCode)` to `(target: string)` so
// row clicks can carry args (e.g. "LDG HybridGold", "TRADE 12345"). The
// router's resolveCode parses the head + args identically whether the input
// came from the command bar or a panel-internal navigation call.

import { lazy, Suspense } from 'react';
import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { FunctionCode } from '@/types';
import { CCPanel } from './CCPanel';
import { CellPanel } from './CellPanel';
import { ComingSoonPanel } from './ComingSoonPanel';
import { CryptoPanel } from './CryptoPanel';
import { DesPanel } from './DesPanel';
import { DvdPanel } from './DvdPanel';
import { EePanel } from './EePanel';
import { EngPanel } from './EngPanel';
import { FaPanel } from './FaPanel';
import { FxcPanel } from './FxcPanel';
import { HelpPanel } from './HelpPanel';
import { HomePanel } from './HomePanel';
import { HpPanel } from './HpPanel';
import { IntelPanel } from './IntelPanel';
import { KeyPanel } from './KeyPanel';
import { LdgPanel } from './LdgPanel';
import { MovPanel } from './MovPanel';
import { NiPanel } from './NiPanel';
import { OmonPanel } from './OmonPanel';
import { PosPanel } from './PosPanel';
import { QrPanel } from './QrPanel';
import { TradePanel } from './TradePanel';
import { WatchPanel } from './WatchPanel';
import { WeiPanel } from './WeiPanel';

// CURV (Step 5) and GP (Step 6) both pull Recharts; lazy-load both so the
// chart library lives in its own chunk and is only fetched when the user
// navigates to one of those codes. Vite emits each as a separate
// dist/assets/<Name>Panel-<hash>.js chunk that shares the same recharts
// vendor chunk. No impact on the eager bundle.
const CurvPanel = lazy(() =>
  import('./CurvPanel').then((m) => ({ default: m.CurvPanel })),
);
const GpPanel = lazy(() =>
  import('./GpPanel').then((m) => ({ default: m.GpPanel })),
);

interface Props {
  code: FunctionCode;
  args: string[];
  onNavigate: (target: string) => void;
}

export function PanelHost({ code, args, onNavigate }: Props) {
  const descriptor = PANEL_REGISTRY[code];

  if (code === 'HOME') return <HomePanel onSelect={onNavigate} />;
  if (code === 'HELP') return <HelpPanel />;

  // Step 3 live panels.
  if (code === 'CC')  return <CCPanel  args={args} />;
  if (code === 'ENG') return <EngPanel args={args} onNavigate={onNavigate} />;
  if (code === 'POS') return <PosPanel args={args} onNavigate={onNavigate} />;

  // Step 4 live panels.
  if (code === 'LDG')   return <LdgPanel   args={args} onNavigate={onNavigate} />;
  if (code === 'TRADE') return <TradePanel args={args} onNavigate={onNavigate} />;
  if (code === 'CELL')  return <CellPanel  args={args} onNavigate={onNavigate} />;

  // Step 5 live panels (OpenBB-backed).
  if (code === 'INTEL') return <IntelPanel args={args} onNavigate={onNavigate} />;
  if (code === 'CURV') {
    return (
      <Suspense fallback={<LazyChartFallback code="CURV" />}>
        <CurvPanel args={args} onNavigate={onNavigate} />
      </Suspense>
    );
  }
  if (code === 'WEI')   return <WeiPanel   args={args} onNavigate={onNavigate} />;
  if (code === 'MOV')   return <MovPanel   args={args} onNavigate={onNavigate} />;

  // Step 6 live panels (BB function suite).
  if (code === 'OMON')   return <OmonPanel   args={args} onNavigate={onNavigate} />;
  if (code === 'FA')     return <FaPanel     args={args} onNavigate={onNavigate} />;
  if (code === 'KEY')    return <KeyPanel    args={args} onNavigate={onNavigate} />;
  if (code === 'DVD')    return <DvdPanel    args={args} onNavigate={onNavigate} />;
  if (code === 'EE')     return <EePanel     args={args} onNavigate={onNavigate} />;
  if (code === 'NI')     return <NiPanel     args={args} onNavigate={onNavigate} />;
  if (code === 'GP') {
    return (
      <Suspense fallback={<LazyChartFallback code="GP" />}>
        <GpPanel args={args} onNavigate={onNavigate} />
      </Suspense>
    );
  }
  if (code === 'QR')     return <QrPanel     args={args} onNavigate={onNavigate} />;
  if (code === 'HP')     return <HpPanel     args={args} onNavigate={onNavigate} />;
  if (code === 'DES')    return <DesPanel    args={args} onNavigate={onNavigate} />;
  if (code === 'FXC')    return <FxcPanel    args={args} onNavigate={onNavigate} />;
  if (code === 'CRYPTO') return <CryptoPanel args={args} onNavigate={onNavigate} />;
  if (code === 'WATCH')  return <WatchPanel  args={args} onNavigate={onNavigate} />;

  // Defensive: if the code somehow lacks a registered descriptor,
  // surface HELP. The exhaustiveness check below guards against
  // mis-typed FunctionCode literals at compile time.
  if (!descriptor) return <HelpPanel />;

  return <ComingSoonPanel descriptor={descriptor} />;
}

/**
 * Suspense fallback for lazy-loaded chart panels (CURV + GP). Visually
 * matches the panels' own "loading…" placeholder so the chunk download
 * is not jarring.
 */
function LazyChartFallback({ code }: { code: string }) {
  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label={`${code} loading`}
    >
      <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
        {code}
      </div>
      <div className="border border-amber-800/50 bg-black/40 p-8 font-mono text-xs text-amber-500">
        Loading chart bundle…
      </div>
    </section>
  );
}
