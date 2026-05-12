// =============================================================================
// ustec_trend_follow_5m_planB_sweep.cpp -- Plan B: regime gate + cost-ratio
// =============================================================================
//
// 2026-05-12 (Claude / Jo): with Plan A best (sl_mult=3.0, tp_mult=7.0,
//   prove_secs=150, prove_pts=2.0) pinned, sweep:
//     MIN_ATR_PTS:       {5, 10, 15, 20, 30}    (5)
//     MIN_SL_PTS_FLOOR:  {10, 15, 22, 30}        (4)
//     cost_ratio_min:    {1.5, 2.0, 3.0}         (3)
//   = 60 sweep cells + 1 baseline (live config)
//
// Holds Plan A best fixed; cost gate ratio is the main lever.
// =============================================================================

#define OMEGA_BACKTEST 1

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// The harness reuses the engine fork from planA_sweep.cpp by repeating it here
// (single TU; no dependency). The fork class is identical.

namespace ustec {

struct TradeRecord {
    int64_t ts_ms_entry = 0, ts_ms_exit = 0;
    int     cell = 0;
    bool    is_long = false;
    double  entry_price = 0.0, exit_price = 0.0;
    std::string exit_reason;
    int     duration_s = 0;
    double  gross_pts = 0.0, gross_usd = 0.0;
    double  spread_at_entry_pts = 0.0, modeled_cost_usd = 0.0, net_usd = 0.0;
    int     hour_utc = 0;
    double  size = 0.10;
    double  mfe_pts = 0.0, mae_pts = 0.0;
    double  atr_at_entry = 0.0;
    int     bars_held = 0;
};

struct UstecBar {
    int64_t bar_start_ms = 0;
    double  open=0, high=0, low=0, close=0;
};

class UstecTfEngine {
public:
    int     donchian_N = 20;
    double  keltner_K = 2.0;
    int     ema_period = 20;
    int     atr_period = 14;
    double  donchian_sl_mult = 2.0;
    double  donchian_tp_mult = 4.0;
    double  keltner_sl_mult = 2.0;
    double  keltner_tp_mult = 4.0;
    double  prove_it_secs = 90.0;
    double  prove_it_min_fav_pts = 4.0;
    double  min_sl_pts_floor = 15.0;
    double  min_atr_pts = 10.0;
    double  max_spread = 5.0;
    double  cost_ratio_min = 1.5;
    double  lot = 0.1;
    int     session_start_utc = 0;
    int     session_end_utc = 24;
    bool    enable_donchian = true, enable_keltner = true, enable_mutex = true;
    std::function<void(const TradeRecord&)> trade_sink;

    struct Pos {
        bool active=false; bool is_long=false;
        double entry_px=0, tp_px=0, sl_px=0, atr_at_entry=0;
        int64_t entry_ts_ms=0; int bars_held=0; int cooldown_bars=0;
        double mfe_pts=0, mae_pts=0; bool proved=false;
        double spread_at_entry=0; int hour_utc_at_entry=0;
    };
    std::array<Pos, 2> pos{};
    static constexpr int kBarHistoryMax = 64;
    std::deque<UstecBar> bars_;
    double atr14_ = 0; int atr_warmup_count_ = 0;
    double ema20_ = 0; bool ema_initialised_ = false;

    void on_5m_bar(const UstecBar& bar, double bid, double ask, int64_t now_ms, int hr) noexcept {
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistoryMax) bars_.pop_front();
        if ((int)bars_.size() >= 2) {
            const auto& cur = bars_.back();
            const auto& prev = bars_[bars_.size()-2];
            double tr = std::max(cur.high-cur.low, std::max(std::abs(cur.high-prev.close), std::abs(cur.low-prev.close)));
            if (atr_warmup_count_ < atr_period) { atr14_ = (atr14_*atr_warmup_count_ + tr)/(atr_warmup_count_+1); ++atr_warmup_count_; }
            else { atr14_ = (atr14_*(atr_period-1) + tr)/atr_period; }
        }
        double c = bars_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else { double a = 2.0/(ema_period+1); ema20_ = a*c + (1-a)*ema20_; }
        for (auto& p : pos) { if (p.cooldown_bars>0) --p.cooldown_bars; if (p.active) ++p.bars_held; }
        if ((int)bars_.size() < std::max(donchian_N, ema_period)+2) return;
        if (atr14_ <= 0) return;
        if (ask-bid > max_spread) return;
        if (hr < session_start_utc || hr >= session_end_utc) return;
        if (atr14_ < min_atr_pts) return;
        for (int ci=0; ci<2; ++ci) {
            if (ci==0 && !enable_donchian) continue;
            if (ci==1 && !enable_keltner) continue;
            if (pos[ci].active || pos[ci].cooldown_bars>0) continue;
            int side = (ci==0) ? _sig_donchian() : _sig_keltner();
            if (side==0) continue;
            if (enable_mutex && _other_same_dir(ci, side)) continue;
            _fire(ci, side, bid, ask, now_ms, hr);
        }
    }

    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        for (int ci=0; ci<2; ++ci) { if (pos[ci].active) _manage(ci, bid, ask, now_ms); }
    }
    bool any_open() const noexcept { return pos[0].active || pos[1].active; }

private:
    int _sig_donchian() const noexcept {
        const int N = donchian_N;
        if ((int)bars_.size() < N+1) return 0;
        const int last = (int)bars_.size()-1;
        double hi = bars_[last-N].high, lo = bars_[last-N].low;
        for (int k=last-N+1; k<=last-1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }
    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_<=0) return 0;
        if ((int)bars_.size() < ema_period+2) return 0;
        const auto& cur = bars_.back();
        double up = ema20_ + keltner_K*atr14_;
        double lo = ema20_ - keltner_K*atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }
    bool _other_same_dir(int ci, int side) const noexcept {
        const bool want_long = (side>0);
        for (int k=0;k<2;++k) { if (k==ci||!pos[k].active) continue; if (pos[k].is_long==want_long) return true; }
        return false;
    }
    bool _cost_viable(double sp, double tp) const noexcept {
        if (cost_ratio_min<=0) return true;
        double cost = sp*20*lot + 2.0*20*lot;
        double gross = tp*20*lot;
        return gross >= cost*cost_ratio_min;
    }
    void _fire(int ci, int side, double bid, double ask, int64_t now_ms, int hr) noexcept {
        double entry = (side>0)?ask:bid;
        if (entry<=0||atr14_<=0) return;
        double sl_m = (ci==0)?donchian_sl_mult:keltner_sl_mult;
        double tp_m = (ci==0)?donchian_tp_mult:keltner_tp_mult;
        double sl_atr = sl_m*atr14_;
        double sl_d = std::max(sl_atr, min_sl_pts_floor);
        double tp_d = tp_m*atr14_;
        if (sl_d>sl_atr && sl_atr>0) { double r = sl_d/sl_atr; tp_d *= r; }
        double sp = ask-bid;
        if (!_cost_viable(sp, tp_d)) return;
        auto& p = pos[ci];
        p.active=true; p.is_long=(side>0); p.entry_px=entry;
        p.sl_px=(side>0)?entry-sl_d:entry+sl_d; p.tp_px=(side>0)?entry+tp_d:entry-tp_d;
        p.atr_at_entry=atr14_; p.entry_ts_ms=now_ms; p.bars_held=0; p.cooldown_bars=0;
        p.mfe_pts=0; p.mae_pts=0; p.proved=false; p.spread_at_entry=sp; p.hour_utc_at_entry=hr;
    }
    void _manage(int ci, double bid, double ask, int64_t now_ms) noexcept {
        auto& p = pos[ci];
        double mid = (bid+ask)*0.5;
        if (mid>0 && p.entry_px>0) {
            double fav = p.is_long?(mid-p.entry_px):(p.entry_px-mid);
            if (fav>p.mfe_pts) p.mfe_pts=fav;
            if (fav<p.mae_pts) p.mae_pts=fav;
            if (!p.proved && p.mfe_pts>=prove_it_min_fav_pts) p.proved=true;
        }
        double el = (p.entry_ts_ms>0)?(now_ms-p.entry_ts_ms)/1000.0:0.0;
        if (!p.proved && el>=prove_it_secs) { double xp=p.is_long?bid:ask; _close(ci, xp, "PROVE_IT_FAIL", now_ms); return; }
        bool tp=false, sl=false; double xp=0;
        if (p.is_long) {
            if (bid<=p.sl_px) { xp=p.sl_px; sl=true; }
            else if (bid>=p.tp_px) { xp=p.tp_px; tp=true; }
        } else {
            if (ask>=p.sl_px) { xp=p.sl_px; sl=true; }
            else if (ask<=p.tp_px) { xp=p.tp_px; tp=true; }
        }
        if (!tp && !sl) return;
        _close(ci, xp, tp?"TP_HIT":"SL_HIT", now_ms);
    }
    void _close(int ci, double xp, const char* reason, int64_t now_ms) noexcept {
        auto& p = pos[ci]; if (!p.active) return;
        double pts = p.is_long?(xp-p.entry_px):(p.entry_px-xp);
        double gross_usd = pts*20*lot;
        double cost = 2.0*20*lot + p.spread_at_entry*20*lot;
        TradeRecord tr;
        tr.ts_ms_entry=p.entry_ts_ms; tr.ts_ms_exit=now_ms;
        tr.cell=ci; tr.is_long=p.is_long;
        tr.entry_price=p.entry_px; tr.exit_price=xp;
        tr.exit_reason=reason; tr.duration_s=(int)((now_ms-p.entry_ts_ms)/1000);
        tr.gross_pts=pts*lot; tr.gross_usd=gross_usd;
        tr.spread_at_entry_pts=p.spread_at_entry; tr.modeled_cost_usd=cost;
        tr.net_usd=gross_usd-cost; tr.hour_utc=p.hour_utc_at_entry;
        tr.size=lot; tr.mfe_pts=p.mfe_pts; tr.mae_pts=p.mae_pts;
        tr.atr_at_entry=p.atr_at_entry; tr.bars_held=p.bars_held;
        if (trade_sink) trade_sink(tr);
        p.active=false; p.cooldown_bars=1;
    }
};

struct HistTickStreamer {
    std::ifstream f; std::string line; bool opened=false;
    bool open(const char* p) { f.open(p); opened = f.is_open(); return opened; }
    static int64_t parse_hist_ts(const char* s, size_t len) {
        if (len<18) return -1;
        int Y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
        int M=(s[4]-'0')*10+(s[5]-'0'), D=(s[6]-'0')*10+(s[7]-'0');
        int hh=(s[9]-'0')*10+(s[10]-'0'), mm=(s[11]-'0')*10+(s[12]-'0');
        int ss=(s[13]-'0')*10+(s[14]-'0'), ms=(s[15]-'0')*100+(s[16]-'0')*10+(s[17]-'0');
        struct tm utc{}; utc.tm_year=Y-1900; utc.tm_mon=M-1; utc.tm_mday=D;
        utc.tm_hour=hh; utc.tm_min=mm; utc.tm_sec=ss;
        return (int64_t)timegm(&utc)*1000 + ms;
    }
    bool next(int64_t& ts, double& bid, double& ask) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const char* p=line.c_str(); const char* c1=std::strchr(p, ',');
            if (!c1) continue; const char* c2=std::strchr(c1+1, ',');
            if (!c2) continue;
            ts = parse_hist_ts(p, (size_t)(c1-p));
            if (ts<0) continue;
            char buf[32]; size_t L1=(size_t)(c2-(c1+1));
            if (L1>=sizeof(buf)) continue;
            std::memcpy(buf, c1+1, L1); buf[L1]=0; bid = std::strtod(buf, nullptr);
            const char* c3=std::strchr(c2+1, ',');
            size_t L2=c3?(size_t)(c3-(c2+1)):std::strlen(c2+1);
            if (L2>=sizeof(buf)) continue;
            std::memcpy(buf, c2+1, L2); buf[L2]=0; ask = std::strtod(buf, nullptr);
            return true;
        }
        return false;
    }
};

static inline int hour_utc_from_ts_ms(int64_t ts) {
    time_t t=(time_t)(ts/1000); struct tm u{}; gmtime_r(&t, &u); return u.tm_hour;
}

struct BarBuilder {
    int64_t cur_start=0; UstecBar cur; bool has=false;
    bool on_tick(int64_t ts, double bid, double ask, UstecBar& out) {
        double mid = (bid+ask)*0.5;
        int64_t bs = (ts/(5*60*1000))*(5*60*1000);
        if (!has) { cur_start=bs; cur.bar_start_ms=bs; cur.open=cur.high=cur.low=cur.close=mid; has=true; return false; }
        if (bs != cur_start) { out=cur; cur_start=bs; cur.bar_start_ms=bs; cur.open=cur.high=cur.low=cur.close=mid; return true; }
        if (mid>cur.high) cur.high=mid; if (mid<cur.low) cur.low=mid; cur.close=mid;
        return false;
    }
};

} // namespace ustec

struct ComboConfig {
    double min_atr_pts;
    double min_sl_floor;
    double cost_ratio;
    int    tag = 0; // 0=sweep, 1=baseline (current live)
};

struct ComboResult {
    ComboConfig cfg;
    int n_trades=0, n_wins=0;
    double gross=0, net=0, cost=0, sumw=0, suml=0;
    int tp=0, sl=0, pi=0, dc=0, kc=0;
    // OOS subset (last 4 months)
    int oos_trades=0, oos_wins=0;
    double oos_net=0;
    int oos_tp=0, oos_sl=0, oos_pi=0;
};

int main(int argc, char** argv) {
    if (argc<2) { std::fprintf(stderr, "usage: %s <csvs...> [--oos-cutoff-ts MS] [--engine-summary-out P]\n", argv[0]); return 2; }
    std::vector<std::string> csv_paths;
    std::string es_out = "outputs/ustec_trend_follow_5m_planB_engsum.csv";
    std::string lb_out = "outputs/ustec_trend_follow_5m_planB_leaderboard.csv";
    int64_t oos_cutoff_ms = 0;  // trades with entry_ts_ms >= cutoff are OOS
    for (int i=1; i<argc; ++i) {
        const char* a=argv[i];
        if (std::strcmp(a, "--engine-summary-out")==0 && i+1<argc) es_out = argv[++i];
        else if (std::strcmp(a, "--leaderboard-out")==0 && i+1<argc) lb_out = argv[++i];
        else if (std::strcmp(a, "--oos-cutoff-ts")==0 && i+1<argc) oos_cutoff_ms = std::strtoll(argv[++i], nullptr, 10);
        else if (a[0]!='-') csv_paths.push_back(a);
    }
    if (csv_paths.empty()) { std::fprintf(stderr, "[err] no csvs\n"); return 2; }
    std::sort(csv_paths.begin(), csv_paths.end());

    // Plan A best: sl_mult=3.0, tp_mult=7.0, prove_secs=150, prove_pts=2.0
    const double pinA_sl = 3.0;
    const double pinA_tp = 7.0;
    const double pinA_psec = 150.0;
    const double pinA_ppts = 2.0;

    std::vector<double> atr_grid     = {5.0, 10.0, 15.0, 20.0, 30.0};
    std::vector<double> slfloor_grid = {10.0, 15.0, 22.0, 30.0};
    std::vector<double> ratio_grid   = {1.5, 2.0, 3.0};

    std::vector<ComboConfig> combos;
    // Baseline = live config (sl=2.0, tp=4.0, psec=90, ppts=4.0, atr=10, slf=15, ratio=1.5)
    combos.push_back({10.0, 15.0, 1.5, 1});
    for (double atr : atr_grid)
      for (double slf : slfloor_grid)
        for (double r : ratio_grid)
          combos.push_back({atr, slf, r, 0});
    const int N = (int)combos.size();
    std::printf("[planB] %d combos (1 baseline + %d sweep)\n", N, N-1);
    std::printf("[planB] oos_cutoff_ts=%lld\n", (long long)oos_cutoff_ms);

    std::vector<ustec::UstecTfEngine> engines(N);
    std::vector<ComboResult> results(N);
    for (int i=0; i<N; ++i) {
        engines[i].min_atr_pts = combos[i].min_atr_pts;
        engines[i].min_sl_pts_floor = combos[i].min_sl_floor;
        engines[i].cost_ratio_min = combos[i].cost_ratio;
        if (combos[i].tag == 1) {
            // baseline = live constexpr
            engines[i].donchian_sl_mult = 2.0; engines[i].donchian_tp_mult = 4.0;
            engines[i].keltner_sl_mult  = 2.0; engines[i].keltner_tp_mult  = 4.0;
            engines[i].prove_it_secs = 90.0; engines[i].prove_it_min_fav_pts = 4.0;
        } else {
            // sweep = Plan A best pinned
            engines[i].donchian_sl_mult = pinA_sl; engines[i].donchian_tp_mult = pinA_tp;
            engines[i].keltner_sl_mult  = pinA_sl; engines[i].keltner_tp_mult  = pinA_tp;
            engines[i].prove_it_secs = pinA_psec; engines[i].prove_it_min_fav_pts = pinA_ppts;
        }
        results[i].cfg = combos[i];
        ComboResult* rp = &results[i];
        engines[i].trade_sink = [rp, oos_cutoff_ms](const ustec::TradeRecord& tr) {
            ComboResult& r = *rp;
            r.n_trades++; r.gross+=tr.gross_usd; r.cost+=tr.modeled_cost_usd; r.net+=tr.net_usd;
            if (tr.net_usd>0) { r.n_wins++; r.sumw+=tr.net_usd; } else r.suml+=tr.net_usd;
            if (tr.exit_reason=="TP_HIT") r.tp++;
            else if (tr.exit_reason=="SL_HIT") r.sl++;
            else if (tr.exit_reason=="PROVE_IT_FAIL") r.pi++;
            if (tr.cell==0) r.dc++; else r.kc++;
            if (oos_cutoff_ms>0 && tr.ts_ms_entry>=oos_cutoff_ms) {
                r.oos_trades++; r.oos_net+=tr.net_usd;
                if (tr.net_usd>0) r.oos_wins++;
                if (tr.exit_reason=="TP_HIT") r.oos_tp++;
                else if (tr.exit_reason=="SL_HIT") r.oos_sl++;
                else if (tr.exit_reason=="PROVE_IT_FAIL") r.oos_pi++;
            }
        };
    }

    ustec::BarBuilder builder;
    ustec::HistTickStreamer st;
    int64_t ts=0, total=0; double bid=0, ask=0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& path : csv_paths) {
        std::printf("[planB] streaming %s\n", path.c_str()); std::fflush(stdout);
        if (!st.open(path.c_str())) continue;
        int64_t fc=0;
        while (st.next(ts, bid, ask)) {
            ++fc; ++total;
            if (bid<=0||ask<=0) continue;
            for (int i=0; i<N; ++i) if (engines[i].any_open()) engines[i].on_tick(bid, ask, ts);
            ustec::UstecBar cb;
            if (builder.on_tick(ts, bid, ask, cb)) {
                int hr = ustec::hour_utc_from_ts_ms(cb.bar_start_ms);
                for (int i=0; i<N; ++i) engines[i].on_5m_bar(cb, bid, ask, ts, hr);
            }
        }
        st.f.close();
        auto dt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  [planB] %lld ticks, elapsed=%llds\n", (long long)fc, (long long)dt);
        std::fflush(stdout);
    }

    // Write engine summary (additive across runs)
    std::ofstream es(es_out);
    es << "combo_id,tag,min_atr,min_sl_floor,cost_ratio,n_trades,n_wins,gross_usd,net_usd,cost_usd,sumw,suml,tp,sl,pi,dc,kc,oos_trades,oos_wins,oos_net,oos_tp,oos_sl,oos_pi\n";
    for (int i=0; i<N; ++i) {
        const ComboResult& r = results[i];
        es << i << "," << r.cfg.tag << "," << std::fixed << std::setprecision(2)
           << r.cfg.min_atr_pts << "," << r.cfg.min_sl_floor << "," << r.cfg.cost_ratio << ","
           << r.n_trades << "," << r.n_wins << ","
           << std::setprecision(4) << r.gross << "," << r.net << "," << r.cost << ","
           << r.sumw << "," << r.suml << ","
           << r.tp << "," << r.sl << "," << r.pi << "," << r.dc << "," << r.kc << ","
           << r.oos_trades << "," << r.oos_wins << ","
           << std::setprecision(4) << r.oos_net << ","
           << r.oos_tp << "," << r.oos_sl << "," << r.oos_pi << "\n";
    }
    es.close();

    // Sorted leaderboard
    std::vector<int> order(N); for (int i=0; i<N; ++i) order[i]=i;
    std::sort(order.begin(), order.end(), [&](int a, int b){ return results[a].net > results[b].net; });
    std::ofstream lf(lb_out);
    lf << "rank,combo_id,tag,min_atr,min_sl_floor,cost_ratio,n_trades,n_wins,WR_pct,net_usd,gross_usd,cost_usd,mean_net,PF,tp,sl,pi,dc,kc,oos_trades,oos_net\n";
    for (int rnk=0; rnk<N; ++rnk) {
        int ci = order[rnk]; const ComboResult& r = results[ci];
        double wr = r.n_trades>0 ? 100.0*r.n_wins/r.n_trades : 0;
        double mn = r.n_trades>0 ? r.net/r.n_trades : 0;
        double pf = (std::fabs(r.suml)>1e-9) ? -r.sumw/r.suml : 0;
        lf << (rnk+1) << "," << ci << "," << r.cfg.tag << ","
           << std::fixed << std::setprecision(2) << r.cfg.min_atr_pts << ","
           << r.cfg.min_sl_floor << "," << r.cfg.cost_ratio << ","
           << r.n_trades << "," << r.n_wins << ","
           << wr << "," << r.net << "," << r.gross << "," << r.cost << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << r.tp << "," << r.sl << "," << r.pi << "," << r.dc << "," << r.kc << ","
           << r.oos_trades << "," << std::setprecision(2) << r.oos_net << "\n";
    }
    lf.close();
    auto dt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-t0).count();
    std::printf("[planB] %lld ticks across %d engines in %llds\n", (long long)total, N, (long long)dt);
    {
        const ComboResult& b = results[0];
        std::printf("\n=== BASELINE (live constexpr config) ===\n");
        std::printf("  trades=%d WR=%.2f%% net=$%.2f gross=$%.2f cost=$%.2f TP=%d SL=%d PI=%d D=%d K=%d\n",
            b.n_trades, b.n_trades>0?100.0*b.n_wins/b.n_trades:0, b.net, b.gross, b.cost,
            b.tp, b.sl, b.pi, b.dc, b.kc);
    }
    std::printf("\n=== TOP 10 by net_usd ===\n");
    std::printf("%4s %5s %4s %6s %6s %6s %8s %12s %7s %8s %10s\n",
                "rank","id","tag","ATR","SLF","RAT","trades","net$","WR%","oos_tr","oos_net");
    for (int r=0; r<std::min(10, N); ++r) {
        int ci=order[r]; const ComboResult& x=results[ci];
        double wr=x.n_trades>0?100.0*x.n_wins/x.n_trades:0;
        std::printf("%4d %5d %4d %6.2f %6.2f %6.2f %8d %12.2f %7.2f %8d %10.2f\n",
            r+1, ci, x.cfg.tag, x.cfg.min_atr_pts, x.cfg.min_sl_floor, x.cfg.cost_ratio,
            x.n_trades, x.net, wr, x.oos_trades, x.oos_net);
    }
    return 0;
}
