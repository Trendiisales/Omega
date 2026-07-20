// AurumBreakPullback.cpp
// AURUM-BREAK-PULLBACK-V1
//
// Self-contained C++20 backtest / paper-trading engine for XAUUSD and CME MGC.
// The engine:
//   * builds 1m bars from large CSV tick/quote files without loading all ticks,
//   * aggregates 5m / 15m / 30m bars,
//   * trades London and New York opening-range breakout pullbacks,
//   * confirms direction on both XAU and MGC,
//   * routes each trade to the cheaper viable venue,
//   * books all entries/exits from simulated actual fills,
//   * supports long and short trades,
//   * applies structural stops, cost floors, partial profit, trailing exits,
//   * enforces session, daily, weekly and account drawdown locks,
//   * writes an honest per-trade CSV ledger.
//
// IMPORTANT:
//   This executable is a research/backtest and paper-execution core. It does not
//   contain broker credentials or a live broker/FIX adapter. Live order routing
//   must call the same Engine::on_minute() state machine and replace SimExecution
//   with the broker-specific adapter while preserving actual-fill accounting.
//
// Build on macOS:
//   clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
//     "$HOME/Downloads/AurumBreakPullback.cpp" \
//     -o "$HOME/Downloads/aurum_break_pullback"
//
// Example run:
//   "$HOME/Downloads/aurum_break_pullback" \
//     --xau "/Users/jo/Tick/XAUUSD.csv" \
//     --mgc "/Users/jo/Tick/MGC.csv" \
//     --equity 100000 \
//     --ledger "$HOME/Downloads/aurum_trades.csv"
//
// Input CSV requirements:
//   Sorted ascending by time, with a header. Timestamp may be ISO-8601 or epoch
//   seconds/milliseconds/microseconds/nanoseconds. Supported price columns:
//     timestamp/time/datetime/date_time
//     bid, ask, last, price, close
//     volume/vol/size/qty (optional)
//   If bid and ask exist, XAU uses midpoint. Otherwise price/last/close is used.
//
// Copyright: generated for Jo, 2026.

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurum {

constexpr double kBp = 10000.0;
constexpr double kEps = 1e-12;
constexpr int64_t kSecPerDay = 86400;

[[nodiscard]] bool finite_positive(double x) {
    return std::isfinite(x) && x > 0.0;
}

[[nodiscard]] double clamp_value(double v, double lo, double hi) {
    return std::max(lo, std::min(v, hi));
}

[[nodiscard]] std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

[[nodiscard]] std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

[[nodiscard]] std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            out.push_back(trim_copy(field));
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    out.push_back(trim_copy(field));
    return out;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view sv) {
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

// Howard Hinnant civil-date algorithms, public domain.
[[nodiscard]] constexpr int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

struct CivilDateTime {
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
};

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

[[nodiscard]] int weekday_from_days(int64_t z) {
    // 1970-01-01 was Thursday. Return 0=Sunday ... 6=Saturday.
    int w = static_cast<int>((z + 4) % 7);
    if (w < 0) w += 7;
    return w;
}

[[nodiscard]] unsigned days_in_month(int y, unsigned m) {
    static constexpr std::array<unsigned, 12> mdays = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (m != 2) return mdays[m - 1];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    return leap ? 29U : 28U;
}

[[nodiscard]] unsigned nth_weekday_of_month(int y, unsigned m, int weekday, unsigned nth) {
    const int first_w = weekday_from_days(days_from_civil(y, m, 1));
    const unsigned delta = static_cast<unsigned>((weekday - first_w + 7) % 7);
    return 1U + delta + 7U * (nth - 1U);
}

[[nodiscard]] unsigned last_weekday_of_month(int y, unsigned m, int weekday) {
    const unsigned last_day = days_in_month(y, m);
    const int last_w = weekday_from_days(days_from_civil(y, m, last_day));
    const unsigned delta = static_cast<unsigned>((last_w - weekday + 7) % 7);
    return last_day - delta;
}

[[nodiscard]] int64_t make_utc_epoch(int y, unsigned m, unsigned d,
                                     unsigned hh, unsigned mm, unsigned ss) {
    return days_from_civil(y, m, d) * kSecPerDay
         + static_cast<int64_t>(hh) * 3600
         + static_cast<int64_t>(mm) * 60
         + static_cast<int64_t>(ss);
}

[[nodiscard]] CivilDateTime utc_to_civil(int64_t epoch_sec) {
    int64_t days = epoch_sec / kSecPerDay;
    int64_t sod = epoch_sec % kSecPerDay;
    if (sod < 0) {
        sod += kSecPerDay;
        --days;
    }
    CivilDateTime c = civil_from_days(days);
    c.hour = static_cast<unsigned>(sod / 3600);
    c.minute = static_cast<unsigned>((sod % 3600) / 60);
    c.second = static_cast<unsigned>(sod % 60);
    return c;
}

[[nodiscard]] bool parse_iso8601(const std::string& input, int64_t& out_epoch) {
    std::string s = trim_copy(input);
    if (s.size() < 10) return false;

    auto digit = [&](size_t p) -> int {
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return -1;
        return s[p] - '0';
    };
    auto two = [&](size_t p) -> int {
        const int a = digit(p), b = digit(p + 1);
        return (a < 0 || b < 0) ? -1 : a * 10 + b;
    };
    auto four = [&](size_t p) -> int {
        const int a = digit(p), b = digit(p + 1), c = digit(p + 2), d = digit(p + 3);
        return (a < 0 || b < 0 || c < 0 || d < 0) ? -1 : a * 1000 + b * 100 + c * 10 + d;
    };

    const int y = four(0);
    const int mo = two(5);
    const int da = two(8);
    if (y < 1970 || mo < 1 || mo > 12 || da < 1 || da > 31) return false;
    if (s[4] != '-' || s[7] != '-') return false;

    int hh = 0, mm = 0, ss = 0;
    size_t pos = 10;
    if (pos < s.size() && (s[pos] == 'T' || s[pos] == ' ')) {
        if (pos + 8 >= s.size()) return false;
        hh = two(pos + 1);
        mm = two(pos + 4);
        ss = two(pos + 7);
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;
        pos += 9;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
        }
    }

    int offset_sec = 0;
    if (pos < s.size()) {
        if (s[pos] == 'Z' || s[pos] == 'z') {
            ++pos;
        } else if (s[pos] == '+' || s[pos] == '-') {
            const int sign = s[pos] == '+' ? 1 : -1;
            if (pos + 2 >= s.size()) return false;
            const int oh = two(pos + 1);
            int om = 0;
            if (pos + 3 < s.size() && s[pos + 3] == ':') {
                if (pos + 5 >= s.size()) return false;
                om = two(pos + 4);
                pos += 6;
            } else {
                if (pos + 4 >= s.size()) return false;
                om = two(pos + 3);
                pos += 5;
            }
            if (oh < 0 || oh > 23 || om < 0 || om > 59) return false;
            offset_sec = sign * (oh * 3600 + om * 60);
        }
    }

    out_epoch = make_utc_epoch(y, static_cast<unsigned>(mo), static_cast<unsigned>(da),
                               static_cast<unsigned>(hh), static_cast<unsigned>(mm),
                               static_cast<unsigned>(std::min(ss, 59))) - offset_sec;
    return true;
}

[[nodiscard]] bool parse_timestamp(const std::string& raw, int64_t& epoch_sec) {
    const std::string s = trim_copy(raw);
    if (s.empty()) return false;

    bool numeric = true;
    bool decimal_seen = false;
    for (char c : s) {
        if (c == '.' && !decimal_seen) {
            decimal_seen = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+') {
            numeric = false;
            break;
        }
    }

    if (numeric) {
        auto dv = parse_double(s);
        if (!dv) return false;
        double v = *dv;
        const double av = std::fabs(v);
        if (av > 1e17) v /= 1e9;       // nanoseconds
        else if (av > 1e14) v /= 1e6;  // microseconds
        else if (av > 1e11) v /= 1e3;  // milliseconds
        epoch_sec = static_cast<int64_t>(std::floor(v));
        return epoch_sec > 0;
    }

    return parse_iso8601(s, epoch_sec);
}

[[nodiscard]] std::string iso_utc(int64_t epoch_sec) {
    const CivilDateTime c = utc_to_civil(epoch_sec);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << c.year << '-'
        << std::setw(2) << c.month << '-'
        << std::setw(2) << c.day << 'T'
        << std::setw(2) << c.hour << ':'
        << std::setw(2) << c.minute << ':'
        << std::setw(2) << c.second << 'Z';
    return oss.str();
}

[[nodiscard]] bool london_dst(int64_t utc_epoch) {
    const CivilDateTime c = utc_to_civil(utc_epoch);
    const unsigned start_day = last_weekday_of_month(c.year, 3, 0);   // Sunday
    const unsigned end_day = last_weekday_of_month(c.year, 10, 0);
    const int64_t start = make_utc_epoch(c.year, 3, start_day, 1, 0, 0);
    const int64_t end = make_utc_epoch(c.year, 10, end_day, 1, 0, 0);
    return utc_epoch >= start && utc_epoch < end;
}

[[nodiscard]] bool new_york_dst(int64_t utc_epoch) {
    const CivilDateTime c = utc_to_civil(utc_epoch);
    const unsigned start_day = nth_weekday_of_month(c.year, 3, 0, 2); // second Sunday
    const unsigned end_day = nth_weekday_of_month(c.year, 11, 0, 1);  // first Sunday
    const int64_t start = make_utc_epoch(c.year, 3, start_day, 7, 0, 0); // 02:00 EST
    const int64_t end = make_utc_epoch(c.year, 11, end_day, 6, 0, 0);    // 02:00 EDT
    return utc_epoch >= start && utc_epoch < end;
}

struct LocalClock {
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    int weekday = 4;
    int minute_of_day = 0;
    int64_t local_day_index = 0;
};

[[nodiscard]] LocalClock london_clock(int64_t utc_epoch) {
    const int offset = london_dst(utc_epoch) ? 3600 : 0;
    const int64_t local_epoch = utc_epoch + offset;
    const CivilDateTime c = utc_to_civil(local_epoch);
    const int64_t day_index = days_from_civil(c.year, c.month, c.day);
    return LocalClock{c.year, c.month, c.day, weekday_from_days(day_index),
                      static_cast<int>(c.hour * 60 + c.minute), day_index};
}

[[nodiscard]] LocalClock new_york_clock(int64_t utc_epoch) {
    const int offset = new_york_dst(utc_epoch) ? -4 * 3600 : -5 * 3600;
    const int64_t local_epoch = utc_epoch + offset;
    const CivilDateTime c = utc_to_civil(local_epoch);
    const int64_t day_index = days_from_civil(c.year, c.month, c.day);
    return LocalClock{c.year, c.month, c.day, weekday_from_days(day_index),
                      static_cast<int>(c.hour * 60 + c.minute), day_index};
}

[[nodiscard]] int64_t monday_of_week(int64_t local_day_index) {
    const int weekday = weekday_from_days(local_day_index); // Sun=0
    const int days_since_monday = (weekday + 6) % 7;
    return local_day_index - days_since_monday;
}

struct MinuteBar {
    int64_t t = 0; // minute start UTC epoch
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    double avg_spread = 0.0;
    uint64_t samples = 0;

    [[nodiscard]] bool valid() const {
        return t > 0 && finite_positive(open) && finite_positive(high)
            && finite_positive(low) && finite_positive(close)
            && high + kEps >= std::max(open, close)
            && low <= std::min(open, close) + kEps;
    }
};

struct Bar {
    int64_t start = 0;
    int64_t end = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;

    [[nodiscard]] bool valid() const {
        return start > 0 && end > start && finite_positive(open)
            && finite_positive(high) && finite_positive(low) && finite_positive(close);
    }
};

struct CsvColumns {
    int time = -1;
    int bid = -1;
    int ask = -1;
    int price = -1;
    int volume = -1;
};

[[nodiscard]] int find_header_index(const std::vector<std::string>& headers,
                                    const std::vector<std::string>& names) {
    for (size_t i = 0; i < headers.size(); ++i) {
        const std::string h = lower_copy(trim_copy(headers[i]));
        for (const auto& n : names) {
            if (h == n) return static_cast<int>(i);
        }
    }
    return -1;
}

[[nodiscard]] CsvColumns infer_columns(const std::vector<std::string>& headers) {
    CsvColumns c;
    c.time = find_header_index(headers, {
        "timestamp", "time", "datetime", "date_time", "date", "epoch", "ts"
    });
    c.bid = find_header_index(headers, {"bid", "bid_price", "bidprice"});
    c.ask = find_header_index(headers, {"ask", "ask_price", "askprice"});
    c.price = find_header_index(headers, {
        "last", "price", "mid", "close", "last_price", "trade_price"
    });
    c.volume = find_header_index(headers, {
        "volume", "vol", "size", "qty", "quantity", "trade_size"
    });
    return c;
}

class MinuteCsvReader {
public:
    MinuteCsvReader(std::string path, std::string symbol)
        : path_(std::move(path)), symbol_(std::move(symbol)), in_(path_) {
        if (!in_) {
            throw std::runtime_error("Cannot open " + symbol_ + " CSV: " + path_);
        }
        std::string header;
        if (!std::getline(in_, header)) {
            throw std::runtime_error("Empty CSV: " + path_);
        }
        headers_ = split_csv_line(header);
        cols_ = infer_columns(headers_);
        if (cols_.time < 0) {
            throw std::runtime_error(symbol_ + " CSV has no recognised timestamp column");
        }
        if (!((cols_.bid >= 0 && cols_.ask >= 0) || cols_.price >= 0)) {
            throw std::runtime_error(symbol_ + " CSV needs bid+ask or price/last/close");
        }
    }

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] uint64_t rows_read() const { return rows_read_; }
    [[nodiscard]] uint64_t rows_bad() const { return rows_bad_; }

    bool next(MinuteBar& out) {
        out = {};
        while (true) {
            ParsedRow row;
            if (pending_) {
                row = *pending_;
                pending_.reset();
            } else {
                if (!read_row(row)) {
                    if (current_.samples > 0) {
                        out = finalise_current();
                        current_ = {};
                        return true;
                    }
                    return false;
                }
            }

            const int64_t minute = (row.epoch / 60) * 60;
            if (current_.samples == 0) {
                start_current(minute, row);
                continue;
            }
            if (minute == current_.t) {
                update_current(row);
                continue;
            }
            if (minute < current_.t) {
                ++rows_bad_;
                continue;
            }

            pending_ = row;
            out = finalise_current();
            current_ = {};
            return true;
        }
    }

private:
    struct ParsedRow {
        int64_t epoch = 0;
        double price = 0.0;
        double volume = 0.0;
        double spread = 0.0;
    };

    struct Current {
        int64_t t = 0;
        double open = 0.0;
        double high = 0.0;
        double low = 0.0;
        double close = 0.0;
        double volume = 0.0;
        double spread_sum = 0.0;
        uint64_t samples = 0;
    } current_;

    bool read_row(ParsedRow& row) {
        std::string line;
        while (std::getline(in_, line)) {
            ++rows_read_;
            if (line.empty()) continue;
            const auto fields = split_csv_line(line);
            auto get = [&](int idx) -> std::string {
                if (idx < 0 || static_cast<size_t>(idx) >= fields.size()) return {};
                return fields[static_cast<size_t>(idx)];
            };

            int64_t epoch = 0;
            if (!parse_timestamp(get(cols_.time), epoch)) {
                ++rows_bad_;
                continue;
            }

            double price = 0.0;
            double spread = 0.0;
            if (cols_.bid >= 0 && cols_.ask >= 0) {
                const auto bid = parse_double(get(cols_.bid));
                const auto ask = parse_double(get(cols_.ask));
                if (!bid || !ask || !finite_positive(*bid) || !finite_positive(*ask) || *ask < *bid) {
                    ++rows_bad_;
                    continue;
                }
                price = (*bid + *ask) * 0.5;
                spread = *ask - *bid;
            } else {
                const auto p = parse_double(get(cols_.price));
                if (!p || !finite_positive(*p)) {
                    ++rows_bad_;
                    continue;
                }
                price = *p;
            }

            double volume = 0.0;
            if (cols_.volume >= 0) {
                if (const auto v = parse_double(get(cols_.volume)); v && *v > 0.0) {
                    volume = *v;
                }
            }
            if (volume <= 0.0) volume = 1.0;

            row = ParsedRow{epoch, price, volume, spread};
            return true;
        }
        return false;
    }

    void start_current(int64_t minute, const ParsedRow& row) {
        current_.t = minute;
        current_.open = current_.high = current_.low = current_.close = row.price;
        current_.volume = row.volume;
        current_.spread_sum = row.spread;
        current_.samples = 1;
    }

    void update_current(const ParsedRow& row) {
        current_.high = std::max(current_.high, row.price);
        current_.low = std::min(current_.low, row.price);
        current_.close = row.price;
        current_.volume += row.volume;
        current_.spread_sum += row.spread;
        ++current_.samples;
    }

    [[nodiscard]] MinuteBar finalise_current() const {
        MinuteBar b;
        b.t = current_.t;
        b.open = current_.open;
        b.high = current_.high;
        b.low = current_.low;
        b.close = current_.close;
        b.volume = current_.volume;
        b.samples = current_.samples;
        b.avg_spread = current_.samples > 0
            ? current_.spread_sum / static_cast<double>(current_.samples)
            : 0.0;
        return b;
    }

    std::string path_;
    std::string symbol_;
    std::ifstream in_;
    std::vector<std::string> headers_;
    CsvColumns cols_;
    std::optional<ParsedRow> pending_;
    uint64_t rows_read_ = 0;
    uint64_t rows_bad_ = 0;
};

class BarAggregator {
public:
    explicit BarAggregator(int minutes) : seconds_(minutes * 60) {
        if (minutes <= 0) throw std::invalid_argument("Invalid aggregation interval");
    }

    std::optional<Bar> update(const MinuteBar& m) {
        if (!m.valid()) return std::nullopt;
        const int64_t bucket = (m.t / seconds_) * seconds_;
        if (!active_) {
            start(bucket, m);
            return std::nullopt;
        }
        if (bucket == current_.start) {
            add(m);
            return std::nullopt;
        }
        if (bucket < current_.start) return std::nullopt;
        Bar done = current_;
        start(bucket, m);
        return done;
    }

    std::optional<Bar> flush() {
        if (!active_) return std::nullopt;
        active_ = false;
        return current_;
    }

private:
    void start(int64_t bucket, const MinuteBar& m) {
        active_ = true;
        current_.start = bucket;
        current_.end = bucket + seconds_;
        current_.open = m.open;
        current_.high = m.high;
        current_.low = m.low;
        current_.close = m.close;
        current_.volume = m.volume;
    }

    void add(const MinuteBar& m) {
        current_.high = std::max(current_.high, m.high);
        current_.low = std::min(current_.low, m.low);
        current_.close = m.close;
        current_.volume += m.volume;
    }

    int64_t seconds_ = 0;
    bool active_ = false;
    Bar current_;
};

class EMA {
public:
    explicit EMA(int period)
        : alpha_(2.0 / (static_cast<double>(period) + 1.0)), period_(period) {
        if (period <= 0) throw std::invalid_argument("EMA period must be positive");
    }

    double update(double x) {
        if (!std::isfinite(x)) return value_;
        if (!ready_) {
            value_ = x;
            ready_ = true;
        } else {
            value_ += alpha_ * (x - value_);
        }
        ++count_;
        return value_;
    }

    [[nodiscard]] bool ready() const { return ready_ && count_ >= static_cast<uint64_t>(period_); }
    [[nodiscard]] double value() const { return value_; }

private:
    double alpha_ = 0.0;
    double value_ = 0.0;
    int period_ = 0;
    bool ready_ = false;
    uint64_t count_ = 0;
};

class ATR {
public:
    explicit ATR(int period) : period_(period) {
        if (period <= 0) throw std::invalid_argument("ATR period must be positive");
    }

    double update(const Bar& b) {
        if (!b.valid()) return atr_;
        double tr = b.high - b.low;
        if (has_prev_) {
            tr = std::max({tr, std::fabs(b.high - prev_close_), std::fabs(b.low - prev_close_)});
        }
        prev_close_ = b.close;
        has_prev_ = true;
        if (count_ < static_cast<uint64_t>(period_)) {
            sum_ += tr;
            ++count_;
            atr_ = sum_ / static_cast<double>(count_);
        } else {
            atr_ = ((atr_ * (period_ - 1)) + tr) / static_cast<double>(period_);
            ++count_;
        }
        return atr_;
    }

    [[nodiscard]] bool ready() const { return count_ >= static_cast<uint64_t>(period_); }
    [[nodiscard]] double value() const { return atr_; }

private:
    int period_ = 0;
    uint64_t count_ = 0;
    double sum_ = 0.0;
    double atr_ = 0.0;
    double prev_close_ = 0.0;
    bool has_prev_ = false;
};

class RollingValues {
public:
    explicit RollingValues(size_t max_size) : max_size_(max_size) {}

    void push(double x) {
        values_.push_back(x);
        if (values_.size() > max_size_) values_.erase(values_.begin());
    }

    [[nodiscard]] bool full() const { return values_.size() >= max_size_; }
    [[nodiscard]] size_t size() const { return values_.size(); }
    [[nodiscard]] double back() const { return values_.empty() ? 0.0 : values_.back(); }
    [[nodiscard]] double from_back(size_t offset) const {
        if (offset >= values_.size()) return 0.0;
        return values_[values_.size() - 1 - offset];
    }

private:
    size_t max_size_ = 0;
    std::vector<double> values_;
};

enum class Direction { None = 0, Long = 1, Short = -1 };
enum class Venue { None, XAU, MGC };
enum class SessionId { None, London, NewYork };
enum class EngineState {
    Flat,
    BuildingRange,
    RegimeReady,
    BreakoutConfirmed,
    WaitPullback,
    EntryPending,
    PositionInitial,
    PositionCostProtected,
    PositionPartial,
    PositionRunner,
    DailyLocked,
    WeeklyLocked,
    DrawdownLocked
};

enum class ExitReason {
    None,
    InitialStop,
    ProtectedStop,
    RunnerStop,
    FailedBreakout,
    SessionFlatten,
    RiskGovernor,
    EndOfData
};

[[nodiscard]] const char* to_string(Direction d) {
    switch (d) {
        case Direction::Long: return "LONG";
        case Direction::Short: return "SHORT";
        default: return "NONE";
    }
}

[[nodiscard]] const char* to_string(Venue v) {
    switch (v) {
        case Venue::XAU: return "XAU";
        case Venue::MGC: return "MGC";
        default: return "NONE";
    }
}

[[nodiscard]] const char* to_string(SessionId s) {
    switch (s) {
        case SessionId::London: return "LONDON";
        case SessionId::NewYork: return "NEW_YORK";
        default: return "NONE";
    }
}

[[nodiscard]] const char* to_string(ExitReason r) {
    switch (r) {
        case ExitReason::InitialStop: return "INITIAL_STOP";
        case ExitReason::ProtectedStop: return "PROTECTED_STOP";
        case ExitReason::RunnerStop: return "RUNNER_STOP";
        case ExitReason::FailedBreakout: return "FAILED_BREAKOUT";
        case ExitReason::SessionFlatten: return "SESSION_FLATTEN";
        case ExitReason::RiskGovernor: return "RISK_GOVERNOR";
        case ExitReason::EndOfData: return "END_OF_DATA";
        default: return "NONE";
    }
}

struct Config {
    std::string xau_path;
    std::string mgc_path;
    std::string ledger_path = "aurum_trades.csv";

    double starting_equity = 100000.0;
    double risk_fraction = 0.0020;
    double max_risk_fraction = 0.0025;
    double max_single_mgc_risk_fraction = 0.0030;

    double xau_base_rt_cost_bp = 6.0;
    double xau_entry_slippage_bp = 1.0;
    double xau_exit_slippage_bp = 2.0;
    double mgc_rt_commission_usd = 2.50;
    double mgc_entry_slippage_ticks = 1.0;
    double mgc_exit_slippage_ticks = 1.0;
    double mgc_tick_size = 0.10;
    double mgc_tick_value = 1.00;
    double mgc_ounces_per_contract = 10.0;

    int atr_period_15m = 20;
    int ema_fast_30m = 20;
    int ema_slow_30m = 50;
    int ema_slope_lookback_30m = 4;

    int opening_range_minutes = 30;
    int pullback_timeout_bars_5m = 6;
    double breakout_buffer_atr = 0.10;
    double max_breakout_bar_atr = 1.20;
    double pullback_distance_atr = 0.20;
    double invalid_inside_atr = 0.15;

    double min_atr_to_cost_multiple = 5.0;
    double min_opening_range_atr = 0.50;
    double max_opening_range_atr = 1.80;
    double min_stop_cost_multiple = 3.0;
    double min_stop_atr = 0.55;
    double max_stop_atr = 1.10;
    double structural_buffer_atr = 0.10;

    double protect_trigger_r = 1.00;
    double partial_trigger_r = 1.25;
    double partial_fraction = 0.40;
    double runner_trigger_r = 1.75;
    double runner_atr = 1.20;
    double protected_extra_bp = 2.0;
    double runner_floor_r = 0.25;

    int failed_exit_minutes = 60;
    double failed_exit_max_mfe_r = 0.60;

    double daily_loss_fraction = 0.0050;
    int daily_max_losses = 2;
    double weekly_loss_fraction = 0.0125;
    double account_drawdown_fraction = 0.0300;
    double daily_profit_arm_r = 1.00;
    double daily_profit_giveback_r = 0.50;

    int max_trades_per_day = 2;
    bool enable_longs = true;
    bool enable_shorts = true;
    bool verbose = false;
};

struct InstrumentIndicators {
    BarAggregator agg5{5};
    BarAggregator agg15{15};
    BarAggregator agg30{30};
    ATR atr15;
    EMA ema_fast30;
    EMA ema_slow30;
    RollingValues slow_history;

    double last_atr15 = 0.0;
    double last_ema_fast = 0.0;
    double last_ema_slow = 0.0;
    double last_ema_slow_old = 0.0;
    double last_30_close = 0.0;
    std::optional<Bar> last5;
    std::optional<Bar> last15;
    std::optional<Bar> last30;

    explicit InstrumentIndicators(const Config& cfg)
        : atr15(cfg.atr_period_15m),
          ema_fast30(cfg.ema_fast_30m),
          ema_slow30(cfg.ema_slow_30m),
          slow_history(static_cast<size_t>(cfg.ema_slope_lookback_30m + 2)) {}

    struct Completed {
        std::optional<Bar> b5;
        std::optional<Bar> b15;
        std::optional<Bar> b30;
    };

    Completed update(const MinuteBar& m, const Config& cfg) {
        Completed c;
        c.b5 = agg5.update(m);
        c.b15 = agg15.update(m);
        c.b30 = agg30.update(m);
        if (c.b5) last5 = c.b5;
        if (c.b15) {
            last15 = c.b15;
            last_atr15 = atr15.update(*c.b15);
        }
        if (c.b30) {
            last30 = c.b30;
            last_30_close = c.b30->close;
            last_ema_fast = ema_fast30.update(c.b30->close);
            last_ema_slow = ema_slow30.update(c.b30->close);
            slow_history.push(last_ema_slow);
            if (slow_history.size() > static_cast<size_t>(cfg.ema_slope_lookback_30m)) {
                last_ema_slow_old = slow_history.from_back(
                    static_cast<size_t>(cfg.ema_slope_lookback_30m));
            }
        }
        return c;
    }

    [[nodiscard]] bool ready(const Config& cfg) const {
        return atr15.ready() && ema_fast30.ready() && ema_slow30.ready()
            && slow_history.size() > static_cast<size_t>(cfg.ema_slope_lookback_30m)
            && last_30_close > 0.0 && last_atr15 > 0.0;
    }
};

struct SessionTracker {
    SessionId id = SessionId::None;
    int64_t local_day = std::numeric_limits<int64_t>::min();
    bool active = false;
    bool range_complete = false;
    bool trade_used = false;
    bool loss_lock = false;
    int start_minute = 0;
    int range_end_minute = 0;
    int entry_end_minute = 0;
    int flatten_minute = 0;

    double xau_high = -std::numeric_limits<double>::infinity();
    double xau_low = std::numeric_limits<double>::infinity();
    double mgc_high = -std::numeric_limits<double>::infinity();
    double mgc_low = std::numeric_limits<double>::infinity();

    double xau_vwap_num = 0.0;
    double xau_vwap_den = 0.0;
    double mgc_vwap_num = 0.0;
    double mgc_vwap_den = 0.0;

    void reset(SessionId sid, int64_t day, int start, int range_end,
               int entry_end, int flatten) {
        id = sid;
        local_day = day;
        active = true;
        range_complete = false;
        trade_used = false;
        loss_lock = false;
        start_minute = start;
        range_end_minute = range_end;
        entry_end_minute = entry_end;
        flatten_minute = flatten;
        xau_high = mgc_high = -std::numeric_limits<double>::infinity();
        xau_low = mgc_low = std::numeric_limits<double>::infinity();
        xau_vwap_num = xau_vwap_den = 0.0;
        mgc_vwap_num = mgc_vwap_den = 0.0;
    }

    void update_vwap(const MinuteBar& xau, const MinuteBar& mgc) {
        const double xau_typ = (xau.high + xau.low + xau.close) / 3.0;
        const double mgc_typ = (mgc.high + mgc.low + mgc.close) / 3.0;
        xau_vwap_num += xau_typ * std::max(1.0, xau.volume);
        xau_vwap_den += std::max(1.0, xau.volume);
        mgc_vwap_num += mgc_typ * std::max(1.0, mgc.volume);
        mgc_vwap_den += std::max(1.0, mgc.volume);
    }

    [[nodiscard]] double xau_vwap() const {
        return xau_vwap_den > 0.0 ? xau_vwap_num / xau_vwap_den : 0.0;
    }

    [[nodiscard]] double mgc_vwap() const {
        return mgc_vwap_den > 0.0 ? mgc_vwap_num / mgc_vwap_den : 0.0;
    }
};

struct Setup {
    SessionId session = SessionId::None;
    Direction direction = Direction::None;
    int64_t breakout_time = 0;
    int pullback_bars = 0;
    double xau_breakout = 0.0;
    double mgc_breakout = 0.0;
    double xau_atr = 0.0;
    double mgc_atr = 0.0;
    double xau_pullback_swing = 0.0;
    double mgc_pullback_swing = 0.0;
    double xau_entry_trigger = 0.0;
    double mgc_entry_trigger = 0.0;
    int64_t pending_since = 0;

    void clear() { *this = {}; }
};

struct Position {
    Venue venue = Venue::None;
    Direction direction = Direction::None;
    SessionId session = SessionId::None;
    int64_t entry_time = 0;
    double entry_fill = 0.0;
    double quantity = 0.0; // XAU ounces or MGC contracts
    double remaining = 0.0;
    double initial_stop = 0.0;
    double active_stop = 0.0;
    double initial_r_price = 0.0;
    double initial_risk_dollars = 0.0;
    double highest = 0.0;
    double lowest = 0.0;
    double mfe_r = 0.0;
    double realised_partial = 0.0;
    double partial_gross = 0.0;
    double partial_exit_cost = 0.0;
    double entry_cost = 0.0;
    double stressed_cost_bp = 0.0;
    bool cost_protected = false;
    bool partial_taken = false;
    bool runner_active = false;

    [[nodiscard]] bool open() const {
        return venue != Venue::None && direction != Direction::None && remaining > 0.0;
    }

    void clear() { *this = {}; }
};

struct TradeRecord {
    int trade_id = 0;
    SessionId session = SessionId::None;
    Venue venue = Venue::None;
    Direction direction = Direction::None;
    int64_t entry_time = 0;
    int64_t exit_time = 0;
    double entry_fill = 0.0;
    double exit_fill = 0.0;
    double quantity = 0.0;
    double initial_stop = 0.0;
    double mfe_r = 0.0;
    double gross_pnl = 0.0;
    double costs = 0.0;
    double net_pnl = 0.0;
    double pnl_r = 0.0;
    double equity_after = 0.0;
    ExitReason reason = ExitReason::None;
};

struct DailyRisk {
    int64_t ny_day = std::numeric_limits<int64_t>::min();
    int64_t week_monday = std::numeric_limits<int64_t>::min();
    double day_start_equity = 0.0;
    double week_start_equity = 0.0;
    double daily_realised = 0.0;
    double weekly_realised = 0.0;
    double daily_cumulative_r = 0.0;
    double daily_peak_r = 0.0;
    int daily_losses = 0;
    int daily_trades = 0;
    bool daily_locked = false;
    bool weekly_locked = false;
    bool drawdown_locked = false;
};

struct VenueCost {
    double normal_rt_bp = 0.0;
    double stress_rt_bp = 0.0;
    double entry_slip_price = 0.0;
    double exit_slip_price = 0.0;
};

class Engine {
public:
    explicit Engine(Config cfg)
        : cfg_(std::move(cfg)),
          xau_ind_(cfg_),
          mgc_ind_(cfg_),
          equity_(cfg_.starting_equity),
          high_water_(cfg_.starting_equity) {
        if (!finite_positive(cfg_.starting_equity)) {
            throw std::invalid_argument("Starting equity must be positive");
        }
        if (cfg_.risk_fraction <= 0.0 || cfg_.risk_fraction > cfg_.max_risk_fraction) {
            throw std::invalid_argument("risk_fraction must be positive and <= max_risk_fraction");
        }
        ledger_.open(cfg_.ledger_path);
        if (!ledger_) throw std::runtime_error("Cannot create ledger: " + cfg_.ledger_path);
        ledger_ << "trade_id,session,venue,direction,entry_time,exit_time,entry_fill,exit_fill,"
                   "quantity,initial_stop,mfe_r,gross_pnl,costs,net_pnl,pnl_r,equity_after,exit_reason\n";
    }

    void on_minute(const MinuteBar& xau, const MinuteBar& mgc) {
        if (!xau.valid() || !mgc.valid()) return;
        last_xau_ = xau;
        last_mgc_ = mgc;
        current_time_ = std::max(xau.t, mgc.t);

        roll_risk_periods(current_time_);
        const auto xcomp = xau_ind_.update(xau, cfg_);
        const auto mcomp = mgc_ind_.update(mgc, cfg_);

        update_sessions(xau, mgc);
        update_open_pnl_and_locks();

        if (position_.open()) {
            manage_position(xau, mgc);
        }

        if (!position_.open()) {
            enforce_lock_state();
            if (!risk_locked()) {
                if (xcomp.b5 && mcomp.b5 && xcomp.b5->end == mcomp.b5->end) {
                    process_completed_5m(*xcomp.b5, *mcomp.b5);
                }
                process_pending_entry(xau, mgc);
            }
        }
    }

    void finish() {
        if (position_.open()) {
            const MinuteBar& b = position_.venue == Venue::XAU ? last_xau_ : last_mgc_;
            close_position(b.close, current_time_, ExitReason::EndOfData);
        }
        ledger_.flush();
        print_summary();
    }

private:
    [[nodiscard]] bool risk_locked() const {
        return risk_.daily_locked || risk_.weekly_locked || risk_.drawdown_locked;
    }

    void enforce_lock_state() {
        if (risk_.drawdown_locked) state_ = EngineState::DrawdownLocked;
        else if (risk_.weekly_locked) state_ = EngineState::WeeklyLocked;
        else if (risk_.daily_locked) state_ = EngineState::DailyLocked;
        else if (state_ == EngineState::DailyLocked || state_ == EngineState::WeeklyLocked) {
            state_ = EngineState::Flat;
        }
    }

    void roll_risk_periods(int64_t utc_time) {
        const LocalClock ny = new_york_clock(utc_time);
        const int64_t ny_day = ny.local_day_index;
        const int64_t week = monday_of_week(ny_day);

        if (risk_.ny_day != ny_day) {
            risk_.ny_day = ny_day;
            risk_.day_start_equity = equity_;
            risk_.daily_realised = 0.0;
            risk_.daily_cumulative_r = 0.0;
            risk_.daily_peak_r = 0.0;
            risk_.daily_losses = 0;
            risk_.daily_trades = 0;
            risk_.daily_locked = false;
            london_.active = false;
            newyork_.active = false;
            setup_.clear();
            if (!position_.open()) state_ = EngineState::Flat;
        }
        if (risk_.week_monday != week) {
            risk_.week_monday = week;
            risk_.week_start_equity = equity_;
            risk_.weekly_realised = 0.0;
            risk_.weekly_locked = false;
            if (!position_.open() && !risk_.drawdown_locked) state_ = EngineState::Flat;
        }
    }

    void update_sessions(const MinuteBar& xau, const MinuteBar& mgc) {
        update_one_session(london_, SessionId::London, london_clock(current_time_),
                           7 * 60, 7 * 60 + cfg_.opening_range_minutes,
                           11 * 60 + 30, 16 * 60 + 30, xau, mgc);
        update_one_session(newyork_, SessionId::NewYork, new_york_clock(current_time_),
                           8 * 60, 8 * 60 + cfg_.opening_range_minutes,
                           12 * 60 + 30, 16 * 60 + 30, xau, mgc);
    }

    void update_one_session(SessionTracker& s, SessionId id, const LocalClock& clock,
                            int start, int range_end, int entry_end, int flatten,
                            const MinuteBar& xau, const MinuteBar& mgc) {
        if (clock.weekday == 0 || clock.weekday == 6) return;
        if (clock.minute_of_day < start || clock.minute_of_day > flatten) return;

        if (!s.active || s.local_day != clock.local_day_index) {
            s.reset(id, clock.local_day_index, start, range_end, entry_end, flatten);
        }

        s.update_vwap(xau, mgc);
        if (clock.minute_of_day >= start && clock.minute_of_day < range_end) {
            s.xau_high = std::max(s.xau_high, xau.high);
            s.xau_low = std::min(s.xau_low, xau.low);
            s.mgc_high = std::max(s.mgc_high, mgc.high);
            s.mgc_low = std::min(s.mgc_low, mgc.low);
            state_ = position_.open() ? state_ : EngineState::BuildingRange;
        }
        if (clock.minute_of_day >= range_end
            && std::isfinite(s.xau_high) && std::isfinite(s.xau_low)
            && std::isfinite(s.mgc_high) && std::isfinite(s.mgc_low)) {
            s.range_complete = true;
        }
    }

    [[nodiscard]] SessionTracker* active_entry_session(int64_t t) {
        const LocalClock lon = london_clock(t);
        if (london_.active && london_.range_complete
            && lon.local_day_index == london_.local_day
            && lon.minute_of_day >= london_.range_end_minute
            && lon.minute_of_day <= london_.entry_end_minute
            && !london_.trade_used && !london_.loss_lock) {
            return &london_;
        }
        const LocalClock ny = new_york_clock(t);
        if (newyork_.active && newyork_.range_complete
            && ny.local_day_index == newyork_.local_day
            && ny.minute_of_day >= newyork_.range_end_minute
            && ny.minute_of_day <= newyork_.entry_end_minute
            && !newyork_.trade_used && !newyork_.loss_lock) {
            return &newyork_;
        }
        return nullptr;
    }

    [[nodiscard]] Direction regime_direction(const SessionTracker& s) const {
        if (!xau_ind_.ready(cfg_) || !mgc_ind_.ready(cfg_)) return Direction::None;

        const bool xau_long = xau_ind_.last_30_close > xau_ind_.last_ema_slow
            && xau_ind_.last_ema_fast > xau_ind_.last_ema_slow
            && xau_ind_.last_ema_slow > xau_ind_.last_ema_slow_old
            && last_xau_.close > s.xau_vwap();
        const bool mgc_long = mgc_ind_.last_30_close > mgc_ind_.last_ema_slow
            && mgc_ind_.last_ema_fast > mgc_ind_.last_ema_slow
            && mgc_ind_.last_ema_slow > mgc_ind_.last_ema_slow_old
            && last_mgc_.close > s.mgc_vwap();

        const bool xau_short = xau_ind_.last_30_close < xau_ind_.last_ema_slow
            && xau_ind_.last_ema_fast < xau_ind_.last_ema_slow
            && xau_ind_.last_ema_slow < xau_ind_.last_ema_slow_old
            && last_xau_.close < s.xau_vwap();
        const bool mgc_short = mgc_ind_.last_30_close < mgc_ind_.last_ema_slow
            && mgc_ind_.last_ema_fast < mgc_ind_.last_ema_slow
            && mgc_ind_.last_ema_slow < mgc_ind_.last_ema_slow_old
            && last_mgc_.close < s.mgc_vwap();

        if (cfg_.enable_longs && xau_long && mgc_long) return Direction::Long;
        if (cfg_.enable_shorts && xau_short && mgc_short) return Direction::Short;
        return Direction::None;
    }

    [[nodiscard]] double atr_bp(double atr, double price) const {
        return finite_positive(atr) && finite_positive(price) ? atr / price * kBp : 0.0;
    }

    [[nodiscard]] VenueCost xau_cost(double price) const {
        VenueCost c;
        c.normal_rt_bp = cfg_.xau_base_rt_cost_bp
                       + cfg_.xau_entry_slippage_bp
                       + cfg_.xau_exit_slippage_bp;
        c.stress_rt_bp = std::max(1.5 * c.normal_rt_bp, c.normal_rt_bp + 4.0);
        c.entry_slip_price = price * cfg_.xau_entry_slippage_bp / kBp;
        c.exit_slip_price = price * cfg_.xau_exit_slippage_bp / kBp;
        return c;
    }

    [[nodiscard]] VenueCost mgc_cost(double price) const {
        VenueCost c;
        const double notional = price * cfg_.mgc_ounces_per_contract;
        const double slippage_usd = (cfg_.mgc_entry_slippage_ticks + cfg_.mgc_exit_slippage_ticks)
                                  * cfg_.mgc_tick_value;
        c.normal_rt_bp = notional > 0.0
            ? (cfg_.mgc_rt_commission_usd + slippage_usd) / notional * kBp
            : std::numeric_limits<double>::infinity();
        c.stress_rt_bp = std::max(1.5 * c.normal_rt_bp, c.normal_rt_bp + 4.0);
        c.entry_slip_price = cfg_.mgc_entry_slippage_ticks * cfg_.mgc_tick_size;
        c.exit_slip_price = cfg_.mgc_exit_slippage_ticks * cfg_.mgc_tick_size;
        return c;
    }

    void process_completed_5m(const Bar& x5, const Bar& m5) {
        if (position_.open() || risk_locked()) return;
        if (risk_.daily_trades >= cfg_.max_trades_per_day) {
            risk_.daily_locked = true;
            return;
        }

        SessionTracker* session = active_entry_session(x5.end - 1);
        if (!session) {
            if (setup_.session != SessionId::None) setup_.clear();
            return;
        }

        const Direction regime = regime_direction(*session);
        if (regime == Direction::None) {
            if (setup_.session == session->id) setup_.clear();
            return;
        }
        state_ = EngineState::RegimeReady;

        const double x_atr_bp = atr_bp(xau_ind_.last_atr15, x5.close);
        const double m_atr_bp = atr_bp(mgc_ind_.last_atr15, m5.close);
        const VenueCost xc = xau_cost(x5.close);
        const VenueCost mc = mgc_cost(m5.close);

        if (x_atr_bp < cfg_.min_atr_to_cost_multiple * xc.stress_rt_bp) return;
        if (m_atr_bp < cfg_.min_atr_to_cost_multiple * mc.stress_rt_bp) return;

        const double x_range = session->xau_high - session->xau_low;
        const double m_range = session->mgc_high - session->mgc_low;
        if (x_range < cfg_.min_opening_range_atr * xau_ind_.last_atr15
            || x_range > cfg_.max_opening_range_atr * xau_ind_.last_atr15
            || m_range < cfg_.min_opening_range_atr * mgc_ind_.last_atr15
            || m_range > cfg_.max_opening_range_atr * mgc_ind_.last_atr15) {
            return;
        }

        const double x_break_long = session->xau_high + cfg_.breakout_buffer_atr * xau_ind_.last_atr15;
        const double x_break_short = session->xau_low - cfg_.breakout_buffer_atr * xau_ind_.last_atr15;
        const double m_break_long = session->mgc_high + cfg_.breakout_buffer_atr * mgc_ind_.last_atr15;
        const double m_break_short = session->mgc_low - cfg_.breakout_buffer_atr * mgc_ind_.last_atr15;

        if (setup_.session == SessionId::None) {
            bool confirmed = false;
            if (regime == Direction::Long) {
                confirmed = x5.close > x_break_long && m5.close > m_break_long;
            } else {
                confirmed = x5.close < x_break_short && m5.close < m_break_short;
            }
            if (!confirmed) return;
            if ((x5.high - x5.low) > cfg_.max_breakout_bar_atr * xau_ind_.last_atr15
                || (m5.high - m5.low) > cfg_.max_breakout_bar_atr * mgc_ind_.last_atr15) {
                return;
            }

            setup_.session = session->id;
            setup_.direction = regime;
            setup_.breakout_time = x5.end;
            setup_.pullback_bars = 0;
            setup_.xau_breakout = regime == Direction::Long ? x_break_long : x_break_short;
            setup_.mgc_breakout = regime == Direction::Long ? m_break_long : m_break_short;
            setup_.xau_atr = xau_ind_.last_atr15;
            setup_.mgc_atr = mgc_ind_.last_atr15;
            setup_.xau_pullback_swing = regime == Direction::Long ? x5.low : x5.high;
            setup_.mgc_pullback_swing = regime == Direction::Long ? m5.low : m5.high;
            state_ = EngineState::WaitPullback;
            return;
        }

        if (setup_.session != session->id || setup_.direction != regime) {
            setup_.clear();
            return;
        }
        if (state_ != EngineState::WaitPullback) return;

        ++setup_.pullback_bars;
        if (setup_.pullback_bars > cfg_.pullback_timeout_bars_5m) {
            setup_.clear();
            state_ = EngineState::Flat;
            return;
        }

        const bool long_side = setup_.direction == Direction::Long;
        if (long_side) {
            setup_.xau_pullback_swing = std::min(setup_.xau_pullback_swing, x5.low);
            setup_.mgc_pullback_swing = std::min(setup_.mgc_pullback_swing, m5.low);
        } else {
            setup_.xau_pullback_swing = std::max(setup_.xau_pullback_swing, x5.high);
            setup_.mgc_pullback_swing = std::max(setup_.mgc_pullback_swing, m5.high);
        }

        bool x_valid = false;
        bool m_valid = false;
        if (long_side) {
            x_valid = x5.low <= setup_.xau_breakout + cfg_.pullback_distance_atr * setup_.xau_atr
                   && x5.close >= session->xau_high - cfg_.invalid_inside_atr * setup_.xau_atr;
            m_valid = m5.low <= setup_.mgc_breakout + cfg_.pullback_distance_atr * setup_.mgc_atr
                   && m5.close >= session->mgc_high - cfg_.invalid_inside_atr * setup_.mgc_atr;
            if (x5.close < session->xau_high - cfg_.invalid_inside_atr * setup_.xau_atr
                || m5.close < session->mgc_high - cfg_.invalid_inside_atr * setup_.mgc_atr) {
                setup_.clear();
                state_ = EngineState::Flat;
                return;
            }
        } else {
            x_valid = x5.high >= setup_.xau_breakout - cfg_.pullback_distance_atr * setup_.xau_atr
                   && x5.close <= session->xau_low + cfg_.invalid_inside_atr * setup_.xau_atr;
            m_valid = m5.high >= setup_.mgc_breakout - cfg_.pullback_distance_atr * setup_.mgc_atr
                   && m5.close <= session->mgc_low + cfg_.invalid_inside_atr * setup_.mgc_atr;
            if (x5.close > session->xau_low + cfg_.invalid_inside_atr * setup_.xau_atr
                || m5.close > session->mgc_low + cfg_.invalid_inside_atr * setup_.mgc_atr) {
                setup_.clear();
                state_ = EngineState::Flat;
                return;
            }
        }

        if (x_valid && m_valid) {
            setup_.xau_entry_trigger = long_side ? x5.high : x5.low;
            setup_.mgc_entry_trigger = long_side ? m5.high : m5.low;
            setup_.pending_since = x5.end;
            state_ = EngineState::EntryPending;
        }
    }

    void process_pending_entry(const MinuteBar& xau, const MinuteBar& mgc) {
        if (state_ != EngineState::EntryPending || position_.open()) return;
        SessionTracker* s = setup_.session == SessionId::London ? &london_ : &newyork_;
        if (!s || s->trade_used || s->loss_lock) {
            setup_.clear();
            state_ = EngineState::Flat;
            return;
        }

        const bool long_side = setup_.direction == Direction::Long;
        const bool x_triggered = long_side
            ? xau.high >= setup_.xau_entry_trigger
            : xau.low <= setup_.xau_entry_trigger;
        const bool m_triggered = long_side
            ? mgc.high >= setup_.mgc_entry_trigger
            : mgc.low <= setup_.mgc_entry_trigger;

        // Both markets must still confirm on the entry minute.
        if (!(x_triggered && m_triggered)) {
            if (current_time_ - setup_.pending_since > 30 * 60) {
                setup_.clear();
                state_ = EngineState::Flat;
            }
            return;
        }

        const Venue venue = choose_venue();
        if (venue == Venue::None) {
            setup_.clear();
            state_ = EngineState::Flat;
            return;
        }
        if (!open_position(venue)) {
            setup_.clear();
            state_ = EngineState::Flat;
            return;
        }

        s->trade_used = true;
        ++risk_.daily_trades;
        setup_.clear();
    }

    [[nodiscard]] Venue choose_venue() const {
        const VenueCost xc = xau_cost(setup_.xau_entry_trigger);
        const VenueCost mc = mgc_cost(setup_.mgc_entry_trigger);

        const bool mgc_viable = mgc_contracts_for_trade(mc) >= 1.0;
        if (mgc_viable && mc.normal_rt_bp < xc.normal_rt_bp) return Venue::MGC;
        return Venue::XAU;
    }

    [[nodiscard]] double mgc_contracts_for_trade(const VenueCost& cost) const {
        const double trigger = setup_.mgc_entry_trigger;
        if (!finite_positive(trigger)) return 0.0;
        const double adverse_entry = setup_.direction == Direction::Long
            ? trigger + cost.entry_slip_price
            : trigger - cost.entry_slip_price;
        const double atr = setup_.mgc_atr;
        const double raw_stop = setup_.direction == Direction::Long
            ? setup_.mgc_pullback_swing - cfg_.structural_buffer_atr * atr
            : setup_.mgc_pullback_swing + cfg_.structural_buffer_atr * atr;
        const double raw_bp = std::fabs(adverse_entry - raw_stop) / adverse_entry * kBp;
        const double min_bp = std::max(cfg_.min_stop_cost_multiple * cost.stress_rt_bp,
                                       cfg_.min_stop_atr * atr_bp(atr, adverse_entry));
        const double max_bp = cfg_.max_stop_atr * atr_bp(atr, adverse_entry);
        const double stop_bp = std::max(raw_bp, min_bp);
        if (stop_bp > max_bp || stop_bp <= 0.0) return 0.0;
        const double stop_distance = adverse_entry * stop_bp / kBp;
        const double per_contract_loss = stop_distance * cfg_.mgc_ounces_per_contract
                                       + cfg_.mgc_rt_commission_usd
                                       + (cfg_.mgc_entry_slippage_ticks + cfg_.mgc_exit_slippage_ticks)
                                         * cfg_.mgc_tick_value;
        const double risk_budget = equity_ * cfg_.risk_fraction;
        if (per_contract_loss > equity_ * cfg_.max_single_mgc_risk_fraction) return 0.0;
        return std::floor(risk_budget / per_contract_loss);
    }

    bool open_position(Venue venue) {
        const bool long_side = setup_.direction == Direction::Long;
        const double trigger = venue == Venue::XAU ? setup_.xau_entry_trigger : setup_.mgc_entry_trigger;
        const double atr = venue == Venue::XAU ? setup_.xau_atr : setup_.mgc_atr;
        const double swing = venue == Venue::XAU ? setup_.xau_pullback_swing : setup_.mgc_pullback_swing;
        const VenueCost cost = venue == Venue::XAU ? xau_cost(trigger) : mgc_cost(trigger);

        const double entry = long_side ? trigger + cost.entry_slip_price
                                       : trigger - cost.entry_slip_price;
        const double raw_stop = long_side
            ? swing - cfg_.structural_buffer_atr * atr
            : swing + cfg_.structural_buffer_atr * atr;
        const double raw_bp = std::fabs(entry - raw_stop) / entry * kBp;
        const double atrbp = atr_bp(atr, entry);
        const double min_bp = std::max(cfg_.min_stop_cost_multiple * cost.stress_rt_bp,
                                       cfg_.min_stop_atr * atrbp);
        const double max_bp = cfg_.max_stop_atr * atrbp;
        const double stop_bp = std::max(raw_bp, min_bp);
        if (!(stop_bp > 0.0 && stop_bp <= max_bp)) return false;

        const double stop = long_side ? entry * (1.0 - stop_bp / kBp)
                                      : entry * (1.0 + stop_bp / kBp);
        const double r_price = std::fabs(entry - stop);
        const double risk_budget = equity_ * cfg_.risk_fraction;
        if (!(r_price > 0.0 && risk_budget > 0.0)) return false;

        double qty = 0.0;
        double entry_cost = 0.0;
        double risk_dollars = 0.0;
        if (venue == Venue::XAU) {
            const double stressed_cost_per_oz = entry * cost.stress_rt_bp / kBp;
            qty = risk_budget / (r_price + stressed_cost_per_oz);
            if (!(qty > 0.0)) return false;
            entry_cost = qty * entry * (cfg_.xau_base_rt_cost_bp * 0.5) / kBp;
            risk_dollars = qty * r_price + qty * stressed_cost_per_oz;
        } else {
            const double per_contract_risk = r_price * cfg_.mgc_ounces_per_contract
                                           + cfg_.mgc_rt_commission_usd
                                           + (cfg_.mgc_entry_slippage_ticks + cfg_.mgc_exit_slippage_ticks)
                                             * cfg_.mgc_tick_value;
            qty = std::floor(risk_budget / per_contract_risk);
            if (qty < 1.0 || per_contract_risk > equity_ * cfg_.max_single_mgc_risk_fraction) {
                return false;
            }
            entry_cost = qty * cfg_.mgc_rt_commission_usd * 0.5;
            risk_dollars = qty * per_contract_risk;
        }

        position_.venue = venue;
        position_.direction = setup_.direction;
        position_.session = setup_.session;
        position_.entry_time = current_time_;
        position_.entry_fill = entry;
        position_.quantity = qty;
        position_.remaining = qty;
        position_.initial_stop = stop;
        position_.active_stop = stop;
        position_.initial_r_price = r_price;
        position_.initial_risk_dollars = risk_dollars;
        position_.highest = entry;
        position_.lowest = entry;
        position_.mfe_r = 0.0;
        position_.realised_partial = 0.0;
        position_.partial_gross = 0.0;
        position_.partial_exit_cost = 0.0;
        position_.entry_cost = entry_cost;
        position_.stressed_cost_bp = cost.stress_rt_bp;
        state_ = EngineState::PositionInitial;

        if (cfg_.verbose) {
            std::cout << "[ENTRY] " << iso_utc(current_time_) << ' '
                      << to_string(position_.session) << ' '
                      << to_string(position_.venue) << ' '
                      << to_string(position_.direction)
                      << " fill=" << position_.entry_fill
                      << " stop=" << position_.initial_stop
                      << " qty=" << position_.quantity
                      << " risk$=" << position_.initial_risk_dollars << '\n';
        }
        return true;
    }

    [[nodiscard]] double current_atr_for_position() const {
        return position_.venue == Venue::XAU ? xau_ind_.last_atr15 : mgc_ind_.last_atr15;
    }

    [[nodiscard]] double venue_multiplier() const {
        return position_.venue == Venue::XAU ? 1.0 : cfg_.mgc_ounces_per_contract;
    }

    [[nodiscard]] double normal_exit_cost(double qty) const {
        if (position_.venue == Venue::XAU) {
            return qty * position_.entry_fill * (cfg_.xau_base_rt_cost_bp * 0.5) / kBp;
        }
        return qty * cfg_.mgc_rt_commission_usd * 0.5;
    }

    [[nodiscard]] double apply_exit_slippage(double raw_price, Direction d) const {
        if (position_.venue == Venue::XAU) {
            const double slip = raw_price * cfg_.xau_exit_slippage_bp / kBp;
            return d == Direction::Long ? raw_price - slip : raw_price + slip;
        }
        const double slip = cfg_.mgc_exit_slippage_ticks * cfg_.mgc_tick_size;
        return d == Direction::Long ? raw_price - slip : raw_price + slip;
    }

    [[nodiscard]] double gross_pnl(double entry, double exit, double qty, Direction d) const {
        const double diff = d == Direction::Long ? exit - entry : entry - exit;
        return diff * qty * venue_multiplier();
    }

    void manage_position(const MinuteBar& xau, const MinuteBar& mgc) {
        const MinuteBar& b = position_.venue == Venue::XAU ? xau : mgc;
        const bool long_side = position_.direction == Direction::Long;

        // Conservative bar handling: active stop is checked before favourable excursion.
        const bool stop_hit = long_side ? b.low <= position_.active_stop
                                        : b.high >= position_.active_stop;
        if (stop_hit) {
            ExitReason reason = ExitReason::InitialStop;
            if (position_.runner_active) reason = ExitReason::RunnerStop;
            else if (position_.cost_protected) reason = ExitReason::ProtectedStop;
            close_position(position_.active_stop, b.t, reason);
            return;
        }

        position_.highest = std::max(position_.highest, b.high);
        position_.lowest = std::min(position_.lowest, b.low);
        const double favourable = long_side
            ? position_.highest - position_.entry_fill
            : position_.entry_fill - position_.lowest;
        position_.mfe_r = position_.initial_r_price > 0.0
            ? favourable / position_.initial_r_price : 0.0;

        if (!position_.cost_protected && position_.mfe_r >= cfg_.protect_trigger_r) {
            const double floor_distance = position_.entry_fill
                * (position_.stressed_cost_bp + cfg_.protected_extra_bp) / kBp;
            const double floor_price = long_side
                ? position_.entry_fill + floor_distance
                : position_.entry_fill - floor_distance;
            if (long_side) position_.active_stop = std::max(position_.active_stop, floor_price);
            else position_.active_stop = std::min(position_.active_stop, floor_price);
            position_.cost_protected = true;
            state_ = EngineState::PositionCostProtected;
        }

        if (!position_.partial_taken && position_.mfe_r >= cfg_.partial_trigger_r) {
            const double target = long_side
                ? position_.entry_fill + cfg_.partial_trigger_r * position_.initial_r_price
                : position_.entry_fill - cfg_.partial_trigger_r * position_.initial_r_price;
            take_partial(target, b.t);
            if (!position_.open()) return;

            const double cost_floor_distance = position_.entry_fill
                * (position_.stressed_cost_bp + cfg_.protected_extra_bp) / kBp;
            const double r_floor_distance = cfg_.runner_floor_r * position_.initial_r_price;
            const double floor_distance = std::max(cost_floor_distance, r_floor_distance);
            const double floor_price = long_side
                ? position_.entry_fill + floor_distance
                : position_.entry_fill - floor_distance;
            if (long_side) position_.active_stop = std::max(position_.active_stop, floor_price);
            else position_.active_stop = std::min(position_.active_stop, floor_price);
            position_.partial_taken = true;
            state_ = EngineState::PositionPartial;
        }

        if (position_.mfe_r >= cfg_.runner_trigger_r) {
            position_.runner_active = true;
            state_ = EngineState::PositionRunner;
        }

        if (position_.runner_active) {
            const double atr = current_atr_for_position();
            if (finite_positive(atr)) {
                const double chandelier = long_side
                    ? position_.highest - cfg_.runner_atr * atr
                    : position_.lowest + cfg_.runner_atr * atr;
                if (long_side) position_.active_stop = std::max(position_.active_stop, chandelier);
                else position_.active_stop = std::min(position_.active_stop, chandelier);
            }
        }

        // If a newly armed floor or chandelier also lies inside this minute's
        // range, assume the stop was hit. This deliberately avoids optimistic
        // same-bar sequencing when tick order is unavailable.
        const bool newly_armed_stop_hit = long_side
            ? b.low <= position_.active_stop
            : b.high >= position_.active_stop;
        if (newly_armed_stop_hit) {
            ExitReason reason = position_.runner_active
                ? ExitReason::RunnerStop
                : (position_.cost_protected ? ExitReason::ProtectedStop
                                            : ExitReason::InitialStop);
            close_position(position_.active_stop, b.t, reason);
            return;
        }

        const int elapsed_minutes = static_cast<int>((b.t - position_.entry_time) / 60);
        const SessionTracker& s = position_.session == SessionId::London ? london_ : newyork_;
        const double vwap = position_.venue == Venue::XAU ? s.xau_vwap() : s.mgc_vwap();
        const bool crossed_vwap = long_side ? b.close < vwap : b.close > vwap;
        if (elapsed_minutes >= cfg_.failed_exit_minutes
            && position_.mfe_r < cfg_.failed_exit_max_mfe_r
            && crossed_vwap) {
            close_position(b.close, b.t, ExitReason::FailedBreakout);
            return;
        }

        const LocalClock ny = new_york_clock(b.t);
        if (ny.minute_of_day >= 16 * 60 + 30) {
            close_position(b.close, b.t, ExitReason::SessionFlatten);
        }
    }

    void take_partial(double raw_price, int64_t t) {
        const double qty = position_.quantity * cfg_.partial_fraction;
        const double actual_qty = std::min(qty, position_.remaining);
        if (!(actual_qty > 0.0)) return;
        const double fill = apply_exit_slippage(raw_price, position_.direction);
        const double gross = gross_pnl(position_.entry_fill, fill, actual_qty, position_.direction);
        const double costs = normal_exit_cost(actual_qty);
        position_.partial_gross += gross;
        position_.partial_exit_cost += costs;
        position_.realised_partial += gross - costs;
        position_.remaining -= actual_qty;
        if (position_.remaining <= kEps) position_.remaining = 0.0;

        if (cfg_.verbose) {
            std::cout << "[PARTIAL] " << iso_utc(t)
                      << " fill=" << fill << " qty=" << actual_qty
                      << " net=" << (gross - costs) << '\n';
        }
    }

    void close_position(double raw_price, int64_t t, ExitReason reason) {
        if (!position_.open()) return;
        const double exit_fill = apply_exit_slippage(raw_price, position_.direction);
        const double remaining_qty = position_.remaining;
        const double gross_remaining = gross_pnl(position_.entry_fill, exit_fill,
                                                  remaining_qty, position_.direction);
        const double exit_cost_remaining = normal_exit_cost(remaining_qty);
        const double gross_all = position_.partial_gross + gross_remaining;
        const double costs = position_.entry_cost + position_.partial_exit_cost
                           + exit_cost_remaining;
        const double net = gross_all - costs;
        const double pnl_r = position_.initial_risk_dollars > 0.0
            ? net / position_.initial_risk_dollars : 0.0;

        equity_ += net;
        high_water_ = std::max(high_water_, equity_);
        risk_.daily_realised += net;
        risk_.weekly_realised += net;
        risk_.daily_cumulative_r += pnl_r;
        risk_.daily_peak_r = std::max(risk_.daily_peak_r, risk_.daily_cumulative_r);
        if (net < 0.0) ++risk_.daily_losses;

        TradeRecord rec;
        rec.trade_id = ++trade_counter_;
        rec.session = position_.session;
        rec.venue = position_.venue;
        rec.direction = position_.direction;
        rec.entry_time = position_.entry_time;
        rec.exit_time = t;
        rec.entry_fill = position_.entry_fill;
        rec.exit_fill = exit_fill;
        rec.quantity = position_.quantity;
        rec.initial_stop = position_.initial_stop;
        rec.mfe_r = position_.mfe_r;
        rec.gross_pnl = gross_all;
        rec.costs = costs;
        rec.net_pnl = net;
        rec.pnl_r = pnl_r;
        rec.equity_after = equity_;
        rec.reason = reason;
        records_.push_back(rec);
        write_trade(rec);

        SessionTracker& s = position_.session == SessionId::London ? london_ : newyork_;
        if (net < 0.0) s.loss_lock = true;

        if (cfg_.verbose) {
            std::cout << "[EXIT] " << iso_utc(t) << ' '
                      << to_string(reason)
                      << " net=" << net << " R=" << pnl_r
                      << " equity=" << equity_ << '\n';
        }

        position_.clear();
        state_ = EngineState::Flat;
        update_open_pnl_and_locks();
    }

    void update_open_pnl_and_locks() {
        double open_net = 0.0;
        if (position_.open()) {
            const MinuteBar& b = position_.venue == Venue::XAU ? last_xau_ : last_mgc_;
            const double mark = b.close;
            const double gross = gross_pnl(position_.entry_fill, mark,
                                            position_.remaining, position_.direction);
            const double estimated_exit_cost = normal_exit_cost(position_.remaining);
            open_net = position_.realised_partial + gross
                     - position_.entry_cost - estimated_exit_cost;
        }

        const double day_pnl = risk_.daily_realised + open_net;
        if (risk_.day_start_equity > 0.0
            && day_pnl <= -cfg_.daily_loss_fraction * risk_.day_start_equity) {
            risk_.daily_locked = true;
        }
        if (risk_.daily_losses >= cfg_.daily_max_losses) risk_.daily_locked = true;
        if (risk_.daily_trades >= cfg_.max_trades_per_day) risk_.daily_locked = true;
        if (risk_.daily_peak_r >= cfg_.daily_profit_arm_r
            && risk_.daily_peak_r - risk_.daily_cumulative_r >= cfg_.daily_profit_giveback_r) {
            risk_.daily_locked = true;
        }

        if (risk_.week_start_equity > 0.0
            && risk_.weekly_realised + open_net
                <= -cfg_.weekly_loss_fraction * risk_.week_start_equity) {
            risk_.weekly_locked = true;
        }

        const double marked_equity = equity_ + open_net;
        high_water_ = std::max(high_water_, marked_equity);
        if (high_water_ > 0.0
            && marked_equity <= high_water_ * (1.0 - cfg_.account_drawdown_fraction)) {
            risk_.drawdown_locked = true;
        }

        if (position_.open() && risk_locked()) {
            // Do not forcibly liquidate merely because a new-entry lock engaged.
            // Existing protective stops continue. The only immediate forced exit is
            // an account drawdown lock, where preserving capital takes priority.
            if (risk_.drawdown_locked) {
                const MinuteBar& b = position_.venue == Venue::XAU ? last_xau_ : last_mgc_;
                close_position(b.close, current_time_, ExitReason::RiskGovernor);
            }
        }
    }

    void write_trade(const TradeRecord& r) {
        ledger_ << r.trade_id << ','
                << to_string(r.session) << ','
                << to_string(r.venue) << ','
                << to_string(r.direction) << ','
                << iso_utc(r.entry_time) << ','
                << iso_utc(r.exit_time) << ','
                << std::fixed << std::setprecision(8)
                << r.entry_fill << ',' << r.exit_fill << ',' << r.quantity << ','
                << r.initial_stop << ',' << r.mfe_r << ',' << r.gross_pnl << ','
                << r.costs << ',' << r.net_pnl << ',' << r.pnl_r << ','
                << r.equity_after << ',' << to_string(r.reason) << '\n';
    }

    void print_summary() const {
        double gross_profit = 0.0;
        double gross_loss = 0.0;
        double net = 0.0;
        double peak = cfg_.starting_equity;
        double eq = cfg_.starting_equity;
        double max_dd = 0.0;
        int wins = 0;
        int losses = 0;
        int longs = 0;
        int shorts = 0;
        int xau_trades = 0;
        int mgc_trades = 0;
        double sum_r = 0.0;

        for (const auto& r : records_) {
            net += r.net_pnl;
            sum_r += r.pnl_r;
            if (r.net_pnl > 0.0) {
                gross_profit += r.net_pnl;
                ++wins;
            } else if (r.net_pnl < 0.0) {
                gross_loss += -r.net_pnl;
                ++losses;
            }
            if (r.direction == Direction::Long) ++longs;
            if (r.direction == Direction::Short) ++shorts;
            if (r.venue == Venue::XAU) ++xau_trades;
            if (r.venue == Venue::MGC) ++mgc_trades;
            eq += r.net_pnl;
            peak = std::max(peak, eq);
            max_dd = std::max(max_dd, peak - eq);
        }

        const int n = static_cast<int>(records_.size());
        const double pf = gross_loss > 0.0
            ? gross_profit / gross_loss
            : (gross_profit > 0.0 ? std::numeric_limits<double>::infinity() : 0.0);
        const double win_rate = n > 0 ? 100.0 * wins / static_cast<double>(n) : 0.0;
        const double ret_pct = cfg_.starting_equity > 0.0
            ? 100.0 * net / cfg_.starting_equity : 0.0;
        const double dd_pct = cfg_.starting_equity > 0.0
            ? 100.0 * max_dd / cfg_.starting_equity : 0.0;
        const double profit_dd = max_dd > 0.0 ? net / max_dd : 0.0;

        std::cout << "\n=== AURUM-BREAK-PULLBACK-V1 SUMMARY ===\n"
                  << "Trades:               " << n << '\n'
                  << "Wins / losses:        " << wins << " / " << losses << '\n'
                  << "Win rate:             " << std::fixed << std::setprecision(2)
                  << win_rate << "%\n"
                  << "Long / short:         " << longs << " / " << shorts << '\n'
                  << "XAU / MGC routed:     " << xau_trades << " / " << mgc_trades << '\n'
                  << "Gross profit:         $" << gross_profit << '\n'
                  << "Gross loss:           $" << gross_loss << '\n'
                  << "Profit factor:        " << pf << '\n'
                  << "Net PnL:              $" << net << '\n'
                  << "Return:               " << ret_pct << "%\n"
                  << "Maximum drawdown:     $" << max_dd << " (" << dd_pct << "%)\n"
                  << "Net / max drawdown:   " << profit_dd << '\n'
                  << "Average R per trade:  " << (n > 0 ? sum_r / n : 0.0) << '\n'
                  << "Ending equity:        $" << equity_ << '\n'
                  << "Ledger:               " << cfg_.ledger_path << '\n'
                  << "Drawdown lock active: " << (risk_.drawdown_locked ? "YES" : "NO") << '\n';

        std::cout << "\nProduction gate reminder:\n"
                  << "  PF >= 1.35 normal cost, PF >= 1.15 at 2x cost,\n"
                  << "  both chronological halves positive, >=70% positive WF folds,\n"
                  << "  net/max-DD >= 2.0 normal cost and >=1.0 at 2x cost,\n"
                  << "  longs and shorts must pass independently.\n";
    }

    Config cfg_;
    InstrumentIndicators xau_ind_;
    InstrumentIndicators mgc_ind_;
    SessionTracker london_;
    SessionTracker newyork_;
    Setup setup_;
    Position position_;
    DailyRisk risk_;
    EngineState state_ = EngineState::Flat;
    MinuteBar last_xau_;
    MinuteBar last_mgc_;
    int64_t current_time_ = 0;
    double equity_ = 0.0;
    double high_water_ = 0.0;
    int trade_counter_ = 0;
    std::vector<TradeRecord> records_;
    std::ofstream ledger_;
};

struct Args {
    Config cfg;
    bool help = false;
};

[[nodiscard]] std::string require_value(int argc, char** argv, int& i, const std::string& key) {
    if (i + 1 >= argc) throw std::runtime_error("Missing value for " + key);
    return argv[++i];
}

[[nodiscard]] double require_double(int argc, char** argv, int& i, const std::string& key) {
    const std::string s = require_value(argc, argv, i, key);
    const auto v = parse_double(s);
    if (!v) throw std::runtime_error("Invalid number for " + key + ": " + s);
    return *v;
}

[[nodiscard]] int require_int(int argc, char** argv, int& i, const std::string& key) {
    const std::string s = require_value(argc, argv, i, key);
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        throw std::runtime_error("Invalid integer for " + key + ": " + s);
    }
    return v;
}

[[nodiscard]] Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") a.help = true;
        else if (key == "--xau") a.cfg.xau_path = require_value(argc, argv, i, key);
        else if (key == "--mgc") a.cfg.mgc_path = require_value(argc, argv, i, key);
        else if (key == "--ledger") a.cfg.ledger_path = require_value(argc, argv, i, key);
        else if (key == "--equity") a.cfg.starting_equity = require_double(argc, argv, i, key);
        else if (key == "--risk") a.cfg.risk_fraction = require_double(argc, argv, i, key);
        else if (key == "--xau-base-cost-bp") a.cfg.xau_base_rt_cost_bp = require_double(argc, argv, i, key);
        else if (key == "--xau-entry-slip-bp") a.cfg.xau_entry_slippage_bp = require_double(argc, argv, i, key);
        else if (key == "--xau-exit-slip-bp") a.cfg.xau_exit_slippage_bp = require_double(argc, argv, i, key);
        else if (key == "--mgc-rt-commission") a.cfg.mgc_rt_commission_usd = require_double(argc, argv, i, key);
        else if (key == "--mgc-entry-slip-ticks") a.cfg.mgc_entry_slippage_ticks = require_double(argc, argv, i, key);
        else if (key == "--mgc-exit-slip-ticks") a.cfg.mgc_exit_slippage_ticks = require_double(argc, argv, i, key);
        else if (key == "--opening-range-min") a.cfg.opening_range_minutes = require_int(argc, argv, i, key);
        else if (key == "--risk-per-trade") a.cfg.risk_fraction = require_double(argc, argv, i, key);
        else if (key == "--long-only") a.cfg.enable_shorts = false;
        else if (key == "--short-only") a.cfg.enable_longs = false;
        else if (key == "--verbose") a.cfg.verbose = true;
        else throw std::runtime_error("Unknown argument: " + key);
    }
    return a;
}

void print_help(const char* exe) {
    std::cout
        << "AURUM-BREAK-PULLBACK-V1\n\n"
        << "Usage:\n  " << exe << " --xau XAU.csv --mgc MGC.csv [options]\n\n"
        << "Required:\n"
        << "  --xau PATH                    XAUUSD tick/quote/bar CSV\n"
        << "  --mgc PATH                    MGC tick/trade/bar CSV\n\n"
        << "Main options:\n"
        << "  --ledger PATH                 Output trade ledger CSV\n"
        << "  --equity N                    Starting equity, default 100000\n"
        << "  --risk-per-trade F            Fraction, default 0.0020 (0.20%)\n"
        << "  --xau-base-cost-bp N          XAU round-trip base cost, default 6\n"
        << "  --xau-entry-slip-bp N         XAU adverse entry slippage, default 1\n"
        << "  --xau-exit-slip-bp N          XAU adverse exit slippage, default 2\n"
        << "  --mgc-rt-commission N         MGC round-trip commission/contract, default 2.50\n"
        << "  --mgc-entry-slip-ticks N      MGC entry slippage, default 1 tick\n"
        << "  --mgc-exit-slip-ticks N       MGC exit slippage, default 1 tick\n"
        << "  --opening-range-min N         Default 30\n"
        << "  --long-only                   Disable short entries\n"
        << "  --short-only                  Disable long entries\n"
        << "  --verbose                     Print every entry/partial/exit\n"
        << "  --help                        Show this help\n\n"
        << "CSV columns are detected by header name. Time must be UTC or include an offset.\n";
}

struct MergeStats {
    uint64_t matched = 0;
    uint64_t xau_only = 0;
    uint64_t mgc_only = 0;
};

void run_engine(const Config& cfg) {
    MinuteCsvReader xr(cfg.xau_path, "XAU");
    MinuteCsvReader mr(cfg.mgc_path, "MGC");
    Engine engine(cfg);

    MinuteBar xb, mb;
    bool has_x = xr.next(xb);
    bool has_m = mr.next(mb);
    MergeStats stats;

    while (has_x && has_m) {
        if (xb.t == mb.t) {
            engine.on_minute(xb, mb);
            ++stats.matched;
            has_x = xr.next(xb);
            has_m = mr.next(mb);
        } else if (xb.t < mb.t) {
            ++stats.xau_only;
            has_x = xr.next(xb);
        } else {
            ++stats.mgc_only;
            has_m = mr.next(mb);
        }

        if (cfg.verbose && stats.matched > 0 && stats.matched % 250000 == 0) {
            std::cout << "[PROGRESS] matched_minutes=" << stats.matched
                      << " xau_rows=" << xr.rows_read()
                      << " mgc_rows=" << mr.rows_read() << '\n';
        }
    }

    engine.finish();
    std::cout << "\n=== INPUT / MERGE ===\n"
              << "Matched minutes:       " << stats.matched << '\n'
              << "XAU-only minutes:      " << stats.xau_only << '\n'
              << "MGC-only minutes:      " << stats.mgc_only << '\n'
              << "XAU rows / bad:        " << xr.rows_read() << " / " << xr.rows_bad() << '\n'
              << "MGC rows / bad:        " << mr.rows_read() << " / " << mr.rows_bad() << '\n';

    if (stats.matched == 0) {
        throw std::runtime_error(
            "No matching UTC minutes. Confirm both timestamps are UTC/aligned and files overlap.");
    }
}

} // namespace aurum

int main(int argc, char** argv) {
    try {
        aurum::Args args = aurum::parse_args(argc, argv);
        if (args.help || argc == 1) {
            aurum::print_help(argv[0]);
            return args.help ? 0 : 1;
        }
        if (args.cfg.xau_path.empty() || args.cfg.mgc_path.empty()) {
            aurum::print_help(argv[0]);
            throw std::runtime_error("Both --xau and --mgc are required");
        }
        aurum::run_engine(args.cfg);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
