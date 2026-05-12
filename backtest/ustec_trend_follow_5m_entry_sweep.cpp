// =============================================================================
// ustec_trend_follow_5m_entry_sweep.cpp -- Entry-side: signal-parameter sweep
// =============================================================================
//
// 2026-05-12 (Claude / Jo): with Plan A best (sl=3.0, tp=7.0, psec=150, ppts=2)
//   and Plan B winning ATR-floor (MIN_ATR=20) pinned, sweep entry-side knobs:
//     donchian_N:   {10, 15, 20, 25, 30}      (5)
//     keltner_K:    {1.5, 2.0, 2.5, 3.0}      (4)
//     session_hr:   {(0,24), (7,22), (13,21), (12,17)}   (4)
//     cell_enable:  {both, donchian-only, keltner-only}  (3)
//   = 5 * 4 * 4 * 3 = 240 cells -> too many. Trim:
//     donchian_N:   {15, 20, 25}        (3)
//     keltner_K:    {1.5, 2.0, 2.5}     (3)
//     session_hr:   {(0,24), (13,21), (12,17)}   (3)
//     cell_enable:  {both, donchian, keltner}    (3)
//   = 81 cells + 1 baseline (live with MIN_ATR=20 pinned, all else live).
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

namespace ustec {

struct TradeRecord {
    int64_t ts_ms_entry=0, ts_ms_exit=0;
    int cell=0; bool is_long=false;
    double entry_price=0, exit_price=0;
    std::string exit_reason;
    int duration_s=0;
    double gross_pts=0, gross_usd=0, spread_at_entry_pts=0, modeled_cost_usd=0, net_usd=0;
    int hour_utc=0; double size=0.10;
    double mfe_pts=0, mae_pts=0, atr_at_entry=0;
    int bars_held=0;
};
struct UstecBar { int64_t bar_start_ms=0; double open=0,high=0,low=0,close=0; };

class UstecTfEngine {
public:
    int donchian_N=20; double keltner_K=2.0;
    int ema_period=20, atr_period=14;
    double donchian_sl_mult=2.0, donchian_tp_mult=4.0;
    double keltner_sl_mult=2.0, keltner_tp_mult=4.0;
    double prove_it_secs=90.0, prove_it_min_fav_pts=4.0;
    double min_sl_pts_floor=15.0, min_atr_pts=10.0;
    double max_spread=5.0, cost_ratio_min=1.5, lot=0.1;
    int session_start_utc=0, session_end_utc=24;
    bool enable_donchian=true, enable_keltner=true, enable_mutex=true;
    std::function<void(const TradeRecord&)> trade_sink;
    struct Pos { bool active=false,is_long=false; double entry_px=0,tp_px=0,sl_px=0,atr_at_entry=0;
        int64_t entry_ts_ms=0; int bars_held=0,cooldown_bars=0;
        double mfe_pts=0,mae_pts=0; bool proved=false; double spread_at_entry=0; int hour_utc_at_entry=0; };
    std::array<Pos,2> pos{};
    static constexpr int kBarHistoryMax=64;
    std::deque<UstecBar> bars_;
    double atr14_=0; int atr_warmup_count_=0;
    double ema20_=0; bool ema_initialised_=false;
    void on_5m_bar(const UstecBar& bar, double bid, double ask, int64_t now_ms, int hr) noexcept {
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistoryMax) bars_.pop_front();
        if ((int)bars_.size() >= 2) {
            const auto& cur=bars_.back(); const auto& prev=bars_[bars_.size()-2];
            double tr=std::max(cur.high-cur.low, std::max(std::abs(cur.high-prev.close), std::abs(cur.low-prev.close)));
            if (atr_warmup_count_<atr_period) { atr14_=(atr14_*atr_warmup_count_+tr)/(atr_warmup_count_+1); ++atr_warmup_count_; }
            else atr14_=(atr14_*(atr_period-1)+tr)/atr_period;
        }
        double c=bars_.back().close;
        if (!ema_initialised_) { ema20_=c; ema_initialised_=true; }
        else { double a=2.0/(ema_period+1); ema20_=a*c+(1-a)*ema20_; }
        for (auto& p:pos) { if (p.cooldown_bars>0) --p.cooldown_bars; if (p.active) ++p.bars_held; }
        if ((int)bars_.size() < std::max(donchian_N, ema_period)+2) return;
        if (atr14_<=0) return;
        if (ask-bid > max_spread) return;
        if (hr<session_start_utc || hr>=session_end_utc) return;
        if (atr14_<min_atr_pts) return;
        for (int ci=0; ci<2; ++ci) {
            if (ci==0 && !enable_donchian) continue;
            if (ci==1 && !enable_keltner) continue;
            if (pos[ci].active || pos[ci].cooldown_bars>0) continue;
            int side=(ci==0)?_sig_donchian():_sig_keltner();
            if (side==0) continue;
            if (enable_mutex && _other_same(ci, side)) continue;
            _fire(ci, side, bid, ask, now_ms, hr);
        }
    }
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        for (int ci=0;ci<2;++ci) if (pos[ci].active) _manage(ci, bid, ask, now_ms);
    }
    bool any_open() const noexcept { return pos[0].active||pos[1].active; }
private:
    int _sig_donchian() const noexcept {
        const int N=donchian_N;
        if ((int)bars_.size()<N+1) return 0;
        const int last=(int)bars_.size()-1;
        double hi=bars_[last-N].high, lo=bars_[last-N].low;
        for (int k=last-N+1; k<=last-1; ++k) { if (bars_[k].high>hi) hi=bars_[k].high; if (bars_[k].low<lo) lo=bars_[k].low; }
        if (bars_[last].close>hi) return +1;
        if (bars_[last].close<lo) return -1;
        return 0;
    }
    int _sig_keltner() const noexcept {
        if (!ema_initialised_||atr14_<=0) return 0;
        if ((int)bars_.size()<ema_period+2) return 0;
        const auto& cur=bars_.back();
        double up=ema20_+keltner_K*atr14_, lo=ema20_-keltner_K*atr14_;
        if (cur.close>up) return +1;
        if (cur.close<lo) return -1;
        return 0;
    }
    bool _other_same(int ci, int side) const noexcept {
        const bool wl=(side>0);
        for (int k=0;k<2;++k) { if (k==ci||!pos[k].active) continue; if (pos[k].is_long==wl) return true; }
        return false;
    }
    bool _cv(double sp, double tp) const noexcept {
        if (cost_ratio_min<=0) return true;
        double c=sp*20*lot+2.0*20*lot, g=tp*20*lot;
        return g>=c*cost_ratio_min;
    }
    void _fire(int ci, int side, double bid, double ask, int64_t now_ms, int hr) noexcept {
        double entry=(side>0)?ask:bid;
        if (entry<=0||atr14_<=0) return;
        double sl_m=(ci==0)?donchian_sl_mult:keltner_sl_mult;
        double tp_m=(ci==0)?donchian_tp_mult:keltner_tp_mult;
        double sa=sl_m*atr14_, sd=std::max(sa, min_sl_pts_floor), td=tp_m*atr14_;
        if (sd>sa && sa>0) { double r=sd/sa; td*=r; }
        double sp=ask-bid;
        if (!_cv(sp, td)) return;
        auto& p=pos[ci];
        p.active=true; p.is_long=(side>0); p.entry_px=entry;
        p.sl_px=(side>0)?entry-sd:entry+sd; p.tp_px=(side>0)?entry+td:entry-td;
        p.atr_at_entry=atr14_; p.entry_ts_ms=now_ms; p.bars_held=0; p.cooldown_bars=0;
        p.mfe_pts=0; p.mae_pts=0; p.proved=false; p.spread_at_entry=sp; p.hour_utc_at_entry=hr;
    }
    void _manage(int ci, double bid, double ask, int64_t now_ms) noexcept {
        auto& p=pos[ci]; double mid=(bid+ask)*0.5;
        if (mid>0 && p.entry_px>0) {
            double fav=p.is_long?(mid-p.entry_px):(p.entry_px-mid);
            if (fav>p.mfe_pts) p.mfe_pts=fav;
            if (fav<p.mae_pts) p.mae_pts=fav;
            if (!p.proved && p.mfe_pts>=prove_it_min_fav_pts) p.proved=true;
        }
        double el=(p.entry_ts_ms>0)?(now_ms-p.entry_ts_ms)/1000.0:0.0;
        if (!p.proved && el>=prove_it_secs) { double xp=p.is_long?bid:ask; _close(ci, xp, "PROVE_IT_FAIL", now_ms); return; }
        bool tp=false, sl=false; double xp=0;
        if (p.is_long) { if (bid<=p.sl_px){xp=p.sl_px;sl=true;} else if (bid>=p.tp_px){xp=p.tp_px;tp=true;} }
        else            { if (ask>=p.sl_px){xp=p.sl_px;sl=true;} else if (ask<=p.tp_px){xp=p.tp_px;tp=true;} }
        if (!tp && !sl) return;
        _close(ci, xp, tp?"TP_HIT":"SL_HIT", now_ms);
    }
    void _close(int ci, double xp, const char* reason, int64_t now_ms) noexcept {
        auto& p=pos[ci]; if (!p.active) return;
        double pts=p.is_long?(xp-p.entry_px):(p.entry_px-xp);
        double g=pts*20*lot, c=2.0*20*lot+p.spread_at_entry*20*lot;
        TradeRecord tr; tr.ts_ms_entry=p.entry_ts_ms; tr.ts_ms_exit=now_ms;
        tr.cell=ci; tr.is_long=p.is_long;
        tr.entry_price=p.entry_px; tr.exit_price=xp;
        tr.exit_reason=reason; tr.duration_s=(int)((now_ms-p.entry_ts_ms)/1000);
        tr.gross_pts=pts*lot; tr.gross_usd=g; tr.spread_at_entry_pts=p.spread_at_entry;
        tr.modeled_cost_usd=c; tr.net_usd=g-c; tr.hour_utc=p.hour_utc_at_entry;
        tr.size=lot; tr.mfe_pts=p.mfe_pts; tr.mae_pts=p.mae_pts;
        tr.atr_at_entry=p.atr_at_entry; tr.bars_held=p.bars_held;
        if (trade_sink) trade_sink(tr);
        p.active=false; p.cooldown_bars=1;
    }
};

struct HistTickStreamer {
    std::ifstream f; std::string line; bool opened=false;
    bool open(const char* p) { f.open(p); opened=f.is_open(); return opened; }
    static int64_t parse_hist_ts(const char* s, size_t len) {
        if (len<18) return -1;
        int Y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
        int M=(s[4]-'0')*10+(s[5]-'0'), D=(s[6]-'0')*10+(s[7]-'0');
        int hh=(s[9]-'0')*10+(s[10]-'0'), mm=(s[11]-'0')*10+(s[12]-'0');
        int ss=(s[13]-'0')*10+(s[14]-'0'), ms=(s[15]-'0')*100+(s[16]-'0')*10+(s[17]-'0');
        struct tm u{}; u.tm_year=Y-1900; u.tm_mon=M-1; u.tm_mday=D;
        u.tm_hour=hh; u.tm_min=mm; u.tm_sec=ss;
        return (int64_t)timegm(&u)*1000+ms;
    }
    bool next(int64_t& ts, double& bid, double& ask) {
        if (!opened) return false;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const char* p=line.c_str(); const char* c1=std::strchr(p, ',');
            if (!c1) continue; const char* c2=std::strchr(c1+1, ',');
            if (!c2) continue;
            ts=parse_hist_ts(p, (size_t)(c1-p));
            if (ts<0) continue;
            char buf[32]; size_t L=(size_t)(c2-(c1+1));
            if (L>=sizeof(buf)) continue;
            std::memcpy(buf, c1+1, L); buf[L]=0; bid=std::strtod(buf, nullptr);
            const char* c3=std::strchr(c2+1, ','); size_t L2=c3?(size_t)(c3-(c2+1)):std::strlen(c2+1);
            if (L2>=sizeof(buf)) continue;
            std::memcpy(buf, c2+1, L2); buf[L2]=0; ask=std::strtod(buf, nullptr);
            return true;
        }
        return false;
    }
};

static inline int hour_utc_from_ts_ms(int64_t ts) { time_t t=(time_t)(ts/1000); struct tm u{}; gmtime_r(&t, &u); return u.tm_hour; }

struct BarBuilder {
    int64_t cur_start=0; UstecBar cur; bool has=false;
    bool on_tick(int64_t ts, double bid, double ask, UstecBar& out) {
        double mid=(bid+ask)*0.5;
        int64_t bs=(ts/(5*60*1000))*(5*60*1000);
        if (!has) { cur_start=bs; cur.bar_start_ms=bs; cur.open=cur.high=cur.low=cur.close=mid; has=true; return false; }
        if (bs!=cur_start) { out=cur; cur_start=bs; cur.bar_start_ms=bs; cur.open=cur.high=cur.low=cur.close=mid; return true; }
        if (mid>cur.high) cur.high=mid; if (mid<cur.low) cur.low=mid; cur.close=mid;
        return false;
    }
};
} // namespace ustec

struct ComboConfig {
    int    donchian_N;
    double keltner_K;
    int    sess_start;
    int    sess_end;
    int    cell_mode; // 0=both, 1=donchian-only, 2=keltner-only
    int    tag=0;
};

struct ComboResult {
    ComboConfig cfg;
    int n_trades=0, n_wins=0;
    double gross=0, net=0, cost=0, sumw=0, suml=0;
    int tp=0, sl=0, pi=0, dc=0, kc=0;
    int oos_trades=0, oos_wins=0; double oos_net=0;
};

int main(int argc, char** argv) {
    if (argc<2) { std::fprintf(stderr, "usage: %s <csvs...>\n", argv[0]); return 2; }
    std::vector<std::string> csv_paths;
    std::string es_out="outputs/ustec_trend_follow_5m_entry_engsum.csv";
    std::string lb_out="outputs/ustec_trend_follow_5m_entry_leaderboard.csv";
    int64_t oos_cutoff=0;
    for (int i=1;i<argc;++i) {
        const char* a=argv[i];
        if (std::strcmp(a, "--engine-summary-out")==0 && i+1<argc) es_out=argv[++i];
        else if (std::strcmp(a, "--leaderboard-out")==0 && i+1<argc) lb_out=argv[++i];
        else if (std::strcmp(a, "--oos-cutoff-ts")==0 && i+1<argc) oos_cutoff=std::strtoll(argv[++i], nullptr, 10);
        else if (a[0]!='-') csv_paths.push_back(a);
    }
    std::sort(csv_paths.begin(), csv_paths.end());

    std::vector<int>    Ns        = {15, 20, 25};
    std::vector<double> Ks        = {1.5, 2.0, 2.5};
    std::vector<std::pair<int,int>> sessions = { {0,24}, {13,21}, {12,17} };
    std::vector<int>    modes     = {0, 1, 2}; // both, donchian, keltner

    std::vector<ComboConfig> combos;
    // Baseline: Plan A best + Plan B winner (MIN_ATR=20) with live entry params
    combos.push_back({20, 2.0, 0, 24, 0, 1});
    for (int N : Ns)
      for (double K : Ks)
        for (auto& s : sessions)
          for (int m : modes)
            combos.push_back({N, K, s.first, s.second, m, 0});
    const int N_combos = (int)combos.size();
    std::printf("[entry] %d combos (1 baseline + %d sweep)\n", N_combos, N_combos-1);

    std::vector<ustec::UstecTfEngine> engines(N_combos);
    std::vector<ComboResult> results(N_combos);
    for (int i=0; i<N_combos; ++i) {
        // All engines use Plan A best + Plan B winner pinned
        engines[i].donchian_sl_mult=3.0; engines[i].donchian_tp_mult=7.0;
        engines[i].keltner_sl_mult=3.0;  engines[i].keltner_tp_mult=7.0;
        engines[i].prove_it_secs=150.0;  engines[i].prove_it_min_fav_pts=2.0;
        engines[i].min_atr_pts=20.0;     engines[i].min_sl_pts_floor=15.0;
        engines[i].cost_ratio_min=1.5;
        // Entry-side variants
        engines[i].donchian_N    = combos[i].donchian_N;
        engines[i].keltner_K     = combos[i].keltner_K;
        engines[i].session_start_utc = combos[i].sess_start;
        engines[i].session_end_utc   = combos[i].sess_end;
        engines[i].enable_donchian = (combos[i].cell_mode == 0 || combos[i].cell_mode == 1);
        engines[i].enable_keltner  = (combos[i].cell_mode == 0 || combos[i].cell_mode == 2);

        results[i].cfg = combos[i];
        ComboResult* rp = &results[i];
        engines[i].trade_sink = [rp, oos_cutoff](const ustec::TradeRecord& tr) {
            ComboResult& r=*rp;
            r.n_trades++; r.gross+=tr.gross_usd; r.cost+=tr.modeled_cost_usd; r.net+=tr.net_usd;
            if (tr.net_usd>0) { r.n_wins++; r.sumw+=tr.net_usd; } else r.suml+=tr.net_usd;
            if (tr.exit_reason=="TP_HIT") r.tp++;
            else if (tr.exit_reason=="SL_HIT") r.sl++;
            else if (tr.exit_reason=="PROVE_IT_FAIL") r.pi++;
            if (tr.cell==0) r.dc++; else r.kc++;
            if (oos_cutoff>0 && tr.ts_ms_entry>=oos_cutoff) {
                r.oos_trades++; r.oos_net+=tr.net_usd;
                if (tr.net_usd>0) r.oos_wins++;
            }
        };
    }

    ustec::BarBuilder builder;
    ustec::HistTickStreamer st;
    int64_t ts=0, total=0; double bid=0, ask=0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& path : csv_paths) {
        std::printf("[entry] streaming %s\n", path.c_str()); std::fflush(stdout);
        if (!st.open(path.c_str())) continue;
        int64_t fc=0;
        while (st.next(ts, bid, ask)) {
            ++fc; ++total;
            if (bid<=0||ask<=0) continue;
            for (int i=0; i<N_combos; ++i) if (engines[i].any_open()) engines[i].on_tick(bid, ask, ts);
            ustec::UstecBar cb;
            if (builder.on_tick(ts, bid, ask, cb)) {
                int hr=ustec::hour_utc_from_ts_ms(cb.bar_start_ms);
                for (int i=0; i<N_combos; ++i) engines[i].on_5m_bar(cb, bid, ask, ts, hr);
            }
        }
        st.f.close();
        auto dt=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  [entry] %lld ticks elapsed=%llds\n", (long long)fc, (long long)dt);
        std::fflush(stdout);
    }
    std::ofstream es(es_out);
    es << "combo_id,tag,donchian_N,keltner_K,sess_start,sess_end,cell_mode,n_trades,n_wins,gross,net,cost,sumw,suml,tp,sl,pi,dc,kc,oos_trades,oos_wins,oos_net\n";
    for (int i=0;i<N_combos;++i) {
        const ComboResult& r=results[i];
        es << i << "," << r.cfg.tag << "," << r.cfg.donchian_N << ","
           << std::fixed << std::setprecision(2) << r.cfg.keltner_K << ","
           << r.cfg.sess_start << "," << r.cfg.sess_end << "," << r.cfg.cell_mode << ","
           << r.n_trades << "," << r.n_wins << ","
           << std::setprecision(4) << r.gross << "," << r.net << "," << r.cost << ","
           << r.sumw << "," << r.suml << ","
           << r.tp << "," << r.sl << "," << r.pi << "," << r.dc << "," << r.kc << ","
           << r.oos_trades << "," << r.oos_wins << "," << r.oos_net << "\n";
    }
    es.close();
    std::vector<int> ord(N_combos); for (int i=0;i<N_combos;++i) ord[i]=i;
    std::sort(ord.begin(), ord.end(), [&](int a, int b){ return results[a].net > results[b].net; });
    std::ofstream lf(lb_out);
    lf << "rank,combo_id,tag,donchian_N,keltner_K,sess_start,sess_end,cell_mode,n_trades,n_wins,WR_pct,net,gross,cost,mean_net,PF,tp,sl,pi,dc,kc,oos_trades,oos_net\n";
    for (int r=0;r<N_combos;++r) {
        int ci=ord[r]; const ComboResult& x=results[ci];
        double wr=x.n_trades>0?100.0*x.n_wins/x.n_trades:0;
        double mn=x.n_trades>0?x.net/x.n_trades:0;
        double pf=(std::fabs(x.suml)>1e-9)?-x.sumw/x.suml:0;
        lf << (r+1) << "," << ci << "," << x.cfg.tag << ","
           << x.cfg.donchian_N << "," << std::fixed << std::setprecision(2) << x.cfg.keltner_K << ","
           << x.cfg.sess_start << "," << x.cfg.sess_end << "," << x.cfg.cell_mode << ","
           << x.n_trades << "," << x.n_wins << "," << wr << ","
           << x.net << "," << x.gross << "," << x.cost << ","
           << std::setprecision(4) << mn << "," << pf << ","
           << x.tp << "," << x.sl << "," << x.pi << "," << x.dc << "," << x.kc << ","
           << x.oos_trades << "," << std::setprecision(2) << x.oos_net << "\n";
    }
    lf.close();
    auto dt=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-t0).count();
    std::printf("[entry] %lld ticks across %d engines in %llds\n", (long long)total, N_combos, (long long)dt);

    {
        const ComboResult& b = results[0];
        std::printf("\n=== BASELINE (Plan A+B pinned, live entry params) ===\n");
        std::printf("  trades=%d WR=%.2f%% net=$%.2f gross=$%.2f cost=$%.2f TP=%d SL=%d PI=%d D=%d K=%d oos_tr=%d oos_net=$%.2f\n",
            b.n_trades, b.n_trades>0?100.0*b.n_wins/b.n_trades:0, b.net, b.gross, b.cost,
            b.tp, b.sl, b.pi, b.dc, b.kc, b.oos_trades, b.oos_net);
    }
    std::printf("\n=== TOP 10 by net ===\n");
    std::printf("%4s %4s %4s %4s %4s %4s %4s %4s %7s %10s %7s %7s %10s\n",
        "rank","id","tag","N","K","S0","S1","mode","trades","net$","WR%","oos","oos_net");
    for (int r=0;r<std::min(10, N_combos);++r) {
        int ci=ord[r]; const ComboResult& x=results[ci];
        double wr=x.n_trades>0?100.0*x.n_wins/x.n_trades:0;
        std::printf("%4d %4d %4d %4d %4.1f %4d %4d %4d %7d %10.2f %7.2f %7d %10.2f\n",
            r+1, ci, x.cfg.tag, x.cfg.donchian_N, x.cfg.keltner_K, x.cfg.sess_start, x.cfg.sess_end, x.cfg.cell_mode,
            x.n_trades, x.net, wr, x.oos_trades, x.oos_net);
    }
    return 0;
}
