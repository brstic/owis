// J2534 PassThru transport (Windows): ISO 15765 on the diagnostic CAN (11- and 29-bit) and ISO 14230
// (KWP2000) on the K-line for the older cars. Works with any J2534-1 04.04 adapter; tested target is
// the Tactrix OpenPort 2.0. 32-bit on purpose - the adapter drivers are 32-bit DLLs.
#pragma once
#ifdef _WIN32
#include "bus.h"
#include "tp20.h"
#include <windows.h>
#include <cstring>

namespace pt {

#define PASSTHRU_MSG_DATA_SIZE 4128
typedef struct {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[PASSTHRU_MSG_DATA_SIZE];
} PASSTHRU_MSG;
typedef struct { unsigned long Parameter, Value; } SCONFIG;
typedef struct { unsigned long NumOfParams; SCONFIG *ConfigPtr; } SCONFIG_LIST;
typedef struct { unsigned long NumOfBytes; unsigned char *BytePtr; } SBYTE_ARRAY;
enum { J_CAN = 5, J_ISO14230 = 4, J_ISO15765 = 6, J_STATUS_NOERROR = 0, J_PASS_FILTER = 1, J_FLOW_CONTROL_FILTER = 3,
       J_ISO15765_FRAME_PAD = 0x40, J_CAN_29BIT_ID = 0x100, J_CAN_ID_BOTH = 0x800, J_K_LINE_ONLY = 0x1000,
       J_TX_MSG_TYPE = 0x01, J_START_OF_MESSAGE = 0x02,
       J_SET_CONFIG = 0x02, J_FIVE_BAUD_INIT = 0x04, J_FAST_INIT = 0x05, J_CLEAR_RX_BUFFER = 0x08,
       J_P_DATA_RATE = 0x01, J_P_LOOPBACK = 0x03, J_P_P1_MAX = 0x07, J_P_P3_MIN = 0x0A, J_P_P4_MIN = 0x0C,
       J_P_TIDLE = 0x13, J_P_TINIL = 0x14, J_P_TWUP = 0x15, J_P_PARITY = 0x16 };
typedef long (__stdcall *PF_Open)(const void*, unsigned long*);
typedef long (__stdcall *PF_Close)(unsigned long);
typedef long (__stdcall *PF_Connect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*);
typedef long (__stdcall *PF_Disconnect)(unsigned long);
typedef long (__stdcall *PF_ReadMsgs)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *PF_WriteMsgs)(unsigned long, const PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *PF_StartMsgFilter)(unsigned long, unsigned long, const PASSTHRU_MSG*,
                                  const PASSTHRU_MSG*, const PASSTHRU_MSG*, unsigned long*);
typedef long (__stdcall *PF_StopMsgFilter)(unsigned long, unsigned long);
typedef long (__stdcall *PF_Ioctl)(unsigned long, unsigned long, const void*, void*);

struct J2534Api {
    HMODULE dll = nullptr;
    PF_Open open = nullptr; PF_Close close = nullptr; PF_Connect connect = nullptr; PF_Disconnect disconnect = nullptr;
    PF_ReadMsgs readMsgs = nullptr; PF_WriteMsgs writeMsgs = nullptr; PF_StartMsgFilter startFilter = nullptr;
    PF_StopMsgFilter stopFilter = nullptr; PF_Ioctl ioctl = nullptr;

    static std::string findLibrary() {
        HKEY hk;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\PassThruSupport.04.04", 0, KEY_READ | KEY_WOW64_32KEY, &hk) != ERROR_SUCCESS) return "";
        char name[256]; DWORD idx = 0; std::string best, firstAny;
        for (;;) { DWORD nlen = sizeof(name);
            if (RegEnumKeyExA(hk, idx++, name, &nlen, 0, 0, 0, 0) != ERROR_SUCCESS) break;
            HKEY sub; if (RegOpenKeyExA(hk, name, 0, KEY_READ | KEY_WOW64_32KEY, &sub) != ERROR_SUCCESS) continue;
            char lib[512]; DWORD lsz = sizeof(lib), type = 0;
            if (RegQueryValueExA(sub, "FunctionLibrary", 0, &type, (LPBYTE)lib, &lsz) == ERROR_SUCCESS) {
                std::string path(lib); if (firstAny.empty()) firstAny = path;
                std::string low = path; for (char &ch : low) ch = (char)tolower((unsigned char)ch);
                if (low.find("op20") != std::string::npos || low.find("tactrix") != std::string::npos ||
                    low.find("openport") != std::string::npos) best = path;
            }
            RegCloseKey(sub);
        }
        RegCloseKey(hk);
        return best.empty() ? firstAny : best;
    }
    bool loadDll() {
        if (dll) return true;
        std::string p = findLibrary();
        if (!p.empty()) dll = LoadLibraryA(p.c_str());
        if (!dll) dll = LoadLibraryA("op20pt32.dll");
        return dll != nullptr;
    }
    bool load() {
        if (open) return true;
        if (!loadDll()) return false;
        open = (PF_Open)GetProcAddress(dll, "PassThruOpen");
        close = (PF_Close)GetProcAddress(dll, "PassThruClose");
        connect = (PF_Connect)GetProcAddress(dll, "PassThruConnect");
        disconnect = (PF_Disconnect)GetProcAddress(dll, "PassThruDisconnect");
        readMsgs = (PF_ReadMsgs)GetProcAddress(dll, "PassThruReadMsgs");
        writeMsgs = (PF_WriteMsgs)GetProcAddress(dll, "PassThruWriteMsgs");
        startFilter = (PF_StartMsgFilter)GetProcAddress(dll, "PassThruStartMsgFilter");
        stopFilter = (PF_StopMsgFilter)GetProcAddress(dll, "PassThruStopMsgFilter");
        ioctl = (PF_Ioctl)GetProcAddress(dll, "PassThruIoctl");
        if (!open || !connect || !readMsgs || !writeMsgs || !startFilter || !stopFilter) { open = nullptr; return false; }
        return true;
    }
    // 1 = adapter connected, 0 = driver present but no device, -1 = no J2534 driver
    int detect() {
        if (!loadDll()) return -1;
        PF_Open o = (PF_Open)GetProcAddress(dll, "PassThruOpen"); PF_Close c = (PF_Close)GetProcAddress(dll, "PassThruClose");
        if (!o || !c) return -1;
        unsigned long d = 0;
        if (o(nullptr, &d) == J_STATUS_NOERROR) { c(d); return 1; }
        return 0;
    }
};
inline J2534Api &j2534() { static J2534Api a; return a; }

class J2534Bus : public Bus, public RawCan {
    unsigned long dev = 0, chan = 0, filt = 0, kchan = 0, kfilt = 0, cchan = 0, cfilt = 0;
    bool hasFilt = false, connected = false, kOpen = false, kUp = false, cOpen = false;
    Tp20 tp{*this};
    unsigned long kBaud = 0;
    static void putId(PASSTHRU_MSG &m, unsigned long id) {
        m.Data[0] = (id >> 24) & 0xFF; m.Data[1] = (id >> 16) & 0xFF; m.Data[2] = (id >> 8) & 0xFF; m.Data[3] = id & 0xFF;
    }
    bool ext() const { return txId > 0x7FF || rxId > 0x7FF; }
public:
    // ---- raw CAN (for VW TP 2.0). Adapters that allow only one protocol at a time get the ISO 15765
    //      channel closed while TP 2.0 is in use and reopened afterwards.
    bool rawOpen() override {
        J2534Api &a = j2534();
        if (cOpen) return true;
        if (a.connect(dev, J_CAN, 0, 500000, &cchan) != J_STATUS_NOERROR) {
            if (connected) { if (hasFilt) a.stopFilter(chan, filt); hasFilt = false; a.disconnect(chan); connected = false; }
            if (a.connect(dev, J_CAN, 0, 500000, &cchan) != J_STATUS_NOERROR) { error("[ERROR] the adapter cannot open raw CAN for VW TP 2.0."); return false; }
        }
        cOpen = true;
        PASSTHRU_MSG mask = {0}, patt = {0};
        mask.ProtocolID = patt.ProtocolID = J_CAN; mask.DataSize = patt.DataSize = 4;
        a.startFilter(cchan, J_PASS_FILTER, &mask, &patt, nullptr, &cfilt);     // all frames; Tp20 picks its ids
        return true;
    }
    void rawClose() override {
        J2534Api &a = j2534();
        if (cOpen) { a.stopFilter(cchan, cfilt); a.disconnect(cchan); cOpen = false; }
    }
    bool sendFrame(const CanFrame &f) override {
        J2534Api &a = j2534(); PASSTHRU_MSG m = {0}; unsigned long num = 1;
        m.ProtocolID = J_CAN; putId(m, f.id);
        for (size_t i = 0; i < f.d.size() && i < 8; i++) m.Data[4 + i] = f.d[i];
        m.DataSize = 4 + (unsigned long)std::min<size_t>(8, f.d.size());
        return a.writeMsgs(cchan, &m, &num, 500) == J_STATUS_NOERROR;
    }
    bool recvFrame(CanFrame &f, unsigned timeoutMs) override {
        J2534Api &a = j2534(); uint32_t t0 = nowMs();
        do {
            PASSTHRU_MSG m = {0}; unsigned long num = 1;
            long st = a.readMsgs(cchan, &m, &num, 20);
            if (st != J_STATUS_NOERROR || num == 0 || m.DataSize < 4 || (m.RxStatus & J_TX_MSG_TYPE)) continue;
            f.id = ((unsigned long)m.Data[0] << 24) | (m.Data[1] << 16) | (m.Data[2] << 8) | m.Data[3];
            f.d.assign(m.Data + 4, m.Data + m.DataSize);
            return true;
        } while (nowMs() - t0 < timeoutMs);
        return false;
    }
    bool ensureIso() {
        J2534Api &a = j2534();
        if (connected) return true;
        rawClose();
        if (a.connect(dev, J_ISO15765, J_CAN_ID_BOTH, 500000, &chan) != J_STATUS_NOERROR &&
            a.connect(dev, J_ISO15765, 0, 500000, &chan) != J_STATUS_NOERROR) { error("[ERROR] PassThruConnect (CAN 500k) failed."); return false; }
        connected = true; return true;
    }

    bool open() override {
        J2534Api &a = j2534();
        if (!a.load()) { error("[ERROR] J2534 driver not found or incomplete. Install the adapter drivers (Tactrix: op20pt32.dll)."); return false; }
        if (a.open(nullptr, &dev) != J_STATUS_NOERROR) { error("[ERROR] PassThruOpen failed - adapter not connected?"); return false; }
        if (a.connect(dev, J_ISO15765, J_CAN_ID_BOTH, 500000, &chan) != J_STATUS_NOERROR &&
            a.connect(dev, J_ISO15765, 0, 500000, &chan) != J_STATUS_NOERROR) { error("[ERROR] PassThruConnect (CAN 500k) failed."); a.close(dev); dev = 0; return false; }
        connected = true; return true;
    }
    void close() override {
        J2534Api &a = j2534();
        if (tp.connected()) tp.close();
        rawClose();
        if (hasFilt && a.stopFilter) a.stopFilter(chan, filt);
        if (kOpen && a.disconnect) a.disconnect(kchan);
        if (connected && a.disconnect) a.disconnect(chan);
        if (dev && a.close) a.close(dev);
        connected = hasFilt = kOpen = kUp = false; dev = chan = kchan = 0;
    }
    // ---- K-line (ISO 14230): one channel, opened when the first K-line unit is addressed
    bool kOpenChannel() {
        J2534Api &a = j2534();
        if (kOpen && kBaud != (unsigned long)klBaud) { a.disconnect(kchan); kOpen = false; }
        if (kOpen) return true;
        if (a.connect(dev, J_ISO14230, J_K_LINE_ONLY, (unsigned long)klBaud, &kchan) != J_STATUS_NOERROR &&
            a.connect(dev, J_ISO14230, 0, (unsigned long)klBaud, &kchan) != J_STATUS_NOERROR) { error("[ERROR] the adapter cannot open the K-line (ISO 14230)."); return false; }
        kOpen = true; kBaud = (unsigned long)klBaud;
        if (a.ioctl) {
            SCONFIG c[] = {{J_P_LOOPBACK, 0}, {J_P_P1_MAX, 40}, {J_P_P3_MIN, 110}, {J_P_P4_MIN, 10}, {J_P_TIDLE, 300}, {J_P_TINIL, 25}, {J_P_TWUP, 50}, {J_P_PARITY, 0}};
            for (auto &x : c) { SCONFIG_LIST l = {1, &x}; a.ioctl(kchan, J_SET_CONFIG, &l, nullptr); }     // one by one: unknown ones are just refused
        }
        PASSTHRU_MSG mask = {0}, patt = {0};
        mask.ProtocolID = patt.ProtocolID = J_ISO14230; mask.DataSize = patt.DataSize = 1;                    // pass everything
        a.startFilter(kchan, J_PASS_FILTER, &mask, &patt, nullptr, &kfilt);
        return true;
    }
    bool kInit() {
        J2534Api &a = j2534(); kUp = false;
        if (!a.ioctl) return false;
        a.ioctl(kchan, J_CLEAR_RX_BUFFER, nullptr, nullptr);
        auto fastInit = [&]() -> bool {
            PASSTHRU_MSG in = {0}, out = {0}; in.ProtocolID = out.ProtocolID = J_ISO14230;
            in.Data[0] = 0x81; in.Data[1] = (unsigned char)txId; in.Data[2] = (unsigned char)rxId; in.Data[3] = 0x81; in.DataSize = 4;
            return a.ioctl(kchan, J_FAST_INIT, &in, &out) == J_STATUS_NOERROR;
        };
        auto fiveBaud = [&]() -> bool {
            unsigned char addr = (unsigned char)(kl5Baud ? kl5Baud : txId), kw[8] = {0};
            SBYTE_ARRAY in = {1, &addr}, out = {2, kw};
            return a.ioctl(kchan, J_FIVE_BAUD_INIT, &in, &out) == J_STATUS_NOERROR;
        };
        kUp = klInit == 1 ? (fiveBaud() || fastInit()) : (fastInit() || (kl5Baud && fiveBaud()));
        return kUp;
    }
    bool target(uint32_t tx, uint32_t rx, int linkKind = LINK_CAN) override {
        J2534Api &a = j2534();
        if (tp.connected() && (linkKind != LINK_TP20 || tx != tp.address())) tp.close();
        Bus::target(tx, rx, linkKind);
        if (linkKind == LINK_KLINE) { if (!kOpenChannel()) return false; kInit(); return true; }
        if (linkKind == LINK_TP20) { if (!rawOpen()) return false; if (!tp.connected()) tp.open((uint8_t)tx); return true; }
        if (!ensureIso()) return false;
        if (hasFilt) { a.stopFilter(chan, filt); hasFilt = false; }
        PASSTHRU_MSG mask = {0}, patt = {0}, flow = {0};
        mask.ProtocolID = patt.ProtocolID = flow.ProtocolID = J_ISO15765;
        mask.TxFlags = patt.TxFlags = flow.TxFlags = J_ISO15765_FRAME_PAD | (ext() ? J_CAN_29BIT_ID : 0);
        mask.DataSize = patt.DataSize = flow.DataSize = 4;
        putId(mask, ext() ? 0x1FFFFFFF : 0x7FF); putId(patt, rx); putId(flow, tx);
        if (a.startFilter(chan, J_FLOW_CONTROL_FILTER, &mask, &patt, &flow, &filt) != J_STATUS_NOERROR) { error("[ERROR] StartMsgFilter failed."); return false; }
        hasFilt = true;
        if (a.ioctl) a.ioctl(chan, J_CLEAR_RX_BUFFER, nullptr, nullptr);
        return true;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        J2534Api &a = j2534(); PASSTHRU_MSG m = {0}; unsigned long num = 1;
        if (kind == LINK_TP20) { tp.keepAlive(); return tp.sendMessage(p); }
        if (kind == LINK_KLINE) {
            if (!kUp || p.size() > 255) return false;
            m.ProtocolID = J_ISO14230; size_t h = 0;
            if (p.size() < 64) { m.Data[h++] = (unsigned char)(0x80 | p.size()); m.Data[h++] = (unsigned char)txId; m.Data[h++] = (unsigned char)rxId; }
            else { m.Data[h++] = 0x80; m.Data[h++] = (unsigned char)txId; m.Data[h++] = (unsigned char)rxId; m.Data[h++] = (unsigned char)p.size(); }
            memcpy(m.Data + h, p.data(), p.size()); m.DataSize = (unsigned long)(h + p.size());
            return a.writeMsgs(kchan, &m, &num, 1000) == J_STATUS_NOERROR;          // the adapter appends the checksum
        }
        m.ProtocolID = J_ISO15765; m.TxFlags = J_ISO15765_FRAME_PAD | (ext() ? J_CAN_29BIT_ID : 0);
        putId(m, txId); for (size_t i = 0; i < p.size() && i < 4000; i++) m.Data[4 + i] = p[i];
        m.DataSize = 4 + (unsigned long)p.size();
        return a.writeMsgs(chan, &m, &num, 1000) == J_STATUS_NOERROR;
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) override {
        J2534Api &a = j2534(); uint32_t t0 = nowMs();
        if (kind == LINK_TP20) return tp.recvMessage(out, timeoutMs);
        while (nowMs() - t0 < timeoutMs) {
            PASSTHRU_MSG m = {0}; unsigned long num = 1;
            long st = a.readMsgs(kind == LINK_KLINE ? kchan : chan, &m, &num, 50);
            if (st != J_STATUS_NOERROR || num == 0) continue;
            if (m.RxStatus & (J_TX_MSG_TYPE | J_START_OF_MESSAGE)) continue;
            if (kind == LINK_KLINE) {
                // <fmt> [<target> <source>] [<len>] <data...> [<checksum>]
                if (m.DataSize < 2) continue;
                size_t h = 1; unsigned char fmt = m.Data[0]; size_t len = fmt & 0x3F;
                if (fmt & 0x80) { if (m.DataSize < 4) continue; if (m.Data[1] != (unsigned char)rxId || m.Data[2] != (unsigned char)txId) continue; h = 3; }
                if (len == 0) { if (m.DataSize <= h) continue; len = m.Data[h++]; }
                if (len == 0 || h + len > m.DataSize) continue;
                out.assign(m.Data + h, m.Data + h + len);
                return true;
            }
            if (m.DataSize <= 4) continue;
            unsigned long id = ((unsigned long)m.Data[0] << 24) | (m.Data[1] << 16) | (m.Data[2] << 8) | m.Data[3];
            if (id != rxId) continue;
            out.assign(m.Data + 4, m.Data + m.DataSize);
            return true;
        }
        return false;
    }
};

} // namespace pt
#endif
