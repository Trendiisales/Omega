// bigcap_momo_ibkr.cpp -- the ONLY Omega TU (besides ibkr_exec.cpp) that pulls in
// the TWS API for the in-process BigCapMomo engine.
//
// Implements the thin omega::bigcap_momo_ibkr:: interface (BigCapMomoIbkr.hpp) by
// wrapping a file-static EWrapper engine on its own reader/pump thread. The TWS
// headers (and their C <stdlib.h> includes) stay out of the giant main Omega TU,
// avoiding the namespace pollution that broke std::atoll on the first IBKR build.
//
// Compiled into Omega.exe only (CMake), where OMEGA_WITH_IBKR is defined. For any
// other target the interface degrades to safe no-ops.
//
// The scan / SPY-200MA regime / day-gate / ignition / vol-surge / wide-trail
// entry+exit logic is a VERBATIM port of the validated standalone
// ibkr/BigCapMomoEngine.cpp (do NOT alter -- it's the backtested edge). The only
// additions here are: a mutex over the position book, collect_positions() for the
// GUI, omega::TradeRecord emission on close, and thread lifecycle (start/stop).
#include "BigCapMomoIbkr.hpp"

#ifdef OMEGA_WITH_IBKR

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "ScannerSubscription.h"
#include "Contract.h"
#include "Order.h"
#include "RiskManager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace omega {
namespace bigcap_momo_ibkr {
namespace {

// ── per-symbol live state (ported from the standalone Sym + telemetry fields) ──
struct Sym {
    Contract c;
    double day_open = 0.0;
    // ── 5m bar feed via reqHistoricalData(keepUpToDate): forming-bar cache + warmup flag ──
    bool   hist_done=false;                       // initial historical dump done -> live updates eval entries
    long   hb_t=-1;                               // epoch sec of the forming 5m bar (formatDate=2)
    double hb_o=0, hb_h=0, hb_l=0, hb_c=0, hb_v=0;// forming 5m bar OHLCV
    std::deque<double> closes;                    // finalized 5m closes (ignition lookback)
    std::deque<double> vols;                      // finalized 5m volumes (surge avg)
    bool   inpos=false; double entry=0, peak=0; int hold=0; double notional=0;
    double atr=0, atr_sum=0, prev_c_atr=0; int atr_n=0;   // Wilder ATR for ATR-trail (S-2026-06-18)
    // ── telemetry additions ──
    double  last=0;                               // latest traded price (for current/unrealized)
    double  trough=0;                             // min price since entry (for MAE)
    long    size=0;                               // share size at entry
    int64_t entry_ts=0;                           // epoch seconds of entry
};

class Engine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Config cfg_;
    OrderId nextId_=0;
    std::map<int,Sym> subs_;                      // reqId -> Sym (real-time-bar stream id)
    int next_rt_=20000; bool scanDone_=false;
    RiskManager risk_{50000.0};
    std::mutex book_mu_;                          // guards subs_ vs collect_positions thread
    std::function<void(const TradeRecord&)> on_trade_record_;
    int trade_id_=0;
    // S-2026-06-20: handshake liveness. eConnect returns true on bare TCP even when
    // the gateway never completes the API session (stale clientId / not logged in).
    // nextValidId arrival is the ONLY proof the API session is live. start() waits on
    // this with a timeout so a dead handshake fails LOUD (BIGCAP_IBKR_DOWN) instead of
    // a silent "connected, returned true, scanned nothing" death (root cause 06-19).
    std::atomic<bool> handshake_done_{false};

    // ── market-regime detector (SPY 200MA + rising) ──
    static const int MKT_REQ=9001;
    std::deque<double> mkt_closes_;
    bool market_ok_=false;
    Contract spy() const { Contract c; c.symbol="SPY"; c.secType="STK"; c.exchange="SMART"; c.currency="USD"; return c; }
    void recompute_regime(){
        if(!cfg_.regime_gate){ market_ok_=true; return; }
        const int MKT_SMA=200, MKT_SLOPE_LB=20;
        int n=(int)mkt_closes_.size(), need=MKT_SMA+MKT_SLOPE_LB;
        if(n<need){ market_ok_=false; return; }
        double sma=0,smap=0;
        for(int k=0;k<MKT_SMA;k++){ sma+=mkt_closes_[n-1-k]; smap+=mkt_closes_[n-1-MKT_SLOPE_LB-k]; }
        sma/=MKT_SMA; smap/=MKT_SMA; double c=mkt_closes_[n-1];
        bool was=market_ok_; market_ok_=(c>sma && sma>smap);
        if(market_ok_!=was){ printf("[BigCapMomo] REGIME %s (SPY=%.2f sma200=%.2f rising=%d)\n",
            market_ok_?"ON uptrend":"OFF grind/chop->FLAT",c,sma,(int)(sma>smap)); fflush(stdout); }
    }

public:
    Engine(){ risk_.new_day(); cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void set_config(const Config& c){ cfg_=c; }
    bool handshake_done() const { return handshake_done_.load(std::memory_order_relaxed); }
    void set_sink(std::function<void(const TradeRecord&)> cb){ on_trade_record_=std::move(cb); }

    bool connect(){
        if(!cli_->eConnect(cfg_.host.c_str(),cfg_.port,cfg_.client_id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true;
    }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    void disconnect(){ if(cli_) cli_->eDisconnect(); }

    // open-position snapshot for the GUI (thread-safe). LONG-only engine.
    std::vector<PositionSnapshot> collect_positions(){
        std::vector<PositionSnapshot> v;
        std::lock_guard<std::mutex> lk(book_mu_);
        for(auto& kv : subs_){
            Sym& s=kv.second; if(!s.inpos) continue;
            PositionSnapshot ps;
            ps.symbol         = s.c.symbol;
            ps.side           = "LONG";
            ps.size           = (double)s.size;
            ps.entry          = s.entry;
            ps.current        = s.last;
            ps.unrealized_pnl = (s.last - s.entry) * (double)s.size;
            ps.mfe            = (s.peak  - s.entry) * (double)s.size;
            ps.mae            = (s.entry - s.trough)* (double)s.size;
            ps.engine         = cfg_.engine_tag;
            ps.entry_ts       = s.entry_ts;
            ps.tp             = 0.0;                          // runner: no TP
            ps.sl             = (cfg_.atr_len>0 && s.atr>0) ? (s.peak - cfg_.atr_mult*s.atr)
                                                            : s.peak * (1 - cfg_.trail_pct); // current trailing stop
            v.push_back(ps);
        }
        return v;
    }

    void nextValidId(OrderId id) override { nextId_=id;
        handshake_done_.store(true, std::memory_order_relaxed);   // API session live (root-cause guard 06-20)
        cli_->reqMarketDataType(cfg_.market_data_type);   // 1=live 2=frozen 3=delayed 4=delayed-frozen
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_GAIN";
        s.abovePrice=cfg_.px_min; s.aboveVolume=100000;
        // S-2026-06-19: real big-cap floor (micro-caps = slippage death). IBKR
        // marketCapAbove unit = MILLIONS (2000 = $2B; 2e9 = the 0-rows bug).
        if(cfg_.market_cap_above_musd > 0.0) s.marketCapAbove = cfg_.market_cap_above_musd;
        cli_->reqScannerSubscription(9100,s,TagValueListSPtr(),TagValueListSPtr());
        if(cfg_.regime_gate) cli_->reqHistoricalData(MKT_REQ, spy(), "", "1 Y", "1 day", "TRADES", 1, 1, true, TagValueListSPtr());
        printf("[BigCapMomo] scan TOP_PERC_GAIN (gate>=%.0f%% trail=%.0f%% IG=%.0f%% volx=%.0f regime_gate=%d) %s\n",
               cfg_.gate_pct,cfg_.trail_pct*100,cfg_.ig_pct,cfg_.volx,(int)cfg_.regime_gate,cfg_.paper_only?"PAPER":"LIVE"); fflush(stdout);
    }

    // ── historical bars: MKT_REQ = SPY daily (regime); subs_ rids = per-symbol 5m feed ──
    void historicalData(TickerId rid, const Bar& b) override {
        if((int)rid==MKT_REQ){
            mkt_closes_.push_back(b.close); if((int)mkt_closes_.size()>400) mkt_closes_.pop_front(); return; }
        // per-symbol 5m warmup bar -> seed rolling history + session open (NO entry eval)
        std::lock_guard<std::mutex> lk(book_mu_);
        auto it=subs_.find((int)rid); if(it==subs_.end()) return; Sym& s=it->second;
        if(s.day_open<=0) s.day_open=b.open;
        s.last=b.close;
        s.closes.push_back(b.close); s.vols.push_back(DecimalFunctions::decimalToDouble(b.volume));
        if((int)s.closes.size()>200) s.closes.pop_front();
        if((int)s.vols.size()>200)   s.vols.pop_front();
    }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        if(rid==MKT_REQ){ recompute_regime();
            printf("[BigCapMomo] regime seeded: %zu SPY daily closes, market_ok=%d\n",mkt_closes_.size(),(int)market_ok_); fflush(stdout); return; }
        std::lock_guard<std::mutex> lk(book_mu_);
        auto it=subs_.find(rid); if(it!=subs_.end()) it->second.hist_done=true;   // live updates now eval entries
    }
    void historicalDataUpdate(TickerId rid, const Bar& b) override {
        if((int)rid==MKT_REQ){
            if(!mkt_closes_.empty()) mkt_closes_.back()=b.close; else mkt_closes_.push_back(b.close);
            recompute_regime(); return; }
        // per-symbol live 5m bar: IBKR re-sends the forming bar each update, new ts on rollover.
        std::lock_guard<std::mutex> lk(book_mu_);
        auto it=subs_.find((int)rid); if(it==subs_.end()) return; Sym& s=it->second;
        long t = atol(b.time.c_str());                    // epoch sec (formatDate=2)
        double v = DecimalFunctions::decimalToDouble(b.volume);
        s.last=b.close; if(s.inpos && (s.trough<=0 || b.low<s.trough)) s.trough=b.low;
        if(s.hb_t<0){ s.hb_t=t; s.hb_o=b.open; s.hb_h=b.high; s.hb_l=b.low; s.hb_c=b.close; s.hb_v=v; return; }
        if(t==s.hb_t){ s.hb_o=b.open; s.hb_h=b.high; s.hb_l=b.low; s.hb_c=b.close; s.hb_v=v; return; }
        // new timestamp -> previous 5m bar complete -> evaluate (entries only after warmup)
        if(s.hist_done) on_5m_bar(s, s.hb_o, s.hb_h, s.hb_l, s.hb_c, s.hb_v, s.hb_t);
        s.hb_t=t; s.hb_o=b.open; s.hb_h=b.high; s.hb_l=b.low; s.hb_c=b.close; s.hb_v=v;
    }

    void scannerData(int,int rank,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        if(rank>=20) return;                         // top-20 movers only
        int rid=next_rt_++; Sym x; x.c=cd.contract;
        { std::lock_guard<std::mutex> lk(book_mu_); subs_[rid]=x; }
        // 5m bars via reqHistoricalData(keepUpToDate) -- works with the US-equity data
        // entitlement (reqRealTimeBars is live-only and 420s even when the data is held).
        // formatDate=2 -> bar.time is epoch seconds; useRTH=1; endDateTime="" required for keepUpToDate.
        cli_->reqHistoricalData(rid, x.c, "", "1 D", "5 mins", "TRADES", 1, 2, true, TagValueListSPtr());
    }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }

    // called with book_mu_ HELD (from historicalDataUpdate, on 5m bar completion)
    void on_5m_bar(Sym& s, double o, double h, double l, double c, double v, long t) {
        if(s.day_open<=0) s.day_open=o;
        // ── Wilder ATR every bar (for ATR-trail), S-2026-06-18 ──
        { double tr=h-l; if(s.prev_c_atr>0){ double a=h-s.prev_c_atr; if(a<0)a=-a; double b=s.prev_c_atr-l; if(b<0)b=-b; if(a>tr)tr=a; if(b>tr)tr=b; }
          s.prev_c_atr=c;
          if(cfg_.atr_len>0){ if(s.atr_n<cfg_.atr_len){ s.atr_sum+=tr; if(++s.atr_n==cfg_.atr_len) s.atr=s.atr_sum/cfg_.atr_len; }
                              else s.atr=(s.atr*(cfg_.atr_len-1)+tr)/cfg_.atr_len; } }
        // ── manage open position first (gain-protect exit: ATR-trail + BE-ratchet + ride) ──
        if(s.inpos){
            s.hold++;
            if(h>s.peak) s.peak=h;
            double tstop = (cfg_.trail_pct>0.0) ? s.peak*(1-cfg_.trail_pct) : 0.0;        // %-trail (if on)
            if(cfg_.atr_len>0 && s.atr>0){ double a=s.peak - cfg_.atr_mult*s.atr; if(a>tstop) tstop=a; }  // ATR-trail
            if(cfg_.be_arm_pct>0 && s.peak >= s.entry*(1+cfg_.be_arm_pct)){              // BE-ratchet lock
                double bf=s.entry*(1+cfg_.be_floor_pct); if(bf>tstop) tstop=bf; }
            if(tstop>0 && l<=tstop){ close_pos(s,tstop,"TRAIL"); return; }
            if(s.hold>=cfg_.maxhold){
                bool in_profit = c > s.entry;   // ride a still-profitable winner past the clock
                if(!(cfg_.maxhold_skip_if_profit && in_profit)){ close_pos(s,c,"MAXHOLD"); return; }
            }
            return;
        }
        // ── entry gates (mirror the validated backtest exactly) ──
        bool fire=false;
        if(c>=cfg_.px_min && s.day_open>0 && (market_ok_ || !cfg_.regime_gate)){
            double day_up=(c/s.day_open-1)*100;
            if(day_up>=cfg_.gate_pct && (int)s.vols.size()>=20 && (int)s.closes.size()>=cfg_.lb){
                double avgv=0; for(int k=0;k<20;k++) avgv+=s.vols[s.vols.size()-1-k]; avgv/=20.0;
                double base=s.closes[s.closes.size()-cfg_.lb];
                bool vol_ok = (cfg_.volx<=0.0) || (avgv>0 && v>=cfg_.volx*avgv);
                if(vol_ok && base>0 && (c/base-1)*100>=cfg_.ig_pct) fire=true;
            }
        }
        if(fire){
            double notional=risk_.allow_entry(s.c.symbol,c,c*(1-cfg_.trail_pct));
            if(cfg_.notional_usd>0 && notional>cfg_.notional_usd) notional=cfg_.notional_usd;
            if(notional>0){
                long sz=(long)std::max(1.0,notional/c); risk_.on_open(s.c.symbol,notional);
                s.inpos=true; s.entry=c; s.peak=h; s.trough=l; s.hold=0; s.notional=notional;
                s.size=sz; s.entry_ts=(int64_t)std::time(nullptr); s.last=c;
                printf("[BigCapMomo] %s LONG %s c=%.2f day_up=%.0f%% sz=%ld notional=$%.0f [conc=%d]\n",
                    cfg_.paper_only?"PAPER":"LIVE",s.c.symbol.c_str(),c,(c/s.day_open-1)*100,sz,notional,risk_.open_count());
                fflush(stdout);
                if(!cfg_.paper_only){ Order ord; ord.action="BUY"; ord.orderType="MKT";
                    ord.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,s.c,ord); }
            }
        }
        // push finalized bar into history (after entry eval, so lookback excludes current)
        s.closes.push_back(c); s.vols.push_back(v);
        if((int)s.closes.size()>200) s.closes.pop_front();
        if((int)s.vols.size()>200)   s.vols.pop_front();
    }

    // called with book_mu_ HELD
    void close_pos(Sym& s,double px,const char*why){
        double ret=(px*(1-0.0008))/s.entry-1.0;       // 8bps exit slip for the log/pnl
        risk_.on_close(s.c.symbol, ret*s.notional);
        printf("[BigCapMomo] %s EXIT %s %s entry=%.2f exit=%.2f ret=%.1f%% held=%d*5m\n",
            cfg_.paper_only?"PAPER":"LIVE",s.c.symbol.c_str(),why,s.entry,px,ret*100,s.hold); fflush(stdout);
        // ── emit closed trade to the shadow ledger / telemetry ──
        if(on_trade_record_){
            TradeRecord tr;
            tr.id         = ++trade_id_;
            tr.symbol     = s.c.symbol;
            tr.side       = "LONG";
            tr.entryPrice = s.entry;
            tr.exitPrice  = px;
            tr.tp         = 0.0;
            tr.sl         = s.peak*(1-cfg_.trail_pct);
            tr.size       = (double)s.size;
            tr.pnl        = (px - s.entry) * (double)s.size;            // gross price*size
            tr.mfe        = (s.peak  - s.entry) * (double)s.size;
            tr.mae        = (s.entry - s.trough)* (double)s.size;
            tr.entryTs    = s.entry_ts;
            tr.exitTs     = (int64_t)std::time(nullptr);
            tr.exitReason = (std::strcmp(why,"MAXHOLD")==0) ? "TIMEOUT" : "SL_HIT";
            tr.engine     = cfg_.engine_tag;
            tr.shadow     = cfg_.paper_only;                            // shadow == not routing live
            on_trade_record_(tr);
        }
        if(!cfg_.paper_only){ Order ord; ord.action="SELL"; ord.orderType="MKT";
            ord.totalQuantity=DecimalFunctions::doubleToDecimal((double)s.size); cli_->placeOrder(nextId_++,s.c,ord); }
        s.inpos=false; s.hold=0; s.trough=0; s.size=0;
    }

    void error(int,int code,const std::string& m,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158&&code!=162&&code!=200&&code!=2150&&code!=10090)
            printf("[BigCapMomo] err %d: %s\n",code,m.c_str()); }
};

// ── file-static singleton + thread lifecycle (mirrors ibkr_exec::eng()) ──
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
void set_on_trade_record(std::function<void(const TradeRecord&)> cb){ eng().set_sink(std::move(cb)); }

std::vector<PositionSnapshot> collect_positions(){
    if(!g_enabled.load() || !g_running.load()) return {};
    return eng().collect_positions();
}

bool start(){
    if(!g_enabled.load()) return false;
    if(g_running.exchange(true)) return true;          // already running -> no-op
    eng().set_config(g_cfg);
    if(!eng().connect()){
        printf("[BigCapMomo] connect failed (%s:%d id=%d)\n",g_cfg.host.c_str(),g_cfg.port,g_cfg.client_id); fflush(stdout);
        g_running.store(false); return false;
    }
    g_stop.store(false);
    g_thread = std::thread([]{
        printf("[BigCapMomo] in-process IBKR engine thread up (%s:%d)\n",g_cfg.host.c_str(),g_cfg.port); fflush(stdout);
        while(!g_stop.load()) eng().pump();            // long-running intraday stream
        eng().disconnect();
        printf("[BigCapMomo] engine thread stopped\n"); fflush(stdout);
    });
    // S-2026-06-20: eConnect()==true only means TCP connected. The gateway can accept
    // the socket yet never complete the API session (stale clientId, gateway not logged
    // in / API not enabled) -> nextValidId never arrives -> the engine scans NOTHING,
    // silently, while start() returns true and no alert fires. That is the exact 06-19
    // failure (9x "activating", zero [BigCapMomo] output, zero trades, no alarm).
    // Confirm the handshake; if it never lands, return false so omega_main raises the
    // loud [SYSTEM-ALERT] BIGCAP_IBKR_DOWN + GUI health banner instead of silent death.
    for(int i=0; i<120 && !eng().handshake_done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // up to ~12s
    if(!eng().handshake_done()){
        printf("[BigCapMomo] HANDSHAKE TIMEOUT 12s -- %s:%d accepted TCP but no nextValidId "
               "(clientId %d stale from a prior restart? gateway logged in + API enabled?) "
               "-- reporting DOWN\n", g_cfg.host.c_str(), g_cfg.port, g_cfg.client_id);
        fflush(stdout);
        return false;   // -> omega_main [SYSTEM-ALERT] BIGCAP_IBKR_DOWN
    }
    printf("[BigCapMomo] API handshake OK (nextValidId) -- engine live %s:%d id=%d\n",
           g_cfg.host.c_str(), g_cfg.port, g_cfg.client_id); fflush(stdout);
    return true;
}

void stop(){
    if(!g_running.load()) return;
    g_stop.store(true);
    if(g_thread.joinable()) g_thread.join();
    g_running.store(false);
}

} // namespace bigcap_momo_ibkr
} // namespace omega

#else  // !OMEGA_WITH_IBKR -- safe no-op stubs

namespace omega {
namespace bigcap_momo_ibkr {
void configure(const Config&) {}
void set_enabled(bool) {}
bool is_enabled() { return false; }
void set_on_trade_record(std::function<void(const TradeRecord&)>) {}
std::vector<PositionSnapshot> collect_positions() { return {}; }
bool start() { return false; }
void stop() {}
} // namespace bigcap_momo_ibkr
} // namespace omega

#endif
