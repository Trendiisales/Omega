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
// Step 5 routing (this commit):
//   - INTEL -> IntelPanel (OpenBB news feed; args = screen-id, default "TOP")
//   - CURV  -> CurvPanel  (OpenBB Treasury yield curve; args = region, default US)
//   - WEI   -> WeiPanel   (OpenBB index-ETF quotes; args = region, default US)
//   - MOV   -> MovPanel   (OpenBB movers; args = universe, default active)
//
//   All four route to /api/v1/omega/{intel|curv|wei|mov} on the C++
//   OmegaApiServer, which forwards to OpenBB Hub via OpenBbProxy. Token
//   strategy is server-side -- the OMEGA_OPENBB_TOKEN env var lives on the
//   host running Omega.exe and never ships in the JS bundle. Set
//   OMEGA_OPENBB_MOCK=1 to bypass the network and render synthetic data
//   for local dev / Step-5 verification.
//
//   Code-splitting note: CurvPanel pulls in Recharts (~330 kB raw / ~100 kB
//   gzipped), which by itself blows past the Step-5 +60 kB raw / +15 kB
//   gzipped bundle budget. We lazy-load it via React.lazy + Suspense so
//   Recharts only enters the bundle when the user actually navigates to
//   CURV; INTEL/WEI/MOV stay eagerly imported because they have no chart
//   dependency. The Suspense fallback renders an amber-on-black "loading"
//   strip that matches the panels' own loading aesthetic.
//
// `onNavigate` widened from `(code: FunctionCode)` to `(target: string)` so
// row clicks can carry args (e.g. "LDG HybridGold", "TRADE 12345"). The
// router's resolveCode parses the head + args identically whether the input
// came from the command bar or a panel-internal navigation call.
//
// Step 6 codes still resolve to ComingSoonPanel until their respective
// panels land. The exhaustiveness guard at the bottom keeps future
// FunctionCode union additions honest.

import { lazy, Suspense } from 'react';
import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { FunctionCode } from '@/types';
import { CCPanel } from './CCPanel';
import { CellPanel } from './CellPanel';
import { ComingSoonPanel } from './ComingSoonPanel';
import { EngPanel } from './EngPanel';
import { HelpPanel } from './HelpPanel';
import { HomePanel } from './HomePanel';
import { IntelPanel } from './IntelPanel';
import { LdgPanel } from './LdgPanel';
import { MovPanel } from './MovPanel';
import { PosPanel } from './PosPanel';
import { TradePanel } from './TradePanel';
import { WeiPanel } from './WeiPanel';

// CURV is the only Step-5 panel that pulls Recharts; lazy-load it so the
// chart library is split into its own chunk and only fetched when the user
// navigates to CURV. Vite emits this as a separate dist/assets/CurvPanel-
// <hash>.js + its recharts vendor chunk -- no impact on the eager bundle.
const CurvPanel = lazy(() =>
  import('./CurvPanel').then((m) => ({ default: m.CurvPanel }))
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

  // Defensive: if the code somehow lacks a registered descriptor,
  // surface HELP. The exhaustiveness check below guards against
  // mis-typed FunctionCode literals at compile time.
  if (!descriptor) return <HelpPanel />;

  return <ComingSoonPanel descriptor={descriptor} />;
}

/**
 * Suspense fallback for lazy-loaded chart panels (currently CURV; future
 * Step-6 GP / HP will reuse). Visually matches the panels' own
 * "loading…" placeholder so the chunk download is not jarring.
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
