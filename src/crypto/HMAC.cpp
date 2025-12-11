#include "HMAC.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace Omega {

// SHA-256 constants
static const uint32_t K[64] = {
  0x428a2f98ul,0x71374491ul,0xb5c0fbcful,0xe9b5dba5ul,0x3956c25bul,0x59f111f1ul,0x923f82a4ul,0xab1c5ed5ul,
  0xd807aa98ul,0x12835b01ul,0x243185beul,0x550c7dc3ul,0x72be5d74ul,0x80deb1feul,0x9bdc06a7ul,0xc19bf174ul,
  0xe49b69c1ul,0xefbe4786ul,0x0fc19dc6ul,0x240ca1ccul,0x2de92c6ful,0x4a7484aaul,0x5cb0a9dcul,0x76f988daul,
  0x983e5152ul,0xa831c66dul,0xb00327c8ul,0xbf597fc7ul,0xc6e00bf3ul,0xd5a79147ul,0x06ca6351ul,0x14292967ul,
  0x27b70a85ul,0x2e1b2138ul,0x4d2c6dfcul,0x53380d13ul,0x650a7354ul,0x766a0abbul,0x81c2c92eul,0x92722c85ul,
  0xa2bfe8a1ul,0xa81a664bul,0xc24b8b70ul,0xc76c51a3ul,0xd192e819ul,0xd6990624ul,0xf40e3585ul,0x106aa070ul,
  0x19a4c116ul,0x1e376c08ul,0x2748774cul,0x34b0bcb5ul,0x391c0cb3ul,0x4ed8aa4aul,0x5b9cca4ful,0x682e6ff3ul,
  0x748f82eeul,0x78a5636ful,0x84c87814ul,0x8cc70208ul,0x90befffaul,0xa4506cebul,0xbef9a3f7ul,0xc67178f2ul
};

static inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32-n));
}

std::vector<uint8_t> HMAC::sha256(const std::vector<uint8_t>& data) {
    uint64_t bitLen = data.size() * 8;

    std::vector<uint8_t> msg = data;
    msg.push_back(0x80);
    while((msg.size() % 64) != 56)
        msg.push_back(0x00);

    for(int i=7;i>=0;i--)
        msg.push_back((bitLen >> (i*8)) & 0xFF);

    uint32_t H0=0x6a09e667ul, H1=0xbb67ae85ul, H2=0x3c6ef372ul, H3=0xa54ff53aul;
    uint32_t H4=0x510e527ful, H5=0x9b05688cul, H6=0x1f83d9abul, H7=0x5be0cd19ul;

    for(size_t c=0;c<msg.size();c+=64) {
        uint32_t w[64];

        for(int i=0;i<16;i++) {
            w[i]  = (uint32_t)msg[c + i*4 + 0] << 24;
            w[i] |= (uint32_t)msg[c + i*4 + 1] << 16;
            w[i] |= (uint32_t)msg[c + i*4 + 2] << 8;
            w[i] |= (uint32_t)msg[c + i*4 + 3];
        }
        for(int i=16;i<64;i++) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a=H0,b=H1,c1=H2,d=H3,e=H4,f=H5,g=H6,h=H7;

        for(int i=0;i<64;i++) {
            uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch = (e&f) ^ ((~e)&g);
            uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj = (a&b) ^ (a&c1) ^ (b&c1);
            uint32_t temp2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c1;
            c1 = b;
            b = a;
            a = temp1 + temp2;
        }

        H0+=a; H1+=b; H2+=c1; H3+=d;
        H4+=e; H5+=f; H6+=g; H7+=h;
    }

    std::vector<uint8_t> out(32);
    auto W = [&](uint32_t v,int i){
        out[i+0]=(v>>24)&0xFF;
        out[i+1]=(v>>16)&0xFF;
        out[i+2]=(v>>8)&0xFF;
        out[i+3]=v&0xFF;
    };
    W(H0,0); W(H1,4); W(H2,8); W(H3,12);
    W(H4,16);W(H5,20);W(H6,24);W(H7,28);
    return out;
}

std::string HMAC::hmac_sha256(const std::string& key,const std::string& msg) {
    std::vector<uint8_t> k(key.begin(), key.end());
    if(k.size()>64) k=sha256(k);
    k.resize(64,0);

    std::vector<uint8_t> oKey(64), iKey(64);
    for(int i=0;i<64;i++){
        oKey[i]=k[i]^0x5c;
        iKey[i]=k[i]^0x36;
    }

    std::vector<uint8_t> inner = iKey;
    inner.insert(inner.end(), msg.begin(), msg.end());
    auto innerHash = sha256(inner);

    std::vector<uint8_t> outer = oKey;
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    auto outHash = sha256(outer);

    std::ostringstream o;
    for(auto c:outHash)
        o<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)c;
    return o.str();
}

}
