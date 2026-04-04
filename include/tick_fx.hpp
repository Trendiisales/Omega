// tick_fx.hpp — per-symbol tick handlers
// Extracted from on_tick(). Same translation unit — all static functions visible.
// Do NOT #include from anywhere else.

// ── XAGUSD ─────────────────────────────────────────────────
static void on_tick_silver(const std::string& sym, double bid, double ask) {
    // XAGUSD FULLY DISABLED -- real-tick backtest on 42M ticks (Jan 2023-Jan 2025) FAILED.
    //
    // SilverTurtleTickEngine result (N=20, TBS=300, SL=$0.10, TP=$0.30):
    //   Sharpe=-16.23  MaxDD=$18,381  Trades=8616  WR=31.8%
    //   0/24 positive months. Losing every single month for 2 years.
    //
    // ROOT CAUSE: 65% of trades exit via TIMEOUT (45min), avg gain only $0.61.
    //   TP=$0.30 requires 49x the actual avg 45-min price move.
    //   Silver reverts faster than gold -- Turtle architecture not viable.
    //
    // FULL AUDIT (12 strategies on synthetic ticks) -- all rejected:
    //   Compression breakout: Sharpe -1.25  (confirmed broken on real data too)
    //   Session momentum:     Sharpe -0.00
    //   VWAP Stretch:         Sharpe  1.68  (below threshold)
    //   NBM COMEX:            Sharpe  1.73  (below threshold)
    //   COMEX ORB:            Sharpe  1.42  (below threshold)
    //   TurtleTick (real):    Sharpe -16.23 (catastrophic fail on real data)
    //
    // CONCLUSION: No viable silver engine exists. Silver stays disabled.
    // Do not re-enable without a fundamentally different approach validated
    // on this same 42M-tick dataset.
    (void)bid; (void)ask;
}
}

// ── EURUSD ─────────────────────────────────────────────────
static void on_tick_eurusd(const std::string& sym, double bid, double ask) {
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;  // for wall_above/below context
    const bool base_can_fx = symbol_gate("EURUSD",
        g_eng_eurusd.pos.active                ||
        g_bracket_eurusd.pos.active            ||
        g_vwap_rev_eurusd.has_open_position(), "", tradeable, lat_ok, regime, bid, ask); // ADDED
    const auto sdec_fx = sup_decision(g_sup_eurusd, g_eng_eurusd, base_can_fx, sym, bid, ask);
    // Notify FX cascade engine if EURUSD just fired a signal this tick
    {
        static bool s_eur_was_armed = false;
        const bool eur_armed_now = (g_eng_eurusd.phase == omega::Phase::BREAKOUT_WATCH);
        // Detect transition: was armed, now FLAT with active pos = signal fired
        if (s_eur_was_armed && !eur_armed_now && g_eng_eurusd.pos.active)
            g_ca_fx_cascade.notify_eurusd_signal(g_eng_eurusd.pos.is_long);
        s_eur_was_armed = eur_armed_now;
    }
    if (sdec_fx.allow_breakout && !g_bracket_eurusd.pos.active)
        dispatch(g_eng_eurusd, g_sup_eurusd, base_can_fx, &sdec_fx);
    // SIM: BracketEngine on FX -- no edge. 25 trades WR 20% -$935. Disabled.
    // if (sdec_fx.allow_bracket && !g_eng_eurusd.pos.active)
    //     dispatch_bracket(g_bracket_eurusd, ...);
    // ?? EURUSD manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_vwap_rev_eurusd.has_open_position()) {
        auto vwap_eur_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == "TP_HIT") g_vwap_rev_eurusd.notify_tp_hit(tr.side == "LONG");
            ca_on_close(tr);
        };
        g_vwap_rev_eurusd.on_tick(sym, bid, ask, 0.0, vwap_eur_cb);
    }

    // VWAP Reversion: EURUSD -- anchored to London open (08:00 UTC).
    // EUR/USD primary session is London (08:00-17:00 UTC).
    // Using midnight UTC gave a stale 8hr anchor by NY open.
    if (!g_vwap_rev_eurusd.has_open_position() && base_can_fx) {
        static double  s_eur_london_open     = 0.0;
        static int     s_eur_london_open_day = -1;
        {
            const auto t_eur = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            struct tm ti_eur; gmtime_s(&ti_eur, &t_eur);
            const bool in_london = (ti_eur.tm_hour >= 8);
            if (in_london && ti_eur.tm_yday != s_eur_london_open_day) {
                s_eur_london_open     = (bid + ask) * 0.5;
                s_eur_london_open_day = ti_eur.tm_yday;
                g_vwap_rev_eurusd.reset_ewm_vwap(s_eur_london_open);
            }
        }
        if (s_eur_london_open > 0.0) {
            const auto vr = g_vwap_rev_eurusd.on_tick(sym, bid, ask, s_eur_london_open, ca_on_close,
                g_macro_ctx.vix, g_macro_ctx.eur_l2_imbalance);
            if (vr.valid) {
                g_telemetry.UpdateLastSignal("EURUSD", vr.is_long?"LONG":"SHORT", vr.entry, vr.reason, "VWAP_REV", regime.c_str(), "VWAP_REV", vr.tp, vr.sl);
                const double conf_mult = (vr.confluence_score >= 4) ? 3.0 :
                                        (vr.confluence_score == 3) ? 2.0 :
                                        (vr.confluence_score == 2) ? 1.5 : 1.0;
                if (!enter_directional("EURUSD", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true, bid, ask, sym, regime))
                    g_vwap_rev_eurusd.cancel();
                else g_vwap_rev_eurusd.patch_size(g_last_directional_lot);
            }
        }
    }
    // London fix window (20:55-21:00 UTC) -- WM/Reuters benchmark rebalancing
    if (base_can_fx) {
        const omega::edges::CVDState eur_cvd = g_edges.cvd.get("EURUSD");
        const auto fix_sig = g_edges.fx_fix.on_tick_london("EURUSD", bid, ask, eur_cvd.normalised(), nowSec());
        if (fix_sig.valid) {
            g_telemetry.UpdateLastSignal("EURUSD", fix_sig.is_long?"LONG":"SHORT", fix_sig.entry, fix_sig.reason, "FX_FIX", regime.c_str(), "LONDON_FIX", fix_sig.tp, fix_sig.sl);
            enter_directional("EURUSD", fix_sig.is_long, fix_sig.entry, fix_sig.sl, fix_sig.tp, 0.01, true, bid, ask, sym, regime);
        }
    }
}
}

// ── GBPUSD ─────────────────────────────────────────────────
static void on_tick_gbpusd(const std::string& sym, double bid, double ask) {
    g_macro_ctx.gbp_mid_price = (bid + ask) * 0.5;  // for wall_above/below context
    // ?? FX group bracket guard -- only one bracket across GBPUSD/AUDUSD/NZDUSD/USDJPY ??
    // These four pairs share high USD-correlation: simultaneous brackets create
    // duplicated directional USD exposure. EURUSD is intentionally excluded --
    // different liquidity tier and independent signal flow.
    // any_fx_bracket_active removed: GBPUSD bracket disabled (SIM: 26T WR 15%)
    const bool base_can_fx2 = symbol_gate("GBPUSD",
        g_eng_gbpusd.pos.active                    ||
        g_bracket_gbpusd.pos.active                ||
        g_ca_fx_cascade.has_open_gbpusd(), "", tradeable, lat_ok, regime, bid, ask);         // ADDED
    const auto sdec_fx2 = sup_decision(g_sup_gbpusd, g_eng_gbpusd, base_can_fx2, sym, bid, ask);
    if (sdec_fx2.allow_breakout && !g_bracket_gbpusd.pos.active)
        dispatch(g_eng_gbpusd, g_sup_gbpusd, base_can_fx2, &sdec_fx2);
    // SIM: BracketEngine on FX -- no edge. 26 trades WR 15% -$1272. Disabled.
    // if (sdec_fx2.allow_bracket && !g_eng_gbpusd.pos.active && !any_fx_bracket_active)
    //     dispatch_bracket(g_bracket_gbpusd, ...);
    // ?? GBPUSD manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_ca_fx_cascade.has_open_gbpusd()) { g_ca_fx_cascade.on_tick_gbpusd(bid, ask, ca_on_close); }

    // FX cascade: EURUSD-driven GBPUSD entry.
    // Gate: per-leg check (not aggregate) so AUD/NZD legs can fire simultaneously.
    if (!g_ca_fx_cascade.has_open_gbpusd() && base_can_fx2) {
        const auto cas = g_ca_fx_cascade.on_tick_gbpusd(bid, ask, ca_on_close);
        if (cas.valid) {
            g_telemetry.UpdateLastSignal("GBPUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
            if (!enter_directional("GBPUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                    g_ca_fx_cascade.cancel_gbpusd();
                else g_ca_fx_cascade.patch_size_gbp(g_last_directional_lot);
        }
    }
}
}

// ── AUDUSD/NZDUSD/USDJPY ───────────────────────────────────
static void on_tick_audusd(const std::string& sym, double bid, double ask) {
    // Update live USDJPY rate for dynamic tick_value_multiplier()
    if (sym == "USDJPY") g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);
    // ?? FX group bracket guard -- shared across GBPUSD/AUDUSD/NZDUSD/USDJPY ??
    const bool any_fx_bracket_active =
        g_bracket_gbpusd.has_open_position() ||
        g_bracket_audusd.has_open_position() ||
        g_bracket_nzdusd.has_open_position() ||
        g_bracket_usdjpy.has_open_position();
    const auto t2 = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti2; gmtime_s(&ti2, &t2);
    const int h2 = ti2.tm_hour;
    const bool asia_ok = !g_cfg.asia_fx_asia_only || (h2 >= 22 || h2 < 7);
    if (asia_ok) {
        if (sym == "AUDUSD") {
            const bool bc_aud = symbol_gate("AUDUSD", g_eng_audusd.pos.active || g_bracket_audusd.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_aud = sup_decision(g_sup_audusd, g_eng_audusd, bc_aud, sym, bid, ask);
            if (sd_aud.allow_breakout && !g_bracket_audusd.pos.active) dispatch(g_eng_audusd, g_sup_audusd, bc_aud, &sd_aud);
            if (sd_aud.allow_bracket && !g_eng_audusd.pos.active && !any_fx_bracket_active)
                // SIM: BracketEngine on FX -- no edge. Disabled.
                // dispatch_bracket(g_bracket_audusd, ...);
            // ?? AUDUSD manage block -- ALWAYS run when position open (SL/trail fix) ??
            if (g_ca_fx_cascade.has_open_audusd()) { g_ca_fx_cascade.on_tick_audusd(bid, ask, ca_on_close); }

            // FX cascade: EURUSD-driven AUDUSD entry (0.73 corr).
            // Per-leg gate: fires independently of GBP/NZD leg state.
            if (!g_ca_fx_cascade.has_open_audusd() && bc_aud) {
                const auto cas = g_ca_fx_cascade.on_tick_audusd(bid, ask, ca_on_close);
                if (cas.valid) {
                    g_telemetry.UpdateLastSignal("AUDUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
                    if (!enter_directional("AUDUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                        g_ca_fx_cascade.cancel_audusd();
                        else g_ca_fx_cascade.patch_size_aud(g_last_directional_lot);
                }
            }
        }
        if (sym == "NZDUSD") {
            const bool bc_nzd = symbol_gate("NZDUSD", g_eng_nzdusd.pos.active || g_bracket_nzdusd.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_nzd = sup_decision(g_sup_nzdusd, g_eng_nzdusd, bc_nzd, sym, bid, ask);
            if (sd_nzd.allow_breakout && !g_bracket_nzdusd.pos.active) dispatch(g_eng_nzdusd, g_sup_nzdusd, bc_nzd, &sd_nzd);
            // SIM: BracketEngine on FX -- no edge. Disabled.
            // if (sd_nzd.allow_bracket && !g_eng_nzdusd.pos.active && !any_fx_bracket_active)
            //     dispatch_bracket(g_bracket_nzdusd, ...);
            // ?? NZDUSD manage block -- ALWAYS run when position open (SL/trail fix) ??
            if (g_ca_fx_cascade.has_open_nzdusd()) { g_ca_fx_cascade.on_tick_nzdusd(bid, ask, ca_on_close); }

            // FX cascade: EURUSD-driven NZDUSD entry (0.69 corr).
            // Per-leg gate: fires independently of GBP/AUD leg state.
            if (!g_ca_fx_cascade.has_open_nzdusd() && bc_nzd) {
                const auto cas = g_ca_fx_cascade.on_tick_nzdusd(bid, ask, ca_on_close);
                if (cas.valid) {
                    g_telemetry.UpdateLastSignal("NZDUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
                    if (!enter_directional("NZDUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                        g_ca_fx_cascade.cancel_nzdusd();
                        else g_ca_fx_cascade.patch_size_nzd(g_last_directional_lot);
                }
            }
        }
        if (sym == "USDJPY") {
            const bool bc_jpy = symbol_gate("USDJPY", g_eng_usdjpy.pos.active || g_bracket_usdjpy.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_jpy = sup_decision(g_sup_usdjpy, g_eng_usdjpy, bc_jpy, sym, bid, ask);
            if (sd_jpy.allow_breakout && !g_bracket_usdjpy.pos.active) dispatch(g_eng_usdjpy, g_sup_usdjpy, bc_jpy, &sd_jpy);
            // SIM: BracketEngine on FX -- no edge. Disabled.
            // if (sd_jpy.allow_bracket && !g_eng_usdjpy.pos.active && !any_fx_bracket_active)
            //     dispatch_bracket(g_bracket_usdjpy, ...);
            // ?? USDJPY manage block -- ALWAYS run when position open (SL/trail fix) ??
            if (g_ca_carry_unwind.has_open_position()) { g_ca_carry_unwind.on_tick(bid, ask, g_macro_ctx.vix, ca_on_close); }

            // Carry unwind: VIX spike + JPY bid
            if (!g_ca_carry_unwind.has_open_position() && bc_jpy) {
                const auto cu = g_ca_carry_unwind.on_tick(bid, ask, g_macro_ctx.vix, ca_on_close);
                if (cu.valid) {
                    g_telemetry.UpdateLastSignal("USDJPY", cu.is_long?"LONG":"SHORT", cu.entry, cu.reason, "CARRY_UNWIND", regime.c_str(), "CARRY_UNWIND", cu.tp, cu.sl);
                    if (!enter_directional("USDJPY", cu.is_long, cu.entry, cu.sl, cu.tp, 0.01, true, bid, ask, sym, regime))
                        g_ca_carry_unwind.cancel();
                else g_ca_carry_unwind.patch_size(g_last_directional_lot);
                }
            }
            // Tokyo fix window (00:55-01:00 UTC) -- mechanical JPY rebalancing flow
            if (bc_jpy) {
                const omega::edges::CVDState jpy_cvd = g_edges.cvd.get("USDJPY");
                const auto fix_sig = g_edges.fx_fix.on_tick_tokyo(bid, ask, jpy_cvd.normalised(), nowSec());
                if (fix_sig.valid) {
                    g_telemetry.UpdateLastSignal("USDJPY", fix_sig.is_long?"LONG":"SHORT", fix_sig.entry, fix_sig.reason, "FX_FIX", regime.c_str(), "TOKYO_FIX", fix_sig.tp, fix_sig.sl);
                    enter_directional("USDJPY", fix_sig.is_long, fix_sig.entry, fix_sig.sl, fix_sig.tp, 0.01, true, bid, ask, sym, regime);
                }
            }
        }
    }
}
}

