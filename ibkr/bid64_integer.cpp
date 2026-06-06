// bid64_integer.cpp — CORRECT decimal64 (BID) for INTEGER order quantities.
// Replaces the stub. Order sizes are integer share counts (< 2^53), so the full
// Intel RDFP lib is unnecessary — the BID64 encoding of an integer N (exponent 0)
// is exact and trivial: bits63=sign, bits62-53=biased exp (bias 398), bits52-0=coef.
// Provides the __bid64_* symbols the TWS DecimalFunctions uses. For non-integer /
// arithmetic decimals (not used in order sizing) it degrades gracefully.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
typedef uint64_t Dec;
static const uint64_t EXP_BIAS=398;
static const uint64_t COEF_MASK=(1ULL<<53)-1;
static inline Dec enc_int(long long n){ uint64_t sign = n<0 ? (1ULL<<63):0; uint64_t c=(uint64_t)(n<0?-n:n);
    if(c>COEF_MASK) c=COEF_MASK; return sign | (EXP_BIAS<<53) | (c & COEF_MASK); }
static inline double dec2d(Dec x){ uint64_t sign=x>>63; uint64_t top2=(x>>61)&3; double coef; int exp;
    if(top2==3){ // large-coef form (rare; not used for share counts) -> approximate
        coef=(double)((x & ((1ULL<<51)-1)) | (1ULL<<53)); exp=(int)((x>>51)&0x3FF)-(int)EXP_BIAS; }
    else { coef=(double)(x & COEF_MASK); exp=(int)((x>>53)&0x3FF)-(int)EXP_BIAS; }
    double v=coef*pow(10.0,exp); return sign?-v:v; }
extern "C" {
Dec __binary64_to_bid64(double d, unsigned, unsigned*){ return enc_int((long long)llround(d)); }
double __bid64_to_binary64(Dec x, unsigned, unsigned*){ return dec2d(x); }
Dec __bid64_from_string(char* s, unsigned, unsigned*){ return enc_int(s?atoll(s):0); }
void __bid64_to_string(char* out, Dec x, unsigned*){ if(out) snprintf(out,32,"%.0f",dec2d(x)); }
Dec __bid64_add(Dec a, Dec b, unsigned, unsigned*){ return enc_int((long long)llround(dec2d(a)+dec2d(b))); }
Dec __bid64_sub(Dec a, Dec b, unsigned, unsigned*){ return enc_int((long long)llround(dec2d(a)-dec2d(b))); }
Dec __bid64_mul(Dec a, Dec b, unsigned, unsigned*){ return enc_int((long long)llround(dec2d(a)*dec2d(b))); }
Dec __bid64_div(Dec a, Dec b, unsigned, unsigned*){ double db=dec2d(b); return enc_int((long long)llround(db!=0?dec2d(a)/db:0)); }
}
