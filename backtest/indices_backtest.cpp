// indices_backtest.cpp — bracket engine backtest using M1 OHLC bar data
// Compile: g++ -O3 -std=c++17 indices_backtest.cpp -o indices_backtest
// Run US500: ./indices_backtest ~/tick/bars/us500_m1.csv ~/tick/bt_us500
// Run USTEC: ./indices_backtest ~/tick/bars/ustec_m1.csv ~/tick/bt_ustec
//
// Signal: momentum breakout — price moves mom_pct in one bar → enter
// Exit:   TP=pct of price, SL=pct of price, or timeout
// Matches live engine params from [OMEGA-PARAMS]:
//   US500.F TP=0.6% SL=0.4% hold=180s
//   USTEC.F TP=0.64% SL=0.4% hold=180s

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
#include <string>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <cstdio>

static constexpr double RISK_DOLLARS  = 80.0;
static constexpr double LOT_STEP      = 0.01;
static constexpr double LOT_MIN       = 0.01;
static constexpr double LOT_MAX       = 0.50;
static constexpr int    WARMUP_BARS   = 50;

// ── Bar ───────────────────────────────────────────────────────────────────
struct Bar {
    long long ts_ms = 0;
    double open = 0, high = 0, low = 0, close = 0;
    int    volume = 0;
    double spread = 0;
};

static bool parse_bar(const std::string& line, Bar& b) {
    const char* p = line.c_str(); char* e;
    b.ts_ms  = std::strtoll(p, &e, 10); if (*e != ',') return false; p=e+1;
    b.open   = std::strtod(p, &e);      if (*e != ',') return false; p=e+1;
    b.high   = std::strtod(p, &e);      if (*e != ',') return false; p=e+1;
    b.low    = std::strtod(p, &e);      if (*e != ',') return false; p=e+1;
    b.close  = std::strtod(p, &e);      if (*e != ',') return false; p=e+1;
    b.volume = (int)std::strtol(p, &e, 10);
    if (*e == ',') { p=e+1; b.spread = std::strtod(p,&e); }
    if (b.ts_ms<=0||b.open<=0||b.close<=0) return false;
    if (b.open<10||b.open>100000) return false;
    return true;
}

// ── Session (UTC) ─────────────────────────────────────────────────────────
static int bar_hm(long long ts_ms) {
    time_t t = (time_t)(ts_ms/1000);
    struct tm u{}; gmtime_r(&t,&u);
    return u.tm_hour*60 + u.tm_min;
}
static bool ny_session(long long ts)    { int h=bar_hm(ts); return h>=(13*60+30)&&h<(20*60); }
static bool london_session(long long ts){ int h=bar_hm(ts); return h>=(8*60)&&h<(16*60+30); }
static bool any_session(long long ts)   { return ny_session(ts)||london_session(ts); }

// ── Config ────────────────────────────────────────────────────────────────
struct Config {
    double tp_pct;        // take profit % of price
    double sl_pct;        // stop loss % of price
    double mom_pct;       // momentum: bar range / price must exceed this
    double vol_pct;       // ATR(20) / price must exceed this to trade
    int    hold_bars;     // max hold in bars (1 bar = 1 min)
    int    gap_bars;      // min bars between trades
    int    session;       // 0=any 1=ny 2=london
    const char* name;
};

// ── Trade ─────────────────────────────────────────────────────────────────
struct Trade {
    long long entry_ms, exit_ms;
    int       dir;
    double    entry_px, exit_px, size;
    double    gross, net, mfe, mae;
    int       hold_bars;
    const char* reason;
    const char* cfg_name;
};

// ── State ─────────────────────────────────────────────────────────────────
struct State {
    const Config& cfg;
    std::deque<double> atr_buf;   // rolling 20-bar ATR (high-low)
    double cur_atr = 0;
    bool   in_pos  = false;
    int    dir     = 0;
    double entry   = 0, size = 0;
    double mfe = 0, mae = 0;
    long long entry_ms = 0;
    int    bars_held = 0;
    int    cooldown  = 0;
    int    bar_n     = 0;
    std::vector<Trade> trades;

    explicit State(const Config& c) : cfg(c) {}

    void update_atr(const Bar& b) {
        atr_buf.push_back(b.high - b.low);
        if ((int)atr_buf.size() > 20) atr_buf.pop_front();
        if ((int)atr_buf.size() == 20) {
            double s = 0; for (double v:atr_buf) s+=v;
            cur_atr = s/20.0;
        }
    }

    void close(const Bar& b, double exit_px, const char* reason) {
        double move  = dir==1 ? exit_px-entry : entry-exit_px;
        double tp    = entry * cfg.tp_pct/100.0;
        double sl    = entry * cfg.sl_pct/100.0;
        double gross;
        if (!strcmp(reason,"TP_HIT"))      gross =  tp*size;
        else if (!strcmp(reason,"SL_HIT")) gross = -sl*size;
        else                               gross =  move*size;
        double spread_cost = b.spread > 0 ? b.spread*size : (entry*0.0004*size);
        double net = gross - spread_cost;
        trades.push_back({entry_ms, b.ts_ms, dir, entry, exit_px, size,
                          gross, net, mfe, mae, bars_held, reason, cfg.name});
        in_pos=false; cooldown=cfg.gap_bars;
    }

    void on_bar(const Bar& b) {
        ++bar_n;
        update_atr(b);
        if (cooldown>0) --cooldown;
        if (bar_n < WARMUP_BARS || cur_atr<=0) return;

        bool sess_ok;
        if      (cfg.session==1) sess_ok = ny_session(b.ts_ms);
        else if (cfg.session==2) sess_ok = london_session(b.ts_ms);
        else                     sess_ok = any_session(b.ts_ms);

        // ── Manage open position ─────────────────────────────────────
        if (in_pos) {
            ++bars_held;
            double tp = entry * cfg.tp_pct/100.0;
            double sl = entry * cfg.sl_pct/100.0;
            // Simulate bar: check SL first, then TP (pessimistic)
            double worst = dir==1 ? b.low  : b.high;
            double best  = dir==1 ? b.high : b.low;
            double move_worst = dir==1 ? worst-entry : entry-worst;
            double move_best  = dir==1 ? best-entry  : entry-best;
            mfe = std::max(mfe, move_best);
            mae = std::min(mae, move_worst);
            if (move_worst <= -sl)        close(b, dir==1 ? entry-sl : entry+sl, "SL_HIT");
            else if (move_best >= tp)     close(b, dir==1 ? entry+tp : entry-tp, "TP_HIT");
            else if (bars_held>=cfg.hold_bars) close(b, b.close, "TIMEOUT");
            return;
        }

        if (cooldown>0 || !sess_ok) return;

        // Volatility gate
        double min_atr = b.close * cfg.vol_pct/100.0;
        if (cur_atr < min_atr) return;

        // Momentum: bar range vs threshold
        double range    = b.high - b.low;
        double mom_thresh = b.close * cfg.mom_pct/100.0;
        if (range < mom_thresh) return;

        // Direction: close relative to open
        double move = b.close - b.open;
        bool long_sig  = move >  mom_thresh*0.5;
        bool short_sig = move < -mom_thresh*0.5;
        if (!long_sig && !short_sig) return;

        // Size
        double sl_pts = b.close * cfg.sl_pct/100.0;
        size = std::max(LOT_MIN, std::min(LOT_MAX,
            std::floor(RISK_DOLLARS/sl_pts/LOT_STEP)*LOT_STEP));

        in_pos     = true;
        dir        = long_sig ? 1 : -1;
        entry      = long_sig ? b.close : b.close;  // enter at close of signal bar
        entry_ms   = b.ts_ms;
        bars_held  = 0; mfe=0; mae=0;
    }
};

// ── Stats ─────────────────────────────────────────────────────────────────
static void print_stats(const State& s) {
    const auto& T = s.trades;
    if (T.empty()) { printf("  %-42s  0 trades\n", s.cfg.name); return; }
    int n=T.size(), wins=0, n_win=0, n_loss=0;
    double net=0,peak=0,dd=0,cur=0,sw=0,sl=0;
    for (auto& t:T) {
        if (t.net>0){++wins;sw+=t.net;++n_win;}else{sl+=t.net;++n_loss;}
        net+=t.net; cur+=t.net;
        if(cur>peak)peak=cur;
        double d=peak-cur; if(d>dd)dd=d;
    }
    double wr=100.0*wins/n;
    double aw=n_win?sw/n_win:0, al=n_loss?sl/n_loss:0;
    double poff=(n_loss&&al)?std::fabs(aw/al):0;
    printf("  %-42s  n=%4d  WR=%5.1f%%  net=$%8.2f  avg=$%6.2f  "
           "win=$%6.2f  loss=$%7.2f  poff=%.3f  DD=$%7.2f\n",
           s.cfg.name,n,wr,net,net/n,aw,al,poff,dd);
}

static void write_csv(const State& s, const std::string& dir) {
    std::string p = dir+"/bt_"+s.cfg.name+".csv";
    FILE* f=fopen(p.c_str(),"w"); if(!f)return;
    fprintf(f,"entry_ms,exit_ms,dir,entry,exit,size,gross,net,mfe,mae,bars,reason\n");
    for (auto& t:s.trades)
        fprintf(f,"%lld,%lld,%d,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
                (long long)t.entry_ms,(long long)t.exit_ms,t.dir,
                t.entry_px,t.exit_px,t.size,t.gross,t.net,
                t.mfe,t.mae,t.hold_bars,t.reason);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,"usage: %s bars.csv output_dir\n",argv[0]); return 1;
    }
    const char* csv   = argv[1];
    const char* outd  = argv[2];

    // ── Configs ────────────────────────────────────────────────────────
    // tp_pct, sl_pct, mom_pct, vol_pct, hold_bars, gap_bars, session, name
    static const Config CONFIGS[] = {
        // Live engine params
        { 0.60, 0.40, 0.20, 0.05, 180, 90, 1, "live_tp060_sl040_ny"        },
        { 0.60, 0.40, 0.20, 0.05, 180, 90, 2, "live_tp060_sl040_ldn"       },
        { 0.60, 0.40, 0.20, 0.05, 180, 90, 0, "live_tp060_sl040_all"       },
        // Wider TP
        { 0.80, 0.40, 0.20, 0.05, 240, 90, 1, "tp080_sl040_ny"             },
        { 1.00, 0.40, 0.20, 0.05, 360, 90, 1, "tp100_sl040_ny"             },
        { 1.20, 0.50, 0.20, 0.05, 480, 90, 1, "tp120_sl050_ny"             },
        // Tighter SL (more trades, smaller loss)
        { 0.60, 0.25, 0.15, 0.05, 180, 60, 1, "tp060_sl025_ny"             },
        { 0.80, 0.30, 0.15, 0.05, 240, 60, 1, "tp080_sl030_ny"             },
        // Momentum filter variants
        { 0.60, 0.40, 0.30, 0.05, 180, 90, 1, "mom030_tp060_sl040_ny"      },
        { 0.60, 0.40, 0.40, 0.05, 180, 90, 1, "mom040_tp060_sl040_ny"      },
        { 0.80, 0.40, 0.40, 0.05, 240, 90, 1, "mom040_tp080_sl040_ny"      },
        // Vol expansion gate (only trade when ATR elevated)
        { 0.60, 0.40, 0.20, 0.10, 180, 90, 1, "vol010_tp060_sl040_ny"      },
        { 0.60, 0.40, 0.20, 0.15, 180, 90, 1, "vol015_tp060_sl040_ny"      },
        { 0.80, 0.40, 0.30, 0.15, 240, 90, 1, "vol015_mom030_tp080_ny"     },
        // All-session with strong filters
        { 0.80, 0.40, 0.40, 0.10, 240, 90, 0, "all_mom040_vol010_tp080"    },
    };
    static constexpr int N=(int)(sizeof(CONFIGS)/sizeof(CONFIGS[0]));

    // Load bars
    std::ifstream f(csv); if(!f.is_open()){fprintf(stderr,"Cannot open %s\n",csv);return 1;}
    std::vector<Bar> bars;
    std::string line;
    std::getline(f,line); // header
    while(std::getline(f,line)){
        Bar b; if(parse_bar(line,b)) bars.push_back(b);
    }
    printf("[BACKTEST] %s: %zu bars\n",csv,bars.size());

    // Sort by time
    std::sort(bars.begin(),bars.end(),[](const Bar&a,const Bar&b){return a.ts_ms<b.ts_ms;});

    std::vector<State> states;
    states.reserve(N);
    for(int i=0;i<N;++i) states.emplace_back(CONFIGS[i]);

    for(const auto& b:bars)
        for(auto& s:states) s.on_bar(b);

    printf("\n=== RESULTS (%zu bars) ===\n\n",bars.size());
    printf("  %-42s  %4s  %6s  %9s  %7s  %6s  %7s  %5s  %8s\n",
           "Config","n","WR%","net$","avg$","win$","loss$","poff","DD$");
    printf("  %s\n",std::string(125,'-').c_str());
    for(const auto& s:states) print_stats(s);

    char cmd[512]; snprintf(cmd,sizeof(cmd),"mkdir -p %s",outd); system(cmd);
    for(const auto& s:states) write_csv(s,outd);
    printf("\n[DONE] CSVs → %s\n",outd);
    return 0;
}
