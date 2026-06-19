# DISABLED-ENGINE PURGE — operator directive 2026-06-19
#
# Operator: "get rid of these disabled engines, log it and delete them".
# All 115 engine instances with enabled=false (or disabled via g_disable_* global).
# 88 are inline config-instances in engine_init.hpp (shared classes); rest have headers.
# Code + all wiring (globals/init/register/heartbeat/open_positions/persistence/dispatch)
# removed. Git history preserves them. The 17 ENABLED engines are KEPT untouched:
#   g_bigcap_momo g_dj30_turtle_d1 g_fx_xrev_eurgbp g_gold_orb_retrace g_gold_volbrk_m30 g_idx_bear_short_nas g_nas_turtle_d1 g_regime_adaptor g_spx_turtle_d1 g_survivor g_trend_rider g_xau_sess_nypm g_xau_tf_1h g_xau_tf_2h g_xau_tf_4h g_xau_tf_d1 g_xau_threebar_30m 
#
# global	declaring_header	wiring_site_files
g_adhull_ger	INLINE_in_engine_init	4
g_adhull_xau	INLINE_in_engine_init	4
g_amr_audusd	INLINE_in_engine_init	3
g_amr_eurgbp	INLINE_in_engine_init	3
g_amr_eurusd	AtrMeanRevGridEngine.hpp	5
g_amr_gbpusd	INLINE_in_engine_init	4
g_amr_ger40	INLINE_in_engine_init	4
g_amr_nas100	INLINE_in_engine_init	4
g_amr_nzdusd	INLINE_in_engine_init	3
g_amr_us500	INLINE_in_engine_init	4
g_audusd_turtle_h4	INLINE_in_engine_init	4
g_bband_scalp	XauBBScalpD1Engine.hpp	5
g_break_d1	INLINE_in_engine_init	0
g_c1_retuned	C1RetunedPortfolio.hpp	5
g_ca_brent_wti	INLINE_in_engine_init	7
g_ca_carry_unwind	INLINE_in_engine_init	6
g_ca_eia_fade	INLINE_in_engine_init	7
g_ca_esnq	engine_config.hpp	8
g_ca_fx_cascade	INLINE_in_engine_init	5
g_connors_nas	INLINE_in_engine_init	4
g_donchian	DonchianEngine.hpp	7
g_ema_pullback	INLINE_in_engine_init	3
g_eur_gbp_pairs	INLINE_in_engine_init	3
g_eurusd_turtle_h4	INLINE_in_engine_init	4
g_fvgcont_nas	INLINE_in_engine_init	4
g_fvgcont_nas10	INLINE_in_engine_init	4
g_fvgcont_nas30	INLINE_in_engine_init	3
g_fx_scalp_audusd	INLINE_in_engine_init	3
g_fx_scalp_eurusd	retired_micro_engines.hpp	4
g_fx_scalp_gbpusd	INLINE_in_engine_init	3
g_fx_scalp_usdcad	INLINE_in_engine_init	3
g_fx_scalp_usdjpy	INLINE_in_engine_init	3
g_gbpusd_turtle_h4	INLINE_in_engine_init	4
g_ger40_kelt	Ger40KeltnerH1Engine.hpp	6
g_ger40_london_brk	INLINE_in_engine_init	3
g_ger40_turtle_h4	INLINE_in_engine_init	4
g_gold	GoldEngineStack.hpp	1
g_gold_oversold	INLINE_in_engine_init	4
g_gold_panic_bounce	INLINE_in_engine_init	3
g_gold_regime_daily	INLINE_in_engine_init	4
g_gold_scalp_pyramid	retired_micro_engines.hpp	5
g_gold_seasonal	INLINE_in_engine_init	4
g_gold_ultimate_engine	GoldUltimateEngine.hpp	4
g_h1	INLINE_in_engine_init	0
g_h4_regime_gold	INLINE_in_engine_init	5
g_idd_sp	INLINE_in_engine_init	3
g_idd_uk100	INLINE_in_engine_init	3
g_idd_us30	INLINE_in_engine_init	3
g_idx_bear_short_sp	INLINE_in_engine_init	3
g_macro_crash	MacroCrashEngine.hpp	7
g_minimal_h4_ger40	INLINE_in_engine_init	4
g_minimal_h4_gold	XauTrendFollow4hEngine.hpp	8
g_minimal_h4_us30	engine_config.hpp	8
g_nas_orb_retrace	INLINE_in_engine_init	3
g_nbm_gold_london	XauNbmD1Engine.hpp	10
g_nbm_nas	XauNbmD1Engine.hpp	8
g_nbm_nq	XauNbmD1Engine.hpp	8
g_nbm_oil_london	XauNbmD1Engine.hpp	8
g_nbm_sp	XauNbmD1Engine.hpp	8
g_nbm_us30	XauNbmD1Engine.hpp	8
g_nq	INLINE_in_engine_init	0
g_nq_momentum	INLINE_in_engine_init	3
g_nzdusd_turtle_h4	INLINE_in_engine_init	4
g_orb_dj30	INLINE_in_engine_init	3
g_orb_estx50	INLINE_in_engine_init	8
g_orb_estx50_v2	INLINE_in_engine_init	3
g_orb_ger30	INLINE_in_engine_init	8
g_orb_nas100	INLINE_in_engine_init	3
g_orb_uk100	INLINE_in_engine_init	8
g_orb_us	INLINE_in_engine_init	8
g_overnight_nas	INLINE_in_engine_init	4
g_overnight_spx	INLINE_in_engine_init	3
g_pdhl_rev	INLINE_in_engine_init	4
g_peachy_orb_nas	INLINE_in_engine_init	3
g_pump_manager	INLINE_in_engine_init	3
g_rsi_extreme	INLINE_in_engine_init	4
g_rsi_reversal	INLINE_in_engine_init	6
g_sp	INLINE_in_engine_init	0
g_supertrend_gold	INLINE_in_engine_init	4
g_trend_pb_ger40	INLINE_in_engine_init	8
g_trend_pb_gold	EMACrossEngine.hpp	12
g_trend_pb_nq	INLINE_in_engine_init	9
g_trend_pb_sp	INLINE_in_engine_init	8
g_trendline_break	INLINE_in_engine_init	3
g_trendline_break_gbp	INLINE_in_engine_init	3
g_trendline_break_jpy	INLINE_in_engine_init	3
g_tsmom	DonchianEngine.hpp	5
g_tsmom_v2	INLINE_in_engine_init	4
g_us30_3bar_mom_h1	INLINE_in_engine_init	3
g_us30_ensemble	Us30EnsembleEngine.hpp	4
g_usdjpy_turtle_h4	INLINE_in_engine_init	4
g_ustec_tf_5m	UstecTrendFollow5mEngine.hpp	5
g_ustec_tf_htf	UstecTrendFollowHtfEngine.hpp	5
g_vwap_rev_eurusd	INLINE_in_engine_init	7
g_vwap_rev_ger40	INLINE_in_engine_init	8
g_vwap_rev_nq	INLINE_in_engine_init	8
g_vwap_rev_sp	CrossAssetEngines.hpp	10
g_xau_3bar_mom_h4	INLINE_in_engine_init	3
g_xau_bb_scalp_d1	INLINE_in_engine_init	3
g_xau_breakbounce	INLINE_in_engine_init	3
g_xau_d55_gated_m30	INLINE_in_engine_init	3
g_xau_doji_rej_d1	INLINE_in_engine_init	3
g_xau_ema_cross_h4	INLINE_in_engine_init	3
g_xau_inside_bar_d1	INLINE_in_engine_init	3
g_xau_nbm_d1	INLINE_in_engine_init	3
g_xau_outside_bar_d1	INLINE_in_engine_init	3
g_xau_pullback_cont_d1	INLINE_in_engine_init	3
g_xau_pullback_cont_h4	INLINE_in_engine_init	3
g_xau_sess_overnight	SessionMomentumEngine.hpp	4
g_xau_stop_run_d1	INLINE_in_engine_init	3
g_xau_straddle_m15	INLINE_in_engine_init	4
g_xau_straddle_m30	INLINE_in_engine_init	4
g_xau_tf_m15	INLINE_in_engine_init	3
g_xau_tsmom_fast_d1	INLINE_in_engine_init	3
g_xau_turtle_d1	engine_seed_helpers.hpp	5

## REMAINING (batch 1 → green) — woven dispatch, surgical removal needed:
PositionPersistence.hpp:261:    wire_cross(g_overnight_nas,  "OvernightDrift",      "NAS100");   // overnight 
PositionPersistence.hpp:262:    wire_cross(g_connors_nas,    "ConnorsRSI2",         "NAS100");   // RSI2 dip-b
PositionPersistence.hpp:263:    wire_cross(g_adhull_xau,     "AdaptiveHullXAU",     "XAUUSD");   // adaptive-H
PositionPersistence.hpp:264:    wire_cross(g_adhull_ger,     "AdaptiveHullGER",     "GER40");    // adaptive-H
PositionPersistence.hpp:265:    wire_cross(g_supertrend_gold,"SupertrendGold",      "XAUUSD");   // Supertrend
quote_loop.hpp:802:          snap_px("US500.F",b,a); if(b>0&&a>0){g_ca_esnq.force_close(b,a,shutdown_cb,"SHUTD
quote_loop.hpp:803:          snap_px("USTEC.F",b,a); if(b>0&&a>0){g_vwap_rev_nq.force_close(b,a,shutdown_cb,"S
quote_loop.hpp:804:          snap_px("NAS100",b,a);  if(b>0&&a>0){g_nbm_nas.force_close(b,a,shutdown_cb,"SHUTD
quote_loop.hpp:805:          snap_px("DJ30.F",b,a);  if(b>0&&a>0){g_nbm_us30.force_close(b,a,shutdown_cb,"SHUT
quote_loop.hpp:806:          snap_px("EURUSD",b,a);  if(b>0&&a>0){g_vwap_rev_eurusd.force_close(b,a,shutdown_c
quote_loop.hpp:807:          snap_px("GER40",b,a);   if(b>0&&a>0){g_orb_ger30.force_close(b,a,shutdown_cb,"SHU
quote_loop.hpp:808:          snap_px("XAUUSD",b,a);  if(b>0&&a>0){g_trend_pb_gold.force_close(b,a,shutdown_cb,
quote_loop.hpp:809:          snap_px("USOIL.F",b,a); if(b>0&&a>0){g_nbm_oil_london.force_close(b,a,shutdown_cb
quote_loop.hpp:810:          snap_px("UK100",b,a);   if(b>0&&a>0){g_orb_uk100.force_close(b,a,shutdown_cb,"SHU
quote_loop.hpp:811:          snap_px("ESTX50",b,a);  if(b>0&&a>0){g_orb_estx50.force_close(b,a,shutdown_cb,"SH
quote_loop.hpp:812:          snap_px("USOIL.F",b,a); if(b>0&&a>0){g_ca_eia_fade.force_close(b,a,shutdown_cb,"S
quote_loop.hpp:813:          snap_px("USDJPY",b,a);  if(b>0&&a>0){g_ca_carry_unwind.force_close(b,a,shutdown_c
quote_loop.hpp:816:            if(b>0&&a>0){g_ca_fx_cascade.force_close(b,a,shutdown_cb,"SHUTDOWN");}
quote_loop.hpp:817:            if(ab>0&&aa>0){g_ca_fx_cascade.force_close_audusd(ab,aa,shutdown_cb,"SHUTDOWN")
quote_loop.hpp:818:            if(nb>0&&na>0){g_ca_fx_cascade.force_close_nzdusd(nb,na,shutdown_cb,"SHUTDOWN")
quote_loop.hpp:901:              get_px("USOIL.F",b,a);  g_nbm_oil_london.force_close(b,a,scb,"SHUTDOWN");  //
quote_loop.hpp:1033:            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_esnq.force_close(ca_b, ca_a, ca_cb, "RECO
quote_loop.hpp:1035:            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_eia_fade.force_close(ca_b, ca_a, ca_cb, "
quote_loop.hpp:1037:            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_fx_cascade.force_close(ca_b, ca_a, ca_cb,
quote_loop.hpp:1040:              if (aud_b > 0.0 && aud_a > 0.0) g_ca_fx_cascade.force_close_audusd(aud_b, au
quote_loop.hpp:1042:              if (nzd_b > 0.0 && nzd_a > 0.0) g_ca_fx_cascade.force_close_nzdusd(nzd_b, nz
quote_loop.hpp:1044:            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_carry_unwind.force_close(ca_b, ca_a, ca_c
quote_loop.hpp:1051:            if (ca_b > 0.0 && ca_a > 0.0) { g_orb_uk100.force_close(ca_b, ca_a, ca_cb, "RE
quote_loop.hpp:1053:            if (ca_b > 0.0 && ca_a > 0.0) { g_orb_estx50.force_close(ca_b, ca_a, ca_cb, "R
quote_loop.hpp:1059:            if (ca_b > 0.0 && ca_a > 0.0) { g_vwap_rev_eurusd.force_close(ca_b, ca_a, ca_c
on_tick.hpp:225:                        + static_cast<int>(g_ca_esnq.has_open_position())
on_tick.hpp:228:                        + static_cast<int>(g_orb_us.has_open_position())
on_tick.hpp:229:                        + static_cast<int>(g_nbm_sp.has_open_position())
on_tick.hpp:230:                        + static_cast<int>(g_nbm_nq.has_open_position())
on_tick.hpp:231:                        + static_cast<int>(g_nbm_nas.has_open_position())
on_tick.hpp:244:                        + static_cast<int>(g_orb_ger30.has_open_position())
on_tick.hpp:245:                        + static_cast<int>(g_orb_uk100.has_open_position())
on_tick.hpp:249:                        + static_cast<int>(g_ca_eia_fade.has_open_position())
on_tick.hpp:250:                        + static_cast<int>(g_ca_brent_wti.has_open_position())
on_tick.hpp:251:                        + static_cast<int>(g_nbm_oil_london.has_open_position()); // was incor
on_tick.hpp:254:                         + static_cast<int>(g_nbm_gold_london.has_open_position()); // was inc
on_tick.hpp:262:                          + static_cast<int>(g_vwap_rev_eurusd.has_open_position())
on_tick.hpp:787:            if (g_nbm_gold_london.has_open_position())
on_tick.hpp:789:                    g_nbm_gold_london.open_is_long(), g_nbm_gold_london.open_entry(),
on_tick.hpp:807:            if (g_minimal_h4_gold.has_open_position())
on_tick.hpp:809:                    g_minimal_h4_gold.pos_.is_long, g_minimal_h4_gold.pos_.entry,
on_tick.hpp:810:                    g_minimal_h4_gold.pos_.tp,      g_minimal_h4_gold.pos_.sl,
on_tick.hpp:811:                    g_minimal_h4_gold.pos_.size,
on_tick.hpp:812:            if (g_minimal_h4_us30.has_open_position())
on_tick.hpp:814:                    g_minimal_h4_us30.pos_.is_long, g_minimal_h4_us30.pos_.entry,
on_tick.hpp:815:                    g_minimal_h4_us30.pos_.tp,      g_minimal_h4_us30.pos_.sl,
on_tick.hpp:816:                    g_minimal_h4_us30.pos_.size,
on_tick.hpp:850:            if (g_nbm_sp.has_open_position())
on_tick.hpp:851:                push_live_trade("US500.F","NBM", g_nbm_sp.open_is_long(),
on_tick.hpp:852:            if (g_nbm_nq.has_open_position())
on_tick.hpp:853:                push_live_trade("USTEC.F","NBM", g_nbm_nq.open_is_long(),
on_tick.hpp:854:            if (g_nbm_nas.has_open_position())
on_tick.hpp:855:                push_live_trade("NAS100","NBM", g_nbm_nas.open_is_long(),
on_tick.hpp:856:            if (g_nbm_us30.has_open_position())
on_tick.hpp:857:                push_live_trade("DJ30.F","NBM", g_nbm_us30.open_is_long(),
on_tick.hpp:900:            if (g_nbm_oil_london.has_open_position())
on_tick.hpp:901:                push_live_trade("USOIL.F","NBM-London", g_nbm_oil_london.open_is_long(),
on_tick.hpp:943:            if (g_ca_fx_cascade.has_open_position())
on_tick.hpp:944:                push_live_trade("GBPUSD","FxCascade", g_ca_fx_cascade.open_is_long(),
on_tick.hpp:945:            if (g_ca_carry_unwind.has_open_position())
on_tick.hpp:946:                push_live_trade("USDJPY","CarryUnw", g_ca_carry_unwind.open_is_long(),
on_tick.hpp:2037:                 g_orb_us.has_open_position() ||
on_tick.hpp:2323:        if (sym == "XAUUSD") g_gold_seasonal.on_tick(bid, ask, fx_now_ms, handle_closed_trade
on_tick.hpp:2327:        if (sym == "XAUUSD") g_gold_oversold.on_tick(bid, ask, fx_now_ms, handle_closed_trade
engine_config.hpp:549:            if (k=="esnq_enabled")       g_ca_esnq.enabled       = (v == "true" || v == 
engine_config.hpp:550:            if (k=="esnq_confirm_ticks") g_ca_esnq.CONFIRM_TICKS = safe_stoi(v, k);
engine_config.hpp:551:            if (k=="esnq_cooldown_sec")  g_ca_esnq.COOLDOWN_SEC  = safe_stoi(v, k);
engine_config.hpp:552:            if (k=="esnq_tp_pct")        g_ca_esnq.TP_PCT        = safe_stod(v, k);
engine_config.hpp:553:            if (k=="esnq_sl_pct")        g_ca_esnq.SL_PCT        = safe_stod(v, k);
engine_config.hpp:556:            auto& mp = g_minimal_h4_gold.p;
engine_config.hpp:557:            if (k=="enabled")            g_minimal_h4_gold.enabled     = (v == "true" ||
engine_config.hpp:558:            if (k=="shadow_mode")        g_minimal_h4_gold.shadow_mode = (v == "true" ||
engine_config.hpp:578:            auto& mp = g_minimal_h4_us30.p;
engine_config.hpp:579:            if (k=="enabled")            g_minimal_h4_us30.enabled     = (v == "true" ||
engine_config.hpp:580:            if (k=="shadow_mode")        g_minimal_h4_us30.shadow_mode = (v == "true" ||
tick_fx.hpp:196:    if (g_amr_eurusd.enabled) g_amr_eurusd.on_tick(bid, ask, now_ms);
tick_fx.hpp:268:            g_trendline_break_gbp.on_h4_bar(s_tlb_h, s_tlb_l, s_tlb_c,
tick_fx.hpp:340:    if (g_amr_gbpusd.enabled) g_amr_gbpusd.on_tick(bid, ask, now_ms);
tick_fx.hpp:392:            g_trendline_break_jpy.on_h4_bar(s_tlbj_h, s_tlbj_l, s_tlbj_c,
tick_fx.hpp:551:        if (g_amr_audusd.enabled) g_amr_audusd.on_tick(bid, ask, now_ms);
tick_fx.hpp:619:        if (g_amr_nzdusd.enabled) g_amr_nzdusd.on_tick(bid, ask, now_ms);
tick_oil.hpp:21:            g_ca_eia_fade.has_open_position()   ||
tick_oil.hpp:25:        if (g_ca_eia_fade.has_open_position())   { g_ca_eia_fade.on_tick(sym, bid, ask, ca_on_
tick_oil.hpp:26:        if (g_ca_brent_wti.has_open_position())  {
tick_oil.hpp:32:            if (brent_mid2 > 0) g_ca_brent_wti.on_tick_wti(bid, ask, brent_mid2, ca_on_close);
tick_oil.hpp:34:        if (g_nbm_oil_london.has_open_position()) { g_nbm_oil_london.on_tick(sym, bid, ask, ca
tick_oil.hpp:35:        if (!g_ca_eia_fade.has_open_position() && !g_ca_brent_wti.has_open_position() && base_
tick_oil.hpp:36:            if (ef.valid) { if (!enter_directional(sym.c_str(), ef.is_long, ef.entry, ef.sl, e
tick_oil.hpp:37:                else g_ca_eia_fade.patch_size(g_last_directional_lot); }
tick_oil.hpp:39:        if (!g_ca_brent_wti.has_open_position() && !g_ca_eia_fade.has_open_position() && base_
tick_oil.hpp:46:                if (bw.valid) { if (!enter_directional(sym.c_str(), bw.is_long, bw.entry, bw.s
tick_oil.hpp:47:                else g_ca_brent_wti.patch_size(g_last_directional_lot); }
tick_oil.hpp:50:        if (!g_nbm_oil_london.has_open_position()
tick_oil.hpp:51:            && !g_ca_eia_fade.has_open_position()
tick_oil.hpp:52:            && !g_ca_brent_wti.has_open_position()
tick_oil.hpp:61:                else g_nbm_oil_london.patch_size(g_last_directional_lot);
tick_gold.hpp:121:        g_nbm_gold_london.has_open_position()   ||  // London NBM also blocks other gold eng
tick_gold.hpp:134:        g_bband_scalp.has_open_position()                 ||
tick_gold.hpp:1107:                const auto m4sig = g_minimal_h4_gold.on_h4_bar(
tick_gold.hpp:1203:            g_trendline_break.on_h4_bar(s_cur_h4.high, s_cur_h4.low, s_cur_h4.close,
tick_gold.hpp:2202:    if (g_minimal_h4_gold.has_open_position()) {
tick_gold.hpp:2232:    g_adhull_xau.on_tick(bid, ask, now_ms_g);   // adaptive-Hull XAU trend (shadow)
tick_gold.hpp:2233:    g_supertrend_gold.on_tick(bid, ask, now_ms_g);   // Supertrend gold trend (shadow)
tick_gold.hpp:2265:    g_gold_panic_bounce.on_tick(bid, ask, now_ms_g);                   // 2026-06-12 "big r
tick_gold.hpp:2770:    g_gold_regime_daily.on_tick(bid, ask, now_ms_g,
tick_gold.hpp:2789:    g_bband_scalp.on_tick(bid, ask, now_ms_g,
tick_gold.hpp:2803:    if (g_nbm_gold_london.has_open_position()) {
tick_indices.hpp:44:    if (g_amr_us500.enabled) {
tick_indices.hpp:80:        g_orb_us.has_open_position() ||
tick_indices.hpp:83:        g_nbm_sp.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
tick_indices.hpp:115:                else g_ca_esnq.patch_size(g_last_directional_lot);
tick_indices.hpp:119:    if (g_orb_us.has_open_position())       { g_orb_us.on_tick(sym, bid, ask, ca_on_close
tick_indices.hpp:127:    if (g_nbm_sp.has_open_position())       { g_nbm_sp.on_tick(sym, bid, ask, ca_on_close
tick_indices.hpp:145:    if (!g_orb_us.has_open_position() && !g_vwap_rev_sp.has_open_position() && base_can_s
tick_indices.hpp:149:                else g_orb_us.patch_size(g_last_directional_lot);
tick_indices.hpp:153:    if (!g_vwap_rev_sp.has_open_position() && !g_orb_us.has_open_position() && base_can_s
tick_indices.hpp:214:        if (!g_nbm_sp.has_open_position() && !g_orb_us.has_open_position() &&
tick_indices.hpp:220:                else g_nbm_sp.patch_size(g_last_directional_lot);
tick_indices.hpp:236:            && !g_nbm_sp.has_open_position() && base_can_sp && sp_trendpb_ok) {
tick_indices.hpp:267:                   && !g_orb_us.has_open_position()
tick_indices.hpp:270:                   && !g_nbm_sp.has_open_position()
tick_indices.hpp:340:        g_overnight_spx.on_tick(bid, ask, now_ms_isp);   // overnight drift US500 (trend>
tick_indices.hpp:453:        g_nbm_nq.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
tick_indices.hpp:497:    if (g_nbm_nq.has_open_position())      { g_nbm_nq.on_tick(sym, bid, ask, ca_on_close)
tick_indices.hpp:576:            && !g_nbm_nq.has_open_position() && base_can_nq && nq_trendpb_ok) {
tick_indices.hpp:598:        if (!g_nbm_nq.has_open_position() && !g_vwap_rev_nq.has_open_position()
tick_indices.hpp:604:                else g_nbm_nq.patch_size(g_last_directional_lot);
tick_indices.hpp:627:                   && !g_nbm_nq.has_open_position()
tick_indices.hpp:702:        g_nbm_us30.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
tick_indices.hpp:723:    if (g_nbm_us30.has_open_position()) { g_nbm_us30.on_tick(sym, bid, ask, ca_on_close);
tick_indices.hpp:732:        if (!g_nbm_us30.has_open_position() && base_can_us30 && us30_session_ok) {
tick_indices.hpp:737:                else g_nbm_us30.patch_size(g_last_directional_lot);
tick_indices.hpp:758:                   && !g_nbm_us30.has_open_position()
tick_indices.hpp:809:        if (g_minimal_h4_us30.has_open_position()) {
tick_indices.hpp:922:    if (g_amr_ger40.enabled) {
tick_indices.hpp:931:        g_orb_ger30.has_open_position()     ||
tick_indices.hpp:936:    if (g_orb_ger30.has_open_position())      { g_orb_ger30.on_tick(sym, bid, ask, ca_on_
tick_indices.hpp:938:        const double ger_vwap_mgmt = (g_orb_ger30.range_high() + g_orb_ger30.range_low())
tick_indices.hpp:980:        g_adhull_ger.on_tick(bid, ask, now_ms_isg);   // adaptive-Hull GER40 trend (shado
tick_indices.hpp:997:        if (g_minimal_h4_ger40.has_open_position()) {
tick_indices.hpp:1013:        if (g_ger40_turtle_h4.has_open_position()) {
tick_indices.hpp:1053:    if (g_orb_uk100.has_open_position()) { g_orb_uk100.on_tick(sym, bid, ask, ca_on_clos
tick_indices.hpp:1056:    if (!g_orb_uk100.has_open_position() && base_can_uk) {
tick_indices.hpp:1060:                else g_orb_uk100.patch_size(g_last_directional_lot);
tick_indices.hpp:1098:    if (g_orb_estx50.has_open_position()) { g_orb_estx50.on_tick(sym, bid, ask, ca_on_cl
tick_indices.hpp:1101:    if (!g_orb_estx50.has_open_position() && base_can_estx) {
tick_indices.hpp:1105:                else g_orb_estx50.patch_size(g_last_directional_lot);
tick_indices.hpp:1169:    if (g_amr_nas100.enabled) {
tick_indices.hpp:1178:        g_nbm_nas.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
tick_indices.hpp:1197:    if (g_nbm_nas.has_open_position()) { g_nbm_nas.on_tick(sym, bid, ask, ca_on_close); 
tick_indices.hpp:1244:        if (!g_nbm_nas.has_open_position() && base_can_nas && nas_nbm_gate_ok) {
tick_indices.hpp:1249:                else g_nbm_nas.patch_size(g_last_directional_lot);
tick_indices.hpp:1271:                   && !g_nbm_nas.has_open_position()
tick_indices.hpp:1316:        g_peachy_orb_nas.on_tick(bid, ask, now_ms_isn);  // Peachy one-candle ORB-retest
tick_indices.hpp:1317:        g_nas_orb_retrace.on_tick(bid, ask, now_ms_isn); // 2026-06-07 ORB retrace+RUNNE
tick_indices.hpp:1320:        g_overnight_nas.on_tick(bid, ask, now_ms_isn);   // overnight drift (shadow)
tick_indices.hpp:1321:        g_connors_nas.on_tick(bid, ask, now_ms_isn);     // RSI2 dip-buy (shadow)
