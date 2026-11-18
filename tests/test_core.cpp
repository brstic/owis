// Unit tests for the protocol-independent core + a sanity check of the generated vehicle table.
// Build & run on any OS:  c++ -std=c++17 -I../src test_core.cpp -o t && ./t
#include "diag_core.h"
#include "vehicle_g1.h"
#include <cassert>
#include <set>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    std::vector<Dtc> d;
    // UDS: 59 02 FF | C1 20 A2 09 | 00 01 10 08
    CHECK(parseUds1902({0x59,0x02,0xFF, 0xC1,0x20,0xA2,0x09, 0x00,0x01,0x10,0x08}, d));
    CHECK(d.size() == 2 && d[0].code == 0xC120A2 && d[0].status == 0x09 && d[1].code == 0x000110);
    CHECK(dtcName(d[0]) == "C120A2");
    CHECK(udsStatus(0x09) == "active now" && udsStatus(0x08) == "stored" && udsStatus(0x88) == "stored, warning lamp");
    CHECK(parseUds1902({0x59,0x02,0xFF}, d) && d.empty());                      // no faults
    CHECK(!parseUds1902({0x7F,0x19,0x11}, d));                                 // negative response
    CHECK(parseUds1902({0x59,0x02,0xFF, 0xC1,0x20}, d) && d.empty());           // truncated record ignored
    // KWP: 58 02 | 18 72 E0 | 18 70 20
    CHECK(parseKwp18({0x58,0x02, 0x18,0x72,0xE0, 0x18,0x70,0x20}, d));
    CHECK(d.size() == 2 && d[0].code == 0x1872 && d[0].bits == 16 && dtcName(d[0]) == "P1872");
    CHECK(sae16(0xC155) == "U0155" && sae16(0x4123) == "C0123" && sae16(0x8001) == "B0001");
    CHECK(printable((const uint8_t*)"  970618\x01 ", 10) == "970618");

    // Generated table: every module key is unique, CAN pairs distinct, lookups work.
    using namespace veh_G1;
    std::set<int> keys; std::set<uint16_t> reqs; int n = 0;
    for (const Module *m = MODULES; m->key >= 0; m++, n++) {
        CHECK(keys.insert(m->key).second);
        if (m->req) { CHECK(reqs.insert(m->req).second); CHECK(m->resp > m->req); }
    }
    CHECK(n >= 30);
    Dtc p1872{0x1872, 16, 0x20};
    const DtcText *t = findText(DTCS, M_PDK, p1872);
    CHECK(t && t->en && t->en[0]);
    if (t) printf("PDK P1872 -> %s\n", t->en);
    Dtc pasm{0x000110, 24, 0x09};
    t = findText(DTCS, M_PASM, pasm);
    CHECK(t && t->en[0]);
    if (t) printf("PASM 000110 -> %s\n", t->en);
    // German must not be embedded in the public build
    for (const DtcText *x = DTCS; x->module >= 0; x++) if (x->de && x->de[0]) { CHECK(!"de text in public header"); break; }
    int empty = 0; for (const DtcText *x = DTCS; x->module >= 0; x++) if (!x->en[0]) empty++;
    printf("modules %d, codes with empty English text: %d\n", n, empty);
    printf(fails ? "%d FAILURE(S)\n" : "ALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
