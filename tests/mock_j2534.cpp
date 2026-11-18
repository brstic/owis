// Mock J2534 PassThru DLL (op20pt32.dll): the simulated car of src/sim.h behind the real J2534 API, so
// PorscheTool.exe can be tested end-to-end without a car or an adapter (e.g. under Wine):
//   i686-w64-mingw32-g++ -shared -static -O2 -Isrc tests/mock_j2534.cpp -o op20pt32.dll -Wl,--kill-at
// Put op20pt32.dll next to PorscheTool.exe (on a PC without a registered J2534 driver).
// Channel 1 = ISO 15765 (CAN), channel 2 = ISO 14230 (K-line, fast and 5-baud init). See sim.h for the units.
#include <windows.h>
#include "sim.h"

typedef struct {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[4128];
} PASSTHRU_MSG;
typedef struct { unsigned long NumOfBytes; unsigned char *BytePtr; } SBYTE_ARRAY;

static sim::Car g_car;
static std::deque<PASSTHRU_MSG> g_rx[3];           // per channel
static unsigned long g_flt = 0;

static void collect(int chan, unsigned long kAddr) {
    while (!g_car.rx.empty()) {
        auto r = g_car.rx.front(); g_car.rx.pop_front();
        PASSTHRU_MSG m = {0}; const std::vector<uint8_t> &p = r.second;
        if (chan == 1) {
            m.ProtocolID = 6;
            m.Data[0] = (r.first >> 24) & 0xFF; m.Data[1] = (r.first >> 16) & 0xFF; m.Data[2] = (r.first >> 8) & 0xFF; m.Data[3] = r.first & 0xFF;
            memcpy(m.Data + 4, p.data(), p.size()); m.DataSize = 4 + (unsigned long)p.size();
        } else {                                    // K-line frame: fmt, target (tester), source (ECU), data, checksum
            m.ProtocolID = 4; size_t h = 0; unsigned char cs = 0;
            if (p.size() < 64) { m.Data[h++] = (unsigned char)(0x80 | p.size()); m.Data[h++] = 0xF1; m.Data[h++] = (unsigned char)kAddr; }
            else { m.Data[h++] = 0x80; m.Data[h++] = 0xF1; m.Data[h++] = (unsigned char)kAddr; m.Data[h++] = (unsigned char)p.size(); }
            memcpy(m.Data + h, p.data(), p.size()); h += p.size();
            for (size_t i = 0; i < h; i++) cs = (unsigned char)(cs + m.Data[i]);
            m.Data[h] = cs; m.DataSize = (unsigned long)h + 1; m.ExtraDataIndex = (unsigned long)h;
        }
        g_rx[chan].push_back(m);
    }
}

extern "C" {
__declspec(dllexport) long __stdcall PassThruOpen(const void*, unsigned long *d) { *d = 1; return 0; }
__declspec(dllexport) long __stdcall PassThruClose(unsigned long) { return 0; }
__declspec(dllexport) long __stdcall PassThruConnect(unsigned long, unsigned long proto, unsigned long, unsigned long, unsigned long *c) {
    if (proto == 6) { *c = 1; return 0; }
    if (proto == 4) { *c = 2; return 0; }
    return 1;                                       // ERR_NOT_SUPPORTED
}
__declspec(dllexport) long __stdcall PassThruDisconnect(unsigned long c) { if (c < 3) g_rx[c].clear(); return 0; }
__declspec(dllexport) long __stdcall PassThruStartMsgFilter(unsigned long, unsigned long, const PASSTHRU_MSG*, const PASSTHRU_MSG*,
                                                           const PASSTHRU_MSG*, unsigned long *f) { *f = ++g_flt; return 0; }
__declspec(dllexport) long __stdcall PassThruStopMsgFilter(unsigned long, unsigned long) { return 0; }
__declspec(dllexport) long __stdcall PassThruIoctl(unsigned long c, unsigned long code, const void *in, void *out) {
    if (code == 8) { if (c < 3) g_rx[c].clear(); return 0; }                                    // CLEAR_RX_BUFFER
    if (code == 5 && c == 2 && in) {                                                              // FAST_INIT
        const PASSTHRU_MSG *q = (const PASSTHRU_MSG*)in; unsigned long addr = q->Data[1];
        if (q->DataSize < 4 || !g_car.hasKline(addr)) { Sleep(60); return 9; }                   // ERR_TIMEOUT
        if (out) { PASSTHRU_MSG *r = (PASSTHRU_MSG*)out; memset(r, 0, sizeof *r); r->ProtocolID = 4;
            unsigned char d[] = {0x83, 0xF1, (unsigned char)addr, 0xC1, 0xEF, 0x8F}; memcpy(r->Data, d, 6); r->DataSize = 6; }
        return 0;
    }
    if (code == 4 && c == 2 && in) {                                                              // FIVE_BAUD_INIT
        const SBYTE_ARRAY *a = (const SBYTE_ARRAY*)in;
        if (!a->NumOfBytes || !g_car.hasKline(a->BytePtr[0])) { Sleep(300); return 9; }
        if (out) { SBYTE_ARRAY *k = (SBYTE_ARRAY*)out; if (k->NumOfBytes >= 2) { k->BytePtr[0] = 0xEF; k->BytePtr[1] = 0x8F; k->NumOfBytes = 2; } }
        return 0;
    }
    return 0;
}
__declspec(dllexport) long __stdcall PassThruWriteMsgs(unsigned long c, const PASSTHRU_MSG *m, unsigned long *num, unsigned long) {
    for (unsigned long i = 0; i < *num; i++) {
        if (c == 1 && m[i].DataSize > 4) {
            unsigned long id = ((unsigned long)m[i].Data[0] << 24) | (m[i].Data[1] << 16) | (m[i].Data[2] << 8) | m[i].Data[3];
            g_car.handle(id, m[i].Data + 4, m[i].DataSize - 4); collect(1, 0);
        } else if (c == 2 && m[i].DataSize >= 4) {
            unsigned long addr = m[i].Data[1]; size_t h = 3, len = m[i].Data[0] & 0x3F;
            if (len == 0) len = m[i].Data[h++];
            if (h + len <= m[i].DataSize) { g_car.handle(sim::klineKey(addr), m[i].Data + h, len); collect(2, addr); }
        }
    }
    return 0;
}
__declspec(dllexport) long __stdcall PassThruReadMsgs(unsigned long c, PASSTHRU_MSG *m, unsigned long *num, unsigned long tmo) {
    if (c >= 3 || g_rx[c].empty()) { Sleep(tmo < 20 ? tmo : 20); *num = 0; return 0x10; }       // ERR_BUFFER_EMPTY
    m[0] = g_rx[c].front(); g_rx[c].pop_front(); *num = 1; return 0;
}
}
