# Heap-corruption diagnostic toolkit (2026-04-30 PM)

Tooling and analysis to diagnose and mitigate the heap corruption (`0xc0000374`
fail-fast trap at `ntdll!RtlpHpHeapHandleError+0x103e89`) that has been crashing
`Omega.exe` since 2026-04-29 13:39 UTC.

This branch is **purely additive** -- merging it adds the `ops/heap-corruption/`
folder but does NOT change `include/engine_init.hpp` or any other production
source. The variants in `variant-a-tsmom-only/` and `variant-b-all-four/` take
effect only when explicitly copied over `include/engine_init.hpp` and the
binary is rebuilt.

## What's here

| Path | Purpose |
|---|---|
| `DEPLOYMENT_AND_ASSESSMENT.md` | Diagnostic experiment plan, success/failure criteria, deployment paths, risk register |
| `ROOT_CAUSE_AND_HARDENING.md` | 5 ranked root-cause hypotheses with falsification tests + 16-item hardening menu (short / medium / long-term) |
| `variant-a-tsmom-only/engine_init.hpp` | RECOMMENDED FIRST DEPLOY: full file with `g_tsmom.enabled = false` only (1-line change vs main:include/engine_init.hpp:493) |
| `variant-a-tsmom-only/engine_init.diff` | Unified diff for variant A (1 line) |
| `variant-b-all-four/engine_init.hpp` | FALLBACK: disables tsmom + donchian + ema_pullback + trend_rider (4-line change) |
| `variant-b-all-four/engine_init.diff` | Unified diff for variant B (4 lines) |
| `scripts/enable_appverif_ust.ps1` | Configures AppVerif (Locks + Memory + Handles, no Heaps) + gflags +ust + WER full minidumps. Reversible. |
| `scripts/disable_appverif_ust.ps1` | Clean rollback of every change made by `enable_*` |
| `scripts/capture_status.ps1` | Read-only health check; decodes VerifierFlags, lists recent dumps + crash events |
| `scripts/analyse_dump.ps1` | Wrapper that finds latest dump and runs cdb against it; saves analysis to `<dump>.analysis.txt` |
| `scripts/analyse_dump.windbg` | WinDbg command script (`!analyze -v`, `kP`, `!heap -p -a`, thread inventory, AppVerif stops) |

## End-to-end diagnostic flow

1. **Pre-condition**: stand up a non-prod Windows host with at least 8 GB RAM and the
   matching MSVC toolchain (17.14.40+3e7442088, Win 10.0.20348 SDK). Building
   on the production VPS is NOT viable -- `cl.exe` OOMs at the current 1.6 GB
   free state. This is the bottleneck for everything else; fix it first.
2. **Build variant A**: copy `variant-a-tsmom-only/engine_init.hpp` over
   `include/engine_init.hpp`, then `cmake -B build -DCMAKE_BUILD_TYPE=Release`
   and `cmake --build build --target Omega --config Release`.
3. **Deploy**: `Stop-Service Omega`; `Copy-Item C:\Omega\Omega.exe C:\Omega\Omega.exe.bak-pre-disable-2026-04-30`;
   copy `build\Release\Omega.exe` over `C:\Omega\Omega.exe`; `Start-Service Omega`.
4. **Watch for 60-90 minutes**. If no `0xc0000374` event fires in the
   Application log, Tsmom is the trigger and bug is localised to one ~744-line
   file. If crashes continue, redo with variant B (disable all four new
   engines).
5. **If clean**: re-enable Tsmom on a NON-PROD replica with the AppVerif/+ust
   bundle running. Wait for next crash. Run `scripts\analyse_dump.ps1`. The
   `!heap -p -a` output will name the data structure being corrupted.
6. **If still crashing**: PageHeap on non-prod is the only remaining option.
   Documentation already in the prior session handoff.

Detailed steps and risk register in `DEPLOYMENT_AND_ASSESSMENT.md`.

## Source commit audited

`5aed21a` (origin/main as of 2026-04-30 12:21 NZST). Branch was created from
this commit. No merges from main were applied during the diagnostic work, so
nothing in this branch interacts with downstream changes on main.

## Safety notes

- `engine_init.hpp` variants here ARE NOT applied to `include/engine_init.hpp`
  on this branch. Merging this branch does not disable any engine.
- The PowerShell scripts modify HKLM registry keys for AppVerif/gflags/WER
  configuration. They snapshot existing state to
  `C:\Omega\logs\appverif_ust_ifeo_snapshot.json` and the disable script
  restores from snapshot.
- AppVerif in this configuration uses Locks + Memory + Handles checks ONLY.
  Heaps (the PageHeap-equivalent) is intentionally OFF for the first attempt
  to keep memory overhead at 30-50% rather than the 5-10x PageHeap full would
  cost on a 3 GB VPS.
- Production `Omega.exe` continues running unchanged until you choose to
  build and deploy one of the variants.
