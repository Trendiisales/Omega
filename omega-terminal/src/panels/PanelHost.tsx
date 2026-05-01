// PanelHost — renders the correct panel for a given FunctionCode + args.
//
// Step 1 routing:
//   - HOME  -> HomePanel  (clickable code grid)
//   - HELP  -> HelpPanel  (cheat-sheet of all codes + keys)
//
// Step 3 routing (this commit):
//   - CC   -> CCPanel  (live engines / equity / positions summary)
//   - ENG  -> EngPanel (engines drill-down with sort/filter)
//   - POS  -> PosPanel (open positions table with sort/filter)
//
// Step 4-6 codes still resolve to ComingSoonPanel until their respective
// panels land. The exhaustiveness guard at the bottom keeps future
// FunctionCode union additions honest.

import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { FunctionCode } from '@/types';
import { CCPanel } from './CCPanel';
import { ComingSoonPanel } from './ComingSoonPanel';
import { EngPanel } from './EngPanel';
import { HelpPanel } from './HelpPanel';
import { HomePanel } from './HomePanel';
import { PosPanel } from './PosPanel';

interface Props {
  code: FunctionCode;
  args: string[];
  onNavigate: (code: FunctionCode) => void;
}

export function PanelHost({ code, args, onNavigate }: Props) {
  const descriptor = PANEL_REGISTRY[code];

  if (code === 'HOME') return <HomePanel onSelect={onNavigate} />;
  if (code === 'HELP') return <HelpPanel />;

  // Step 3 live panels.
  if (code === 'CC')  return <CCPanel  args={args} />;
  if (code === 'ENG') return <EngPanel args={args} />;
  if (code === 'POS') return <PosPanel args={args} />;

  // Defensive: if the code somehow lacks a registered descriptor,
  // surface HELP. The exhaustiveness check below guards against
  // mis-typed FunctionCode literals at compile time.
  if (!descriptor) return <HelpPanel />;

  return <ComingSoonPanel descriptor={descriptor} />;
}
