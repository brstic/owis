// Tests the data-pack engine against a real generated pack: request building and response decoding.
//   c++ -std=c++17 -Isrc tests/test_pack.cpp -o t && ./t data/G1_de.json
#include "pack.h"
#include <fstream>
#include <sstream>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
using namespace pk;
static const Module *mod(const Pack &p, const char *k) { for (auto &m : p.mods) if (m.key == k) return &m; return nullptr; }
static const Service *svc(const Pack &p, const Module *m, const char *name) {
    for (auto &s : m->svc) if (p.S(s.name).find(name) != std::string::npos) return &s; return nullptr; }
static std::string hexs(const std::vector<uint8_t> &v) { std::string s; char b[4]; for (auto c : v) { snprintf(b, 4, "%02X ", c); s += b; } return s; }
static void show(const std::vector<Value> &vs) { for (auto &v : vs) printf("      %*s%s = %s %s\n", v.depth * 2, "", v.name.c_str(), v.text.c_str(), v.unit.c_str()); }

int main(int argc, char **argv) {
    std::ifstream f(argc > 1 ? argv[1] : "data/G1_de.json"); std::stringstream ss; ss << f.rdbuf();
    Pack p; std::string err;
    CHECK(p.load(ss.str(), err)); if (!err.empty()) printf("%s\n", err.c_str());
    printf("pack %s: %zu modules, %zu dops, %zu strings\n", p.platform.c_str(), p.mods.size(), p.dops.size(), p.str.size());
    const Module *pasm = mod(p, "PASM"); CHECK(pasm);
    if (!pasm) return 1;
    std::vector<uint8_t> rq; std::vector<Value> vs;
    // 1. ride height front left: 22 00 A0 -> 62 00 A0 01 F4 = 500 mm
    const Service *h = svc(p, pasm, "Kalibrierung H\xC3\xB6he vorne links Read"); CHECK(h);
    if (h) { CHECK(p.buildRequest(*h, {}, rq, err)); printf("height read req: %s\n", hexs(rq).c_str());
        CHECK(rq == std::vector<uint8_t>({0x22, 0x00, 0xA0}));
        CHECK(p.decode(*h, {0x62, 0x00, 0xA0, 0x01, 0xF4}, vs, err)); show(vs);
        CHECK(vs.size() == 1 && vs[0].num == 500 && vs[0].unit == "mm"); }
    // 2. level the car: 31 01 F1 05 <level>, input = texttable key 4 (Normalniveau)
    const Service *lv = svc(p, pasm, "Fahrzeug ausregeln auf Niveau, KDI Start"); CHECK(lv);
    if (lv) { std::vector<std::pair<int,int>> in; p.inputsOf(*lv, in); CHECK(in.size() == 1);
        if (!in.empty()) { const Dop &d = p.dops[in[0].second]; printf("input '%s' options:", p.S(in[0].first).c_str());
            for (auto &s : d.sc) printf(" [%g=%s]", s.lo, p.S(s.text).c_str()); printf("\n"); }
        CHECK(p.buildRequest(*lv, {4}, rq, err)); printf("level req: %s\n", hexs(rq).c_str());
        CHECK(rq == std::vector<uint8_t>({0x31, 0x01, 0xF1, 0x05, 0x04})); }
    // 3. routine result text
    const Service *rr = svc(p, pasm, "Fahrzeug ausregeln auf Niveau, KDI Request Routine Results"); CHECK(rr);
    if (rr) { CHECK(p.decode(*rr, {0x71, 0x03, 0xF1, 0x05, 0x01}, vs, err)); show(vs); CHECK(!vs.empty() && vs[0].text == "Function active");
              CHECK(!p.decode(*rr, {0x71, 0x03, 0xF1, 0x06, 0x01}, vs, err)); }
    // 4. unlearn counter 22 04 A0
    const Service *vz = svc(p, pasm, "Verlernz"); CHECK(vz);
    if (vz) { CHECK(p.decode(*vz, {0x62, 0x04, 0xA0, 0x07}, vs, err)); show(vs); CHECK(!vs.empty() && vs[0].num == 7); }
    // 5. sweep: every service's request can be built (with zero inputs) and decodes a zero-filled answer
    int nsvc = 0, built = 0, dec = 0, vals = 0;
    for (auto &m : p.mods) for (auto &s : m.svc) {
        nsvc++; std::vector<std::pair<int,int>> in; p.inputsOf(s, in);
        std::vector<double> z(in.size(), 0);
        if (p.buildRequest(s, z, rq, err)) built++;
        std::vector<uint8_t> resp(64, 0); bool ok = true;
        for (auto &q : s.rs) if (q.hasConst) { std::vector<uint8_t> tmp; (void)tmp; size_t n = (q.bit + (q.bits?q.bits:8) + 7) / 8;
            uint64_t v = (uint64_t)q.cval; for (size_t k = 0; k < n && q.byte + k < resp.size(); k++) resp[q.byte + n - 1 - k] = (uint8_t)(v >> (8 * k)); }
        if (p.decode(s, resp, vs, err)) { dec++; vals += (int)vs.size(); } else ok = false;
        (void)ok;
    }
    printf("sweep: services %d, requests built %d, responses decoded %d, values %d\n", nsvc, built, dec, vals);
    CHECK(built > nsvc * 95 / 100 && dec > nsvc * 95 / 100);
    printf(fails ? "%d FAILURE(S)\n" : "ALL PACK TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
