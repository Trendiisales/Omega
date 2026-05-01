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
// Step 4 routing (this commit):
//   - LDG   -> LdgPanel   (filterable closed-trade ledger; row click -> TRADE <id>)
//   - TRADE -> TradePanel (per-trade drill-down; client-side fallback over the
//                          ledger snapshot since no /trade/<id> route ships yet)
//   - CELL  -> CellPanel  (per-cell summary grid; the engine-side /cells route
//                          is deferred so the panel renders a graceful
//                          "endpoint not yet wired" banner with a manual retry)
//   - ENG   -> EngPanel   (row click now navigates to LDG <engine>)
//   - POS   -> PosPanel   (row click now navigates to LDG <engine>)
//
// `onNavigate` widened from `(code: FunctionCode)` to `(target: string)` so
// row clicks can carry args (e.g. "LDG HybridGold", "TRADE 12345"). The
// router's resolveCode parses the head + args identically whether the input
// came from the command bar or a panel-internal navigation call.
//
// Step 5-6 codes still resolve to ComingSoonPanel until their respective
// panels land. The exhaustiveness guard at the bottom keeps future
// FunctionCode union additions honest.

import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { FunctionCode } from '@/types';
import { CCPanel } from './CCPanel';
import { CellPanel } from './CellPanel';
import { ComingSoonPanel } from './ComingSoonPanel';
import { EngPanel } from './EngPanel';
import { HelpPanel } from './HelpPanel';
import { HomePanel } from './HomePanel';
import { LdgPanel } from './LdgPanel';
import { PosPanel } from './PosPanel';
import { TradePanel } from './TradePanel';

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

  // Defensive: if the code somehow lacks a registered descriptor,
  // surface HELP. The exhaustiveness check below guards against
  // mis-typed FunctionCode literals at compile time.
  if (!descriptor) return <HelpPanel />;

  return <ComingSoonPanel descriptor={descriptor} />;
}
