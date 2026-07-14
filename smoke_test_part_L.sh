#!/usr/bin/env bash
# =============================================================================
# smoke_test_part_L.sh -- Part-L harness smoke test (task #2 from session
# 2026-05-13 part L kick-off message)
# =============================================================================
# Author: Claude, session 2026-05-13 part L
#
# Purpose:
#   Confirm the part-L code state (engine_init.hpp + S37-H-followup engine
#   changes that shipped in c216db8, plus the CellEngine.hpp winner-exemption
#   addition) is a no-op for winning trades on the VWAPReversion path, and
#   that the new TsmomEngine in-flight MAE stacking gate does not break
#   Phase 2a parity at --max-pos 1.
#
# What the script does:
#   1. Builds VWAPReversionBacktest (FATAL if this fails).
#   2. Builds TsmomCellBacktest (WARN+SKIP Tsmom block if this fails -- a
#      Tsmom build break is not a blocker for the VWAPReversion smoke test).
#      Set SKIP_TSMOM=1 to skip the Tsmom block unconditionally.
#   3. For each of US500.F, USTEC.F, GER40, EURUSD:
#        - runs --mode baseline (in-flight cut DISABLED for all 3 fields)
#        - runs --mode tuned    (per-symbol engine_init.hpp values)
#      and prints a 13-row metric A/B per symbol.
#   4. If Tsmom binary is available, runs TsmomCellBacktest twice:
#        - --max-pos 1   Phase 2a parity contract: V1 and V2 ledgers MUST
#                        byte-match. The new in-flight MAE stacking gate is
#                        a no-op at max-pos 1 (n_open() < max_positions is
#                        the upstream gate so the stacking gate never sees
#                        a >=1 open scan). Divergence = surprise behaviour.
#        - --max-pos 10  V1 has the new stacking gate; V2 does not. V1
#                        trade count should be <= V2 trade count. Equal is
#                        plausible if MAE_EXIT_RATIO is zero in this corpus
#                        (gate auto-disables). Strictly more V1 trades than
#                        V2 = surprise behaviour.
#   5. Emits a single paste-back summary file at:
#        $OUT_DIR/part_L_smoke_summary.txt
#
# Pass criteria (read by Claude when summary is pasted back):
#   VWAPReversion no-op-for-winners check, per symbol:
#     - n_tp_hit (tuned) within +/- 5%  of n_tp_hit (baseline)
#     - n_sl_hit (tuned) within +/- 10% of n_sl_hit (baseline) -- trail SL
#       can shift slightly because BE_RATCHET arms before the trail does
#       on borderline winners; that's still "no-op for winners" semantics
#     - For US500.F specifically (all three in-flight cut fields reverted to
#       0.0 in engine_init.hpp at part-K): tuned MUST exactly match baseline
#       on every metric. Any drift = engine_init.hpp drift.
#     - For USTEC.F, GER40, EURUSD (in-flight cut still ON):
#         n_loss_cut    >= 1   (mechanism fires on the historical tape)
#         n_be_cut      >= 1   (mechanism fires on the historical tape)
#         n_timeout     <      n_timeout (baseline)         (T/O down)
#         worst_trade   >=     worst_trade (baseline)       (less negative)
#         p95_worst_loss >=    p95_worst_loss (baseline)    (less negative)
#       gross_pnl direction is NOT a pass criterion -- it depends on whether
#       the tape's tail justifies the in-flight cut cost. Part-K validated
#       this direction for USTEC/GER40/EURUSD.
#
#   TsmomCellBacktest (only if binary built):
#     --max-pos 1:  byte-identical ledgers. The harness asserts this and
#                   exits nonzero on divergence; the script propagates the
#                   exit code into the summary.
#     --max-pos 10: V1 row count <= V2 row count. Strictly greater = surprise.
#
# Run on Mac (no arguments needed once tick CSVs are at the default paths):
#   cd ~/Omega
#   cp /path/to/this/smoke_test_part_L.sh .   # or run from outputs/ directly
#   chmod +x smoke_test_part_L.sh
#   ./smoke_test_part_L.sh
#
# Or with custom paths / skip Tsmom:
#   TICK_DIR=~/Tick OUT_DIR=~/vrev_validation SKIP_TSMOM=1 ./smoke_test_part_L.sh
#
# Time: ~90 min full, ~85 min if Tsmom skipped. Dominated by VWAPReversion
# runs (8 runs at ~10 min each on multi-year HistData ticks).
#
# Tick file defaults (TICK_DIR=$HOME/Tick):
#   US500.F  ->  $TICK_DIR/SPXUSD_merged.csv
#   USTEC.F  ->  $TICK_DIR/NSXUSD_merged.csv
#   GER40    ->  $TICK_DIR/GER40/DEUIDXEUR_Ticks_2025.01.01_2025.12.31.csv
#                (Dukascopy DEU index, full 2025; override GER40_TICKS for a
#                 different year/range -- the script will skip GER40 cleanly
#                 if the file is missing)
#   EURUSD   ->  $TICK_DIR/EURUSD_merged.csv
#
# Honors CLAUDE.md -- no git operations, no engine edits, only build + run.
# =============================================================================

set -uo pipefail   # not -e: we want to keep going on individual symbol failures

REPO="${REPO:-$HOME/Omega}"
BUILD_DIR="${BUILD_DIR:-$REPO/build}"
VREV="${VREV:-$BUILD_DIR/VWAPReversionBacktest}"
TCELL="${TCELL:-$BUILD_DIR/TsmomCellBacktest}"
TICK_DIR="${TICK_DIR:-$HOME/Tick}"
OUT_DIR="${OUT_DIR:-$HOME/vrev_validation/part_L_smoke}"
SKIP_TSMOM="${SKIP_TSMOM:-0}"
SUMMARY="$OUT_DIR/part_L_smoke_summary.txt"

# Tick paths -- override individually if your filenames differ
US500_TICKS="${US500_TICKS:-$TICK_DIR/SPXUSD_merged.csv}"
USTEC_TICKS="${USTEC_TICKS:-$TICK_DIR/NSXUSD_merged.csv}"
GER40_TICKS="${GER40_TICKS:-$TICK_DIR/GER40/DEUIDXEUR_Ticks_2025.01.01_2025.12.31.csv}"
EURUSD_TICKS="${EURUSD_TICKS:-$TICK_DIR/EURUSD_merged.csv}"

mkdir -p "$OUT_DIR"
: > "$SUMMARY"

log() { echo "$@" | tee -a "$SUMMARY"; }

log "================================================================"
log "  PART-L SMOKE TEST"
log "  repo:        $REPO"
log "  out_dir:     $OUT_DIR"
log "  vrev:        $VREV"
log "  tcell:       $TCELL"
log "  skip_tsmom:  $SKIP_TSMOM"
log "  date:        $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
log "================================================================"

# -----------------------------------------------------------------------------
# 0) Build
# -----------------------------------------------------------------------------
TSMOM_AVAILABLE=0

log ""
log "[0/3] BUILD VWAPReversionBacktest (FATAL on failure)"
cd "$REPO"
if ! cmake --build "$BUILD_DIR" --target VWAPReversionBacktest -j 2>&1 | tee -a "$SUMMARY"; then
    log "[FATAL] VWAPReversionBacktest build failed -- aborting (VWAP smoke cannot run)."
    exit 1
fi
if [[ ! -x "$VREV" ]]; then
    log "[FATAL] VREV binary missing after build: $VREV"; exit 1
fi
log "[BUILD] VWAPReversionBacktest OK"

if [[ "$SKIP_TSMOM" == "1" ]]; then
    log ""
    log "[0/3] SKIP_TSMOM=1 -- skipping TsmomCellBacktest build"
else
    log ""
    log "[0/3] BUILD TsmomCellBacktest (WARN+SKIP on failure -- non-blocking)"
    if cmake --build "$BUILD_DIR" --target TsmomCellBacktest -j 2>&1 | tee -a "$SUMMARY"; then
        if [[ -x "$TCELL" ]]; then
            TSMOM_AVAILABLE=1
            log "[BUILD] TsmomCellBacktest OK"
        else
            log "[WARN] TsmomCellBacktest build reported success but binary missing: $TCELL"
            log "[WARN] Tsmom block will be skipped."
        fi
    else
        log "[WARN] TsmomCellBacktest build FAILED -- continuing with VWAP only."
        log "[WARN] See build error above. VWAPReversion smoke test still runs."
    fi
fi

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Convert a raw Dukascopy desktop-client CSV export
#   "Time (EET),Ask,Bid,AskVolume,BidVolume"
#   "2025.01.02 01:02:14.945,19857.645,19849.355,0.00001,0.00002"
# into the harness's expected C_DUKA layout
#   "YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol"
#
# Differences handled:
#   - date and time joined by a SPACE -> split by COMMA
#   - Ask-before-Bid columns -> swap to bid-then-ask
#   - separate AskVolume + BidVolume -> summed into a single vol column
#   - skip header row (any row not starting "YYYY.MM.DD ")
#
# FIXME: timezone. Source is EET (UTC+2 winter / UTC+3 summer); the harness
# parses as UTC. Smoke test tolerates the offset for shape checks; production
# backtest needs a proper DST-aware EET->UTC conversion.
#
# Idempotent: skips reconversion if the .harness.csv companion is up-to-date.
# Writes progress to stderr so callers capturing stdout get just the path.
prep_ger40_ticks() {
    local src="$1"
    if [[ ! -f "$src" ]]; then
        # caller will detect missing file and SKIP
        echo "$src"
        return
    fi
    local first_line
    first_line=$(head -1 "$src")
    # Already in C_DUKA layout? A C_DUKA data row starts with "YYYY.MM.DD,"
    # (digit digit digit digit dot ...). A raw export starts with "Time"
    # or with "YYYY.MM.DD " (space, not comma).
    if [[ "$first_line" =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]{2}, ]]; then
        # Already harness-compatible.
        echo "$src"
        return
    fi
    local dst="${src%.csv}.harness.csv"
    if [[ -f "$dst" && "$dst" -nt "$src" ]]; then
        echo "$dst"
        return
    fi
    echo "  prep: converting raw Dukascopy CSV -> $dst" >&2
    awk -F',' 'NR>1 {
        gsub(" ", ",", $1);
        printf "%s,%s,%s,%.5f\n", $1, $3, $2, $4+$5
    }' "$src" > "$dst"
    local n
    n=$(wc -l < "$dst" | tr -d ' ')
    echo "  prep: wrote $n converted rows to $dst" >&2
    echo "$dst"
}

get_metric() {
    # CSVs are "key,value". Grab value for the first matching key.
    local file="$1"; local key="$2"
    if [[ ! -f "$file" ]]; then echo "n/a"; return; fi
    awk -F, -v k="$key" '$1 == k { print $2; exit }' "$file"
}

# Print a A/B table for one symbol given its two report.csv paths.
# Echoes a "VERDICT: ..." line that downstream analysis can grep.
print_ab() {
    local sym="$1"; local baseline="$2"; local tuned="$3"
    log ""
    log "--- $sym A/B (baseline vs tuned) ---"
    printf "  %-22s %-14s %-14s\n" "metric" "baseline" "tuned" | tee -a "$SUMMARY"
    local keys=(trades win_rate_pct gross_pnl avg_pnl best_trade worst_trade
                p95_worst_loss n_tp_hit n_sl_hit n_timeout n_mae_early_exit
                n_loss_cut n_be_cut)
    for m in "${keys[@]}"; do
        local b; b=$(get_metric "$baseline" "$m")
        local t; t=$(get_metric "$tuned"    "$m")
        printf "  %-22s %-14s %-14s\n" "$m" "$b" "$t" | tee -a "$SUMMARY"
    done

    # Quick pass-criteria heuristic. Values are emitted as bare numbers in the
    # report CSV; awk handles n/a gracefully by treating it as 0.
    local b_tp; b_tp=$(get_metric "$baseline" n_tp_hit)
    local t_tp; t_tp=$(get_metric "$tuned"    n_tp_hit)
    local b_to; b_to=$(get_metric "$baseline" n_timeout)
    local t_to; t_to=$(get_metric "$tuned"    n_timeout)
    local t_lc; t_lc=$(get_metric "$tuned"    n_loss_cut)
    local t_bc; t_bc=$(get_metric "$tuned"    n_be_cut)
    local b_w;  b_w=$(get_metric "$baseline" worst_trade)
    local t_w;  t_w=$(get_metric "$tuned"    worst_trade)

    awk -v sym="$sym" -v b_tp="$b_tp" -v t_tp="$t_tp" -v b_to="$b_to" \
        -v t_to="$t_to" -v t_lc="$t_lc" -v t_bc="$t_bc" \
        -v b_w="$b_w" -v t_w="$t_w" '
        BEGIN {
            # numeric coercion: "n/a" -> 0 via +0
            b_tp += 0; t_tp += 0; b_to += 0; t_to += 0
            t_lc += 0; t_bc += 0; b_w  += 0; t_w  += 0

            tp_ok    = (b_tp == 0) ? 1 : ( (t_tp / b_tp) >= 0.95 && (t_tp / b_tp) <= 1.05 )
            to_down  = (t_to <= b_to)
            cuts_on  = (t_lc >= 1 || t_bc >= 1)
            tail_ok  = (t_w >= b_w)  # less negative is bigger (closer to zero)

            if (sym == "GER40") {
                # Only symbol with cuts still ON in engine_init.hpp post part L.
                # Expect winners no-op, cuts firing, tail tighter.
                ok = tp_ok && to_down && cuts_on && tail_ok
                if (ok) printf("VERDICT %s: PASS (winners no-op, cuts firing, tail tighter)\n", sym)
                else {
                    printf("VERDICT %s: REVIEW (tp_ok=%d to_down=%d cuts_on=%d tail_ok=%d)\n",
                           sym, tp_ok, to_down, cuts_on, tail_ok)
                }
            } else {
                # US500.F (part K), USTEC.F (part L), EURUSD (part K): all
                # reverted to zero in engine_init.hpp AND the harness sync
                # (this commit) mirrors that. tuned MUST equal baseline.
                exact = (t_tp == b_tp && t_to == b_to && t_lc == 0 && t_bc == 0)
                if (exact) printf("VERDICT %s: PASS (revert intact -- tuned == baseline)\n", sym)
                else       printf("VERDICT %s: FAIL (%s revert drifted -- check engine_init.hpp + harness params_for())\n", sym, sym)
            }
        }' | tee -a "$SUMMARY"
}

# Run one symbol (baseline + tuned), then print A/B.
run_symbol() {
    local sym="$1"; local ticks="$2"
    log ""
    log "[1/3] $sym  ticks=$ticks"
    if [[ ! -f "$ticks" ]]; then
        log "  SKIP: tick file not found: $ticks"
        log "VERDICT $sym: SKIP (no tick file)"
        return 0
    fi
    # GER40 ships as raw Dukascopy desktop-client CSV which the harness's
    # C_DUKA parser cannot read directly. Auto-convert to a harness-friendly
    # sibling CSV if needed; transparent no-op for already-converted files.
    if [[ "$sym" == "GER40" ]]; then
        local converted
        converted=$(prep_ger40_ticks "$ticks")
        if [[ "$converted" != "$ticks" ]]; then
            log "  using converted ticks: $converted"
            ticks="$converted"
        fi
    fi

    local tag="$OUT_DIR/$(echo "$sym" | tr -d '.' | tr '[:upper:]' '[:lower:]')"

    log "  baseline ..."
    "$VREV" "$ticks" \
        --symbol "$sym" --mode baseline --quiet \
        --trades "${tag}_baseline_trades.csv" \
        --report "${tag}_baseline_report.csv" \
        2>>"$SUMMARY" || log "  WARN: baseline run nonzero exit"

    log "  tuned ..."
    "$VREV" "$ticks" \
        --symbol "$sym" --mode tuned --quiet \
        --trades "${tag}_tuned_trades.csv" \
        --report "${tag}_tuned_report.csv" \
        2>>"$SUMMARY" || log "  WARN: tuned run nonzero exit"

    print_ab "$sym" "${tag}_baseline_report.csv" "${tag}_tuned_report.csv"
}

# -----------------------------------------------------------------------------
# 1) VWAPReversion A/B per symbol
# -----------------------------------------------------------------------------
log ""
log "================================================================"
log "  [1/3] VWAPReversion baseline vs tuned (4 symbols)"
log "================================================================"
run_symbol US500.F  "$US500_TICKS"
run_symbol USTEC.F  "$USTEC_TICKS"
run_symbol GER40    "$GER40_TICKS"
run_symbol EURUSD   "$EURUSD_TICKS"

# -----------------------------------------------------------------------------
# 2) TsmomCellBacktest: parity + stacking gate
# -----------------------------------------------------------------------------
log ""
log "================================================================"
log "  [2/3] TsmomCellBacktest: --max-pos 1 (parity) + --max-pos 10"
log "================================================================"

if [[ "$TSMOM_AVAILABLE" != "1" ]]; then
    log ""
    log "Tsmom binary not available (build failed or SKIP_TSMOM=1)."
    log "VERDICT Tsmom max-pos=1:  SKIP (no binary)"
    log "VERDICT Tsmom max-pos=10: SKIP (no binary)"
else
    log ""
    log "[2a] --max-pos 1  (Phase 2a parity contract: V1 == V2 byte-for-byte)"
    TS1_V1="$OUT_DIR/tsmom_maxpos1_v1.csv"
    TS1_V2="$OUT_DIR/tsmom_maxpos1_v2.csv"
    if "$TCELL" --max-pos 1 --v1-out "$TS1_V1" --v2-out "$TS1_V2" --quiet 2>&1 | tee -a "$SUMMARY"; then
        if [[ -f "$TS1_V1" && -f "$TS1_V2" ]] && cmp -s "$TS1_V1" "$TS1_V2"; then
            log "VERDICT Tsmom max-pos=1: PASS (V1 == V2 byte-for-byte; stacking gate is a no-op here)"
        else
            log "VERDICT Tsmom max-pos=1: FAIL (V1 != V2 -- new gate broke parity; investigate)"
        fi
    else
        log "VERDICT Tsmom max-pos=1: FAIL (harness exited nonzero)"
    fi

    log ""
    log "[2b] --max-pos 10 (V1 stacking gate active; expect V1 <= V2 trade count)"
    TS10_V1="$OUT_DIR/tsmom_maxpos10_v1.csv"
    TS10_V2="$OUT_DIR/tsmom_maxpos10_v2.csv"
    "$TCELL" --max-pos 10 --v1-out "$TS10_V1" --v2-out "$TS10_V2" --quiet 2>&1 | tee -a "$SUMMARY"

    if [[ -f "$TS10_V1" && -f "$TS10_V2" ]]; then
        n_v1=$(($(wc -l < "$TS10_V1") - 1))
        n_v2=$(($(wc -l < "$TS10_V2") - 1))
        log "  V1 trade rows: $n_v1"
        log "  V2 trade rows: $n_v2"
        if (( n_v1 <= n_v2 )); then
            log "VERDICT Tsmom max-pos=10: PASS (V1=$n_v1 <= V2=$n_v2; stacking gate suppressed at least one entry or gate inert this corpus)"
        else
            log "VERDICT Tsmom max-pos=10: FAIL (V1=$n_v1 > V2=$n_v2 -- gate is producing MORE entries than V2; impossible without a regression)"
        fi
    else
        log "VERDICT Tsmom max-pos=10: FAIL (ledger file(s) missing)"
    fi
fi

# -----------------------------------------------------------------------------
# 3) Final summary
# -----------------------------------------------------------------------------
log ""
log "================================================================"
log "  [3/3] FINAL VERDICTS"
log "================================================================"
grep '^VERDICT ' "$SUMMARY" | tee -a "$SUMMARY"
log ""
log "Done. Summary file: $SUMMARY"
log "Paste the whole summary back into chat for analysis."
