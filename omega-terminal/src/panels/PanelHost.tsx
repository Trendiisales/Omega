// PanelHost — renders the correct panel for a given FunctionCode.
//
// Step 1 routing rule:
//   - HOME  -> HomePanel  (clickable code grid)
//   - HELP  -> HelpPanel  (cheat-sheet of all codes + keys)
//   - else  -> ComingSoonPanel (generic placeholder showing target step)
//
// Steps 3-6 will replace the default branch with a per-code switch as
// each panel becomes live. The exhaustiveness guard at the bottom keeps
// future additions to the FunctionCode union honest.

import { PANEL_REGISTRY } from '@/router/functionCodes';
import type { FunctionCode } from '@/types';
import { ComingSoonPanel } from './ComingSoonPanel';
import { HelpPanel } from './HelpPanel';
import { HomePanel } from './HomePanel';

interface Props {
  code: FunctionCode;
  onNavigate: (code: FunctionCode) => void;
}

export function PanelHost({ code, onNavigate }: Props) {
  const descriptor = PANEL_REGISTRY[code];

  if (code === 'HOME') return <HomePanel onSelect={onNavigate} />;
  if (code === 'HELP') return <HelpPanel />;

  // Defensive: if the code somehow lacks a registered descriptor,
  // surface HELP. The exhaustiveness check below guards against
  // mis-typed FunctionCode literals at compile time.
  if (!descriptor) return <HelpPanel />;

  return <ComingSoonPanel descriptor={descriptor} />;
}
