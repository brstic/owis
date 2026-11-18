// porsche-tool - protocol-independent core (no Windows, no J2534): response parsing,
// fault-code formatting and catalogue lookup.  Unit-tested on any OS by tests/test_core.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

enum Proto { PROTO_AUTO = 0, PROTO_UDS = 1, PROTO_KWP = 2 };

struct Module {          // one diagnostic address (variants of the same ECU are merged)
    int key; const char *name;
    uint16_t req, resp;   // primary CAN pair (0 = not reachable directly on the diagnostic CAN)
    uint16_t req2, resp2; // alternative pair (some variants), 0 = none
    int proto;
};
struct DtcText { int module; int bits; uint32_t code; const char *en; const char *de; };

struct Dtc {             // one fault as reported by the control unit
    uint32_t code;        // UDS: 24 bit, KWP: 16 bit
    int bits;             // 24 or 16
    uint8_t status;
};

// ---------------------------------------------------------------- parsing
// UDS 19 02 response: 59 02 <availMask> { DTC_hi DTC_mid DTC_lo status } ...
static inline bool parseUds1902(const std::vector<uint8_t> &r, std::vector<Dtc> &out) {
    out.clear();
    if (r.size() < 3 || r[0] != 0x59 || r[1] != 0x02) return false;
    for (size_t i = 3; i + 4 <= r.size(); i += 4)
        out.push_back({(uint32_t)((r[i] << 16) | (r[i+1] << 8) | r[i+2]), 24, r[i+3]});
    return true;
}
// KWP2000 18 xx response: 58 <count> { DTC_hi DTC_lo status } ...
static inline bool parseKwp18(const std::vector<uint8_t> &r, std::vector<Dtc> &out) {
    out.clear();
    if (r.size() < 2 || r[0] != 0x58) return false;
    for (size_t i = 2; i + 3 <= r.size(); i += 3)
        out.push_back({(uint32_t)((r[i] << 8) | r[i+1]), 16, r[i+2]});
    return true;
}

// ---------------------------------------------------------------- formatting
// 16-bit SAE code -> "P1872" (top two bits select P/C/B/U)
static inline std::string sae16(uint16_t c) {
    char b[8]; snprintf(b, sizeof b, "%c%04X", "PCBU"[c >> 14], c & 0x3FFF); return b;
}
static inline std::string dtcName(const Dtc &d) {
    char b[24];
    if (d.bits == 16) return sae16((uint16_t)d.code);
    snprintf(b, sizeof b, "%06X", d.code & 0xFFFFFF);
    return b;
}
// UDS status byte (ISO 14229 DTCStatusMask)
static inline std::string udsStatus(uint8_t s) {
    std::string t;
    if (s & 0x01) t += "active now";
    else if (s & 0x08) t += "stored";
    else if (s & 0x04) t += "pending";
    else t += "not active";
    if (s & 0x80) t += ", warning lamp";
    return t;
}
// KWP2000 (VW/Porsche style) status byte: bit 5/6 = present/stored, bit 7 = warning lamp
static inline std::string kwpStatus(uint8_t s) {
    std::string t = (s & 0x60) == 0x20 ? "stored" : (s & 0x40) ? "active now" : "stored";
    if (s & 0x80) t += ", warning lamp";
    return t;
}
static inline std::string statusText(const Dtc &d) { return d.bits == 24 ? udsStatus(d.status) : kwpStatus(d.status); }

// Fault text lookup.  KWP codes are matched on 16 bit; UDS codes on 24 bit, falling back to the
// SAE part (top 16 bit) for ECUs that report "P-code + failure type byte".
static inline const DtcText *findText(const DtcText *tab, int module, const Dtc &d) {
    const DtcText *alt = nullptr;
    for (const DtcText *t = tab; t->module >= 0; t++) {
        if (t->module != module) continue;
        if (t->bits == d.bits && t->code == d.code) return t;
        if (d.bits == 24 && t->bits == 16 && t->code == (d.code >> 8)) alt = t;
    }
    return alt;
}

static inline std::string printable(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t a = s.find_first_not_of(' '); return a == std::string::npos ? "" : s.substr(a);
}
