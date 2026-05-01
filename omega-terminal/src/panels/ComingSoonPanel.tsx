// ComingSoonPanel — generic empty body for any code whose `step` > 1.
//
// Reads PanelDescriptor and reports the build step in which the panel
// becomes live, plus a one-line description so the user knows what's
// coming and where to look in the HANDOFF for the underlying spec.

import type { PanelDescriptor } from '@/types';

interface Props {
  descriptor: PanelDescriptor;
}

const STEP_LABEL: Record<number, string> = {
  3: 'Step 3 — Omega engine wiring (CC, ENG, POS)',
  4: 'Step 4 — Ledger, drill-down, cell grid',
  5: 'Step 5 — OpenBB market data: INTEL, CURV, WEI, MOV',
  6: 'Step 6 — BB function suite + WATCH',
};

export function ComingSoonPanel({ descriptor }: Props) {
  const stepLabel = STEP_LABEL[descriptor.step] ?? `Step ${descriptor.step}`;
  return (
    <section
      className="flex h-full w-full items-center justify-center p-8"
      aria-label={`${descriptor.title} panel`}
    >
      <div className="max-w-xl text-center">
        <div className="font-mono text-xs uppercase tracking-[0.4em] text-amber-600">
          {descriptor.code}
        </div>
        <h2 className="mt-2 font-mono text-3xl font-bold text-amber-300">
          {descriptor.title}
        </h2>
        <p className="mt-4 text-sm leading-6 text-amber-500/80">
          {descriptor.description}
        </p>

        <div className="mt-10 inline-flex items-center gap-2 rounded border border-amber-700/50 bg-amber-950/30 px-4 py-2 font-mono text-[10px] uppercase tracking-widest text-amber-500">
          <span aria-hidden="true" className="h-1.5 w-1.5 rounded-full bg-amber-500" />
          <span>{stepLabel}</span>
        </div>

        <p className="mt-6 text-[10px] uppercase tracking-widest text-amber-700">
          step 1 placeholder &mdash; awaiting backend wiring
        </p>
      </div>
    </section>
  );
}
