// ============================================================================
//  OpenDiag (Windows) - open-source multi-module diagnostics for Porsche vehicles
//  Reads identification and fault memory of every control unit on the diagnostic CAN,
//  clears faults, and saves a plain-text report. Talks UDS (ISO 14229) and KWP2000
//  (ISO 14230) over ISO 15765 through any J2534 PassThru adapter (tested target:
//  Tactrix OpenPort 2.0).
//
//  Build (from macOS/Linux with mingw-w64):  ./build.sh
//  License: MIT.  Not affiliated with or endorsed by Porsche AG.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include "diag_core.h"
#include "vehicle_g1.h"

#define APP_TITLE "OpenDiag - Porsche Panamera 970 (2010-2016)"

struct Vehicle { const char *name; const Module *mods; const DtcText *dtcs; };
static const Vehicle VEHICLES[] = {
    {"Panamera 970 (2010-2016)", veh_G1::MODULES, veh_G1::DTCS},
};
static int g_vehicle = 0;

static HWND g_hMain = nullptr, g_hLog = nullptr, g_hStatus = nullptr, g_hVeh = nullptr, g_hMod = nullptr;
static volatile bool g_stop = false, g_busy = false, g_detecting = false;
static int g_devState = -1;
static bool g_autoScan = false;   // "OpenDiag.exe --scan": scan all, save report, exit
static int g_autoJob = 0;         // job started by the command line (--scan / --clear-all)
static FILE *g_report = nullptr;
#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_DEVSTATE (WM_APP + 3)

static void logLine(const std::string &s) {
    char *buf = (char*)malloc(s.size() + 1);
    memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_LOG, 0, (LPARAM)buf);
}
static void rep(const std::string &s) { logLine(s); if (g_report) fprintf(g_report, "%s\r\n", s.c_str()); }
static std::string hex(const uint8_t *d, size_t n) {
    static const char *H = "0123456789ABCDEF"; std::string s;
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; s += ' '; }
    return s;
}
static std::string exeDir() { char p[MAX_PATH]; GetModuleFileNameA(0, p, MAX_PATH);
    std::string s(p); size_t k = s.find_last_of("\\/"); return k == std::string::npos ? "." : s.substr(0, k); }
static std::string stamp() { SYSTEMTIME t; GetLocalTime(&t); char b[32];
    sprintf(b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b; }

// ---------------------------------------------------------------------------
// J2534 minimal definitions
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
#define J_TX_MSG_TYPE         0x01
#define J_CLEAR_RX_BUFFER     0x08
// J2534 functions are __stdcall: on 32-bit Windows a wrong convention corrupts the stack.
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

// Installed J2534 driver from HKLM\SOFTWARE\PassThruSupport.04.04\<vendor>\FunctionLibrary (32-bit view).
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
    unsigned long txId = 0, rxId = 0;
    bool connected = false;
    uint8_t lastNrc = 0; bool gotAnswer = false;   // any frame (positive or negative) from the target

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
        if (pConnect(dev, J_ISO15765, 0, 500000, &chan) != J_STATUS_NOERROR) { logLine("[ERROR] PassThruConnect (CAN 500k) failed."); return false; }
        connected = true; return true;
    }
    // Point the ISO-TP flow-control filter at one control unit.
    bool target(unsigned long tx, unsigned long rx) {
        if (hasFilt) { pStopMsgFilter(chan, filt); hasFilt = false; }
        txId = tx; rxId = rx;
        PASSTHRU_MSG mask = {0}, patt = {0}, flow = {0};
        mask.ProtocolID = patt.ProtocolID = flow.ProtocolID = J_ISO15765;
        mask.TxFlags = patt.TxFlags = flow.TxFlags = J_ISO15765_FRAME_PAD;
        mask.DataSize = patt.DataSize = flow.DataSize = 4;
        putId(mask, 0x7FF); putId(patt, rx); putId(flow, tx);
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
        PASSTHRU_MSG m = {0}; m.ProtocolID = J_ISO15765; m.TxFlags = J_ISO15765_FRAME_PAD;
        putId(m, txId); for (size_t i = 0; i < p.size(); i++) m.Data[4 + i] = p[i];
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
    // Send a request, wait for the positive answer (SID + 0x40). Handles "response pending" (NRC 0x78).
    bool xfer(const std::vector<uint8_t> &p, std::vector<uint8_t> &out, unsigned long tmo = 1500) {
        lastNrc = 0; gotAnswer = false;
        if (!send(p)) return false;
        uint8_t want = p[0] + 0x40;
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < tmo) {
            if (!recv(out, tmo)) return false;
            gotAnswer = true;
            if (!out.empty() && out[0] == want) return true;
            if (out.size() >= 3 && out[0] == 0x7F && out[1] == p[0]) {
                lastNrc = out[2];
                if (out[2] == 0x78) { t0 = GetTickCount(); tmo = 5000; continue; }
                return false;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Per-module operations
// ---------------------------------------------------------------------------
struct ModResult { bool present = false; int proto = 0; std::vector<Dtc> dtcs; std::string ident; };

static std::string nrcText(uint8_t n) {
    switch (n) { case 0x11: return "service not supported"; case 0x12: return "sub-function not supported";
    case 0x13: return "wrong length"; case 0x22: return "conditions not correct"; case 0x31: return "out of range";
    case 0x33: return "security access denied"; case 0x7E: case 0x7F: return "not supported in this session";
    default: { char b[16]; sprintf(b, "NRC 0x%02X", n); return b; } }
}

static void identUds(Bus &b, std::string &id) {
    static const struct { uint16_t did; const char *lbl; } D[] = {
        {0xF187, "part no."}, {0xF189, "SW"}, {0xF191, "HW"}, {0xF197, "system"} };
    std::vector<uint8_t> r;
    for (auto &d : D) {
        if (b.xfer({0x22, (uint8_t)(d.did >> 8), (uint8_t)d.did}, r, 800) && r.size() > 3) {
            std::string v = printable(r.data() + 3, r.size() - 3);
            if (!v.empty()) { if (!id.empty()) id += " | "; id += std::string(d.lbl) + " " + v; }
        }
    }
}
static void identKwp(Bus &b, std::string &id) {
    std::vector<uint8_t> r;
    if (b.xfer({0x1A, 0x9B}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
    else if (b.xfer({0x1A, 0x87}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
}

// Read faults from the current target. proto: hint from the database (UDS / KWP / AUTO).
static bool readFaults(Bus &b, int proto, ModResult &mr) {
    std::vector<uint8_t> r;
    auto tryUds = [&]() -> bool {
        if (b.xfer({0x19, 0x02, 0xFF}, r, 800) && parseUds1902(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_UDS; return true; }
        if (b.gotAnswer) mr.present = true;
        return false;
    };
    auto tryKwp = [&]() -> bool {
        std::vector<uint8_t> s; b.xfer({0x10, 0x81}, s, 600);             // KWP default session (harmless)
        if (b.gotAnswer) mr.present = true;
        if (b.xfer({0x18, 0x00, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
        if (b.gotAnswer) mr.present = true;
        if (b.xfer({0x18, 0x02, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
        if (b.gotAnswer) mr.present = true;
        return false;
    };
    if (proto == PROTO_KWP) { if (tryKwp()) return true; return mr.present && tryUds(); }
    if (tryUds()) return true;
    // UDS refused or unknown -> KWP. If nothing answered at all, the module is absent.
    if (!mr.present && proto == PROTO_UDS) return false;
    return tryKwp();
}

static bool clearFaults(Bus &b, int proto) {
    std::vector<uint8_t> r, s;
    if (proto == PROTO_UDS) {
        if (b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000)) return true;
        b.xfer({0x10, 0x03}, s, 1000);                                       // extended session, retry
        bool ok = b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000);
        b.xfer({0x10, 0x01}, s, 1000);
        return ok;
    }
    if (b.xfer({0x14, 0xFF, 0x00}, r, 5000)) return true;
    b.xfer({0x10, 0x89}, s, 1000);                                           // KWP extended, retry
    bool ok = b.xfer({0x14, 0xFF, 0x00}, r, 5000);
    b.xfer({0x10, 0x81}, s, 1000);
    return ok;
}

static void reportModule(const Module &m, const ModResult &mr, const DtcText *tab) {
    char hdr[160];
    sprintf(hdr, "[%03X] %-38s", m.req, m.name);
    if (!mr.present) { rep(std::string(hdr) + " -- no answer (not fitted or asleep)"); return; }
    std::string p = mr.proto == PROTO_UDS ? "UDS" : mr.proto == PROTO_KWP ? "KWP" : "?";
    char n[64]; sprintf(n, " %s  %d fault(s)", p.c_str(), (int)mr.dtcs.size());
    rep(std::string(hdr) + n);
    if (!mr.ident.empty()) rep("        " + mr.ident);
    for (const Dtc &d : mr.dtcs) {
        const DtcText *t = findText(tab, m.key, d);
        std::string txt = t && t->en && t->en[0] ? t->en : (t && t->de && t->de[0] ? t->de : "(code not in catalogue)");
        rep("    * " + dtcName(d) + "  " + txt + "   [" + statusText(d) + "]");
    }
}

static bool scanOne(Bus &b, const Module &m, const DtcText *tab, bool withIdent) {
    ModResult mr;
    uint16_t pairs[2][2] = {{m.req, m.resp}, {m.req2, m.resp2}};
    for (auto &pp : pairs) {
        if (!pp[0] || g_stop) continue;
        if (!b.target(pp[0], pp[1])) return false;
        if (readFaults(b, m.proto, mr) || mr.present) {
            if (withIdent) { if (mr.proto == PROTO_KWP) identKwp(b, mr.ident); else identUds(b, mr.ident); }
            Module mm = m; mm.req = pp[0];
            reportModule(mm, mr, tab); return mr.present;
        }
    }
    reportModule(m, mr, tab);
    return false;
}

static void openReport(const char *what) {
    std::string path = exeDir() + "\\OpenDiag_" + what + "_" + stamp() + ".txt";
    g_report = fopen(path.c_str(), "w");
    if (g_report) logLine("Report: " + path);
}
static void closeReport() { if (g_report) { fclose(g_report); g_report = nullptr; } }

static void actScanAll(Bus &b) {
    const Vehicle &v = VEHICLES[g_vehicle];
    openReport("scan");
    rep("=== SCAN ALL MODULES - " + std::string(v.name) + " - " + stamp() + " ===");
    int found = 0;
    std::vector<std::string> skipped;
    for (const Module *m = v.mods; m->key >= 0 && !g_stop; m++) {
        if (!m->req) { skipped.push_back(m->name); continue; }
        if (scanOne(b, *m, v.dtcs, true)) found++;
    }
    rep("");
    char s[96]; sprintf(s, "Modules answering: %d", found); rep(s);
    if (!skipped.empty()) {
        std::string t = "Not reachable on the diagnostic CAN yet (sub-bus): ";
        for (size_t i = 0; i < skipped.size(); i++) t += (i ? ", " : "") + skipped[i];
        rep(t);
    }
    closeReport();
}

static void actClearAll(Bus &b) {
    const Vehicle &v = VEHICLES[g_vehicle];
    openReport("clear");
    rep("=== CLEAR ALL FAULTS - " + std::string(v.name) + " ===");
    for (const Module *m = v.mods; m->key >= 0 && !g_stop; m++) {
        if (!m->req) continue;
        ModResult mr;
        if (!b.target(m->req, m->resp)) break;
        readFaults(b, m->proto, mr);
        if (!mr.present) continue;
        bool ok = clearFaults(b, mr.proto ? mr.proto : m->proto);
        rep(std::string(ok ? "[OK]    " : "[FAIL]  ") + m->name + (ok ? "" : "  (" + nrcText(b.lastNrc) + ")"));
    }
    rep("Done. Run SCAN again: faults that come straight back are real and still present.");
    closeReport();
}

static void actModule(Bus &b, bool clear) {
    const Vehicle &v = VEHICLES[g_vehicle];
    int sel = (int)SendMessageA(g_hMod, CB_GETCURSEL, 0, 0);
    int k = 0; const Module *m = v.mods;
    for (; m->key >= 0; m++) if (m->req) { if (k == sel) break; k++; }
    if (m->key < 0) { logLine("Choose a module first."); return; }
    if (!clear) { openReport("module"); scanOne(b, *m, v.dtcs, true); closeReport(); return; }
    ModResult mr;
    if (!b.target(m->req, m->resp)) return;
    readFaults(b, m->proto, mr);
    if (!mr.present) { logLine(std::string(m->name) + ": no answer."); return; }
    bool ok = clearFaults(b, mr.proto ? mr.proto : m->proto);
    logLine(std::string(ok ? "[OK] Faults cleared: " : "[FAIL] Clear refused: ") + m->name + (ok ? "" : " (" + nrcText(b.lastNrc) + ")"));
    if (ok) { Sleep(500); scanOne(b, *m, v.dtcs, false); }
}

// ---------------------------------------------------------------------------
// Worker thread, device detection, GUI
// ---------------------------------------------------------------------------
enum { ID_SCAN = 2001, ID_CLEARALL, ID_READMOD, ID_CLEARMOD, ID_STOP, ID_DETECT, IDC_VEH, IDC_MOD, IDC_LOG };
static DWORD WINAPI worker(LPVOID p) {
    int id = (int)(INT_PTR)p; g_busy = true; g_stop = false;
    Bus b;
    if (b.open()) {
        switch (id) {
        case ID_SCAN: actScanAll(b); break;
        case ID_CLEARALL: actClearAll(b); break;
        case ID_READMOD: actModule(b, false); break;
        case ID_CLEARMOD: actModule(b, true); break;
        }
    }
    b.close();
    g_busy = false;
    PostMessageA(g_hMain, WM_APP_DONE, 0, 0);
    return 0;
}
static void startJob(int id) {
    if (g_busy) { logLine("[BUSY] wait for the current action to finish (or press STOP)."); return; }
    HANDLE t = CreateThread(0, 0, worker, (LPVOID)(INT_PTR)id, 0, 0); if (t) CloseHandle(t);
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
static void fillModules() {
    SendMessageA(g_hMod, CB_RESETCONTENT, 0, 0);
    for (const Module *m = VEHICLES[g_vehicle].mods; m->key >= 0; m++)
        if (m->req) { char t[96]; sprintf(t, "%s  [%03X]", m->name, m->req); SendMessageA(g_hMod, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(g_hMod, CB_SETCURSEL, 0, 0);
}
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        int x = 10, y = 10;
        g_hStatus = CreateWindowA("STATIC", "Adapter: checking...", WS_CHILD | WS_VISIBLE, x, y + 3, 400, 20, h, 0, 0, 0);
        CreateWindowA("BUTTON", "Detect", WS_CHILD | WS_VISIBLE, x + 420, y, 80, 24, h, (HMENU)ID_DETECT, 0, 0);
        y += 32;
        CreateWindowA("STATIC", "Vehicle:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hVeh = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, x + 60, y, 440, 200, h, (HMENU)IDC_VEH, 0, 0);
        for (auto &v : VEHICLES) SendMessageA(g_hVeh, CB_ADDSTRING, 0, (LPARAM)v.name);
        SendMessageA(g_hVeh, CB_SETCURSEL, 0, 0);
        y += 34;
        HFONT big = CreateFontA(19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        HWND b1 = CreateWindowA("BUTTON", "SCAN ALL MODULES", WS_CHILD | WS_VISIBLE, x, y, 244, 54, h, (HMENU)ID_SCAN, 0, 0);
        HWND b2 = CreateWindowA("BUTTON", "CLEAR ALL FAULTS", WS_CHILD | WS_VISIBLE, x + 256, y, 244, 54, h, (HMENU)ID_CLEARALL, 0, 0);
        SendMessageA(b1, WM_SETFONT, (WPARAM)big, 1); SendMessageA(b2, WM_SETFONT, (WPARAM)big, 1);
        y += 64;
        CreateWindowA("STATIC", "Module:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hMod = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x + 60, y, 440, 400, h, (HMENU)IDC_MOD, 0, 0);
        fillModules();
        y += 30;
        CreateWindowA("BUTTON", "Read this module", WS_CHILD | WS_VISIBLE, x + 60, y, 140, 26, h, (HMENU)ID_READMOD, 0, 0);
        CreateWindowA("BUTTON", "Clear this module", WS_CHILD | WS_VISIBLE, x + 210, y, 140, 26, h, (HMENU)ID_CLEARMOD, 0, 0);
        CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, x + 360, y, 140, 26, h, (HMENU)ID_STOP, 0, 0);
        y += 36;
        g_hLog = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               x, y, 500, 400, h, (HMENU)IDC_LOG, 0, 0);
        SendMessageA(g_hLog, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        SendMessageA(g_hLog, WM_SETFONT, (WPARAM)f, 1);
        logLine("OpenDiag - open-source diagnostics. Ignition ON, engine off, battery charger recommended.");
        logLine("Reports are saved next to OpenDiag.exe.");
        SetTimer(h, 1, 3000, 0);
        if (g_autoScan) startJob(g_autoJob); else kickDetect();
        return 0; }
    case WM_TIMER: if (!g_autoScan) kickDetect(); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_hStatus) { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_devState == 1 ? RGB(0, 140, 0) : RGB(190, 0, 0)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_APP_DEVSTATE: {
        g_devState = (int)w - 10;
        SetWindowTextA(g_hStatus, g_devState == 1 ? "Adapter: CONNECTED" : g_devState == 0 ?
                       "Adapter: driver OK, NO DEVICE (check USB)" : "Adapter: J2534 driver NOT FOUND");
        InvalidateRect(g_hStatus, nullptr, TRUE); return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_VEH && HIWORD(w) == CBN_SELCHANGE) { g_vehicle = (int)SendMessageA(g_hVeh, CB_GETCURSEL, 0, 0); fillModules(); return 0; }
        if (id == ID_DETECT) { kickDetect(); return 0; }
        if (id == ID_STOP) { g_stop = true; logLine("[STOP] requested."); return 0; }
        if (id == ID_CLEARALL && MessageBoxA(h,
                "Clear the fault memory of EVERY control unit?\n\n"
                "Do this only after the faults are written down (run SCAN first - the report is saved).\n"
                "Ignition ON, engine OFF.", "Clear all faults", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_CLEARMOD && MessageBoxA(h, "Clear the fault memory of the selected module?", "Clear module",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_SCAN || id == ID_CLEARALL || id == ID_READMOD || id == ID_CLEARMOD) {
            if (g_devState != 1) logLine("[WARN] Adapter not detected - trying anyway.");
            startJob(id);
        }
        return 0; }
    case WM_APP_LOG: {
        char *s = (char*)l; int len = GetWindowTextLengthA(g_hLog);
        SendMessageA(g_hLog, EM_SETSEL, len, len);
        std::string line = std::string(s) + "\r\n";
        SendMessageA(g_hLog, EM_REPLACESEL, 0, (LPARAM)line.c_str());
        free(s); return 0; }
    case WM_APP_DONE: logLine("[DONE]"); if (g_autoScan) DestroyWindow(h); return 0;
    case WM_SIZE: if (g_hLog) { RECT rc; GetClientRect(h, &rc); MoveWindow(g_hLog, 10, 206, rc.right - 20, rc.bottom - 216, TRUE); } return 0;
    case WM_DESTROY: g_stop = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmd, int show) {
    if (cmd && strstr(cmd, "--scan")) { g_autoScan = true; g_autoJob = ID_SCAN; }
    if (cmd && strstr(cmd, "--clear-all")) { g_autoScan = true; g_autoJob = ID_CLEARALL; }   // no prompt: for scripted use
    WNDCLASSA wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "OpenDiagWnd";
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    g_hMain = CreateWindowA("OpenDiagWnd", APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 760, 0, 0, hi, 0);
    ShowWindow(g_hMain, show); UpdateWindow(g_hMain);
    MSG m; while (GetMessage(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
