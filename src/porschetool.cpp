// ============================================================================
//  PorscheTool (Windows) - open-source diagnostics for Porsche vehicles
//  Every control unit: fault memory, live data, identification, actuator tests, routines / basic
//  settings and adaptation values, driven by a data pack per vehicle line (data/<LINE>.json).
//  UDS (ISO 14229) and KWP2000 (ISO 14230) over ISO 15765, 11- and 29-bit CAN, through any J2534
//  PassThru adapter (tested target: Tactrix OpenPort 2.0).
//
//  Build (macOS/Linux with mingw-w64):  ./build.sh        License: MIT.
//  Not affiliated with or endorsed by Porsche AG.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>
#include "diag_core.h"
#include "seedkey.h"
#include "pack.h"

#define APP_TITLE "PorscheTool - open-source Porsche diagnostics"

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static HWND g_hMain, g_hLog, g_hStatus, g_hVeh, g_hMod;
static HWND g_hModWnd, g_hModTitle, g_hCat, g_hSearch, g_hList, g_hOut;
static volatile bool g_stop = false, g_busy = false, g_detecting = false;
static int g_devState = -1;
static bool g_auto = false; static int g_autoJob = 0;
static std::string g_cliVeh, g_cliMod, g_cliFn; static std::vector<double> g_cliIn; static bool g_cliRun = false;
static FILE *g_report = nullptr, *g_sessionLog = nullptr;
static pk::Pack g_pack; static bool g_packOk = false;
struct PackFile { std::string path, title; };
static std::vector<PackFile> g_packs;
static int g_curMod = -1;                       // module shown in the module window
#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_DEVSTATE (WM_APP + 3)
#define WM_APP_SETOUT   (WM_APP + 4)

static std::string exeDir() { char p[MAX_PATH]; GetModuleFileNameA(0, p, MAX_PATH);
    std::string s(p); size_t k = s.find_last_of("\\/"); return k == std::string::npos ? "." : s.substr(0, k); }
static std::string stamp() { SYSTEMTIME t; GetLocalTime(&t); char b[32];
    sprintf(b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b; }
// pack strings are UTF-8, the ANSI window functions want the local code page
static std::string A(const std::string &u) {
    if (u.empty()) return u;
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0); if (n <= 0) return u;
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, "?", nullptr);
    std::string o(m, 0); WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &o[0], m, "?", nullptr);
    if (!o.empty() && o.back() == 0) o.pop_back();
    return o;
}
static std::string U8(const std::string &a) {           // local code page -> UTF-8
    int n = MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, nullptr, 0); if (n <= 0) return a;
    std::wstring w(n, 0); MultiByteToWideChar(CP_ACP, 0, a.c_str(), -1, &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string o(m, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &o[0], m, nullptr, nullptr);
    if (!o.empty() && o.back() == 0) o.pop_back();
    return o;
}
static void post(HWND target, const std::string &s) {
    char *buf = (char*)malloc(s.size() + 1); memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_LOG, (WPARAM)target, (LPARAM)buf);
}
static void logLine(const std::string &s) {
    post(g_hLog, s);
    if (g_sessionLog) { fprintf(g_sessionLog, "%s\r\n", s.c_str()); fflush(g_sessionLog); }
}
static void outLine(const std::string &s) {           // module window output (falls back to the main log)
    post(g_hModWnd ? g_hOut : g_hLog, s);
    if (g_sessionLog) { fprintf(g_sessionLog, "%s\r\n", s.c_str()); fflush(g_sessionLog); }
}
static void outSet(const std::string &s) {            // replace module output (live view)
    char *buf = (char*)malloc(s.size() + 1); memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_SETOUT, 0, (LPARAM)buf);
}
static void rep(const std::string &s) { logLine(s); if (g_report) fprintf(g_report, "%s\r\n", s.c_str()); }
static std::string hex(const uint8_t *d, size_t n) {
    static const char *H = "0123456789ABCDEF"; std::string s;
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; s += ' '; }
    return s;
}

// ---------------------------------------------------------------------------
// J2534
// ---------------------------------------------------------------------------
#define PASSTHRU_MSG_DATA_SIZE 4128
typedef struct {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[PASSTHRU_MSG_DATA_SIZE];
} PASSTHRU_MSG;
#define J_ISO15765            6
#define J_STATUS_NOERROR      0
#define J_FLOW_CONTROL_FILTER 3
#define J_ISO15765_FRAME_PAD  0x40
#define J_CAN_29BIT_ID        0x100
#define J_CAN_ID_BOTH         0x800
#define J_TX_MSG_TYPE         0x01
#define J_CLEAR_RX_BUFFER     0x08
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
static HMODULE g_dll = nullptr;
static PF_Open pOpen; static PF_Close pClose; static PF_Connect pConnect; static PF_Disconnect pDisconnect;
static PF_ReadMsgs pReadMsgs; static PF_WriteMsgs pWriteMsgs; static PF_StartMsgFilter pStartMsgFilter;
static PF_StopMsgFilter pStopMsgFilter; static PF_Ioctl pIoctl;

static std::string findJ2534Library() {
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
static HMODULE loadJ2534Dll() {
    if (g_dll) return g_dll;
    std::string p = findJ2534Library();
    if (!p.empty()) g_dll = LoadLibraryA(p.c_str());
    if (!g_dll) g_dll = LoadLibraryA("op20pt32.dll");
    return g_dll;
}

struct Bus {
    unsigned long dev = 0, chan = 0, filt = 0; bool hasFilt = false;
    unsigned long txId = 0, rxId = 0; bool ext = false;
    bool connected = false;
    uint8_t lastNrc = 0; bool gotAnswer = false;

    bool load() {
        if (pOpen) return true;
        if (!loadJ2534Dll()) { logLine("[ERROR] J2534 driver not found. Install the adapter drivers (Tactrix: op20pt32.dll)."); return false; }
        pOpen = (PF_Open)GetProcAddress(g_dll, "PassThruOpen");
        pClose = (PF_Close)GetProcAddress(g_dll, "PassThruClose");
        pConnect = (PF_Connect)GetProcAddress(g_dll, "PassThruConnect");
        pDisconnect = (PF_Disconnect)GetProcAddress(g_dll, "PassThruDisconnect");
        pReadMsgs = (PF_ReadMsgs)GetProcAddress(g_dll, "PassThruReadMsgs");
        pWriteMsgs = (PF_WriteMsgs)GetProcAddress(g_dll, "PassThruWriteMsgs");
        pStartMsgFilter = (PF_StartMsgFilter)GetProcAddress(g_dll, "PassThruStartMsgFilter");
        pStopMsgFilter = (PF_StopMsgFilter)GetProcAddress(g_dll, "PassThruStopMsgFilter");
        pIoctl = (PF_Ioctl)GetProcAddress(g_dll, "PassThruIoctl");
        if (!pOpen || !pConnect || !pReadMsgs || !pWriteMsgs || !pStartMsgFilter || !pStopMsgFilter)
        { logLine("[ERROR] J2534 entry points missing in DLL."); pOpen = nullptr; return false; }
        return true;
    }
    static void putId(PASSTHRU_MSG &m, unsigned long id) {
        m.Data[0] = (id >> 24) & 0xFF; m.Data[1] = (id >> 16) & 0xFF; m.Data[2] = (id >> 8) & 0xFF; m.Data[3] = id & 0xFF;
    }
    bool open() {
        if (!load()) return false;
        if (pOpen(nullptr, &dev) != J_STATUS_NOERROR) { logLine("[ERROR] PassThruOpen failed - adapter not connected?"); return false; }
        if (pConnect(dev, J_ISO15765, J_CAN_ID_BOTH, 500000, &chan) != J_STATUS_NOERROR &&
            pConnect(dev, J_ISO15765, 0, 500000, &chan) != J_STATUS_NOERROR) { logLine("[ERROR] PassThruConnect (CAN 500k) failed."); return false; }
        connected = true; return true;
    }
    bool target(unsigned long tx, unsigned long rx) {
        if (hasFilt) { pStopMsgFilter(chan, filt); hasFilt = false; }
        txId = tx; rxId = rx; ext = tx > 0x7FF || rx > 0x7FF;
        PASSTHRU_MSG mask = {0}, patt = {0}, flow = {0};
        mask.ProtocolID = patt.ProtocolID = flow.ProtocolID = J_ISO15765;
        mask.TxFlags = patt.TxFlags = flow.TxFlags = J_ISO15765_FRAME_PAD | (ext ? J_CAN_29BIT_ID : 0);
        mask.DataSize = patt.DataSize = flow.DataSize = 4;
        putId(mask, ext ? 0x1FFFFFFF : 0x7FF); putId(patt, rx); putId(flow, tx);
        if (pStartMsgFilter(chan, J_FLOW_CONTROL_FILTER, &mask, &patt, &flow, &filt) != J_STATUS_NOERROR) {
            logLine("[ERROR] StartMsgFilter failed."); return false; }
        hasFilt = true;
        if (pIoctl) pIoctl(chan, J_CLEAR_RX_BUFFER, nullptr, nullptr);
        return true;
    }
    void close() {
        if (hasFilt && pStopMsgFilter) pStopMsgFilter(chan, filt);
        if (connected && pDisconnect) pDisconnect(chan);
        if (dev && pClose) pClose(dev);
        connected = false; hasFilt = false; dev = 0; chan = 0;
    }
    bool send(const std::vector<uint8_t> &p) {
        PASSTHRU_MSG m = {0}; m.ProtocolID = J_ISO15765; m.TxFlags = J_ISO15765_FRAME_PAD | (ext ? J_CAN_29BIT_ID : 0);
        putId(m, txId); for (size_t i = 0; i < p.size() && i < 4000; i++) m.Data[4 + i] = p[i];
        m.DataSize = 4 + (unsigned long)p.size(); unsigned long num = 1;
        return pWriteMsgs(chan, &m, &num, 1000) == J_STATUS_NOERROR;
    }
    bool recv(std::vector<uint8_t> &out, unsigned long timeoutMs) {
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < timeoutMs) {
            PASSTHRU_MSG m = {0}; unsigned long num = 1;
            long st = pReadMsgs(chan, &m, &num, 50);
            if (st != J_STATUS_NOERROR || num == 0) continue;
            if (m.RxStatus & J_TX_MSG_TYPE) continue;
            if (m.DataSize <= 4) continue;
            unsigned long id = (m.Data[0] << 24) | (m.Data[1] << 16) | (m.Data[2] << 8) | m.Data[3];
            if (id != rxId) continue;
            out.assign(m.Data + 4, m.Data + m.DataSize);
            return true;
        }
        return false;
    }
    bool xfer(const std::vector<uint8_t> &p, std::vector<uint8_t> &out, unsigned long tmo = 1500) {
        lastNrc = 0; gotAnswer = false;
        if (p.empty() || !send(p)) return false;
        uint8_t want = p[0] + 0x40;
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < tmo) {
            if (!recv(out, tmo)) return false;
            gotAnswer = true;
            if (!out.empty() && out[0] == want) return true;
            if (out.size() >= 3 && out[0] == 0x7F && out[1] == p[0]) {
                lastNrc = out[2];
                if (out[2] == 0x78) { t0 = GetTickCount(); tmo = 8000; continue; }
                return false;
            }
        }
        return false;
    }
};

static std::string nrcText(uint8_t n) {
    switch (n) {
    case 0x10: return "general reject"; case 0x11: return "service not supported"; case 0x12: return "sub-function not supported";
    case 0x13: return "wrong length / format"; case 0x21: return "busy, repeat request"; case 0x22: return "conditions not correct";
    case 0x24: return "request sequence error"; case 0x31: return "request out of range"; case 0x33: return "security access denied";
    case 0x35: return "invalid key"; case 0x72: return "general programming failure";
    case 0x7E: case 0x7F: return "not supported in this session"; case 0x80: return "not supported in this session (KWP)";
    default: { char b[16]; sprintf(b, "NRC 0x%02X", n); return b; } }
}

// ---------------------------------------------------------------------------
// fault memory (all modules)
// ---------------------------------------------------------------------------
struct ModResult { bool present = false; int proto = 0; std::vector<Dtc> dtcs; std::string ident; };

static void identUds(Bus &b, std::string &id) {
    static const struct { uint16_t did; const char *lbl; } D[] = {{0xF187, "part no."}, {0xF189, "SW"}, {0xF191, "HW"}, {0xF197, "system"}};
    std::vector<uint8_t> r;
    for (auto &d : D)
        if (b.xfer({0x22, (uint8_t)(d.did >> 8), (uint8_t)d.did}, r, 800) && r.size() > 3) {
            std::string v = printable(r.data() + 3, r.size() - 3);
            if (!v.empty()) { if (!id.empty()) id += " | "; id += std::string(d.lbl) + " " + v; }
        }
}
static void identKwp(Bus &b, std::string &id) {
    std::vector<uint8_t> r;
    if (b.xfer({0x1A, 0x9B}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
    else if (b.xfer({0x1A, 0x87}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
}
static bool readFaults(Bus &b, int proto, ModResult &mr) {
    std::vector<uint8_t> r;
    auto tryUds = [&]() -> bool {
        if (b.xfer({0x19, 0x02, 0xFF}, r, 800) && parseUds1902(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_UDS; return true; }
        if (b.gotAnswer) mr.present = true;
        return false;
    };
    auto tryKwp = [&]() -> bool {
        std::vector<uint8_t> s; b.xfer({0x10, 0x81}, s, 600);
        if (b.gotAnswer) mr.present = true;
        if (b.xfer({0x18, 0x00, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
        if (b.gotAnswer) mr.present = true;
        if (b.xfer({0x18, 0x02, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
        if (b.gotAnswer) mr.present = true;
        return false;
    };
    if (proto == PROTO_KWP) { if (tryKwp()) return true; return mr.present && tryUds(); }
    if (tryUds()) return true;
    if (!mr.present && proto == PROTO_UDS) return false;
    return tryKwp();
}
static bool clearFaults(Bus &b, int proto) {
    std::vector<uint8_t> r, s;
    if (proto == PROTO_UDS) {
        if (b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000)) return true;
        b.xfer({0x10, 0x03}, s, 1000);
        bool ok = b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000);
        b.xfer({0x10, 0x01}, s, 1000);
        return ok;
    }
    if (b.xfer({0x14, 0xFF, 0x00}, r, 5000)) return true;
    b.xfer({0x10, 0x89}, s, 1000);
    bool ok = b.xfer({0x14, 0xFF, 0x00}, r, 5000);
    b.xfer({0x10, 0x81}, s, 1000);
    return ok;
}
static std::string dtcTextOf(const pk::Module &m, const Dtc &d) {
    const pk::Dtc *alt = nullptr;
    for (auto &x : m.dtc) {
        if (x.bits == d.bits && x.code == d.code) return g_pack.S(x.text);
        if (d.bits == 24 && x.bits == 16 && x.code == (d.code >> 8)) alt = &x;
    }
    return alt ? g_pack.S(alt->text) : "(code not in catalogue)";
}
static std::string modName(const pk::Module &m) { std::string n = g_pack.S(m.name); return n.empty() ? m.key : n; }

static void reportModule(const pk::Module &m, unsigned long req, const ModResult &mr) {
    char hdr[200];
    sprintf(hdr, "[%03lX] %-40s", req, A(modName(m)).c_str());
    if (!mr.present) { rep(std::string(hdr) + " -- no answer (not fitted or asleep)"); return; }
    char n[64]; sprintf(n, " %s  %d fault(s)", mr.proto == PROTO_UDS ? "UDS" : "KWP", (int)mr.dtcs.size());
    rep(std::string(hdr) + n);
    if (!mr.ident.empty()) rep("        " + mr.ident);
    for (const Dtc &d : mr.dtcs) rep("    * " + dtcName(d) + "  " + A(dtcTextOf(m, d)) + "   [" + statusText(d) + "]");
}
static bool scanOne(Bus &b, const pk::Module &m, bool withIdent, ModResult *res = nullptr, unsigned long *usedReq = nullptr) {
    ModResult mr;
    for (auto &pp : m.ids) {
        if (g_stop) break;
        if (!b.target(pp.first, pp.second)) return false;
        if (readFaults(b, m.proto, mr) || mr.present) {
            if (withIdent) { if (mr.proto == PROTO_KWP) identKwp(b, mr.ident); else identUds(b, mr.ident); }
            reportModule(m, pp.first, mr);
            if (res) *res = mr;
            if (usedReq) *usedReq = pp.first;
            return true;
        }
    }
    reportModule(m, m.ids.empty() ? 0 : m.ids[0].first, mr);
    return false;
}
static void openReport(const char *what) {
    std::string path = exeDir() + "\\PorscheTool_" + what + "_" + stamp() + ".txt";
    g_report = fopen(path.c_str(), "w");
    if (g_report) logLine("Report: " + path);
}
static void closeReport() { if (g_report) { fclose(g_report); g_report = nullptr; } }

static void actScanAll(Bus &b) {
    openReport("scan");
    rep("=== SCAN ALL MODULES - " + A(g_pack.title) + " - " + stamp() + " ===");
    int found = 0, faults = 0; std::vector<std::string> skipped;
    for (auto &m : g_pack.mods) {
        if (g_stop) break;
        if (m.ids.empty()) { skipped.push_back(A(modName(m))); continue; }
        ModResult mr;
        if (scanOne(b, m, true, &mr)) { found++; faults += (int)mr.dtcs.size(); }
    }
    rep("");
    char s[96]; sprintf(s, "Modules answering: %d   Faults: %d", found, faults); rep(s);
    if (!skipped.empty()) {
        std::string t = "Not reachable directly on the diagnostic CAN (sub-bus): ";
        for (size_t i = 0; i < skipped.size(); i++) t += (i ? ", " : "") + skipped[i];
        rep(t);
    }
    closeReport();
}
static void actClearAll(Bus &b) {
    openReport("clear");
    rep("=== CLEAR ALL FAULTS - " + A(g_pack.title) + " ===");
    for (auto &m : g_pack.mods) {
        if (g_stop) break;
        for (auto &pp : m.ids) {
            ModResult mr;
            if (!b.target(pp.first, pp.second)) break;
            readFaults(b, m.proto, mr);
            if (!mr.present) continue;
            bool ok = clearFaults(b, mr.proto ? mr.proto : m.proto);
            rep(std::string(ok ? "[OK]    " : "[FAIL]  ") + A(modName(m)) + (ok ? "" : "  (" + nrcText(b.lastNrc) + ")"));
            break;
        }
    }
    rep("Done. Run SCAN again: faults that come straight back are real and still present.");
    closeReport();
}

// ---------------------------------------------------------------------------
// module functions (data-pack driven)
// ---------------------------------------------------------------------------
struct Entry { char cat = 'O'; std::string name; int main = -1, stop = -1, result = -1, ret = -1; };
static std::vector<Entry> g_entries;         // all functions of the current module
static std::vector<int> g_shown;             // indexes into g_entries shown in the list

static int sidOf(const pk::Service &s) { for (auto &p : s.rq) if (p.hasConst && p.byte == 0) return (int)p.cval; return -1; }
static bool constAt(const pk::Service &s, int byte, double &v) { for (auto &p : s.rq) if (p.hasConst && p.byte == byte) { v = p.cval; return true; } return false; }
static std::string stripSuffix(std::string n) {
    static const char *suf[] = {" Request Routine Results", " RequestRoutineResults", " RequestRoutineResult", " Start Routine", " StartRoutine",
        " Stop Routine", " StopRoutine", " Short Term Adjustment", " Return Control To Ecu", " Return Control To ECU",
        " Freeze Current State", " Read Data By Identifier", " ReadDataByIdentifier", " Read Read", " Read", " Write"};
    for (const char *s : suf) { size_t l = strlen(s); if (n.size() > l && n.compare(n.size() - l, l, s) == 0) return n.substr(0, n.size() - l); }
    return n;
}
static void buildEntries(const pk::Module &m) {
    g_entries.clear();
    std::map<std::string, int> grp;
    for (int i = 0; i < (int)m.svc.size(); i++) {
        const pk::Service &s = m.svc[i];
        int sid = sidOf(s); double sub = -1, key = -1;
        std::string gk; int role = 0;          // 0 main, 1 stop, 2 result, 3 return-control
        if (sid == 0x31 && m.proto != PROTO_KWP && constAt(s, 1, sub)) {
            constAt(s, 2, key); gk = "R" + std::to_string((long long)key);
            role = sub == 2 ? 1 : sub == 3 ? 2 : 0;
        } else if (sid == 0x31 || sid == 0x32 || sid == 0x33) {
            constAt(s, 1, key); gk = "R" + std::to_string((long long)key); role = sid == 0x32 ? 1 : sid == 0x33 ? 2 : 0;
        } else if (sid == 0x2F && constAt(s, 1, key)) {
            double ctl = -1; constAt(s, 3, ctl); gk = "A" + std::to_string((long long)key);
            role = ctl == 0 ? 3 : ctl == 2 ? -1 : 0;
        } else if (sid == 0x30 && constAt(s, 1, key)) {
            double ctl = -1; constAt(s, 2, ctl); gk = "A" + std::to_string((long long)key); role = ctl == 0 ? 3 : 0;
        }
        if (role < 0) continue;                // "freeze current state" is not offered
        int e;
        if (!gk.empty() && grp.count(gk)) e = grp[gk];
        else {
            e = (int)g_entries.size(); g_entries.push_back(Entry());
            g_entries[e].cat = s.cls.empty() ? 'O' : s.cls[0];
            if (!gk.empty()) grp[gk] = e;
        }
        Entry &en = g_entries[e];
        if (role == 0 && en.main < 0) { en.main = i; en.name = stripSuffix(g_pack.S(s.name)); en.cat = s.cls.empty() ? 'O' : s.cls[0]; }
        else if (role == 1) en.stop = i; else if (role == 2) en.result = i; else if (role == 3) en.ret = i;
        if (en.name.empty()) en.name = stripSuffix(g_pack.S(s.name));
    }
    // routines that only have a result (status read) become readable entries
    for (auto &e : g_entries) if (e.main < 0 && e.result >= 0) e.main = e.result;
    g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(), [](const Entry &e) { return e.main < 0; }), g_entries.end());
    std::stable_sort(g_entries.begin(), g_entries.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
}

static const char *CATS[] = {"All functions", "Live data", "Identification", "Actuator tests", "Routines / basic settings",
                             "Adaptation & coding (read)", "Adaptation & coding (write)"};
static const char CATC[] = {0, 'L', 'I', 'A', 'R', 'C', 'W'};

static void fillList() {
    if (!g_hList) return;
    int c = (int)SendMessageA(g_hCat, CB_GETCURSEL, 0, 0); if (c < 0) c = 0;
    char q[128]; GetWindowTextA(g_hSearch, q, sizeof q);
    std::string ql = q; for (char &ch : ql) ch = (char)tolower((unsigned char)ch);
    SendMessageA(g_hList, LB_RESETCONTENT, 0, 0); g_shown.clear();
    for (int i = 0; i < (int)g_entries.size(); i++) {
        const Entry &e = g_entries[i];
        if (CATC[c] && e.cat != CATC[c]) continue;
        std::string n = A(e.name), nl = n; for (char &ch : nl) ch = (char)tolower((unsigned char)ch);
        if (!ql.empty() && nl.find(ql) == std::string::npos) continue;
        static const char *tag[] = {"", "[live] ", "[ident] ", "[actuator] ", "[routine] ", "[read] ", "[WRITE] "};
        int ti = 0; for (int k = 1; k < 7; k++) if (CATC[k] == e.cat) ti = k;
        SendMessageA(g_hList, LB_ADDSTRING, 0, (LPARAM)(std::string(c ? "" : tag[ti]) + n).c_str());
        g_shown.push_back(i);
    }
}

// session handling: enter the extended session when the service does not run in the default one
static void ensureSession(Bus &b, const pk::Module &m, const pk::Service &s, int proto) {
    if (s.sess & pk::S_DEFAULT) return;
    std::vector<uint8_t> r;
    if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, r, 1500); else b.xfer({0x10, 0x03}, r, 1500);
    (void)m;
}
static void testerPresent(Bus &b, int proto) {
    std::vector<uint8_t> r;
    if (proto == PROTO_KWP) b.xfer({0x3E, 0x01}, r, 300); else b.send({0x3E, 0x80});
}
static bool connectModule(Bus &b, const pk::Module &m, int &proto) {
    for (auto &pp : m.ids) {
        if (!b.target(pp.first, pp.second)) return false;
        std::vector<uint8_t> r;
        if (m.proto != PROTO_KWP) { b.xfer({0x10, 0x01}, r, 600); if (b.gotAnswer) { proto = PROTO_UDS; return true; } }
        b.xfer({0x10, 0x81}, r, 600); if (b.gotAnswer) { proto = b.lastNrc == 0x12 || b.lastNrc == 0x11 ? PROTO_UDS : PROTO_KWP; return true; }
    }
    return false;
}
static void printValues(const std::vector<pk::Value> &vs) {
    for (auto &v : vs) {
        std::string ind((size_t)std::max(0, v.depth - 1) * 2, ' ');
        if (v.text.empty() && v.unit.empty()) outLine(ind + A(v.name) + ":");
        else outLine(ind + A(v.name) + " = " + A(v.text) + (v.unit.empty() ? "" : " " + A(v.unit)));
    }
}
// Unlock Codierung security access for a module using the local key file. Returns true if the module
// is unlocked (or was already open / needs no seed); otherwise fills 'why' with a readable reason.
// A correct key is computed locally; only a wrong key risks a control unit's attempt counter, so we
// never send a guessed key - if no key is available we report it and send nothing.
static bool securityUnlock(Bus &b, const pk::Module &m, int proto, std::string &why) {
    if (m.sk.empty()) { why = "no coding key mapped for this control unit"; return false; }
    const sk::Key *k = sk::find(m.sk);
    if (!k) {
        why = sk::loaded() ? ("algorithm " + m.sk + " is not in the key file (not ported yet)")
                           : "key file porschetool.keys not found - coding writes disabled";
        return false;
    }
    std::vector<uint8_t> r;
    if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, r, 1500); else b.xfer({0x10, 0x03}, r, 1500);
    const uint8_t levels[2] = {0x03, 0x01};                 // coding level first, then the generic level
    for (uint8_t lvl : levels) {
        if (!b.xfer({0x27, lvl}, r, 2000)) {
            if (b.lastNrc == 0x12 || b.lastNrc == 0x11) continue;    // sub-function unknown -> try the other level
            why = "seed request refused (" + nrcText(b.lastNrc) + ")"; return false;
        }
        std::vector<uint8_t> seed(r.begin() + (r.size() >= 2 ? 2 : r.size()), r.end());
        bool allzero = !seed.empty(); for (uint8_t x : seed) if (x) { allzero = false; break; }
        if (seed.empty() || allzero) return true;           // already unlocked
        std::vector<uint8_t> s = seed; if ((int)s.size() > k->nb) s.resize(k->nb);
        std::vector<uint8_t> key;
        if (!sk::compute(*k, s, key)) { why = "key computation failed for " + m.sk; return false; }
        std::vector<uint8_t> req = {0x27, (uint8_t)(lvl + 1)};
        req.insert(req.end(), key.begin(), key.end());
        if (b.xfer(req, r, 2000)) return true;              // {0x67, lvl+1} = unlocked
        why = "key rejected by control unit (" + nrcText(b.lastNrc) + ")"; return false;
    }
    why = "control unit did not offer the coding security level"; return false;
}

// run one service; returns true on a positive answer
static bool runService(Bus &b, const pk::Module &m, int si, int proto, const std::vector<double> &in, std::vector<pk::Value> &vals, bool quiet = false) {
    const pk::Service &s = m.svc[si];
    std::vector<uint8_t> rq, r; std::string err;
    if (!g_pack.buildRequest(s, in, rq, err)) { outLine("[ERROR] " + err); return false; }
    ensureSession(b, m, s, proto);
    if (s.sess & 0x100) {                                                          // service needs Codierung unlock
        std::string why;
        if (securityUnlock(b, m, proto, why)) { if (!quiet) outLine("[UNLOCKED] security access granted (" + m.sk + ")"); }
        else if (!quiet) outLine("[LOCKED] " + why);
    }
    if (!quiet) outLine("> " + hex(rq.data(), rq.size()));
    if (!b.xfer(rq, r, 3000)) {
        if (b.lastNrc == 0x7F || b.lastNrc == 0x7E || b.lastNrc == 0x80) {          // retry in the extended session
            std::vector<uint8_t> x; if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, x, 1500); else b.xfer({0x10, 0x03}, x, 1500);
            if (b.xfer(rq, r, 3000)) goto ok;
        }
        if (b.lastNrc == 0x33) {                                                    // security denied: unlock and retry once
            std::string why;
            if (securityUnlock(b, m, proto, why)) {
                if (!quiet) outLine("[UNLOCKED] security access granted (" + m.sk + ")");
                if (b.xfer(rq, r, 3000)) goto ok;
            } else if (!quiet) outLine("[LOCKED] " + why);
        }
        if (b.lastNrc) outLine("[REFUSED] " + nrcText(b.lastNrc) + (b.lastNrc == 0x33 ? " - needs security access (no coding key available for this module)" : ""));
        else outLine("[NO ANSWER] module did not respond");
        return false;
    }
ok:
    if (!quiet) outLine("< " + hex(r.data(), r.size()));
    if (!g_pack.decode(s, r, vals, err)) { outLine("[DECODE] " + err); return false; }
    return true;
}

struct ModJob { int kind; int entry; std::vector<double> inputs; };   // kind: 1 run, 2 live, 3 faults, 4 clear
static void moduleJob(Bus &b, const ModJob &j) {
    if (g_curMod < 0 || g_curMod >= (int)g_pack.mods.size()) return;
    const pk::Module &m = g_pack.mods[g_curMod];
    if (j.kind == 3 || j.kind == 4) {
        ModResult mr; unsigned long rq = 0;
        if (!scanOne(b, m, true, &mr, &rq)) { outLine("Module does not answer."); return; }
        outLine(A(modName(m)) + ": " + std::to_string(mr.dtcs.size()) + " fault(s)");
        for (const Dtc &d : mr.dtcs) outLine("  * " + dtcName(d) + "  " + A(dtcTextOf(m, d)) + "   [" + statusText(d) + "]");
        if (j.kind == 4) {
            bool ok = clearFaults(b, mr.proto ? mr.proto : m.proto);
            outLine(ok ? "[OK] fault memory cleared" : "[REFUSED] " + nrcText(b.lastNrc));
        }
        return;
    }
    const Entry &e = g_entries[j.entry];
    int proto = m.proto;
    if (!connectModule(b, m, proto)) { outLine("Module does not answer (ignition on? fitted?)."); return; }
    std::vector<pk::Value> vals;
    if (j.kind == 2) {                                         // live: repeat until STOP
        outLine("Live: " + A(e.name) + "  (STOP to end)");
        DWORD tp = GetTickCount(); int n = 0;
        while (!g_stop) {
            if (runService(b, m, e.main, proto, j.inputs, vals, true)) {
                std::string t = A(e.name) + "   (update " + std::to_string(++n) + ")\r\n\r\n";
                for (auto &v : vals) {
                    std::string ind((size_t)std::max(0, v.depth - 1) * 2, ' ');
                    t += ind + A(v.name) + (v.text.empty() && v.unit.empty() ? ":" : " = " + A(v.text) + (v.unit.empty() ? "" : " " + A(v.unit))) + "\r\n";
                }
                outSet(t);
            } else { Sleep(500); }
            if (GetTickCount() - tp > 2000) { testerPresent(b, proto); tp = GetTickCount(); }
            Sleep(150);
        }
        return;
    }
    outLine("=== " + A(e.name) + " ===");
    if (e.cat == 'W') {                                         // show the current value first
        const pk::Service &ws = m.svc[e.main]; double key = -1; constAt(ws, 1, key);
        for (int i = 0; i < (int)m.svc.size(); i++) {
            double k2 = -1; int sid = sidOf(m.svc[i]);
            if ((sid == 0x22 || sid == 0x21) && constAt(m.svc[i], 1, k2) && k2 == key) {
                outLine("Current value:"); if (runService(b, m, i, proto, {}, vals, true)) printValues(vals); break; }
        }
    }
    if (!runService(b, m, e.main, proto, j.inputs, vals)) return;
    printValues(vals);
    if (e.cat == 'R' && e.result >= 0 && e.result != e.main) {   // follow the routine until STOP
        outLine("Routine started. Results follow every second - press STOP to end the routine.");
        std::string last; DWORD t0 = GetTickCount(), tp = t0;
        while (!g_stop && GetTickCount() - t0 < (g_auto ? 5000u : 600000u)) {
            Sleep(1000);
            std::vector<pk::Value> rv;
            if (runService(b, m, e.result, proto, {}, rv, true)) {
                std::string t; for (auto &v : rv) t += A(v.name) + " = " + A(v.text) + " " + A(v.unit) + "; ";
                if (t != last) { char ts[16]; sprintf(ts, "[%3lus] ", (GetTickCount() - t0) / 1000); outLine(ts + t); last = t; }
            }
            if (GetTickCount() - tp > 2000) { testerPresent(b, proto); tp = GetTickCount(); }
        }
        if (e.stop >= 0) { outLine("Stopping routine..."); runService(b, m, e.stop, proto, {}, vals, true); }
    } else if (e.cat == 'A') {                                  // hold the actuator until STOP
        if (e.ret >= 0 || e.stop >= 0) {
            outLine("Actuator active - press STOP to hand control back to the module.");
            DWORD tp = GetTickCount();
            DWORD t0 = GetTickCount();
            while (!g_stop && !(g_auto && GetTickCount() - t0 > 3000)) { Sleep(200); if (GetTickCount() - tp > 2000) { testerPresent(b, proto); tp = GetTickCount(); } }
            int back = e.ret >= 0 ? e.ret : e.stop;
            if (runService(b, m, back, proto, {}, vals, true)) outLine("[OK] control returned to the module");
        }
    } else if (e.cat == 'W') {
        outLine("[OK] written. Value now:");
        const pk::Service &ws = m.svc[e.main]; double key = -1; constAt(ws, 1, key);
        for (int i = 0; i < (int)m.svc.size(); i++) {
            double k2 = -1; int sid = sidOf(m.svc[i]);
            if ((sid == 0x22 || sid == 0x21) && constAt(m.svc[i], 1, k2) && k2 == key) { if (runService(b, m, i, proto, {}, vals, true)) printValues(vals); break; }
        }
    }
}

// ---------------------------------------------------------------------------
// input dialog (routine / actuator / write parameters)
// ---------------------------------------------------------------------------
struct InDlg { std::vector<std::pair<int, int>> ins; std::vector<HWND> ctl; std::vector<double> vals; bool ok = false, done = false; };
static InDlg *g_dlg = nullptr;
static LRESULT CALLBACK InProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        if (LOWORD(w) == IDOK && g_dlg) {
            g_dlg->vals.clear();
            for (size_t k = 0; k < g_dlg->ins.size(); k++) {
                const pk::Dop &d = g_pack.dops[g_dlg->ins[k].second];
                if (d.cat == pk::C_TEXT && !d.sc.empty()) {
                    int sel = (int)SendMessageA(g_dlg->ctl[k], CB_GETCURSEL, 0, 0); if (sel < 0) sel = 0;
                    g_dlg->vals.push_back(d.sc[sel].lo);
                } else { char t[64]; GetWindowTextA(g_dlg->ctl[k], t, sizeof t); g_dlg->vals.push_back(atof(t)); }
            }
            g_dlg->ok = true;
        }
        if (g_dlg) g_dlg->done = true;
        DestroyWindow(h); return 0;
    }
    if (msg == WM_CLOSE) { if (g_dlg) g_dlg->done = true; DestroyWindow(h); return 0; }
    return DefWindowProcA(h, msg, w, l);
}
static bool askInputs(HWND owner, const pk::Service &s, const std::string &title, std::vector<double> &out) {
    InDlg d; g_pack.inputsOf(s, d.ins);
    out.clear();
    if (d.ins.empty()) return true;
    int H = 70 + 30 * (int)d.ins.size();
    HWND h = CreateWindowExA(WS_EX_DLGMODALFRAME, "PTInput", A(title).c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             200, 200, 520, H + 40, owner, 0, 0, 0);
    for (size_t k = 0; k < d.ins.size(); k++) {
        const pk::Dop &dp = g_pack.dops[d.ins[k].second];
        int y = 12 + 30 * (int)k;
        std::string lbl = A(g_pack.S(d.ins[k].first)); if (dp.unit >= 0) lbl += " [" + A(g_pack.S(dp.unit)) + "]";
        CreateWindowA("STATIC", lbl.c_str(), WS_CHILD | WS_VISIBLE, 10, y + 4, 230, 18, h, 0, 0, 0);
        HWND c;
        if (dp.cat == pk::C_TEXT && !dp.sc.empty()) {
            c = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 245, y, 255, 300, h, 0, 0, 0);
            for (auto &sc : dp.sc) { std::string t = A(g_pack.S(sc.text)); if (t.empty()) t = pk::Pack::fmtNum(sc.lo); SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)t.c_str()); }
            SendMessageA(c, CB_SETCURSEL, 0, 0);
        } else c = CreateWindowA("EDIT", "0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 245, y, 255, 22, h, 0, 0, 0);
        d.ctl.push_back(c);
    }
    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 300, H - 36, 95, 28, h, (HMENU)IDOK, 0, 0);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 405, H - 36, 95, 28, h, (HMENU)IDCANCEL, 0, 0);
    g_dlg = &d; EnableWindow(owner, FALSE);
    MSG msg; while (!d.done && GetMessage(&msg, 0, 0, 0)) { if (!IsDialogMessageA(h, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); } }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_dlg = nullptr;
    out = d.vals; return d.ok;
}

// ---------------------------------------------------------------------------
// worker thread
// ---------------------------------------------------------------------------
enum { ID_SCAN = 2001, ID_CLEARALL, ID_OPENMOD, ID_READMOD, ID_CLEARMOD, ID_STOP, ID_DETECT, IDC_VEH, IDC_MOD, IDC_LOG,
       ID_M_RUN = 2101, ID_M_LIVE, ID_M_STOP, ID_M_FAULTS, ID_M_CLEAR, IDC_M_CAT, IDC_M_SEARCH, IDC_M_LIST, IDC_M_OUT };
struct Job { int id; ModJob mj; };
static DWORD WINAPI worker(LPVOID p) {
    Job *j = (Job*)p; g_busy = true; g_stop = false;
    Bus b;
    if (b.open()) {
        switch (j->id) {
        case ID_SCAN: actScanAll(b); break;
        case ID_CLEARALL: actClearAll(b); break;
        case ID_READMOD: case ID_CLEARMOD: {
            int k = (int)SendMessageA(g_hMod, CB_GETCURSEL, 0, 0), n = 0;
            for (int i = 0; i < (int)g_pack.mods.size(); i++) if (!g_pack.mods[i].ids.empty()) {
                if (n++ == k) { int save = g_curMod; HWND sw = g_hModWnd; g_curMod = i; g_hModWnd = nullptr;
                                moduleJob(b, {j->id == ID_READMOD ? 3 : 4, 0, {}}); g_curMod = save; g_hModWnd = sw; break; } }
            break; }
        default: moduleJob(b, j->mj); break;
        }
    }
    b.close();
    delete j; g_busy = false;
    PostMessageA(g_hMain, WM_APP_DONE, 0, 0);
    return 0;
}
static void startJob(int id, const ModJob &mj = ModJob()) {
    if (g_busy) { logLine("[BUSY] wait for the current action to finish (or press STOP)."); return; }
    if (!g_packOk) { logLine("[ERROR] no vehicle data pack loaded."); return; }
    Job *j = new Job{id, mj};
    HANDLE t = CreateThread(0, 0, worker, j, 0, 0); if (t) CloseHandle(t);
}
static int detectDevice() {
    HMODULE dll = loadJ2534Dll(); if (!dll) return -1;
    PF_Open o = (PF_Open)GetProcAddress(dll, "PassThruOpen"); PF_Close c = (PF_Close)GetProcAddress(dll, "PassThruClose");
    if (!o || !c) return -1;
    unsigned long d = 0;
    if (o(nullptr, &d) == J_STATUS_NOERROR) { c(d); return 1; }
    return 0;
}
static DWORD WINAPI detectThread(LPVOID) {
    int st = detectDevice(); g_detecting = false;
    if (g_hMain) PostMessageA(g_hMain, WM_APP_DEVSTATE, (WPARAM)(st + 10), 0);
    return 0;
}
static void kickDetect() {
    if (g_detecting || g_busy) return;
    g_detecting = true;
    HANDLE t = CreateThread(0, 0, detectThread, 0, 0, 0); if (t) CloseHandle(t); else g_detecting = false;
}

// ---------------------------------------------------------------------------
// data packs
// ---------------------------------------------------------------------------
static void findPacks() {
    g_packs.clear();
    for (std::string dir : {exeDir() + "\\data", exeDir()}) {
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string path = dir + "\\" + fd.cFileName;
            FILE *f = fopen(path.c_str(), "rb"); if (!f) continue;
            char head[1024] = {0}; size_t n = fread(head, 1, sizeof head - 1, f); fclose(f); head[n] = 0;
            const char *t = strstr(head, "\"title\":\"");
            if (!t || !strstr(head, "\"v\":1")) continue;
            t += 9; const char *e = strchr(t, '"');
            g_packs.push_back({path, A(std::string(t, e ? e - t : 0))});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const PackFile &a, const PackFile &b) { return a.title < b.title; });
}
static void fillModules() {
    SendMessageA(g_hMod, CB_RESETCONTENT, 0, 0);
    for (auto &m : g_pack.mods) if (!m.ids.empty()) {
        char t[200]; sprintf(t, "%s  [%03X]", A(modName(m)).c_str(), m.ids[0].first);
        SendMessageA(g_hMod, CB_ADDSTRING, 0, (LPARAM)t);
    }
    SendMessageA(g_hMod, CB_SETCURSEL, 0, 0);
}
static bool loadPack(int i) {
    g_packOk = false; g_pack = pk::Pack();
    if (i < 0 || i >= (int)g_packs.size()) return false;
    FILE *f = fopen(g_packs[i].path.c_str(), "rb"); if (!f) return false;
    std::string text; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    std::string err;
    if (!g_pack.load(text, err)) { logLine("[ERROR] " + g_packs[i].path + ": " + err); return false; }
    g_packOk = true;
    size_t ns = 0; for (auto &m : g_pack.mods) ns += m.svc.size();
    logLine("Vehicle: " + A(g_pack.title) + " - " + std::to_string(g_pack.mods.size()) + " control units, " + std::to_string(ns) + " functions.");
    if (g_hMod) fillModules();
    return true;
}

// ---------------------------------------------------------------------------
// module window
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ModProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        int x = 10, y = 8;
        g_hModTitle = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE, x, y, 760, 20, h, 0, 0, 0);
        y += 26;
        g_hCat = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, x, y, 250, 300, h, (HMENU)IDC_M_CAT, 0, 0);
        for (auto c : CATS) SendMessageA(g_hCat, CB_ADDSTRING, 0, (LPARAM)c);
        SendMessageA(g_hCat, CB_SETCURSEL, 1, 0);
        CreateWindowA("STATIC", "Search:", WS_CHILD | WS_VISIBLE, x + 262, y + 4, 50, 18, h, 0, 0, 0);
        g_hSearch = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, x + 315, y, 250, 22, h, (HMENU)IDC_M_SEARCH, 0, 0);
        y += 30;
        g_hList = CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                x, y, 760, 230, h, (HMENU)IDC_M_LIST, 0, 0);
        y += 236;
        CreateWindowA("BUTTON", "RUN", WS_CHILD | WS_VISIBLE, x, y, 120, 30, h, (HMENU)ID_M_RUN, 0, 0);
        CreateWindowA("BUTTON", "LIVE (repeat)", WS_CHILD | WS_VISIBLE, x + 128, y, 120, 30, h, (HMENU)ID_M_LIVE, 0, 0);
        CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, x + 256, y, 120, 30, h, (HMENU)ID_M_STOP, 0, 0);
        CreateWindowA("BUTTON", "Read faults", WS_CHILD | WS_VISIBLE, x + 400, y, 115, 30, h, (HMENU)ID_M_FAULTS, 0, 0);
        CreateWindowA("BUTTON", "Clear faults", WS_CHILD | WS_VISIBLE, x + 522, y, 115, 30, h, (HMENU)ID_M_CLEAR, 0, 0);
        y += 38;
        g_hOut = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               x, y, 760, 260, h, (HMENU)IDC_M_OUT, 0, 0);
        SendMessageA(g_hOut, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        SendMessageA(g_hOut, WM_SETFONT, (WPARAM)f, 1);
        return 0; }
    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        if (g_hOut) MoveWindow(g_hOut, 10, 340, rc.right - 20, rc.bottom - 350, TRUE);
        if (g_hList) MoveWindow(g_hList, 10, 64, rc.right - 20, 230, TRUE);
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if ((id == IDC_M_CAT && HIWORD(w) == CBN_SELCHANGE) || (id == IDC_M_SEARCH && HIWORD(w) == EN_CHANGE)) { fillList(); return 0; }
        if (id == ID_M_STOP) { g_stop = true; return 0; }
        if (id == ID_M_FAULTS) { startJob(0, {3, 0, {}}); return 0; }
        if (id == ID_M_CLEAR) {
            if (MessageBoxA(h, "Clear the fault memory of this module?", "Clear faults", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                startJob(0, {4, 0, {}});
            return 0; }
        if (id == ID_M_RUN || id == ID_M_LIVE || (id == IDC_M_LIST && HIWORD(w) == LBN_DBLCLK)) {
            int sel = (int)SendMessageA(g_hList, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= (int)g_shown.size()) { MessageBoxA(h, "Choose a function in the list first.", "PorscheTool", MB_OK); return 0; }
            int ei = g_shown[sel]; const Entry &e = g_entries[ei]; const pk::Module &m = g_pack.mods[g_curMod];
            std::vector<double> in;
            if (!askInputs(h, m.svc[e.main], e.name, in)) return 0;
            if (e.cat == 'R' || e.cat == 'A' || e.cat == 'W') {
                std::string q = (e.cat == 'W' ? "WRITE a new value into the control unit:\n\n" : e.cat == 'A' ? "Actuate a component:\n\n" : "Start a routine:\n\n")
                    + A(e.name) + "\n\nThe vehicle must stand still, ignition ON. Follow the workshop manual for this procedure."
                    + (e.cat == 'W' ? "\nThe current value is read and logged first." : "") + "\n\nContinue?";
                if (MessageBoxA(h, q.c_str(), "Confirm", MB_YESNO | (e.cat == 'W' ? MB_ICONWARNING : MB_ICONQUESTION) | MB_DEFBUTTON2) != IDYES) return 0;
            }
            if (id == ID_M_LIVE) SetWindowTextA(g_hOut, "");
            startJob(0, {id == ID_M_LIVE ? 2 : 1, ei, in});
            return 0; }
        return 0; }
    case WM_CLOSE: g_stop = true; DestroyWindow(h); return 0;
    case WM_DESTROY: g_hModWnd = nullptr; g_hList = nullptr; g_hOut = nullptr; return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}
static void openModuleWindow() {
    int k = (int)SendMessageA(g_hMod, CB_GETCURSEL, 0, 0), n = 0, mi = -1;
    for (int i = 0; i < (int)g_pack.mods.size(); i++) if (!g_pack.mods[i].ids.empty()) { if (n++ == k) { mi = i; break; } }
    if (mi < 0) return;
    g_curMod = mi; const pk::Module &m = g_pack.mods[mi];
    buildEntries(m);
    if (!g_hModWnd) g_hModWnd = CreateWindowA("PTModule", "", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 80, 800, 720, 0, 0, 0, 0);
    char t[256]; sprintf(t, "%s   [%03X/%03X]   %d functions", A(modName(m)).c_str(), m.ids[0].first, m.ids[0].second, (int)g_entries.size());
    SetWindowTextA(g_hModTitle, t); SetWindowTextA(g_hModWnd, (A(modName(m)) + " - PorscheTool").c_str());
    SetWindowTextA(g_hOut, "");
    fillList(); SetForegroundWindow(g_hModWnd);
}

// ---------------------------------------------------------------------------
// main window
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        int x = 10, y = 10;
        g_hStatus = CreateWindowA("STATIC", "Adapter: checking...", WS_CHILD | WS_VISIBLE, x, y + 3, 420, 20, h, 0, 0, 0);
        CreateWindowA("BUTTON", "Detect", WS_CHILD | WS_VISIBLE, x + 440, y, 80, 24, h, (HMENU)ID_DETECT, 0, 0);
        y += 32;
        CreateWindowA("STATIC", "Vehicle:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hVeh = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x + 60, y, 460, 400, h, (HMENU)IDC_VEH, 0, 0);
        y += 34;
        HFONT big = CreateFontA(19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        HWND b1 = CreateWindowA("BUTTON", "SCAN ALL MODULES", WS_CHILD | WS_VISIBLE, x, y, 254, 50, h, (HMENU)ID_SCAN, 0, 0);
        HWND b2 = CreateWindowA("BUTTON", "CLEAR ALL FAULTS", WS_CHILD | WS_VISIBLE, x + 266, y, 254, 50, h, (HMENU)ID_CLEARALL, 0, 0);
        SendMessageA(b1, WM_SETFONT, (WPARAM)big, 1); SendMessageA(b2, WM_SETFONT, (WPARAM)big, 1);
        y += 60;
        CreateWindowA("STATIC", "Module:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hMod = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x + 60, y, 460, 500, h, (HMENU)IDC_MOD, 0, 0);
        y += 30;
        HWND b3 = CreateWindowA("BUTTON", "OPEN MODULE (live data, tests, routines)", WS_CHILD | WS_VISIBLE, x, y, 520, 40, h, (HMENU)ID_OPENMOD, 0, 0);
        SendMessageA(b3, WM_SETFONT, (WPARAM)big, 1);
        y += 46;
        CreateWindowA("BUTTON", "Read faults", WS_CHILD | WS_VISIBLE, x, y, 165, 26, h, (HMENU)ID_READMOD, 0, 0);
        CreateWindowA("BUTTON", "Clear faults", WS_CHILD | WS_VISIBLE, x + 177, y, 165, 26, h, (HMENU)ID_CLEARMOD, 0, 0);
        CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, x + 355, y, 165, 26, h, (HMENU)ID_STOP, 0, 0);
        y += 34;
        g_hLog = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               x, y, 520, 400, h, (HMENU)IDC_LOG, 0, 0);
        SendMessageA(g_hLog, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        SendMessageA(g_hLog, WM_SETFONT, (WPARAM)f, 1);
        logLine("PorscheTool - open-source diagnostics. Ignition ON, engine off, battery charger recommended.");
        findPacks();
        for (auto &p : g_packs) SendMessageA(g_hVeh, CB_ADDSTRING, 0, (LPARAM)p.title.c_str());
        if (g_packs.empty()) logLine("[ERROR] No vehicle data found. Put the 'data' folder next to PorscheTool.exe.");
        else {
            int pi = 0;
            for (int i = 0; i < (int)g_packs.size(); i++)
                if (!g_cliVeh.empty() && (g_packs[i].path.find("\\" + g_cliVeh + ".json") != std::string::npos ||
                                          g_packs[i].path.find("\\" + g_cliVeh + "_") != std::string::npos)) { pi = i; break; }
            SendMessageA(g_hVeh, CB_SETCURSEL, pi, 0); loadPack(pi);
        }
        SetTimer(h, 1, 3000, 0);
        if (g_cliRun && g_packOk) {                 // --run <module key> "<function>" [inputs...]
            int mi = -1; for (int i = 0; i < (int)g_pack.mods.size(); i++) if (g_pack.mods[i].key == g_cliMod) mi = i;
            if (mi < 0) { logLine("[CLI] module not found: " + g_cliMod); g_auto = true; PostMessageA(h, WM_APP_DONE, 0, 0); return 0; }
            g_curMod = mi; buildEntries(g_pack.mods[mi]);
            int ei = -1; for (int i = 0; i < (int)g_entries.size(); i++) if (g_entries[i].name.find(g_cliFn) != std::string::npos) { ei = i; break; }
            if (ei < 0) { logLine("[CLI] function not found: " + g_cliFn); g_auto = true; PostMessageA(h, WM_APP_DONE, 0, 0); return 0; }
            g_auto = true; startJob(0, {1, ei, g_cliIn});
        } else if (g_auto) startJob(g_autoJob); else kickDetect();
        return 0; }
    case WM_TIMER: if (!g_auto) kickDetect(); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_hStatus) { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_devState == 1 ? RGB(0, 140, 0) : RGB(190, 0, 0)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_APP_DEVSTATE:
        g_devState = (int)w - 10;
        SetWindowTextA(g_hStatus, g_devState == 1 ? "Adapter: CONNECTED" : g_devState == 0 ? "Adapter: driver OK, NO DEVICE (check USB)" : "Adapter: J2534 driver NOT FOUND");
        InvalidateRect(g_hStatus, nullptr, TRUE); return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_VEH && HIWORD(w) == CBN_SELCHANGE) {
            if (g_busy) return 0;
            if (g_hModWnd) DestroyWindow(g_hModWnd);
            loadPack((int)SendMessageA(g_hVeh, CB_GETCURSEL, 0, 0)); return 0; }
        if (id == ID_DETECT) { kickDetect(); return 0; }
        if (id == ID_STOP) { g_stop = true; logLine("[STOP] requested."); return 0; }
        if (id == ID_OPENMOD) { if (g_packOk) openModuleWindow(); return 0; }
        if (id == ID_CLEARALL && MessageBoxA(h, "Clear the fault memory of EVERY control unit?\n\nRun SCAN first - the report keeps the faults.\nIgnition ON, engine OFF.",
                                            "Clear all faults", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_CLEARMOD && MessageBoxA(h, "Clear the fault memory of the selected module?", "Clear module", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_SCAN || id == ID_CLEARALL || id == ID_READMOD || id == ID_CLEARMOD) {
            if (g_devState != 1) logLine("[WARN] Adapter not detected - trying anyway.");
            startJob(id);
        }
        return 0; }
    case WM_APP_LOG: {
        HWND t = (HWND)w; char *s = (char*)l;
        if (t && IsWindow(t)) {
            int len = GetWindowTextLengthA(t); SendMessageA(t, EM_SETSEL, len, len);
            std::string line = std::string(s) + "\r\n"; SendMessageA(t, EM_REPLACESEL, 0, (LPARAM)line.c_str());
        }
        free(s); return 0; }
    case WM_APP_SETOUT: { char *s = (char*)l; if (g_hOut) SetWindowTextA(g_hOut, s); free(s); return 0; }
    case WM_APP_DONE: logLine("[DONE]"); if (g_hOut) post(g_hOut, "[DONE]"); if (g_auto) DestroyWindow(h); return 0;
    case WM_SIZE: if (g_hLog) { RECT rc; GetClientRect(h, &rc); MoveWindow(g_hLog, 10, 252, rc.right - 20, rc.bottom - 262, TRUE); } return 0;
    case WM_DESTROY: g_stop = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmd, int show) {
    if (cmd && strstr(cmd, "--scan")) { g_auto = true; g_autoJob = ID_SCAN; }
    if (cmd && strstr(cmd, "--clear-all")) { g_auto = true; g_autoJob = ID_CLEARALL; }
    {   // simple argument split (quotes group words)
        std::vector<std::string> av; std::string cur; bool q = false;
        for (const char *c = cmd ? cmd : ""; *c; c++) {
            if (*c == '"') { q = !q; continue; }
            if (*c == ' ' && !q) { if (!cur.empty()) av.push_back(cur); cur.clear(); } else cur += *c;
        }
        if (!cur.empty()) av.push_back(cur);
        for (size_t i = 0; i < av.size(); i++) {
            if (av[i] == "--vehicle" && i + 1 < av.size()) g_cliVeh = av[++i];
            else if (av[i] == "--run" && i + 2 < av.size()) { g_cliRun = true; g_cliMod = av[i + 1]; g_cliFn = U8(av[i + 2]); i += 2;
                while (i + 1 < av.size() && av[i + 1].compare(0, 2, "--") != 0) g_cliIn.push_back(atof(av[++i].c_str())); }
        }
    }
    g_sessionLog = fopen((exeDir() + "\\PorscheTool_log_" + stamp().substr(0, 8) + ".txt").c_str(), "a");
    sk::load(exeDir() + "\\porschetool.keys");     // optional local Codierung key file
    WNDCLASSA wc = {0}; wc.hInstance = hi; wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = WndProc; wc.lpszClassName = "PorscheToolWnd"; RegisterClassA(&wc);
    wc.lpfnWndProc = ModProc; wc.lpszClassName = "PTModule"; RegisterClassA(&wc);
    wc.lpfnWndProc = InProc; wc.lpszClassName = "PTInput"; RegisterClassA(&wc);
    g_hMain = CreateWindowA("PorscheToolWnd", APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 820, 0, 0, hi, 0);
    ShowWindow(g_hMain, show); UpdateWindow(g_hMain);
    MSG m; while (GetMessage(&m, 0, 0, 0)) {
        if (g_hModWnd && IsDialogMessageA(g_hModWnd, &m)) continue;
        TranslateMessage(&m); DispatchMessage(&m);
    }
    if (g_sessionLog) fclose(g_sessionLog);
    return 0;
}
