// PorscheTool data pack: loading, request building and response decoding (ODX semantics).
// Pure C++ (no Windows) so it is unit-tested on any OS against real packs.
#pragma once
#include "json.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pk {

enum Base { B_INT = 0, B_UINT = 1, B_F32 = 2, B_F64 = 3, B_ASCII = 4, B_UTF8 = 5, B_UNI = 6, B_BYTES = 7 };
enum Cat { C_IDENT = 0, C_LINEAR = 1, C_SCALE = 2, C_TEXT = 3 };
enum Sess { S_DEFAULT = 1, S_EXTENDED = 2, S_EOL = 4, S_DEV = 8, S_PROG = 16 };

struct Scale { bool hasLo = false, hasHi = false; double lo = 0, hi = 0; bool lin = false; double n0 = 0, n1 = 1, den = 1; int text = -1; };
struct Field { int name; int byte, bit; int dop; bool hasConst; double cval; };
struct Dop {
    char t = 's';                     // 's' simple, 't' struct, 'f' repeated field
    int base = B_UINT, bits = 8, lentype = 0; bool msb = true; int cat = C_IDENT; std::vector<Scale> sc; int unit = -1;
    std::vector<uint8_t> mask;
    std::vector<Field> f; int item = -1;
};
struct Param { int name; int byte, bit, bits; bool hasConst; double cval; int dop; };
struct Service { int name; std::string cls, sem; int sess; std::vector<Param> rq, rs; };
struct Dtc { uint32_t code; int bits; int text; };
struct Module { std::string key; int name; std::vector<std::pair<int, int>> ids; int proto; std::vector<Service> svc; std::vector<Dtc> dtc; };

struct Value { std::string name, text, unit; double num = NAN; int depth = 0; };

class Pack {
public:
    std::string platform, title, lang;
    std::vector<std::string> str;
    std::vector<Dop> dops;
    std::vector<Module> mods;

    const std::string &S(int i) const { static std::string z; return i >= 0 && i < (int)str.size() ? str[i] : z; }

    bool load(const std::string &text, std::string &err) {
        JVal j; JParser jp;
        if (!jp.parse(text, j) || j.t != JVal::OBJ) { err = "not a valid data pack (JSON)"; return false; }
        if (j["v"].i() != 1) { err = "unsupported pack version"; return false; }
        platform = j["platform"].s; title = j["title"].s; lang = j["lang"].s;
        for (auto &s : j["str"].a) str.push_back(s.s);
        for (auto &d : j["dops"].a) dops.push_back(dopOf(d));
        for (auto &m : j["mods"].a) {
            Module mo; mo.key = m["k"].s; mo.name = m["n"].i(); mo.proto = m["p"].i();
            for (auto &p : m["ids"].a) mo.ids.push_back({p[0].i(), p[1].i()});
            for (auto &s : m["svc"].a) {
                Service sv; sv.name = s[0].i(); sv.cls = s[1].s; sv.sem = s[2].s; sv.sess = s[3].i();
                sv.rq = params(s[4]); sv.rs = params(s[5]); mo.svc.push_back(std::move(sv));
            }
            for (auto &d : m["dtc"].a) mo.dtc.push_back({(uint32_t)d[0].ll(), d[1].i(), d[2].i()});
            mods.push_back(std::move(mo));
        }
        return true;
    }

    // ------------------------------------------------------------------ request
    // Builds the request bytes. 'inputs' supplies physical values for the variable request fields in order
    // (texttable: the raw key is taken from the matching text, or the number itself).
    bool buildRequest(const Service &s, const std::vector<double> &inputs, std::vector<uint8_t> &out, std::string &err) const {
        out.clear(); size_t in = 0;
        for (const Param &p : s.rq) {
            if (p.hasConst) { putBits(out, p.byte, p.bit, p.bits ? p.bits : 8, (uint64_t)p.cval, true); continue; }
            if (p.dop >= 0) {
                if (!encode(p.dop, p.byte, inputs, in, out, err)) return false;
            }
        }
        return true;
    }
    // Variable request inputs (name, dop) in the order buildRequest consumes them.
    void inputsOf(const Service &s, std::vector<std::pair<int, int>> &out) const {
        out.clear();
        for (const Param &p : s.rq) if (!p.hasConst && p.dop >= 0) collectInputs(p.dop, p.name, out);
    }

    // ------------------------------------------------------------------ response
    // Decodes a positive response. Constant parameters (service id, identifier) are checked, not shown.
    bool decode(const Service &s, const std::vector<uint8_t> &r, std::vector<Value> &out, std::string &err) const {
        out.clear();
        for (const Param &p : s.rs) {
            if (p.hasConst) {
                uint64_t v; size_t used;
                if (!getBits(r, p.byte, p.bit, p.bits ? p.bits : 8, true, v, used)) { err = "response too short"; return false; }
                if ((double)v != p.cval) { err = "unexpected response (identifier mismatch)"; return false; }
                continue;
            }
            if (p.dop >= 0) decodeDop(p.dop, p.name, r, p.byte, p.bit, 0, out);
        }
        return true;
    }

    // value -> text (used by decodeDop and by the UI for inputs)
    std::string textOf(const Dop &d, double raw, double &phys, bool &isNum) const {
        isNum = false; phys = raw;
        if (d.cat == C_TEXT) {
            for (auto &s : d.sc) if (inRange(s, raw) && s.text >= 0) return S(s.text);
            char b[32]; snprintf(b, sizeof b, "%.0f", raw); return b;
        }
        if (d.cat == C_LINEAR || d.cat == C_SCALE) {
            for (auto &s : d.sc) {
                if (d.cat == C_SCALE && !inRange(s, raw)) continue;
                if (s.text >= 0 && !s.lin) return S(s.text);
                if (s.lin) { phys = (s.n0 + s.n1 * raw) / (s.den ? s.den : 1); break; }
            }
        }
        isNum = true; return fmtNum(phys);
    }
    static std::string fmtNum(double v) {
        char b[48];
        if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%.0f", v);
        else snprintf(b, sizeof b, "%.4g", v);
        return b;
    }

private:
    static bool inRange(const Scale &s, double raw) {
        if (s.hasLo && raw < s.lo) return false;
        if (s.hasHi && raw > s.hi) return false;
        return true;
    }
    Dop dopOf(const JVal &d) const {
        Dop o;
        if (d[0].s == "s") {
            o.t = 's'; o.base = d[1].i(); o.bits = d[2].i(); o.lentype = d[3].i(); o.msb = d[4].i() != 0; o.cat = d[5].i();
            for (auto &s : d[6].a) {
                Scale c;
                if (!s[0].isNull()) { c.hasLo = true; c.lo = s[0].n; }
                if (!s[1].isNull()) { c.hasHi = true; c.hi = s[1].n; }
                if (!s[3].isNull()) { c.lin = true; c.n0 = s[2].isNull() ? 0 : s[2].n; c.n1 = s[3].n; c.den = s[4].isNull() ? 1 : s[4].n; }
                c.text = s[5].i();
                o.sc.push_back(c);
            }
            o.unit = d[7].i();
            if (d[8].t == JVal::STR) for (size_t k = 0; k + 1 < d[8].s.size(); k += 2) o.mask.push_back((uint8_t)strtoul(d[8].s.substr(k, 2).c_str(), nullptr, 16));
        } else if (d[0].s == "t") {
            o.t = 't';
            for (auto &f : d[1].a) o.f.push_back({f[0].i(), f[1].i(), f[2].i(), f[3].i(), !f[4].isNull(), f[4].isNull() ? 0 : f[4].n});
        } else if (d[0].s == "f") { o.t = 'f'; o.item = d[1].i(); }
        return o;
    }
    std::vector<Param> params(const JVal &a) const {
        std::vector<Param> v;
        for (auto &p : a.a) v.push_back({p[0].i(), p[1].i(), p[2].i(), p[3].i(), !p[4].isNull(), p[4].isNull() ? 0 : p[4].n, p[5].i()});
        return v;
    }

    // ODX bit extraction: BYTE-POSITION + BIT-POSITION (from the least significant bit) + BIT-LENGTH.
    static bool getBits(const std::vector<uint8_t> &r, size_t byte, int bit, int bits, bool msb, uint64_t &v, size_t &used) {
        if (bits <= 0 || bits > 64) return false;
        size_t n = (size_t)(bit + bits + 7) / 8;
        if (n > 8 || byte + n > r.size()) return false;
        uint64_t acc = 0;
        for (size_t k = 0; k < n; k++) acc = (acc << 8) | r[byte + (msb ? k : n - 1 - k)];
        acc >>= bit;
        v = bits == 64 ? acc : (acc & ((1ULL << bits) - 1));
        used = n; return true;
    }
    static void putBits(std::vector<uint8_t> &o, size_t byte, int bit, int bits, uint64_t v, bool msb) {
        size_t n = (size_t)(bit + bits + 7) / 8;
        if (n > 8) n = 8;
        if (o.size() < byte + n) o.resize(byte + n, 0);
        uint64_t acc = 0;
        for (size_t k = 0; k < n; k++) acc = (acc << 8) | o[byte + (msb ? k : n - 1 - k)];
        uint64_t m = (bits >= 64 ? ~0ULL : ((1ULL << bits) - 1)) << bit;
        acc = (acc & ~m) | ((v << bit) & m);
        for (size_t k = 0; k < n; k++) o[byte + (msb ? n - 1 - k : k)] = (uint8_t)(acc >> (8 * k));
    }

    // byte length of a structure (for repeated fields)
    int sizeOf(int di, int depth = 0) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return 0;
        const Dop &d = dops[di];
        if (d.t == 's') return d.lentype == 0 ? (d.bits + 7) / 8 : 0;
        if (d.t == 't') { int m = 0; for (auto &f : d.f) { int e = f.byte + (f.dop >= 0 ? sizeOf(f.dop, depth + 1) : 1); if (e > m) m = e; } return m; }
        return 0;
    }

    void decodeDop(int di, int name, const std::vector<uint8_t> &r, size_t byte, int bit, int depth, std::vector<Value> &out) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return;
        const Dop &d = dops[di];
        if (d.t == 't') {
            if (depth > 0 && name >= 0 && d.f.size() > 1) { Value h; h.name = S(name); h.depth = depth; out.push_back(h); }
            for (auto &f : d.f) {
                if (f.hasConst || f.dop < 0) continue;
                decodeDop(f.dop, f.name, r, byte + f.byte, f.bit, depth + 1, out);
            }
            return;
        }
        if (d.t == 'f') {
            int sz = sizeOf(d.item); if (sz <= 0) return;
            for (size_t b = byte, k = 1; b + sz <= r.size() && k <= 255; b += sz, k++) {
                Value h; char t[24]; snprintf(t, sizeof t, " #%d", (int)k); h.name = S(name) + t; h.depth = depth; out.push_back(h);
                decodeDop(d.item, -1, r, b, 0, depth + 1, out);
            }
            return;
        }
        Value v; v.name = S(name); v.depth = depth; v.unit = S(d.unit);
        if (d.base == B_ASCII || d.base == B_UTF8 || d.base == B_UNI || d.base == B_BYTES) {
            size_t n = d.lentype == 0 ? (size_t)(d.bits / 8) : (byte < r.size() ? r.size() - byte : 0);
            if (byte + n > r.size()) { v.text = "(missing)"; out.push_back(v); return; }
            if (d.base == B_BYTES) { char b[4]; for (size_t k = 0; k < n; k++) { snprintf(b, sizeof b, "%02X", r[byte + k]); v.text += (k ? " " : ""); v.text += b; } }
            else { for (size_t k = 0; k < n; k++) { uint8_t c = r[byte + k]; if (c >= 0x20 && c < 0x7F) v.text += (char)c; else if (c) v.text += ' '; }
                   while (!v.text.empty() && v.text.back() == ' ') v.text.pop_back(); }
            out.push_back(v); return;
        }
        uint64_t raw; size_t used;
        if (!getBits(r, byte, bit, d.bits, d.msb, raw, used)) { v.text = "(missing)"; out.push_back(v); return; }
        if (!d.mask.empty()) { uint64_t m = 0; for (uint8_t b : d.mask) m = (m << 8) | b; raw &= m; }
        double x;
        if (d.base == B_INT && d.bits < 64 && (raw >> (d.bits - 1)) & 1) x = (double)((int64_t)raw - ((int64_t)1 << d.bits));
        else if (d.base == B_F32 && d.bits == 32) { uint32_t u = (uint32_t)raw; float f; memcpy(&f, &u, 4); x = f; }
        else if (d.base == B_F64 && d.bits == 64) { double f; memcpy(&f, &raw, 8); x = f; }
        else x = (double)raw;
        bool isNum; double phys;
        v.text = textOf(d, x, phys, isNum);
        if (isNum) v.num = phys; else v.unit.clear();
        out.push_back(v);
    }

    void collectInputs(int di, int name, std::vector<std::pair<int, int>> &out, int depth = 0) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return;
        const Dop &d = dops[di];
        if (d.t == 't') { for (auto &f : d.f) if (!f.hasConst && f.dop >= 0) collectInputs(f.dop, f.name, out, depth + 1); return; }
        if (d.t == 's') out.push_back({name, di});
    }
    bool encode(int di, size_t byte, const std::vector<double> &in, size_t &k, std::vector<uint8_t> &out, std::string &err, int depth = 0) const {
        const Dop &d = dops[di];
        if (d.t == 't') {
            for (auto &f : d.f) {
                if (f.hasConst) { const Dop *fd = f.dop >= 0 ? &dops[f.dop] : nullptr; putBits(out, byte + f.byte, f.bit, fd ? fd->bits : 8, (uint64_t)f.cval, true); continue; }
                if (f.dop >= 0 && !encode(f.dop, byte + f.byte, in, k, out, err, depth + 1)) return false;
            }
            return true;
        }
        if (d.t != 's') { err = "unsupported input type"; return false; }
        if (k >= in.size()) { err = "missing input value"; return false; }
        double phys = in[k++], raw = phys;
        if ((d.cat == C_LINEAR || d.cat == C_SCALE) && !d.sc.empty() && d.sc[0].lin && d.sc[0].n1 != 0)
            raw = (phys * d.sc[0].den - d.sc[0].n0) / d.sc[0].n1;
        long long iv = llround(raw);
        if (d.base == B_INT && iv < 0) iv += (1LL << d.bits);
        putBits(out, byte, 0, d.bits, (uint64_t)iv, d.msb);
        return true;
    }
};

} // namespace pk
