export const meta = {
  name: 'omega-discovery',
  description: 'Gate-chain a batch of strategy candidates: tombstone-gate -> faithful BT -> adversarial mirage-kill -> verdict',
  whenToUse: 'When you have a batch of candidate edge ideas to vet. Pass them as args (array of {name, desc}). Run ONLY on explicit opt-in.',
  phases: [
    { title: 'Tombstone gate', detail: 'sb search each idea; drop already-DEAD' },
    { title: 'Faithful BT', detail: 'data-integrity gate + faithful tick/daily BT, both-halves both-regimes real cost' },
    { title: 'Mirage kill', detail: 'adversarial verify: bull-beta? squeeze-beta? survivorship? cost-fragile?' },
    { title: 'Verdict', detail: 'tombstone (sb build) or build-candidate; file wiki' },
  ],
}

// args = [{name, desc}, ...]  — a FINITE batch. Loop-until-dry is external:
// the operator brings the next batch. One run = one bounded fan-out (no infinite
// loop possible — Workflow caps agents at 1000 and the batch is finite).
const candidates = Array.isArray(args) && args.length
  ? args
  : [{ name: 'example', desc: 'replace via args — no candidates passed' }]

if (candidates[0].name === 'example') {
  log('No candidates in args — pass [{name, desc}, ...]. Nothing to do.')
  return { error: 'no candidates' }
}

const VERDICT = {
  type: 'object',
  required: ['name', 'verdict', 'why'],
  properties: {
    name: { type: 'string' },
    verdict: { enum: ['DEAD_PREGATE', 'DEAD_BT', 'DEAD_MIRAGE', 'BUILD_CANDIDATE', 'INCONCLUSIVE'] },
    why: { type: 'string' },
    pf: { type: 'string', description: 'faithful BT PF if computed, else n/a' },
    bothHalves: { type: 'boolean' },
    bothRegimes: { type: 'boolean' },
    realCost: { type: 'boolean' },
    mirageClass: { enum: ['none', 'bull-beta', 'squeeze-beta', 'survivorship', 'cost-fragile', 'regime-island'] },
    fileAction: { type: 'string', description: 'exact sb build / wiki entity action to take' },
  },
}

log(`Discovery batch: ${candidates.length} candidate(s)`)

const results = await pipeline(
  candidates,

  // STAGE 1 — tombstone pre-gate (mandatory, cheap, kills re-mining)
  (c) => agent(
    `Pre-mine tombstone gate for strategy candidate "${c.name}": ${c.desc}\n\n` +
    `Run: python3 ~/second-brain/search.py "${c.desc}"\n` +
    `Exit code 2 (a 💀/⚠️ top hit) = ALREADY JUDGED DEAD — read that note. ` +
    `Also check the Omega memory MEMORY.md tombstone list.\n\n` +
    `Return JSON {name, alive(bool), priorVerdict(string|null), basis(string)}. ` +
    `alive=false if a clear tombstone matches and there is NO new basis to re-open.`,
    { label: `gate:${c.name}`, phase: 'Tombstone gate',
      schema: { type: 'object', required: ['name', 'alive', 'basis'],
        properties: { name: { type: 'string' }, alive: { type: 'boolean' },
          priorVerdict: { type: ['string', 'null'] }, basis: { type: 'string' } } } }
  ),

  // STAGE 2 — faithful BT (only if it passed the gate)
  (gate, c) => {
    if (!gate || !gate.alive) {
      return { name: c.name, verdict: 'DEAD_PREGATE', why: gate ? gate.basis : 'gate failed',
        mirageClass: 'none', fileAction: 'already tombstoned — no action' }
    }
    return agent(
      `Faithful backtest of candidate "${c.name}": ${c.desc}\n\n` +
      `MANDATORY per BACKTEST_TRUTH + backtest/ENGINE_BACKTEST_REGISTRY.md:\n` +
      `1. Run backtest/data_integrity_gate.py on every tick file used — a REJECTED file is NOT used.\n` +
      `2. FAITHFUL tick/daily replay, NOT bar-replay (bar overstates ~0.5-0.7 PF).\n` +
      `3. Apply REAL IBKR cost (XAU spot ~$1.6/oz RT; not the old ~0.37 floor).\n` +
      `4. Report PF + net split for BOTH time-halves AND both regimes (2022 bear is the assassin).\n\n` +
      `Return the VERDICT schema with verdict in {DEAD_BT, BUILD_CANDIDATE, INCONCLUSIVE} ` +
      `(mirage check is the next stage — do not pass a clear loser forward).`,
      { label: `bt:${c.name}`, phase: 'Faithful BT', schema: VERDICT }
    )
  },

  // STAGE 3 — adversarial mirage-kill (only survivors of BT)
  (bt, c) => {
    if (!bt || bt.verdict === 'DEAD_PREGATE' || bt.verdict === 'DEAD_BT') return bt
    return parallel(
      ['bull-beta', 'squeeze-beta', 'survivorship', 'cost-fragile', 'regime-island'].map((lens) => () =>
        agent(
          `Adversarially REFUTE candidate "${c.name}" through the ${lens} lens. ` +
          `Its faithful BT looked positive (${bt.pf || 'pf n/a'}). Your job is to prove the edge is ` +
          `actually ${lens}, not real alpha. Default to REFUTED if uncertain — most of this graveyard ` +
          `is exactly this failure. Return {lens, refuted(bool), evidence(string)}.`,
          { label: `kill:${c.name}:${lens}`, phase: 'Mirage kill',
            schema: { type: 'object', required: ['lens', 'refuted', 'evidence'],
              properties: { lens: { type: 'string' }, refuted: { type: 'boolean' },
                evidence: { type: 'string' } } } }
        )
      )
    ).then((votes) => {
      const live = votes.filter(Boolean)
      const refuted = live.filter((v) => v.refuted)
      const survives = refuted.length === 0  // ANY refutation kills it (strict)
      return {
        ...bt,
        verdict: survives ? 'BUILD_CANDIDATE' : 'DEAD_MIRAGE',
        mirageClass: survives ? 'none' : (refuted[0] ? refuted[0].lens : 'unknown'),
        why: survives
          ? `${bt.why} — survived all ${live.length} mirage lenses`
          : `refuted by ${refuted.map((r) => r.lens).join(', ')}`,
      }
    })
  },

  // STAGE 4 — verdict filing instruction (no auto-deploy; build stays gated)
  (v, c) => {
    if (!v) return { name: c.name, verdict: 'INCONCLUSIVE', why: 'pipeline dropped', mirageClass: 'none' }
    const dead = v.verdict.startsWith('DEAD')
    v.fileAction = dead
      ? `sb build (tombstone "${c.name}", basis: ${v.why}); add MEMORY.md + Memory-Omega entity + log.`
      : `BUILD_CANDIDATE — do NOT deploy. Next: adverse-protection backtest (mandatory), wire as SHADOW only, ` +
        `let ledger accumulate n, review before any live size. File Memory-Omega entity + log.`
    return v
  }
)

const out = results.filter(Boolean)
const survivors = out.filter((r) => r.verdict === 'BUILD_CANDIDATE')
log(`Done. ${out.length} judged, ${survivors.length} BUILD_CANDIDATE, ` +
    `${out.length - survivors.length} dead/inconclusive.`)
return { judged: out, survivors }
