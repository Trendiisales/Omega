// gold_ls_harness.hpp — reusable, EDGE-NEUTRAL backtest plumbing for gold (XAU/MGC).
// -----------------------------------------------------------------------------
// SALVAGE PROVENANCE (S-2026-07-20ag): the streaming tick->1m aggregation, the
// Howard-Hinnant civil-date / London+NY DST clocks, the CSV column inference,
// the N-minute BarAggregator, and the EMA/ATR indicators are lifted verbatim
// (edge-neutral plumbing only) from backtest/aurum/AurumBreakPullback.cpp, which
// was audited DEAD (AURUM_BREAKPULLBACK_AUDIT_2026-07-20.md). What was DROPPED:
//   * the entire break-pullback ENTRY THESIS (no edge; net-negative every leg),
//   * the fatal RegimeReady state-clobber,
//   * the DISHONEST fill-at-level + fixed-slip booking (close_position booked
//     exits at the stop LEVEL - a fixed tick, hiding gap-through tails).
// This harness REPLACES booking with HONEST fills (see book_* helpers below):
//   entry (stop-order breakout) = worse-of(open, trigger) + half cost;
//   entry (market-on-open)      = open + half cost;
//   stop  = worse-of(open, stop_level) - half cost  (gap-through booked at the
//           REAL price the bar traded through, min(open,stop) for a long);
//   limit target = booked at the target level (favourable gap NOT credited).
// No look-ahead: signals come from COMPLETED bars; execution on the NEXT bar;
// within a bar the stop is checked BEFORE the favourable excursion updates.
// -----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace goldls {

constexpr double kBp = 10000.0;
constexpr double kEps = 1e-12;
constexpr int64_t kSecPerDay = 86400;

[[nodiscard]] inline bool finite_positive(double x) { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] inline std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
[[nodiscard]] inline std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
[[nodiscard]] inline std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') { field.push_back('"'); ++i; }
            else in_quotes = !in_quotes;
        } else if (c == ',' && !in_quotes) { out.push_back(trim_copy(field)); field.clear(); }
        else field.push_back(c);
    }
    out.push_back(trim_copy(field));
    return out;
}
[[nodiscard]] inline std::optional<double> parse_double(std::string_view sv) {
    if (sv.empty()) return std::nullopt;
    std::string s(sv);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str()) return std::nullopt;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

// ---- Howard Hinnant civil-date (public domain) ----
[[nodiscard]] constexpr int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}
struct CivilDateTime { int year=1970; unsigned month=1, day=1, hour=0, minute=0, second=0; };
[[nodiscard]] constexpr CivilDateTime civil_from_days(int64_t z) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    return CivilDateTime{y, m, d, 0, 0, 0};
}
[[nodiscard]] inline int weekday_from_days(int64_t z) {
    int w = static_cast<int>((z + 4) % 7);
    if (w < 0) w += 7;
    return w;
}
[[nodiscard]] inline unsigned days_in_month(int y, unsigned m) {
    static constexpr std::array<unsigned, 12> md = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m != 2) return md[m - 1];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    return leap ? 29U : 28U;
}
[[nodiscard]] inline int64_t make_utc_epoch(int y, unsigned m, unsigned d, unsigned hh, unsigned mm, unsigned ss) {
    return days_from_civil(y, m, d) * kSecPerDay + static_cast<int64_t>(hh)*3600 + static_cast<int64_t>(mm)*60 + ss;
}
[[nodiscard]] inline CivilDateTime utc_to_civil(int64_t epoch_sec) {
    int64_t days = epoch_sec / kSecPerDay;
    int64_t sod = epoch_sec % kSecPerDay;
    if (sod < 0) { sod += kSecPerDay; --days; }
    CivilDateTime c = civil_from_days(days);
    c.hour = static_cast<unsigned>(sod / 3600);
    c.minute = static_cast<unsigned>((sod % 3600) / 60);
    c.second = static_cast<unsigned>(sod % 60);
    return c;
}
[[nodiscard]] inline unsigned last_weekday_of_month(int y, unsigned m, int weekday) {
    const unsigned last_day = days_in_month(y, m);
    const int last_w = weekday_from_days(days_from_civil(y, m, last_day));
    const unsigned delta = static_cast<unsigned>((last_w - weekday + 7) % 7);
    return last_day - delta;
}
[[nodiscard]] inline unsigned nth_weekday_of_month(int y, unsigned m, int weekday, unsigned nth) {
    const int first_w = weekday_from_days(days_from_civil(y, m, 1));
    const unsigned delta = static_cast<unsigned>((weekday - first_w + 7) % 7);
    return 1U + delta + 7U * (nth - 1U);
}
[[nodiscard]] inline bool london_dst(int64_t e) {
    const CivilDateTime c = utc_to_civil(e);
    const int64_t s = make_utc_epoch(c.year, 3, last_weekday_of_month(c.year,3,0), 1,0,0);
    const int64_t f = make_utc_epoch(c.year,10, last_weekday_of_month(c.year,10,0),1,0,0);
    return e >= s && e < f;
}
[[nodiscard]] inline bool new_york_dst(int64_t e) {
    const CivilDateTime c = utc_to_civil(e);
    const int64_t s = make_utc_epoch(c.year, 3, nth_weekday_of_month(c.year,3,0,2), 7,0,0);
    const int64_t f = make_utc_epoch(c.year,11, nth_weekday_of_month(c.year,11,0,1),6,0,0);
    return e >= s && e < f;
}
struct LocalClock { int minute_of_day=0; int weekday=4; };
[[nodiscard]] inline LocalClock london_clock(int64_t e) {
    const CivilDateTime c = utc_to_civil(e + (london_dst(e) ? 3600 : 0));
    return LocalClock{ static_cast<int>(c.hour*60 + c.minute),
                       weekday_from_days(days_from_civil(c.year,c.month,c.day)) };
}
[[nodiscard]] inline LocalClock new_york_clock(int64_t e) {
    const CivilDateTime c = utc_to_civil(e + (new_york_dst(e) ? -4*3600 : -5*3600));
    return LocalClock{ static_cast<int>(c.hour*60 + c.minute),
                       weekday_from_days(days_from_civil(c.year,c.month,c.day)) };
}

// ---- bars ----
struct MinuteBar {
    int64_t t = 0; double open=0, high=0, low=0, close=0; uint64_t samples=0;
    [[nodiscard]] bool valid() const {
        return t>0 && finite_positive(open) && finite_positive(high) && finite_positive(low)
            && finite_positive(close) && high+kEps>=std::max(open,close) && low<=std::min(open,close)+kEps;
    }
};
struct Bar {
    int64_t start=0, end=0; double open=0, high=0, low=0, close=0;
    [[nodiscard]] bool valid() const {
        return start>0 && end>start && finite_positive(open) && finite_positive(high)
            && finite_positive(low) && finite_positive(close);
    }
};

struct CsvColumns { int time=-1, bid=-1, ask=-1, price=-1; };
[[nodiscard]] inline int find_header_index(const std::vector<std::string>& h, const std::vector<std::string>& names) {
    for (size_t i=0;i<h.size();++i){ const std::string x=lower_copy(trim_copy(h[i])); for (auto& n:names) if (x==n) return (int)i; }
    return -1;
}
[[nodiscard]] inline CsvColumns infer_columns(const std::vector<std::string>& h) {
    CsvColumns c;
    c.time = find_header_index(h, {"timestamp","time","datetime","date_time","date","epoch","ts"});
    c.bid  = find_header_index(h, {"bid","bid_price","bidprice"});
    c.ask  = find_header_index(h, {"ask","ask_price","askprice"});
    c.price= find_header_index(h, {"last","price","mid","close","last_price","trade_price"});
    return c;
}

// Streaming tick/quote CSV -> 1-minute bars. Uses midpoint when bid+ask present.
class MinuteCsvReader {
public:
    explicit MinuteCsvReader(std::string path) : path_(std::move(path)), in_(path_) {
        if (!in_) throw std::runtime_error("Cannot open CSV: " + path_);
        std::string header;
        if (!std::getline(in_, header)) throw std::runtime_error("Empty CSV: " + path_);
        cols_ = infer_columns(split_csv_line(header));
        if (cols_.time < 0) throw std::runtime_error("no timestamp column: " + path_);
        if (!((cols_.bid>=0 && cols_.ask>=0) || cols_.price>=0))
            throw std::runtime_error("needs bid+ask or price: " + path_);
    }
    bool next(MinuteBar& out) {
        out = {};
        while (true) {
            ParsedRow row;
            if (pending_) { row = *pending_; pending_.reset(); }
            else if (!read_row(row)) {
                if (cur_.samples>0){ out=finalise(); cur_={}; return true; }
                return false;
            }
            const int64_t minute = (row.epoch/60)*60;
            if (cur_.samples==0){ start(minute,row); continue; }
            if (minute==cur_.t){ upd(row); continue; }
            if (minute<cur_.t){ continue; }        // out-of-order tick: drop
            pending_=row; out=finalise(); cur_={}; return true;
        }
    }
private:
    struct ParsedRow { int64_t epoch=0; double price=0; };
    struct Cur { int64_t t=0; double open=0,high=0,low=0,close=0; uint64_t samples=0; } cur_;
    static bool parse_ts(const std::string& raw, int64_t& es) {
        auto dv = parse_double(raw); if (!dv) return false;
        double v=*dv; const double a=std::fabs(v);
        if (a>1e17) v/=1e9; else if (a>1e14) v/=1e6; else if (a>1e11) v/=1e3;
        es=(int64_t)std::floor(v); return es>0;
    }
    bool read_row(ParsedRow& row) {
        std::string line;
        while (std::getline(in_, line)) {
            if (line.empty()) continue;
            const auto f = split_csv_line(line);
            auto get=[&](int i)->std::string{ return (i<0||(size_t)i>=f.size())?std::string():f[(size_t)i]; };
            int64_t ep=0; if (!parse_ts(get(cols_.time),ep)) continue;
            double price=0;
            if (cols_.bid>=0 && cols_.ask>=0){
                auto b=parse_double(get(cols_.bid)); auto a=parse_double(get(cols_.ask));
                if (!b||!a||!finite_positive(*b)||!finite_positive(*a)||*a<*b) continue;
                price=(*b+*a)*0.5;
            } else { auto p=parse_double(get(cols_.price)); if (!p||!finite_positive(*p)) continue; price=*p; }
            row=ParsedRow{ep,price}; return true;
        }
        return false;
    }
    void start(int64_t m, const ParsedRow& r){ cur_.t=m; cur_.open=cur_.high=cur_.low=cur_.close=r.price; cur_.samples=1; }
    void upd(const ParsedRow& r){ cur_.high=std::max(cur_.high,r.price); cur_.low=std::min(cur_.low,r.price); cur_.close=r.price; ++cur_.samples; }
    MinuteBar finalise() const { MinuteBar b; b.t=cur_.t; b.open=cur_.open; b.high=cur_.high; b.low=cur_.low; b.close=cur_.close; b.samples=cur_.samples; return b; }
    std::string path_; std::ifstream in_; CsvColumns cols_;
    std::optional<ParsedRow> pending_;
};

// Reads an OHLC BAR csv (ts,o,h,l,c[,spr...]) directly as MinuteBars — one row
// = one MinuteBar with its real O,H,L,C preserved (NOT tick-collapsed). Use for
// m1-bar / H1 / D1 files where MinuteCsvReader (which aggregates tick->1m and so
// flattens each bar to o=h=l=c=close) would destroy the high/low. Header is
// optional: if the first line's first field is non-numeric it's skipped. ts is
// normalised to seconds via the same magnitude heuristic as the tick reader.
class MinuteBarCsvReader {
public:
    explicit MinuteBarCsvReader(std::string path) : path_(std::move(path)), in_(path_) {
        if (!in_) throw std::runtime_error("Cannot open bar CSV: " + path_);
        std::string first;
        std::streampos p0 = in_.tellg();
        if (!std::getline(in_, first)) throw std::runtime_error("Empty bar CSV: " + path_);
        const auto f = split_csv_line(first);
        // header if the timestamp field doesn't parse as a number
        if (!f.empty() && parse_double(f[0]).has_value()) in_.seekg(p0); // headerless: rewind
        // else: keep past the header line
    }
    bool next(MinuteBar& out) {
        out = {};
        std::string line;
        while (std::getline(in_, line)) {
            if (line.empty()) continue;
            const auto f = split_csv_line(line);
            if (f.size() < 5) continue;
            int64_t es=0; if (!parse_ts(f[0], es)) continue;
            auto o=parse_double(f[1]), h=parse_double(f[2]), l=parse_double(f[3]), c=parse_double(f[4]);
            if (!o||!h||!l||!c) continue;
            MinuteBar b; b.t=es; b.open=*o; b.high=*h; b.low=*l; b.close=*c; b.samples=1;
            if (!b.valid()) continue;
            out=b; return true;
        }
        return false;
    }
private:
    static bool parse_ts(const std::string& raw, int64_t& es) {
        auto dv = parse_double(raw); if (!dv) return false;
        double v=*dv; const double a=std::fabs(v);
        if (a>1e17) v/=1e9; else if (a>1e14) v/=1e6; else if (a>1e11) v/=1e3;
        es=(int64_t)std::floor(v); return es>0;
    }
    std::string path_; std::ifstream in_;
};

// N-minute bar aggregator (fed 1m bars).
class BarAggregator {
public:
    explicit BarAggregator(int minutes) : secs_(minutes*60) {
        if (minutes<=0) throw std::invalid_argument("bad interval");
    }
    std::optional<Bar> update(const MinuteBar& m) {
        if (!m.valid()) return std::nullopt;
        const int64_t bucket=(m.t/secs_)*secs_;
        if (!active_){ start(bucket,m); return std::nullopt; }
        if (bucket==cur_.start){ add(m); return std::nullopt; }
        if (bucket<cur_.start) return std::nullopt;
        Bar done=cur_; start(bucket,m); return done;
    }
    std::optional<Bar> flush(){ if(!active_) return std::nullopt; active_=false; return cur_; }
private:
    void start(int64_t b, const MinuteBar& m){ active_=true; cur_.start=b; cur_.end=b+secs_; cur_.open=m.open; cur_.high=m.high; cur_.low=m.low; cur_.close=m.close; }
    void add(const MinuteBar& m){ cur_.high=std::max(cur_.high,m.high); cur_.low=std::min(cur_.low,m.low); cur_.close=m.close; }
    int64_t secs_=0; bool active_=false; Bar cur_;
};

class EMA {
public:
    explicit EMA(int p): alpha_(2.0/(p+1.0)), period_(p) { if(p<=0) throw std::invalid_argument("ema"); }
    double update(double x){ if(!std::isfinite(x)) return v_; if(!ready_){v_=x;ready_=true;} else v_+=alpha_*(x-v_); ++n_; return v_; }
    [[nodiscard]] bool ready() const { return ready_ && n_>=(uint64_t)period_; }
    [[nodiscard]] double value() const { return v_; }
private: double alpha_=0,v_=0; int period_=0; bool ready_=false; uint64_t n_=0;
};

class ATR {
public:
    explicit ATR(int p): period_(p) { if(p<=0) throw std::invalid_argument("atr"); }
    double update(const Bar& b){
        if(!b.valid()) return atr_;
        double tr=b.high-b.low;
        if(has_){ tr=std::max({tr,std::fabs(b.high-pc_),std::fabs(b.low-pc_)}); }
        pc_=b.close; has_=true;
        if(n_<(uint64_t)period_){ sum_+=tr; ++n_; atr_=sum_/n_; }
        else { atr_=((atr_*(period_-1))+tr)/period_; ++n_; }
        return atr_;
    }
    [[nodiscard]] bool ready() const { return n_>=(uint64_t)period_; }
    [[nodiscard]] double value() const { return atr_; }
private: int period_=0; double atr_=0,sum_=0,pc_=0; bool has_=false; uint64_t n_=0;
};

// ============================ HONEST FILL BOOK =============================
// All fills embed cost. cost_bp = full round-trip; half applied each side.
struct HonestBook {
    double cost_bp = 5.0;   // round-trip, in bp of price
    [[nodiscard]] double half_px(double px) const { return (cost_bp * 0.5 / kBp) * px; }

    // Market-on-open entry.
    [[nodiscard]] double entry_open(bool is_long, double open) const {
        return is_long ? open + half_px(open) : open - half_px(open);
    }
    // Stop-order breakout entry at trigger level: worse-of(open, trigger) + half cost.
    [[nodiscard]] double entry_stop(bool is_long, double open, double trigger) const {
        const double raw = is_long ? std::max(open, trigger) : std::min(open, trigger);
        return is_long ? raw + half_px(raw) : raw - half_px(raw);
    }
    // Protective stop exit: worse-of(open, stop_level). Gap-through booked at the
    // REAL price min(open,stop) for a long / max(open,stop) for a short. NEVER level+fixed-slip.
    [[nodiscard]] double exit_stop(bool is_long, double open, double stop_level) const {
        const double raw = is_long ? std::min(open, stop_level) : std::max(open, stop_level);
        return is_long ? raw - half_px(raw) : raw + half_px(raw);
    }
    // Limit target exit: booked at the target level (favourable gap NOT credited).
    [[nodiscard]] double exit_limit(bool is_long, double target) const {
        return is_long ? target - half_px(target) : target + half_px(target);
    }
    // Signal/market exit at a bar price (opposite-signal / time stop): at close.
    [[nodiscard]] double exit_market(bool is_long, double px) const {
        return is_long ? px - half_px(px) : px + half_px(px);
    }
};

} // namespace goldls
