// bid64_stub.cpp — TEMPORARY stub for Intel RDFP decimal (libbid) so the TWS C++
// client links. The gap-short bot uses integer share counts, not decimal math, so
// these are unused at runtime for scanning/simple orders. PRODUCTION TODO: replace
// with the real Intel RDFP Math Library (IntelRDFPMathLib2U) before live order sizing.
#include <cstdint>
#include <cstring>
typedef uint64_t Dec;
extern "C" {
Dec __bid64_add(Dec a, Dec, unsigned, unsigned*){ return a; }
Dec __bid64_sub(Dec a, Dec, unsigned, unsigned*){ return a; }
Dec __bid64_mul(Dec a, Dec, unsigned, unsigned*){ return a; }
Dec __bid64_div(Dec a, Dec, unsigned, unsigned*){ return a; }
Dec __bid64_from_string(char*, unsigned, unsigned*){ return 0; }
void __bid64_to_string(char* s, Dec, unsigned*){ if(s) s[0]=0; }
double __bid64_to_binary64(Dec, unsigned, unsigned*){ return 0.0; }
Dec __binary64_to_bid64(double, unsigned, unsigned*){ return 0; }
}
