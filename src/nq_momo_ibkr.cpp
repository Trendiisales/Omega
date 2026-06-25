// nq_momo_ibkr.cpp -- the third Omega TU (after ibkr_exec.cpp + bigcap_momo_ibkr.cpp)
// that pulls in the TWS API, for the in-process NQ/MNQ futures momentum engine.
//
// Implements the thin omega::nq_momo_ibkr:: interface (NqMomoIbkr.hpp) by wrapping a
// file-static EWrapper engine on its own reader/pump thread. TWS headers stay out of
// the giant main Omega TU (the std::atoll-pollution rule). Compiled into Omega.exe
// only (OMEGA_WITH_IBKR); degrades to safe no-ops elsewhere.
//
// The signal + exit logic is a VERBATIM port of backtest/momo_cont_nq.cpp (the
// validated edge -- PF 2.27 @2pt / 1.89 @4pt, both WF-halves+). Differences from the
// equity BigCapMomoIbkr sibling: ONE contract (no scanner / breadth / market-cap),
// self-regime gate (SMA200 of the instrument's OWN 5m closes, not SPY), and the trail
// uses ATR captured AT ENTRY held FIXED (not a live-recomputed ATR). LONG-only.
#include "NqMomoIbkr.hpp"

#ifdef OMEGA_WITH_IBKR

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace omega {
namespace nq_momo_ibkr {
namespace {

// ── single-instrument live state (ported from momo_cont_nq.cpp's per-bar loop) ──
struct Sym {
    bool   hist_done=false;                       // initial historical dump done -> live updates eval entries
    long   hb_t=-1;                               // epoch sec of the forming 5m bar (formatDate=2)
    double hb_o=0, hb_h=0, hb_l=0, hb_c=0;        // forming 5m bar OHLC
    std::deque<double> closes;                    // finalized 5m closes (ignition lookback + SMA200 regime)
    // Wilder ATR (for the trail) -- accumulated across BOTH the historical seed and live bars
    double atr=0, atr_sum=0, prev_c_atr=0; int atr_n=0;
    // position
    bool   inpos=false; double entry=0, peak=0, trough=0, sl_atr=0; int hold=0;
    double last=0;                                // latest close (current / unrealized)
    int64_t entry_ts=0;
};

class Engine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Config cfg_;
    OrderId nextId_=0;
    static const int BAR_REQ=7100;                // single 5m bar feed reqId
    Sym s_;
    std::mutex book_mu_;                          // guards s_ vs collect_positions thread
    std::function<void(const TradeRecord&)> on_trade_record_;
    int trade_id_=0;
    std::atomic<bool> handshake_done_{false};
    std::atomic<long long> last_activity_ms_{0};
    static long long now_ms(){ return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); }

    Contract contract() const {
        Contract c; c.symbol=cfg_.symbol; c.exchange=cfg_.exchange; c.currency=cfg_.currency;
        // EMPTY month -> CONTFUT (continuous data, never dead on a roll). Else FUT + month.
        if(cfg_.last_trade_month.empty()){ c.secType="CONTFUT"; }
        else { c.secType=cfg_.sec_type; c.lastTradeDateOrContractMonth=cfg_.last_trade_month; }
        return c;
    }

    // Wilder ATR update on a finalized bar (shared by historical seed + live path).
    void update_atr(double h,double l,double c){
        double tr=h-l;
        if(s_.prev_c_atr>0){ double a=h-s_.prev_c_atr; if(a<0)a=-a; double b=s_.prev_c_atr-l; if(b<0)b=-b;
                             if(a>tr)tr=a; if(b>tr)tr=b; }
        s_.prev_c_atr=c;
        if(cfg_.atr_len>0){ if(s_.atr_n<cfg_.atr_len){ s_.atr_sum+=tr; if(++s_.atr_n==cfg_.atr_len) s_.atr=s_.atr_sum/cfg_.atr_len; }
                            else s_.atr=(s_.atr*(cfg_.atr_len-1)+tr)/cfg_.atr_len; }
    }

public:
    Engine(){ cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void set_config(const Config& c){ cfg_=c; }
    void set_client_id(int id){ cfg_.client_id = id; }
    bool handshake_done() const { return handshake_done_.load(std::memory_order_relaxed); }
    long long last_activity_ms() const { return last_activity_ms_.load(std::memory_order_relaxed); }
    void set_sink(std::function<void(const TradeRecord&)> cb){ on_trade_record_=std::move(cb); }
    void log_heartbeat(){
        long long la=last_activity_ms_.load(std::memory_order_relaxed);
        double ago = la>0 ? (now_ms()-la)/1000.0 : -1.0;
        size_t nc; bool ip; { std::lock_guard<std::mutex> lk(book_mu_); nc=s_.closes.size(); ip=s_.inpos; }
        printf("[NqFutMomo] ALIVE %s bars=%zu inpos=%d atr=%.1f last_data=%s\n",
               cfg_.symbol.c_str(), nc, (int)ip, s_.atr, ago<0?"NEVER":(std::to_string((long)ago)+"s ago").c_str());
        fflush(stdout);
    }

    std::vector<PositionSnapshot> collect_positions(){
        std::vector<PositionSnapshot> v;
        std::lock_guard<std::mutex> lk(book_mu_);
        if(!s_.inpos) return v;
        PositionSnapshot ps;
        ps.symbol         = cfg_.symbol;
        ps.side           = "LONG";
        ps.size           = (double)cfg_.contracts;
        ps.entry          = s_.entry;
        ps.current        = s_.last;
        ps.unrealized_pnl = (s_.last - s_.entry) * cfg_.point_value * cfg_.contracts;
        ps.mfe            = (s_.peak  - s_.entry) * cfg_.point_value * cfg_.contracts;
        ps.mae            = (s_.entry - s_.trough)* cfg_.point_value * cfg_.contracts;
        ps.engine         = cfg_.engine_tag;
        ps.entry_ts       = s_.entry_ts;
        ps.tp             = 0.0;                                       // runner: no TP
        ps.sl             = (s_.sl_atr>0) ? (s_.peak - cfg_.atr_mult*s_.sl_atr) : 0.0;
        v.push_back(ps);
        return v;
    }

    void nextValidId(OrderId id) override { nextId_=id;
        handshake_done_.store(true, std::memory_order_relaxed);
        cli_->reqMarketDataType(cfg_.market_data_type);
        // 5m bars via reqHistoricalData(keepUpToDate). "5 D" seeds >200 bars so the SMA200
        // self-regime gate is WARM on connect (warm-seed mandate; no 200-bar cold start).
        // formatDate=2 -> bar.time is epoch seconds; useRTH=0 (futures trade ~24h);
        // endDateTime="" required for keepUpToDate.
        Contract c=contract();
        cli_->reqHistoricalData(BAR_REQ, c, "", "5 D", "5 mins", "TRADES", 0, 2, true, TagValueListSPtr());
        printf("[NqFutMomo] subscribe %s %s%s 5m (IG=%.2f%% LB=%d regime=SMA%d ATR%dx%.1f BE arm%.0f%%/floor%.0f%% maxhold=%d) %s\n",
               cfg_.symbol.c_str(), c.secType.c_str(),
               cfg_.last_trade_month.empty()?"(cont)":(" "+cfg_.last_trade_month).c_str(),
               cfg_.ig_pct, cfg_.lb, cfg_.regime_sma, cfg_.atr_len, cfg_.atr_mult,
               cfg_.be_arm_pct*100, cfg_.be_floor_pct*100, cfg_.maxhold,
               cfg_.paper_only?"PAPER":"LIVE"); fflush(stdout);
    }

    // ── historical seed: warmup bars (NO entry eval) -> rolling closes + ATR ──
    void historicalData(TickerId rid, const Bar& b) override {
        if((int)rid!=BAR_REQ) return;
        std::lock_guard<std::mutex> lk(book_mu_);
        s_.last=b.close;
        update_atr(b.high,b.low,b.close);
        s_.closes.push_back(b.close);
        if((int)s_.closes.size()>cfg_.regime_sma+50) s_.closes.pop_front();
    }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        if(rid!=BAR_REQ) return;
        std::lock_guard<std::mutex> lk(book_mu_);
        s_.hist_done=true;
        printf("[NqFutMomo] seeded: %zu 5m closes, atr=%.1f (warm) -- live entries armed\n",
               s_.closes.size(), s_.atr); fflush(stdout);
    }
    void historicalDataUpdate(TickerId rid, const Bar& b) override {
        if((int)rid!=BAR_REQ) return;
        last_activity_ms_.store(now_ms(), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(book_mu_);
        long t = atol(b.time.c_str());                    // epoch sec (formatDate=2)
        s_.last=b.close; if(s_.inpos && (s_.trough<=0 || b.low<s_.trough)) s_.trough=b.low;
        if(s_.hb_t<0){ s_.hb_t=t; s_.hb_o=b.open; s_.hb_h=b.high; s_.hb_l=b.low; s_.hb_c=b.close; return; }
        if(t==s_.hb_t){ s_.hb_o=b.open; s_.hb_h=b.high; s_.hb_l=b.low; s_.hb_c=b.close; return; }
        // new timestamp -> previous 5m bar complete -> evaluate (entries only after warmup)
        if(s_.hist_done) on_5m_bar(s_.hb_o, s_.hb_h, s_.hb_l, s_.hb_c, s_.hb_t);
        s_.hb_t=t; s_.hb_o=b.open; s_.hb_h=b.high; s_.hb_l=b.low; s_.hb_c=b.close;
    }

    // called with book_mu_ HELD (from historicalDataUpdate, on 5m bar completion)
    void on_5m_bar(double o, double h, double l, double c, long t) {
        update_atr(h,l,c);
        // ── manage open position first (fixed-ATR trail + BE-ratchet + ride) ──
        if(s_.inpos){
            s_.hold++;
            if(h>s_.peak) s_.peak=h;
            double tstop = s_.peak - cfg_.atr_mult*s_.sl_atr;                        // ATR-at-entry trail (fixed)
            if(cfg_.be_arm_pct>0 && s_.peak >= s_.entry*(1+cfg_.be_arm_pct)){        // BE-ratchet lock
                double bf=s_.entry*(1+cfg_.be_floor_pct); if(bf>tstop) tstop=bf; }
            if(l<=tstop){ close_pos(tstop,"TRAIL"); goto push; }
            if(s_.hold>=cfg_.maxhold && !(cfg_.maxhold_skip_if_profit && c>s_.entry)){
                close_pos(c,"MAXHOLD"); goto push; }
            goto push;
        }
        // ── entry gates (mirror momo_cont_nq.cpp exactly) ──
        // STALE-DATA GUARD: never enter off a stale bar (feed stalled/replayed).
        if(cfg_.max_bar_age_sec>0){
            long age = (long)(now_ms()/1000) - t;
            if(age > cfg_.max_bar_age_sec){
                static long s_stalelog=0; long nw=(long)(now_ms()/1000);
                if(nw - s_stalelog > 30){ s_stalelog=nw;
                    printf("[NqFutMomo] STALE-BAR %s age=%lds (>%lds) -- feed stale, entry blocked\n",
                           cfg_.symbol.c_str(), age, cfg_.max_bar_age_sec); fflush(stdout); }
                goto push;
            }
        }
        if((int)s_.closes.size() >= cfg_.lb && s_.atr > 0){
            // self-regime gate: close > SMA200 of own 5m closes (warm-seeded). Block until warm.
            bool regime_ok = true;
            if(cfg_.regime_gate){
                int rg=cfg_.regime_sma;
                if((int)s_.closes.size() < rg) regime_ok=false;               // warmup: block (warm-seed mandate)
                else { double sma=0; for(int k=0;k<rg;k++) sma+=s_.closes[s_.closes.size()-1-k]; sma/=rg;
                       regime_ok = (c > sma); }
            }
            double base=s_.closes[s_.closes.size()-cfg_.lb];                    // close LB bars before current
            double up = base>0 ? (c/base-1)*100 : 0;
            if(regime_ok && up>=cfg_.ig_pct && c>o){                            // ignition + green + regime
                s_.inpos=true; s_.entry=c; s_.peak=h; s_.trough=l; s_.hold=0;
                s_.sl_atr=s_.atr;                                              // FIX ATR at entry (held for trail)
                s_.entry_ts=(int64_t)std::time(nullptr); s_.last=c;
                printf("[NqFutMomo] %s LONG %s c=%.2f up=%.2f%% atr=%.1f sl=%.2f (%dx%.0f contracts)\n",
                    cfg_.paper_only?"PAPER":"LIVE", cfg_.symbol.c_str(), c, up, s_.atr,
                    c-cfg_.atr_mult*s_.atr, cfg_.contracts, cfg_.point_value);
                fflush(stdout);
                if(!cfg_.paper_only){ Order ord; ord.action="BUY"; ord.orderType="MKT";
                    ord.totalQuantity=DecimalFunctions::doubleToDecimal((double)cfg_.contracts);
                    cli_->placeOrder(nextId_++,contract(),ord); }
            }
        }
    push:
        // push finalized bar into history (AFTER entry eval, so lookback excludes current)
        s_.closes.push_back(c);
        if((int)s_.closes.size()>cfg_.regime_sma+50) s_.closes.pop_front();
    }

    // called with book_mu_ HELD
    void close_pos(double px,const char*why){
        double pts=(px - s_.entry) - cfg_.cost_pts;                            // net index points after cost
        double pnl=(px - s_.entry) * cfg_.point_value * cfg_.contracts;        // gross $ (ledger applies cost)
        printf("[NqFutMomo] %s EXIT %s %s entry=%.2f exit=%.2f net=%.1fpt held=%d*5m\n",
            cfg_.paper_only?"PAPER":"LIVE", cfg_.symbol.c_str(), why, s_.entry, px, pts, s_.hold); fflush(stdout);
        if(on_trade_record_){
            TradeRecord tr;
            tr.id         = ++trade_id_;
            tr.symbol     = cfg_.symbol;
            tr.side       = "LONG";
            tr.entryPrice = s_.entry;
            tr.exitPrice  = px;
            tr.tp         = 0.0;
            tr.sl         = s_.peak - cfg_.atr_mult*s_.sl_atr;
            tr.size       = (double)cfg_.contracts;
            tr.pnl        = pnl;
            tr.mfe        = (s_.peak  - s_.entry) * cfg_.point_value * cfg_.contracts;
            tr.mae        = (s_.entry - s_.trough)* cfg_.point_value * cfg_.contracts;
            tr.entryTs    = s_.entry_ts;
            tr.exitTs     = (int64_t)std::time(nullptr);
            tr.exitReason = (std::strcmp(why,"MAXHOLD")==0) ? "TIMEOUT" : "SL_HIT";
            tr.engine     = cfg_.engine_tag;
            tr.shadow     = cfg_.paper_only;
            on_trade_record_(tr);
        }
        if(!cfg_.paper_only){ Order ord; ord.action="SELL"; ord.orderType="MKT";
            ord.totalQuantity=DecimalFunctions::doubleToDecimal((double)cfg_.contracts);
            cli_->placeOrder(nextId_++,contract(),ord); }
        s_.inpos=false; s_.hold=0; s_.trough=0; s_.sl_atr=0;
    }

    // connect + confirm API handshake (nextValidId), mirrors bigcap_momo_ibkr::connect()
    bool connect(){
        handshake_done_.store(false, std::memory_order_relaxed);
        if(!cli_->eConnect(cfg_.host.c_str(),cfg_.port,cfg_.client_id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while(std::chrono::steady_clock::now() < deadline){
            rd_->processMsgs();
            if(handshake_done_.load(std::memory_order_relaxed)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    void hard_disconnect(){ if(cli_) cli_->eDisconnect(); rd_.reset();
                            cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    void disconnect(){ if(cli_) cli_->eDisconnect(); }

    void error(int,int code,const std::string& m,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158&&code!=162&&code!=200&&code!=2150&&code!=10090)
            printf("[NqFutMomo] err %d: %s\n",code,m.c_str()); }
};

// ── file-static singleton + thread lifecycle (mirrors bigcap_momo_ibkr) ──
static Engine&            eng()        { static Engine e; return e; }
static Config             g_cfg{};
static std::atomic<bool>  g_enabled{false};
static std::atomic<bool>  g_running{false};
static std::atomic<bool>  g_stop{false};
static std::thread        g_thread;

} // anonymous namespace

void configure(const Config& cfg){ g_cfg=cfg; eng().set_config(cfg); }
void set_enabled(bool on){ g_enabled.store(on); }
bool is_enabled(){ return g_enabled.load(); }
long long last_activity_ms(){ return g_running.load() ? eng().last_activity_ms() : 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)> cb){ eng().set_sink(std::move(cb)); }

std::vector<PositionSnapshot> collect_positions(){
    if(!g_enabled.load() || !g_running.load()) return {};
    return eng().collect_positions();
}

// paper/live port-vs-routing guard (mirrors bigcap_momo_ibkr::paper_port_guard)
static bool paper_port_guard(const Config& c){
    const bool live_port  = (c.port==7496 || c.port==4001);
    const bool paper_port = (c.port==7497 || c.port==4002);
    if(!c.paper_only){
        if(paper_port){
            printf("[NqFutMomo] ABORT: paper_only=false (LIVE order routing armed) but port %d is a "
                   "PAPER gateway -- inconsistent config. Refusing to start.\n", c.port); fflush(stdout);
            return false;
        }
        printf("[NqFutMomo] *** LIVE ORDER ROUTING ARMED *** paper_only=false on %s:%d -- REAL orders "
               "WILL be sent.\n", c.host.c_str(), c.port); fflush(stdout);
    } else if(live_port){
        printf("[NqFutMomo] shadow OK: paper_only=true on live gateway %d -- logging only, NO orders.\n",
               c.port); fflush(stdout);
    }
    return true;
}

bool start(){
    if(!g_enabled.load()) return false;
    if(!paper_port_guard(g_cfg)){ g_running.store(false); return false; }
    if(g_running.exchange(true)) return true;
    eng().set_config(g_cfg);
    const int base_id = g_cfg.client_id;
    bool live=false; int win_id=base_id;
    for(int attempt=0; attempt<3 && !live; ++attempt){
        const int id=base_id+attempt;
        eng().set_client_id(id);
        if(eng().connect()){
            win_id=id; live=true;
            printf("[NqFutMomo] API handshake OK (nextValidId) -- engine live %s:%d id=%d\n",
                   g_cfg.host.c_str(), g_cfg.port, id); fflush(stdout);
            break;
        }
        printf("[NqFutMomo] connect/handshake FAILED %s:%d id=%d (attempt %d/3) -- %s\n",
               g_cfg.host.c_str(), g_cfg.port, id, attempt+1,
               attempt<2 ? "TCP up but no nextValidId; rotating clientId" : "giving up"); fflush(stdout);
        eng().hard_disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if(!live){
        printf("[NqFutMomo] DOWN -- no API handshake after 3 clientId attempts (%d-%d).\n",
               base_id, base_id+2); fflush(stdout);
        g_running.store(false);
        return false;
    }
    g_stop.store(false);
    g_thread = std::thread([win_id]{
        printf("[NqFutMomo] in-process IBKR engine thread up (%s:%d id=%d)\n",
               g_cfg.host.c_str(), g_cfg.port, win_id); fflush(stdout);
        auto last_hb = std::chrono::steady_clock::now();
        while(!g_stop.load()){
            eng().pump();
            auto now = std::chrono::steady_clock::now();
            if(now - last_hb >= std::chrono::seconds(60)){ eng().log_heartbeat(); last_hb = now; }
        }
        eng().disconnect();
        printf("[NqFutMomo] engine thread stopped\n"); fflush(stdout);
    });
    return true;
}

void stop(){
    if(!g_running.load()) return;
    g_stop.store(true);
    if(g_thread.joinable()) g_thread.join();
    g_running.store(false);
}

} // namespace nq_momo_ibkr
} // namespace omega

#else  // !OMEGA_WITH_IBKR -- safe no-op stubs

namespace omega {
namespace nq_momo_ibkr {
void configure(const Config&) {}
void set_enabled(bool) {}
bool is_enabled() { return false; }
long long last_activity_ms() { return 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)>) {}
std::vector<PositionSnapshot> collect_positions() { return {}; }
bool start() { return false; }
void stop() {}
} // namespace nq_momo_ibkr
} // namespace omega

#endif
