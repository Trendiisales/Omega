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
#include "LukeEntryQuality.hpp"   // S-2026-06-25 A/C daily-setup detector (validated, pure-std)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <vector>
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
    long    last_entry_sd=-1;                      // S-2026-06-23b session-day of last entry (1 entry/name/day cap)
    // S-2026-06-25 Luke ADR pre-filter: 20-day ADR% from a one-shot daily-bar req.
    // Default true = permissive until the filter resolves / when luke_gate is off.
    bool    adr_ok=true; std::deque<double> draw;  // recent daily ranges %
    std::vector<omega::luke::Bar> dbars;           // S-2026-06-25 daily OHLC for the A/C setup detector
    char    luke_setup=0;                          // 'A'/'C' if a valid Luke daily setup is present, else 0
    double  luke_stop=0, luke_trig=0;              // the setup's structural stop / breakout trigger
};

class Engine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Config cfg_;
    OrderId nextId_=0;
    std::map<int,Sym> subs_;                      // reqId -> Sym (real-time-bar stream id)
    std::map<long,std::set<std::string>> day_ignis_;  // S-2026-06-23 breadth: distinct igniters per session-day
    int next_rt_=20000; bool scanDone_=false;
    static const int ADR_BASE=800000;             // S-2026-06-25 ADR pre-filter daily-req rid offset (above all 5m rids)
    std::map<int,int> adr2sym_;                   // ADR-daily rid -> the symbol's 5m rid (subs_ key)
    std::set<std::string> scanned_;               // S-2026-06-25 dedup: union of the two scan codes (no double-subscribe)
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
    // S-2026-06-20 liveness: a connected engine that silently stops receiving scanner/bar
    // data is the failure class no check caught on 06-19. Track last-data time + a scanner
    // hit count; the pump thread logs a 60s heartbeat and exposes last_activity_ms() so the
    // omega_main watchdog can raise [SYSTEM-ALERT] BIGCAP_STALE during RTH.
    std::atomic<long long> last_activity_ms_{0};   // wall-clock ms of last scanner/bar event
    std::atomic<long>      scan_hits_{0};          // cumulative scanner movers seen
    static long long now_ms(){ return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); }

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
        bool was=market_ok_;
        // PRODUCTION (regime_relaxed=false): BULL-only -> trade only in a confirmed uptrend
        //   (close>SMA200 AND SMA200 rising). RELAXED (regime_relaxed=true): not-BEAR -> block
        //   ONLY a confirmed downtrend (close<SMA200 AND SMA200 falling); trade BULL+NEUTRAL.
        // Tier-1 cross-regime daily proxy (backtest/bigcap_engine_protections.py, S-2026-06-24):
        //   on a turtle trend-long, not-BEAR (+6.8%) > none (+4.6%) > BULL-only (-2.4%) — BULL-only
        //   throws away profitable NEUTRAL/sideways days while not-BEAR keeps them and still blocks
        //   the bear bleed. SHADOW A/B ENABLER ONLY, default OFF (production unchanged). Confirm
        //   live shadow PF before flipping; bear protection is preserved either way.
        market_ok_ = cfg_.regime_relaxed ? !(c<sma && sma<smap) : (c>sma && sma>smap);
        if(market_ok_!=was){ printf("[BigCapMomo] REGIME %s (SPY=%.2f sma200=%.2f rising=%d relaxed=%d)\n",
            market_ok_?"ON ok-to-trade":"OFF downtrend->FLAT",c,sma,(int)(sma>smap),(int)cfg_.regime_relaxed); fflush(stdout); }
    }

public:
    Engine(){ risk_.new_day(); cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void set_config(const Config& c){ cfg_=c; }
    bool handshake_done() const { return handshake_done_.load(std::memory_order_relaxed); }
    long long last_activity_ms() const { return last_activity_ms_.load(std::memory_order_relaxed); }
    void log_heartbeat(){
        long long la = last_activity_ms_.load(std::memory_order_relaxed);
        double ago = la>0 ? (now_ms()-la)/1000.0 : -1.0;
        size_t subs; { std::lock_guard<std::mutex> lk(book_mu_); subs=subs_.size(); }
        printf("[BigCapMomo] ALIVE subs=%zu scan_hits=%ld market_ok=%d last_data=%s\n",
               subs, scan_hits_.load(), (int)market_ok_,
               ago<0 ? "NEVER" : (std::to_string((long)ago)+"s ago").c_str());
        fflush(stdout);
    }
    void set_sink(std::function<void(const TradeRecord&)> cb){ on_trade_record_=std::move(cb); }

    void set_client_id(int id){ cfg_.client_id = id; }

    // Connect AND confirm the API handshake synchronously. eConnect()==true is only a
    // TCP connect; nextValidId arriving is the only proof the API session is live. We
    // pump messages here (no long-lived thread yet) for up to ~12s. A gateway that
    // accepts the socket but never handshakes (stale clientId / not logged in / API off)
    // returns false so start() can rotate the clientId and retry, then fail LOUD --
    // instead of the 06-19 silent "connected, returned true, scanned nothing" death.
    bool connect(){
        handshake_done_.store(false, std::memory_order_relaxed);
        if(!cli_->eConnect(cfg_.host.c_str(),cfg_.port,cfg_.client_id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while(std::chrono::steady_clock::now() < deadline){
            rd_->processMsgs();                                  // drain reader queue
            if(handshake_done_.load(std::memory_order_relaxed)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;                                            // TCP up, no nextValidId
    }
    // Clean teardown for a retry: drop socket + reader, rebuild a fresh socket so the
    // next eConnect (new clientId) starts from a clean state.
    void hard_disconnect(){ if(cli_) cli_->eDisconnect(); rd_.reset();
                            cli_=std::make_unique<EClientSocket>(this,&sig_); }
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
        // S-2026-06-25 SECOND SCAN (Luke): TOP_PERC_GAIN finds names UP today, but setup C
        // (inside-day contraction) fires on a QUIET day -> a high-implied-vol scan surfaces the
        // high-ADR names IN A PAUSE that the gainer scan misses. Live-verified: adds 46 unique
        // names to the gainer set. Hits land in the SAME scannerData path (union). Behind luke_gate.
        if(cfg_.luke_gate){
            ScannerSubscription iv; iv.instrument="STK"; iv.locationCode="STK.US.MAJOR";
            iv.scanCode="HIGH_OPT_IMP_VOLAT"; iv.abovePrice=cfg_.px_min; iv.aboveVolume=100000;
            if(cfg_.market_cap_above_musd>0.0) iv.marketCapAbove=cfg_.market_cap_above_musd;
            cli_->reqScannerSubscription(9101,iv,TagValueListSPtr(),TagValueListSPtr());
        }
        if(cfg_.regime_gate) cli_->reqHistoricalData(MKT_REQ, spy(), "", "1 Y", "1 day", "TRADES", 1, 1, true, TagValueListSPtr());
        printf("[BigCapMomo] scan TOP_PERC_GAIN (gate>=%.0f%% trail=%.0f%% IG=%.0f%% volx=%.0f regime_gate=%d) %s\n",
               cfg_.gate_pct,cfg_.trail_pct*100,cfg_.ig_pct,cfg_.volx,(int)cfg_.regime_gate,cfg_.paper_only?"PAPER":"LIVE"); fflush(stdout);
    }

    // ── historical bars: MKT_REQ = SPY daily (regime); subs_ rids = per-symbol 5m feed ──
    void historicalData(TickerId rid, const Bar& b) override {
        if((int)rid==MKT_REQ){
            mkt_closes_.push_back(b.close); if((int)mkt_closes_.size()>400) mkt_closes_.pop_front(); return; }
        if((int)rid>=ADR_BASE){                                   // S-2026-06-25 ADR pre-filter daily bars
            std::lock_guard<std::mutex> lk(book_mu_);
            auto m=adr2sym_.find((int)rid); if(m==adr2sym_.end()) return;
            auto it=subs_.find(m->second); if(it==subs_.end()) return;
            if(b.close>0){ it->second.draw.push_back((b.high-b.low)/b.close*100.0);
                           if((int)it->second.draw.size()>20) it->second.draw.pop_front();
                           it->second.dbars.push_back({b.open,b.high,b.low,b.close}); }  // for A/C detector
            return; }
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
        if(rid>=ADR_BASE){                                       // S-2026-06-25 ADR pre-filter resolves
            std::lock_guard<std::mutex> lk(book_mu_);
            cli_->cancelHistoricalData(rid);
            auto m=adr2sym_.find(rid); if(m==adr2sym_.end()) return;
            auto it=subs_.find(m->second); adr2sym_.erase(m); if(it==subs_.end()) return;
            Sym& s=it->second; double adr=0; for(double r:s.draw) adr+=r;
            if(!s.draw.empty()) adr/=s.draw.size();
            s.adr_ok = s.draw.empty() ? true : (adr >= cfg_.luke_adr_min);  // empty=permissive
            if(!s.adr_ok) printf("[BigCapMomo] ADR-PREFILTER drop %s adr=%.2f%% < %.2f%%\n",
                                 s.c.symbol.c_str(), adr, cfg_.luke_adr_min);
            // --- A/C daily-setup detector: the validated entry-quality gate ---
            // omega::luke::evaluate scans the daily bars for setup A (pullback-to-rising-9/21-EMA)
            // or C (inside-day/micro-VCP breakout) with a tight (<=luke_max_stopw) structural stop.
            // Variants wired via cfg_.luke_mode_A / luke_mode_C. Result gates the live entry below.
            if(s.adr_ok && (int)s.dbars.size()>=56){
                omega::luke::Knobs K; K.adr_min=cfg_.luke_adr_min; K.max_stopw=cfg_.luke_max_stopw;
                auto sig=omega::luke::evaluate(s.dbars,(int)s.dbars.size()-1,K,cfg_.luke_mode_A,cfg_.luke_mode_C);
                s.luke_setup=sig.setup; s.luke_trig=sig.trig; s.luke_stop=sig.stop;
                if(sig.setup) printf("[BigCapMomo] LUKE-SETUP %s = %c (trig=%.2f stop=%.2f stopw=%.1f%%)\n",
                                     s.c.symbol.c_str(), sig.setup, sig.trig, sig.stop, sig.stopw*100);
            }
            std::vector<omega::luke::Bar>().swap(s.dbars);   // free the daily-bar buffer
            fflush(stdout); return; }
        std::lock_guard<std::mutex> lk(book_mu_);
        auto it=subs_.find(rid); if(it!=subs_.end()) it->second.hist_done=true;   // live updates now eval entries
    }
    void historicalDataUpdate(TickerId rid, const Bar& b) override {
        if((int)rid==MKT_REQ){
            if(!mkt_closes_.empty()) mkt_closes_.back()=b.close; else mkt_closes_.push_back(b.close);
            recompute_regime(); return; }
        // per-symbol live 5m bar: IBKR re-sends the forming bar each update, new ts on rollover.
        last_activity_ms_.store(now_ms(), std::memory_order_relaxed);   // liveness: data flowing
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
        if(rank>=20) return;                         // top-20 movers per scan
        last_activity_ms_.store(now_ms(), std::memory_order_relaxed);   // liveness: scanner alive
        { std::lock_guard<std::mutex> lk(book_mu_);
          if(!scanned_.insert(cd.contract.symbol).second) return; }     // already subscribed via the other scan
        scan_hits_.fetch_add(1, std::memory_order_relaxed);
        int rid=next_rt_++; Sym x; x.c=cd.contract;
        { std::lock_guard<std::mutex> lk(book_mu_); subs_[rid]=x; }
        // 5m bars via reqHistoricalData(keepUpToDate) -- works with the US-equity data
        // entitlement (reqRealTimeBars is live-only and 420s even when the data is held).
        // formatDate=2 -> bar.time is epoch seconds; useRTH=1; endDateTime="" required for keepUpToDate.
        cli_->reqHistoricalData(rid, x.c, "", "1 D", "5 mins", "TRADES", 1, 2, true, TagValueListSPtr());
        // S-2026-06-25 ADR PRE-FILTER + A/C setup: one-shot 1-year daily req -> 20-day ADR%. The scanner
        // (TOP_PERC_GAIN) ranks by today's % move, NOT historical range; this drops names whose
        // ADR < luke_adr_min so the live universe matches the validated high-ADR roster (BT
        // reconstruction: rolling top-gainer watchlist + ADR>=4% -> PF 2.74->2.94, held).
        // Behind luke_gate (default OFF). Resolves in historicalDataEnd -> sets Sym.adr_ok.
        if(cfg_.luke_gate){
            int arid=ADR_BASE+rid; adr2sym_[arid]=rid;
            // 1 Y of daily bars: feeds BOTH the 20-day ADR pre-filter AND the A/C setup detector
            // (omega::luke::evaluate needs >=55 daily bars for EMA50/ADR20).
            cli_->reqHistoricalData(arid, x.c, "", "1 Y", "1 day", "TRADES", 1, 2, false, TagValueListSPtr());
        }
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
            // S-2026-06-25 luke tight structural stop (initial); the ATR-trail below takes over as the
            // position moves up (peak rises). Whichever is higher/hit-first governs.
            if(cfg_.luke_gate && s.luke_stop>0 && l<=s.luke_stop){ close_pos(s,s.luke_stop,"LUKE_STOP"); return; }
            double tstop = (cfg_.trail_pct>0.0) ? s.peak*(1-cfg_.trail_pct) : 0.0;        // %-trail (if on)
            if(cfg_.atr_len>0 && s.atr>0){ double a=s.peak - cfg_.atr_mult*s.atr; if(a>tstop) tstop=a; }  // ATR-trail
            if(cfg_.be_arm_pct>0 && s.peak >= s.entry*(1+cfg_.be_arm_pct)){              // BE-ratchet lock
                double bf=s.entry*(1+cfg_.be_floor_pct); if(bf>tstop) tstop=bf; }
            // S-2026-06-24 CLOSE-based give-back: bank a runner once this 5m bar CLOSES
            // retraced giveback_close_frac of the peak gain -- banks the reversal before the
            // wide trail round-trips it back. Validated on the REAL live trades
            // (backtest/coldcut_on_real_trades.py): book -$133 -> +$160 at 0.33. Fires on
            // the close (not the intrabar low) so a noise wick can't trigger it. 0 = off.
            if(cfg_.giveback_close_frac>0.0 && s.peak>s.entry){
                double gain=s.peak-s.entry;
                if((s.peak-c) >= cfg_.giveback_close_frac*gain){ close_pos(s,c,"GB_CLOSE"); return; }
            }
            if(tstop>0 && l<=tstop){ close_pos(s,tstop,"TRAIL"); return; }
            if(s.hold>=cfg_.maxhold){
                bool in_profit = c > s.entry;   // ride a still-profitable winner past the clock
                if(!(cfg_.maxhold_skip_if_profit && in_profit)){ close_pos(s,c,"MAXHOLD"); return; }
            }
            return;
        }
        // ── entry gates (mirror the validated backtest exactly) ──
        // S-2026-06-25 STALE-DATA GUARD: never enter off a stale bar. `t` is the just-closed
        // 5m bar's start (epoch sec); at live completion (now - t) ~= one TF. A bar older than
        // max_bar_age_sec = IBKR feed stalled/replayed/silently delayed -> refuse the entry
        // (exit-management above already ran). Closes the "market_ok=1 last_data=12313s" hole.
        if(cfg_.max_bar_age_sec > 0){
            long age = (long)(now_ms()/1000) - t;
            if(age > cfg_.max_bar_age_sec){
                static long s_stalelog=0; long nw=(long)(now_ms()/1000);
                if(nw - s_stalelog > 30){ s_stalelog=nw;
                    printf("[BigCapMomo] STALE-BAR %s age=%lds (>%lds) -- feed stale, entry blocked\n",
                           s.c.symbol.c_str(), age, cfg_.max_bar_age_sec); fflush(stdout); }
                return;
            }
        }
        bool fire=false; long sd=(t-8*3600)/86400;
        if(cfg_.luke_gate){
            // S-2026-06-25 ARM-AND-WAIT (the validated daily-swing entry). The A/C setup detected on
            // the daily bars (omega::luke::evaluate, historicalDataEnd) ARMS a breakout trigger
            // (luke_trig) + tight structural stop (luke_stop). We ENTER when intraday price BREAKS that
            // armed trigger. The setup forms on a PRIOR quiet bar (pullback/inside-day) and the break
            // fires LATER -- exactly as the BT models it. This REPLACES ignition when luke_gate is on;
            // a same-bar setup+ignition confluence suppressed everything (a breakout day is never itself
            // a pullback/inside day -> 0/28 on June live trades). adr_ok+luke_setup come from the daily
            // pre-filter/detector. v1: setup armed at connect / daily restart (intraday daily-refresh
            // = follow-up). Entry/stop use luke_trig/luke_stop in the open block below.
            if(s.adr_ok && s.luke_setup!=0 && (market_ok_||!cfg_.regime_gate)
               && h>=s.luke_trig && l>s.luke_stop && s.last_entry_sd!=sd){
                fire=true;
                printf("[BigCapMomo] LUKE-BREAK %s setup=%c trig=%.2f stop=%.2f (h=%.2f)\n",
                       s.c.symbol.c_str(), s.luke_setup, s.luke_trig, s.luke_stop, h); fflush(stdout);
            }
        } else {
            if(c>=cfg_.px_min && s.day_open>0 && (market_ok_ || !cfg_.regime_gate)){
                double day_up=(c/s.day_open-1)*100;
                if(day_up>=cfg_.gate_pct && (int)s.vols.size()>=20 && (int)s.closes.size()>=cfg_.lb){
                    double avgv=0; for(int k=0;k<20;k++) avgv+=s.vols[s.vols.size()-1-k]; avgv/=20.0;
                    double base=s.closes[s.closes.size()-cfg_.lb];
                    bool vol_ok = (cfg_.volx<=0.0) || (avgv>0 && v>=cfg_.volx*avgv);
                    // S-2026-06-20 IMPULSE FILTER: the entry bar must thrust >= mult*ATR (filters
                    // weak/stalling breakouts; PF 2.4->5.8, DD 10.4->6.6% on real big-cap data).
                    bool impulse_ok = (cfg_.min_impulse_atr<=0.0) || (s.atr<=0.0)
                                      || ((h - s.closes.back()) >= cfg_.min_impulse_atr * s.atr);
                    if(vol_ok && impulse_ok && base>0 && (c/base-1)*100>=cfg_.ig_pct) fire=true;
                }
            }
            if(fire && cfg_.min_breadth > 1){
                // S-2026-06-23 cross-sectional BREADTH gate (chop/bear protection, ported from the
                // faithfully-BT'd bridge): register this ignition, require >= min_breadth DISTINCT names
                // igniting the same session-day (8:00 UTC roll) before any entry. Causal: same-day prior only.
                auto& set = day_ignis_[sd]; set.insert(s.c.symbol);
                if((int)set.size() < cfg_.min_breadth){
                    printf("[BigCapMomo] %s breadth-gate hold (%zu/%d igniters today, day_up=%.0f%%)\n",
                        s.c.symbol.c_str(), set.size(), cfg_.min_breadth, (c/s.day_open-1)*100); fflush(stdout);
                    fire=false;
                }
            }
            // S-2026-06-23b SINGLE-ENTRY-PER-NAME-PER-DAY (re-entry-into-the-top guard).
            if(fire && s.last_entry_sd==sd) fire=false;
        }
        if(fire){
            // luke arm-and-wait: fill at the armed breakout trigger (or the gap-up open) with the tight
            // structural stop; ignition path keeps its close-fill + %-trail-derived stop.
            double entry_px = cfg_.luke_gate ? std::max(s.luke_trig, o) : c;
            double init_stop = cfg_.luke_gate ? s.luke_stop : c*(1-cfg_.trail_pct);
            double notional=risk_.allow_entry(s.c.symbol,entry_px,init_stop);
            if(cfg_.notional_usd>0 && notional>cfg_.notional_usd) notional=cfg_.notional_usd;
            if(notional>0){
                long sz=(long)std::max(1.0,notional/entry_px); risk_.on_open(s.c.symbol,notional);
                s.inpos=true; s.entry=entry_px; s.peak=h; s.trough=l; s.hold=0; s.notional=notional;
                s.size=sz; s.entry_ts=(int64_t)std::time(nullptr); s.last=c; s.last_entry_sd=sd;
                if(cfg_.luke_gate) s.luke_setup=0;   // consume the armed setup (1 fill per setup)
                printf("[BigCapMomo] %s LONG %s entry=%.2f %s sz=%ld notional=$%.0f [conc=%d]\n",
                    cfg_.paper_only?"PAPER":"LIVE",s.c.symbol.c_str(),entry_px,
                    cfg_.luke_gate?"LUKE-BREAK":"IGNITION",sz,notional,risk_.open_count());
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
long long last_activity_ms(){ return g_running.load() ? eng().last_activity_ms() : 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)> cb){ eng().set_sink(std::move(cb)); }

std::vector<PositionSnapshot> collect_positions(){
    if(!g_enabled.load() || !g_running.load()) return {};
    return eng().collect_positions();
}

// S-2026-06-21: paper/live port-vs-routing guard. Idea lifted from the
// retail IBKR-bot walkthrough (its one genuinely useful safety assert): catch
// the config-error class where the routing intent (paper_only) and the gateway
// port class disagree, BEFORE any order can fire. IBKR ports: 7496/4001 = LIVE,
// 7497/4002 = PAPER. Pure defence -- a correct config passes silently except
// for one explicit ARM banner when real routing is on.
static bool paper_port_guard(const Config& c){
    const bool live_port  = (c.port==7496 || c.port==4001);
    const bool paper_port = (c.port==7497 || c.port==4002);
    if(!c.paper_only){                                 // armed to route REAL orders
        if(paper_port){
            printf("[BigCapMomo] ABORT: paper_only=false (LIVE order routing armed) but "
                   "port %d is a PAPER gateway -- inconsistent config. Refusing to start. "
                   "Set port to a live gateway (4001/7496) or paper_only=true.\n",
                   c.port); fflush(stdout);
            return false;
        }
        printf("[BigCapMomo] *** LIVE ORDER ROUTING ARMED *** paper_only=false on %s:%d -- "
               "REAL orders WILL be sent. Intentional only if you flipped paper_only off "
               "on purpose.\n", c.host.c_str(), c.port); fflush(stdout);
    } else if(live_port){
        printf("[BigCapMomo] shadow OK: paper_only=true on live gateway %d -- logging only, "
               "NO orders routed.\n", c.port); fflush(stdout);
    }
    return true;
}

bool start(){
    if(!g_enabled.load()) return false;
    if(!paper_port_guard(g_cfg)){ g_running.store(false); return false; }  // P0 config-safety gate
    if(g_running.exchange(true)) return true;          // already running -> no-op
    eng().set_config(g_cfg);
    // Connect with HANDSHAKE CONFIRMATION + clientId rotation. connect() returns true
    // ONLY after nextValidId proves the API session is live (not just a TCP accept). A
    // stale clientId left by a prior process -- the exact 06-19 cause -- is dodged by
    // retrying on id+1/id+2. No long-lived pump thread is spawned until live.
    const int base_id = g_cfg.client_id;
    int win_id = base_id; bool live = false;
    for(int attempt=0; attempt<3 && !live; ++attempt){
        const int id = base_id + attempt;
        eng().set_client_id(id);
        if(eng().connect()){
            win_id = id; live = true;
            printf("[BigCapMomo] API handshake OK (nextValidId) -- engine live %s:%d id=%d\n",
                   g_cfg.host.c_str(), g_cfg.port, id); fflush(stdout);
            break;
        }
        printf("[BigCapMomo] connect/handshake FAILED %s:%d id=%d (attempt %d/3) -- %s\n",
               g_cfg.host.c_str(), g_cfg.port, id, attempt+1,
               attempt<2 ? "TCP up but no nextValidId; rotating clientId" : "giving up");
        fflush(stdout);
        eng().hard_disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if(!live){
        printf("[BigCapMomo] DOWN -- no API handshake after 3 clientId attempts (%d-%d). "
               "Gateway logged in? API enabled? clientId range free?\n", base_id, base_id+2);
        fflush(stdout);
        g_running.store(false);
        return false;   // -> omega_main [SYSTEM-ALERT] BIGCAP_IBKR_DOWN + GUI health banner
    }
    g_stop.store(false);
    g_thread = std::thread([win_id]{
        printf("[BigCapMomo] in-process IBKR engine thread up (%s:%d id=%d)\n",
               g_cfg.host.c_str(), g_cfg.port, win_id); fflush(stdout);
        auto last_hb = std::chrono::steady_clock::now();
        while(!g_stop.load()){
            eng().pump();                              // long-running intraday stream (<=2s/iter)
            auto now = std::chrono::steady_clock::now();
            if(now - last_hb >= std::chrono::seconds(60)){ eng().log_heartbeat(); last_hb = now; }
        }
        eng().disconnect();
        printf("[BigCapMomo] engine thread stopped\n"); fflush(stdout);
    });
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
long long last_activity_ms() { return 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)>) {}
std::vector<PositionSnapshot> collect_positions() { return {}; }
bool start() { return false; }
void stop() {}
} // namespace bigcap_momo_ibkr
} // namespace omega

#endif
