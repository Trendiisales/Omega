#!/usr/bin/env python3
"""
add_l2_logger.py — adds per-tick L2 imbalance logging to main.cpp
Run from: ~/omega_repo/  →  python3 add_l2_logger.py

Adds a daily rotating CSV logger that writes:
  timestamp_ms, bid, ask, l2_imbalance, l2_bid_vol, l2_ask_vol, vol_ratio, regime

Output: C:\\Omega\\logs\\l2_ticks_YYYY-MM-DD.csv
One file per day, appended each tick GoldFlow considers entry.
~10k ticks/day × 5 days = 50k rows = enough to validate L2 edge.
"""

import os, shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/src/main.cpp")
stamp  = datetime.now().strftime("%Y%m%d_%H%M%S")
shutil.copy2(TARGET, TARGET + f".bak_{stamp}")
print(f"[OK] Backup: {TARGET}.bak_{stamp}")

with open(TARGET, 'r') as f:
    content = f.read()

# Anchor: the set_trend_bias call just before on_tick at the entry site
ANCHOR = '''                g_gold_flow.set_trend_bias(gold_momentum, gold_sdec.confidence,
                                           sup_trend, gf_wall_ahead, gold_vwap_pts,
                                           gf_expansion_entry, gf_vol_ratio_entry);
                g_gold_flow.on_tick(bid, ask,
                    g_macro_ctx.gold_l2_imbalance,
                    g_gold_stack.ewm_drift(),
                    now_ms_g, flow_on_close,
                    g_macro_ctx.session_slot);'''

L2_LOGGER = '''                g_gold_flow.set_trend_bias(gold_momentum, gold_sdec.confidence,
                                           sup_trend, gf_wall_ahead, gold_vwap_pts,
                                           gf_expansion_entry, gf_vol_ratio_entry);

                // ── L2 tick logger ────────────────────────────────────────────
                // Writes per-tick L2 data so we can backtest with real imbalance.
                // Daily rotating CSV: C:\\Omega\\logs\\l2_ticks_YYYY-MM-DD.csv
                {
                    static FILE*   s_l2f     = nullptr;
                    static int     s_l2_day  = -1;
                    const time_t   t_l2      = (time_t)(now_ms_g / 1000);
                    struct tm      tm_l2{};
                    gmtime_s(&tm_l2, &t_l2);
                    if (tm_l2.tm_yday != s_l2_day) {
                        if (s_l2f) { fclose(s_l2f); s_l2f = nullptr; }
                        char l2path[256];
                        snprintf(l2path, sizeof(l2path),
                            "C:\\\\Omega\\\\logs\\\\l2_ticks_%04d-%02d-%02d.csv",
                            tm_l2.tm_year+1900, tm_l2.tm_mon+1, tm_l2.tm_mday);
                        bool is_new = (GetFileAttributesA(l2path) == INVALID_FILE_ATTRIBUTES);
                        s_l2f = fopen(l2path, "a");
                        if (s_l2f && is_new)
                            fprintf(s_l2f,
                                "ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,"
                                "vol_ratio,regime,vpin,has_pos\\n");
                        s_l2_day = tm_l2.tm_yday;
                    }
                    if (s_l2f) {
                        // Get bid/ask volumes from L2 book
                        double l2_bvol = 0.0, l2_avol = 0.0;
                        {
                            std::lock_guard<std::mutex> lk(g_l2_mtx);
                            auto it = g_l2_books.find("GOLD.F");
                            if (it != g_l2_books.end()) {
                                for (auto& kv : it->second.bids) l2_bvol += kv.second.size_raw;
                                for (auto& kv : it->second.asks) l2_avol += kv.second.size_raw;
                            }
                        }
                        fprintf(s_l2f,
                            "%lld,%.3f,%.3f,%.4f,%.0f,%.0f,%.3f,%d,%.3f,%d\\n",
                            (long long)now_ms_g, bid, ask,
                            g_macro_ctx.gold_l2_imbalance,
                            l2_bvol, l2_avol,
                            gf_vol_ratio_entry,
                            (int)gold_sdec.regime,
                            g_vpin.warmed() ? g_vpin.vpin() : 0.0,
                            (int)g_gold_flow.has_open_position());
                        fflush(s_l2f);
                    }
                }
                // ── end L2 tick logger ────────────────────────────────────────

                g_gold_flow.on_tick(bid, ask,
                    g_macro_ctx.gold_l2_imbalance,
                    g_gold_stack.ewm_drift(),
                    now_ms_g, flow_on_close,
                    g_macro_ctx.session_slot);'''

if ANCHOR in content:
    content = content.replace(ANCHOR, L2_LOGGER)
    with open(TARGET, 'w') as f:
        f.write(content)
    print("[OK] L2 logger added before GoldFlow on_tick entry call")
    print()
    print("Output file: C:\\Omega\\logs\\l2_ticks_YYYY-MM-DD.csv")
    print("Columns: ts_ms, bid, ask, l2_imb, l2_bid_vol, l2_ask_vol,")
    print("         vol_ratio, regime, vpin, has_pos")
    print()
    print("Next:")
    print("  git add src/main.cpp include/GoldFlowEngine.hpp")
    print("  git commit -m 'feat: L2 per-tick logger + revert GoldFlowEngine to 1979c14'")
    print("  git push")
    print("  Then deploy to VPS")
else:
    print("[ERROR] Anchor not found")
    # Find partial
    idx = content.find('g_gold_flow.set_trend_bias')
    print(f"set_trend_bias found at char {idx}")
    print(repr(content[idx:idx+200]))
