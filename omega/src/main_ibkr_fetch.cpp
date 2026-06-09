// omega/src/main_ibkr_fetch.cpp
// CLI: pull historical bars from IBKR (TWS / IB Gateway) into Omega CSV files.
// Only useful when built with -DOMEGA_WITH_IBKR; otherwise it reports that
// IBKR support was not compiled in.
//
//   omega_ibkr_fetch <out_dir> SYM1 SYM2 ... [--port 7497] [--duration "2 Y"]
//                    [--bar "1 day"]
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "omega/IbkrClient.h"

using namespace omega;

static void writeCsv(const std::string& dir, const Series& s) {
    std::string path = dir + "/" + s.symbol + ".csv";
    std::ofstream out(path);
    out << "date,open,high,low,close,volume\n";
    for (const Bar& b : s.bars)
        out << b.date << "," << b.open << "," << b.high << ","
            << b.low << "," << b.close << "," << b.volume << "\n";
    std::cout << "wrote " << s.bars.size() << " bars -> " << path << "\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: omega_ibkr_fetch <out_dir> SYM1 [SYM2 ...] "
                     "[--port P] [--duration \"2 Y\"] [--bar \"1 day\"]\n";
        return 1;
    }
    std::string outDir = argv[1];
    IbkrConfig cfg;
    HistRequest base;
    std::vector<std::string> symbols;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) cfg.port = std::atoi(argv[++i]);
        else if (a == "--duration" && i + 1 < argc) base.durationStr = argv[++i];
        else if (a == "--bar" && i + 1 < argc) base.barSize = argv[++i];
        else if (!a.empty() && a[0] != '-') symbols.push_back(a);
    }
    if (symbols.empty()) { std::cerr << "No symbols given.\n"; return 1; }

    try {
        IbkrClient client(cfg);
        if (!client.connect()) {
            std::cerr << "Could not connect to TWS/IB Gateway on port "
                      << cfg.port << ".\n";
            return 2;
        }
        for (const std::string& sym : symbols) {
            HistRequest req = base;
            req.symbol = sym;
            try {
                Series s = client.fetchHistorical(req);
                writeCsv(outDir, s);
            } catch (const std::exception& e) {
                std::cerr << "  " << sym << ": " << e.what() << "\n";
            }
        }
        client.disconnect();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 3;
    }
    return 0;
}
