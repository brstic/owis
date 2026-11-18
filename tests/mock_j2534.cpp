// Mock J2534 PassThru DLL (op20pt32.dll) that simulates a few Porsche control units, so OpenDiag
// can be tested end-to-end without a car (e.g. under Wine):
//   i686-w64-mingw32-g++ -shared -static -O2 mock_j2534.cpp -o op20pt32.dll -Wl,--kill-at
// Put op20pt32.dll next to OpenDiag.exe (on a PC without a registered J2534 driver).
//
// Simulated units:
//   0x70F/0x779 PDK (KWP2000)   : P1872 stored; clear needs session 0x89
//   0x755/0x7BF PASM (UDS)      : 000110 active; F187 part number
//   0x715/0x77F Airbag (UDS)    : no faults
//   0x753/0x7BD Selector (KWP)  : refuses UDS 0x19 (NRC 0x11) -> tests the UDS->KWP fallback; U0155 stored
//   0x7E0/0x7E8 Engine (KWP)    : sends "response pending" (0x78) once before answering
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include <map>

typedef struct {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[4128];
} PASSTHRU_MSG;

struct Ecu { int proto; std::vector<std::vector<uint8_t>> faults; uint8_t session = 1; bool cleared = false; int pending = 0; };
static std::map<unsigned long, Ecu> g_ecus;          // keyed by request ID
static std::map<unsigned long, unsigned long> g_resp = {{0x70F,0x779},{0x755,0x7BF},{0x715,0x77F},{0x753,0x7BD},{0x7E0,0x7E8}};
static std::deque<PASSTHRU_MSG> g_rx;
static unsigned long g_flt = 0;

static void init() {
    if (!g_ecus.empty()) return;
    g_ecus[0x70F] = {2, {{0x18,0x72,0x20}}};
    g_ecus[0x755] = {1, {{0x00,0x01,0x10,0x09}}};
    g_ecus[0x715] = {1, {}};
    g_ecus[0x753] = {2, {{0xC1,0x55,0x20}}};
    g_ecus[0x7E0] = {2, {}}; g_ecus[0x7E0].pending = 1;
}
static void reply(unsigned long id, const std::vector<uint8_t> &p) {
    PASSTHRU_MSG m = {0}; m.ProtocolID = 6;
    m.Data[0] = 0; m.Data[1] = 0; m.Data[2] = (id >> 8) & 0xFF; m.Data[3] = id & 0xFF;
    memcpy(m.Data + 4, p.data(), p.size()); m.DataSize = 4 + p.size();
    g_rx.push_back(m);
}
static void handle(unsigned long tx, const uint8_t *q, size_t n) {
    init();
    auto it = g_ecus.find(tx); if (it == g_ecus.end()) return;          // module absent: silence
    Ecu &e = it->second; unsigned long rx = g_resp[tx];
    uint8_t sid = q[0];
    auto nrc = [&](uint8_t c) { reply(rx, {0x7F, sid, c}); };
    if (e.pending > 0 && sid == 0x18) { e.pending--; nrc(0x78); }
    if (sid == 0x10) { e.session = q[1]; reply(rx, {0x50, q[1]}); return; }
    if (e.proto == 1) {                                                   // UDS
        if (sid == 0x19 && q[1] == 0x02) { std::vector<uint8_t> r = {0x59,0x02,0xFF};
            if (!e.cleared) for (auto &f : e.faults) r.insert(r.end(), f.begin(), f.end()); reply(rx, r); return; }
        if (sid == 0x22) { const char *v = tx == 0x755 ? "97034301234" : "4H0959655"; std::vector<uint8_t> r = {0x62, q[1], q[2]};
            r.insert(r.end(), v, v + strlen(v)); reply(rx, r); return; }
        if (sid == 0x14) { e.cleared = true; reply(rx, {0x54}); return; }
        return nrc(0x11);
    }
    // KWP
    if (sid == 0x19) return nrc(0x11);
    if (sid == 0x18) { std::vector<uint8_t> r = {0x58, (uint8_t)(e.cleared ? 0 : e.faults.size())};
        if (!e.cleared) for (auto &f : e.faults) r.insert(r.end(), f.begin(), f.end()); reply(rx, r); return; }
    if (sid == 0x1A) { const char *v = "970 PDK 3000  G088"; std::vector<uint8_t> r = {0x5A, q[1]};
        r.insert(r.end(), v, v + strlen(v)); reply(rx, r); return; }
    if (sid == 0x14) { if (tx == 0x70F && e.session != 0x89) return nrc(0x22); e.cleared = true; reply(rx, {0x54, 0xFF, 0x00}); return; }
    nrc(0x11);
}

extern "C" {
__declspec(dllexport) long __stdcall PassThruOpen(const void*, unsigned long *d) { *d = 1; return 0; }
__declspec(dllexport) long __stdcall PassThruClose(unsigned long) { return 0; }
__declspec(dllexport) long __stdcall PassThruConnect(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *c) { *c = 1; return 0; }
__declspec(dllexport) long __stdcall PassThruDisconnect(unsigned long) { return 0; }
__declspec(dllexport) long __stdcall PassThruStartMsgFilter(unsigned long, unsigned long, const PASSTHRU_MSG*, const PASSTHRU_MSG*,
                                                           const PASSTHRU_MSG*, unsigned long *f) { *f = ++g_flt; return 0; }
__declspec(dllexport) long __stdcall PassThruStopMsgFilter(unsigned long, unsigned long) { return 0; }
__declspec(dllexport) long __stdcall PassThruIoctl(unsigned long, unsigned long code, const void*, void*) { if (code == 8) g_rx.clear(); return 0; }
__declspec(dllexport) long __stdcall PassThruWriteMsgs(unsigned long, const PASSTHRU_MSG *m, unsigned long *num, unsigned long) {
    for (unsigned long i = 0; i < *num; i++) {
        unsigned long id = (m[i].Data[2] << 8) | m[i].Data[3];
        if (m[i].DataSize > 4) handle(id, m[i].Data + 4, m[i].DataSize - 4);
    }
    return 0;
}
__declspec(dllexport) long __stdcall PassThruReadMsgs(unsigned long, PASSTHRU_MSG *m, unsigned long *num, unsigned long tmo) {
    if (g_rx.empty()) { Sleep(tmo < 20 ? tmo : 20); *num = 0; return 0x10; }   // ERR_BUFFER_EMPTY
    m[0] = g_rx.front(); g_rx.pop_front(); *num = 1; return 0;
}
}
