// ============================================================================
//  PorscheTool (Windows) - open-source diagnostics for Porsche vehicles
//  Every control unit: fault memory with environment data, live data (with graph), identification,
//  actuator tests, routines / basic settings, adaptation and coding, plus guided one-click service jobs.
//  Driven by a data pack per vehicle line (data/<LINE>.json); the vehicle is recognised from its VIN.
//  UDS (ISO 14229) and KWP2000 (ISO 14230) over ISO 15765 (11/29-bit CAN) and the K-line, through any
//  J2534 PassThru adapter (tested target: Tactrix OpenPort 2.0).
//
//  This file is only the Win32 user interface. Everything the tool does with the car is in engine.h
//  (portable, tested without a car); the adapter is in j2534_bus.h.
//
//  Build (macOS/Linux with mingw-w64):  ./build.sh        License: MIT.
//  Not affiliated with or endorsed by Porsche AG.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <memory>
#include <cstring>
#include <algorithm>
#include "engine.h"
#include "j2534_bus.h"
#include "sim.h"
#include "trace.h"

#define APP_TITLE "PorscheTool " PT_VERSION " - open-source Porsche diagnostics"
using pt::Engine; using pt::Entry;

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static HWND g_hMain, g_hLog, g_hStatus, g_hVeh, g_hMod, g_hRedact;
static HWND g_hModWnd, g_hModTitle, g_hCat, g_hSearch, g_hList, g_hOut;
static HWND g_hJobWnd, g_hJobList, g_hJobDesc, g_hJobOut, g_hGraph;
static HWND g_outWnd;                            // edit control that receives the output of the running job (null = main log)
static volatile bool g_busy = false, g_detecting = false;
static int g_devState = -1;
static bool g_auto = false; static int g_autoJob = 0, g_autoDetectTried = 0;
static std::string g_cliVeh, g_cliMod, g_cliFn, g_cliJob, g_replay; static pk::Inputs g_cliIn;
static bool g_cliRun = false, g_demo = false;
static FILE *g_sessionLog = nullptr;
static Engine g_en;
static std::vector<pt::PackInfo> g_packs; static int g_curPack = -1;
static std::vector<int> g_shown;                 // indexes into g_en.entries shown in the list
static sim::SimBus g_simBus;                     // --demo: one simulated car for the whole session
static pt::TraceWriter g_trace;
#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_DEVSTATE (WM_APP + 3)
#define WM_APP_SETOUT   (WM_APP + 4)
#define WM_APP_VEHICLE  (WM_APP + 5)

static std::string exeDir() { char p[MAX_PATH]; GetModuleFileNameA(0, p, MAX_PATH);
    std::string s(p); size_t k = s.find_last_of("\\/"); return k == std::string::npos ? "." : s.substr(0, k); }
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
static std::string crlf(const std::string &s) {         // lone \n -> \r\n for edit controls and message boxes
    std::string o; for (size_t i = 0; i < s.size(); i++) { if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) o += '\r'; o += s[i]; } return o;
}
static void post(HWND target, const std::string &utf8) {
    std::string s = A(utf8);
    char *buf = (char*)malloc(s.size() + 1); memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_LOG, (WPARAM)target, (LPARAM)buf);
}
static void logLine(const std::string &s) {             // s is UTF-8
    post(g_hLog, s);
    if (g_sessionLog) { fprintf(g_sessionLog, "%s\r\n", s.c_str()); fflush(g_sessionLog); }
}
static void outLine(const std::string &s) {             // output of the running job (module / jobs window, else the main log)
    post(g_outWnd && IsWindow(g_outWnd) ? g_outWnd : g_hLog, s);
    if (g_sessionLog) { fprintf(g_sessionLog, "%s\r\n", s.c_str()); fflush(g_sessionLog); }
}
static void outSet(const std::string &utf8) {           // replace module output (live view)
    std::string s = A(utf8);
    char *buf = (char*)malloc(s.size() + 1); memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_SETOUT, 0, (LPARAM)buf);
}

// ---------------------------------------------------------------------------
// live-data graph: the numeric values of the running live view, each scaled to its own range
// ---------------------------------------------------------------------------
struct Series { std::string name, unit; std::deque<double> v; };
static std::vector<Series> g_series; static CRITICAL_SECTION g_graphLock; static FILE *g_liveCsv = nullptr; static DWORD g_liveT0 = 0;
static void graphReset() {
    EnterCriticalSection(&g_graphLock); g_series.clear(); LeaveCriticalSection(&g_graphLock);
    if (g_liveCsv) { fclose(g_liveCsv); g_liveCsv = nullptr; }
}
static void graphPush(const std::string &fn, const std::vector<pk::Value> &vals) {
    if (!g_liveCsv) {                                    // every live session is also written to a CSV file
        std::string p = exeDir() + "\\PorscheTool_live_" + pt::stampNow() + ".csv";
        g_liveCsv = fopen(p.c_str(), "w"); g_liveT0 = GetTickCount();
        if (g_liveCsv) fprintf(g_liveCsv, "ms;function;value;number;unit\n");
    }
    EnterCriticalSection(&g_graphLock);
    for (auto &x : vals) {
        if (g_liveCsv && !x.text.empty()) fprintf(g_liveCsv, "%lu;%s;%s;%s;%s\n", GetTickCount() - g_liveT0, fn.c_str(), x.name.c_str(), x.text.c_str(), x.unit.c_str());
        if (std::isnan(x.num)) continue;
        std::string key = x.name.empty() ? fn : x.name;
        Series *s = nullptr; for (auto &q : g_series) if (q.name == key) s = &q;
        if (!s) { if (g_series.size() >= 8) continue; g_series.push_back(Series()); s = &g_series.back(); s->name = key; s->unit = x.unit; }
        s->v.push_back(x.num); if (s->v.size() > 600) s->v.pop_front();
    }
    LeaveCriticalSection(&g_graphLock);
    if (g_liveCsv) fflush(g_liveCsv);
    if (g_hGraph) InvalidateRect(g_hGraph, nullptr, FALSE);
}
static LRESULT CALLBACK GraphProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    static const COLORREF COL[8] = {RGB(0, 114, 189), RGB(217, 83, 25), RGB(60, 160, 60), RGB(126, 47, 142), RGB(200, 150, 0), RGB(0, 150, 160), RGB(162, 20, 47), RGB(90, 90, 90)};
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC wdc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc); HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(dc, bmp);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI"); HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        EnterCriticalSection(&g_graphLock);
        int legendH = 18 * (int)std::max<size_t>(1, g_series.size()) + 8;
        RECT plot = {10, 8, rc.right - 10, rc.bottom - legendH};
        HPEN grid = CreatePen(PS_SOLID, 1, RGB(225, 225, 225)); HGDIOBJ op = SelectObject(dc, grid);
        for (int k = 0; k <= 4; k++) { int y = plot.top + (plot.bottom - plot.top) * k / 4; MoveToEx(dc, plot.left, y, 0); LineTo(dc, plot.right, y); }
        SelectObject(dc, op); DeleteObject(grid);
        if (g_series.empty()) { SetTextColor(dc, RGB(120, 120, 120)); const char *t = "Select one or more live-data values in the module window and press LIVE."; TextOutA(dc, 16, 16, t, (int)strlen(t)); }
        for (size_t i = 0; i < g_series.size(); i++) {
            const Series &s = g_series[i]; if (s.v.empty()) continue;
            double lo = s.v[0], hi = s.v[0]; for (double x : s.v) { lo = std::min(lo, x); hi = std::max(hi, x); }
            double span = hi - lo; if (span < 1e-9) span = 1;
            HPEN pen = CreatePen(PS_SOLID, 2, COL[i % 8]); HGDIOBJ o2 = SelectObject(dc, pen);
            size_t n = s.v.size(); int W = plot.right - plot.left, H = plot.bottom - plot.top - 6;
            for (size_t k = 0; k < n; k++) {
                int x = plot.left + (int)((n > 1 ? (double)k / (n - 1) : 0.0) * W);
                int y = plot.bottom - 3 - (int)((s.v[k] - lo) / span * H);
                if (k == 0) MoveToEx(dc, x, y, 0); else LineTo(dc, x, y);
            }
            SelectObject(dc, o2); DeleteObject(pen);
            char t[320]; snprintf(t, sizeof t, "%s = %s %s      (min %s, max %s)", A(s.name).c_str(), pk::Pack::fmtNum(s.v.back()).c_str(), A(s.unit).c_str(),
                                  pk::Pack::fmtNum(lo).c_str(), pk::Pack::fmtNum(hi).c_str());
            SetTextColor(dc, COL[i % 8]); TextOutA(dc, 14, rc.bottom - legendH + 4 + 18 * (int)i, t, (int)strlen(t));
        }
        LeaveCriticalSection(&g_graphLock);
        SelectObject(dc, of); DeleteObject(f);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
        EndPaint(h, &ps); return 0; }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_hGraph = nullptr; return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}
static void openGraph() {
    if (!g_hGraph) g_hGraph = CreateWindowA("PTGraph", "Live data graph - PorscheTool", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 940, 80, 640, 460, 0, 0, 0, 0);
    SetForegroundWindow(g_hGraph);
}

// ---------------------------------------------------------------------------
// function list of the module window
// ---------------------------------------------------------------------------
static const char *CATS[] = {"All functions", "Live data", "Identification", "Actuator tests", "Routines / basic settings",
                             "Adaptation & coding (read)", "Adaptation & coding (write)"};
static const char CATC[] = {0, 'L', 'I', 'A', 'R', 'C', 'W'};

static void fillList() {
    if (!g_hList) return;
    int c = (int)SendMessageA(g_hCat, CB_GETCURSEL, 0, 0); if (c < 0) c = 0;
    char q[128]; GetWindowTextA(g_hSearch, q, sizeof q);
    std::string ql = q; for (char &ch : ql) ch = (char)tolower((unsigned char)ch);
    SendMessageA(g_hList, LB_RESETCONTENT, 0, 0); g_shown.clear();
    for (int i = 0; i < (int)g_en.entries.size(); i++) {
        const Entry &e = g_en.entries[i];
        if (CATC[c] && e.cat != CATC[c]) continue;
        std::string n = A(e.name), nl = n; for (char &ch : nl) ch = (char)tolower((unsigned char)ch);
        if (!ql.empty() && nl.find(ql) == std::string::npos) continue;
        static const char *tag[] = {"", "[live] ", "[ident] ", "[actuator] ", "[routine] ", "[read] ", "[WRITE] "};
        int ti = 0; for (int k = 1; k < 7; k++) if (CATC[k] == e.cat) ti = k;
        SendMessageA(g_hList, LB_ADDSTRING, 0, (LPARAM)(std::string(c ? "" : tag[ti]) + n).c_str());
        g_shown.push_back(i);
    }
}

// ---------------------------------------------------------------------------
// input dialog (routine / actuator / write parameters, job choices)
// ---------------------------------------------------------------------------
struct InDlg { std::vector<std::pair<int, int>> ins; std::vector<HWND> ctl; pk::Inputs vals; bool ok = false, done = false; };
static InDlg *g_dlg = nullptr;
static LRESULT CALLBACK InProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        if (LOWORD(w) == IDOK && g_dlg) {
            g_dlg->vals = pk::Inputs();
            for (size_t k = 0; k < g_dlg->ins.size(); k++) {
                const pk::Dop &d = g_en.pack.dops[g_dlg->ins[k].second];
                if (d.cat == pk::C_TEXT && !d.sc.empty() && !pk::Pack::isTextual(d)) {
                    int sel = (int)SendMessageA(g_dlg->ctl[k], CB_GETCURSEL, 0, 0); if (sel < 0) sel = 0;
                    g_dlg->vals.add(d.sc[sel].lo);
                } else { char t[512]; GetWindowTextA(g_dlg->ctl[k], t, sizeof t); std::string v = t; for (char &c : v) if (c == ',') c = '.';
                         g_dlg->vals.add(atof(v.c_str()), U8(t)); }
            }
            g_dlg->ok = true;
        }
        if (g_dlg) g_dlg->done = true;
        DestroyWindow(h); return 0;
    }
    if (msg == WM_CLOSE) { if (g_dlg) g_dlg->done = true; DestroyWindow(h); return 0; }
    return DefWindowProcA(h, msg, w, l);
}
// Asks for the inputs of a service. With 'step' (a job step) only the inputs the user has to choose are shown.
static bool askInputs(HWND owner, const pk::Service &s, const std::string &title, pk::Inputs &out, const pk::JobStep *step = nullptr) {
    InDlg d; std::vector<std::pair<int, int>> all; g_en.pack.inputsOf(s, all);
    for (size_t k = 0; k < all.size(); k++)
        if (!step || (k < step->kind.size() && step->kind[k] == pk::IN_ASK)) d.ins.push_back(all[k]);
    out = pk::Inputs();
    if (d.ins.empty()) return true;
    int H = 70 + 30 * (int)d.ins.size();
    HWND h = CreateWindowExA(WS_EX_DLGMODALFRAME, "PTInput", A(title).c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             200, 200, 560, H + 40, owner, 0, 0, 0);
    for (size_t k = 0; k < d.ins.size(); k++) {
        const pk::Dop &dp = g_en.pack.dops[d.ins[k].second];
        int y = 12 + 30 * (int)k;
        bool textual = pk::Pack::isTextual(dp);
        std::string lbl = A(g_en.pack.S(d.ins[k].first));
        if (dp.unit >= 0) lbl += " [" + A(g_en.pack.S(dp.unit)) + "]";
        if (textual) {
            lbl += dp.base == pk::B_ASCII || dp.base == pk::B_UTF8 || dp.base == pk::B_UNI ? " [text" : " [hex bytes";
            lbl += dp.lentype == 0 && dp.bits >= 8 ? ", max " + std::to_string(dp.bits / 8) + "]" : "]";
        }
        CreateWindowA("STATIC", lbl.c_str(), WS_CHILD | WS_VISIBLE, 10, y + 4, 270, 18, h, 0, 0, 0);
        HWND c;
        if (dp.cat == pk::C_TEXT && !dp.sc.empty() && !textual) {
            c = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 285, y, 255, 300, h, 0, 0, 0);
            for (auto &sc : dp.sc) { std::string t = A(g_en.pack.S(sc.text)); if (t.empty()) t = pk::Pack::fmtNum(sc.lo); SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)t.c_str()); }
            SendMessageA(c, CB_SETCURSEL, 0, 0);
        } else c = CreateWindowA("EDIT", textual ? "" : "0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 285, y, 255, 22, h, 0, 0, 0);
        d.ctl.push_back(c);
    }
    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 340, H - 36, 95, 28, h, (HMENU)IDOK, 0, 0);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 445, H - 36, 95, 28, h, (HMENU)IDCANCEL, 0, 0);
    g_dlg = &d; EnableWindow(owner, FALSE);
    MSG msg; while (!d.done && GetMessage(&msg, 0, 0, 0)) { if (!IsDialogMessageA(h, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); } }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner); g_dlg = nullptr;
    out = d.vals; return d.ok;
}

// ---------------------------------------------------------------------------
// worker thread: one action at a time, each on a freshly opened adapter
// ---------------------------------------------------------------------------
enum { ID_SCAN = 2001, ID_CLEARALL, ID_OPENMOD, ID_READMOD, ID_CLEARMOD, ID_STOP, ID_DETECT, ID_AUTOVEH, ID_JOBS, ID_REPORT, IDC_VEH, IDC_MOD, IDC_LOG, IDC_REDACT,
       ID_M_RUN = 2101, ID_M_LIVE, ID_M_STOP, ID_M_FAULTS, ID_M_CLEAR, ID_M_GRAPH, IDC_M_CAT, IDC_M_SEARCH, IDC_M_LIST, IDC_M_OUT,
       ID_J_RUN = 2201, ID_J_STOP, IDC_J_LIST, IDC_J_DESC, IDC_J_OUT,
       JOB_MOD_RUN = 3001, JOB_MOD_LIVE, JOB_MOD_FAULTS, JOB_MOD_CLEAR, JOB_GUIDED };
struct Job { int id; int mod = -1; std::vector<int> sel; pk::Inputs in; int job = -1; std::vector<pk::Inputs> jobIn; HWND outWnd = nullptr; };

static int selectedModule() {
    int k = (int)SendMessageA(g_hMod, CB_GETCURSEL, 0, 0), n = 0;
    for (int i = 0; i < (int)g_en.pack.mods.size(); i++) if (!g_en.pack.mods[i].ids.empty()) { if (n++ == k) return i; }
    return -1;
}
static DWORD WINAPI worker(LPVOID p) {
    Job *j = (Job*)p; g_busy = true; g_en.stop = false; g_outWnd = j->outWnd;
    g_en.redactVin = g_hRedact && SendMessageA(g_hRedact, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::unique_ptr<pt::Bus> own; pt::Bus *b = nullptr;
    if (g_demo) b = &g_simBus;
    else if (!g_replay.empty()) { auto rb = new pt::ReplayBus(); own.reset(rb); if (rb->load(g_replay)) b = rb; else logLine("[ERROR] cannot read the recorded session " + g_replay); }
    else { own.reset(new pt::J2534Bus()); b = own.get(); if (g_trace.active()) g_trace.attach(*b); }
    if (b) b->onError = [](const std::string &s) { logLine(s); };
    int vehicle = -2;
    if (b && b->open()) {
        switch (j->id) {
        case ID_SCAN: g_en.actScanAll(*b); break;
        case ID_CLEARALL: g_en.actClearAll(*b); break;
        case ID_AUTOVEH: { std::string how; vehicle = g_en.detectVehicle(*b, g_packs, how); logLine("Vehicle detection: " + how);
            if (vehicle < 0) logLine("Could not identify the vehicle (ignition on?). Choose it in the list.");
            break; }
        case ID_READMOD: case ID_CLEARMOD: g_en.moduleFaults(*b, j->mod, j->id == ID_CLEARMOD); break;
        case JOB_MOD_FAULTS: case JOB_MOD_CLEAR: g_en.moduleFaults(*b, j->mod, j->id == JOB_MOD_CLEAR); break;
        case JOB_MOD_RUN: if (!j->sel.empty()) g_en.runFunction(*b, j->mod, j->sel[0], j->in); break;
        case JOB_MOD_LIVE: graphReset(); g_en.runLive(*b, j->mod, j->sel, j->in); if (g_liveCsv) { fclose(g_liveCsv); g_liveCsv = nullptr; } break;
        case JOB_GUIDED: if (j->job >= 0 && j->job < (int)g_en.pack.jobs.size()) g_en.runJob(*b, g_en.pack.jobs[j->job], j->jobIn); break;
        }
        b->close();
    }
    if (b) b->tap = nullptr;
    delete j; g_busy = false;
    if (vehicle > -2) PostMessageA(g_hMain, WM_APP_VEHICLE, (WPARAM)(vehicle + 1), 0);
    PostMessageA(g_hMain, WM_APP_DONE, 0, 0);
    return 0;
}
static bool startJob(Job j) {
    if (g_busy) { logLine("[BUSY] wait for the current action to finish (or press STOP)."); return false; }
    if (!g_en.packOk && j.id != ID_AUTOVEH) { logLine("[ERROR] no vehicle data pack loaded."); return false; }
    Job *p = new Job(j);
    g_busy = true;
    HANDLE t = CreateThread(0, 0, worker, p, 0, 0); if (t) CloseHandle(t); else { g_busy = false; delete p; return false; }
    return true;
}
static DWORD WINAPI detectThread(LPVOID) {
    int st = pt::j2534().detect(); g_detecting = false;
    if (g_hMain) PostMessageA(g_hMain, WM_APP_DEVSTATE, (WPARAM)(st + 10), 0);
    return 0;
}
static void kickDetect() {
    if (g_demo || !g_replay.empty()) { PostMessageA(g_hMain, WM_APP_DEVSTATE, 11, 0); return; }
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
        do { pt::PackInfo pi; if (pt::readPackHead(dir + "\\" + fd.cFileName, pi)) g_packs.push_back(pi); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const pt::PackInfo &a, const pt::PackInfo &b) { return a.title != b.title ? a.title < b.title : a.lang > b.lang; });
}
static void fillModules() {
    SendMessageA(g_hMod, CB_RESETCONTENT, 0, 0);
    for (auto &m : g_en.pack.mods) if (!m.ids.empty()) {
        char t[240]; snprintf(t, sizeof t, m.link == 1 ? "%s  [K-line %02X]" : m.link == 2 ? "%s  [TP2.0 %02X]" : "%s  [%03X]", A(g_en.modName(m)).c_str(), m.ids[0].first);
        SendMessageA(g_hMod, CB_ADDSTRING, 0, (LPARAM)t);
    }
    SendMessageA(g_hMod, CB_SETCURSEL, 0, 0);
}
static bool loadPack(int i) {
    if (i < 0 || i >= (int)g_packs.size()) return false;
    if (g_hModWnd) DestroyWindow(g_hModWnd);
    if (g_hJobWnd) DestroyWindow(g_hJobWnd);
    std::string err;
    if (!g_en.loadPack(g_packs[i].path, err)) { logLine("[ERROR] " + g_packs[i].path + ": " + err); return false; }
    g_curPack = i;
    logLine("Vehicle: " + g_en.pack.title + " - " + std::to_string(g_en.pack.mods.size()) + " control units, " + std::to_string(g_en.functionCount()) +
            " functions, " + std::to_string(g_en.pack.jobs.size()) + " guided jobs.");
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
        CreateWindowA("STATIC", "Ctrl/Shift+click: several for LIVE", WS_CHILD | WS_VISIBLE, x + 575, y + 4, 190, 18, h, 0, 0, 0);
        y += 30;
        g_hList = CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL,
                                x, y, 760, 230, h, (HMENU)IDC_M_LIST, 0, 0);
        y += 236;
        CreateWindowA("BUTTON", "RUN", WS_CHILD | WS_VISIBLE, x, y, 110, 30, h, (HMENU)ID_M_RUN, 0, 0);
        CreateWindowA("BUTTON", "LIVE (repeat)", WS_CHILD | WS_VISIBLE, x + 116, y, 110, 30, h, (HMENU)ID_M_LIVE, 0, 0);
        CreateWindowA("BUTTON", "GRAPH", WS_CHILD | WS_VISIBLE, x + 232, y, 90, 30, h, (HMENU)ID_M_GRAPH, 0, 0);
        CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, x + 328, y, 110, 30, h, (HMENU)ID_M_STOP, 0, 0);
        CreateWindowA("BUTTON", "Read faults", WS_CHILD | WS_VISIBLE, x + 460, y, 115, 30, h, (HMENU)ID_M_FAULTS, 0, 0);
        CreateWindowA("BUTTON", "Clear faults", WS_CHILD | WS_VISIBLE, x + 582, y, 115, 30, h, (HMENU)ID_M_CLEAR, 0, 0);
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
        if (id == ID_M_STOP) { g_en.stop = true; return 0; }
        if (id == ID_M_GRAPH) { openGraph(); return 0; }
        if (id == ID_M_FAULTS) { Job j; j.id = JOB_MOD_FAULTS; j.mod = g_en.curMod; j.outWnd = g_hOut; startJob(j); return 0; }
        if (id == ID_M_CLEAR) {
            if (MessageBoxA(h, "Clear the fault memory of this module?", "Clear faults", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                Job j; j.id = JOB_MOD_CLEAR; j.mod = g_en.curMod; j.outWnd = g_hOut; startJob(j); }
            return 0; }
        if (id == ID_M_RUN || id == ID_M_LIVE || (id == IDC_M_LIST && HIWORD(w) == LBN_DBLCLK)) {
            if (g_busy) { MessageBoxA(h, "Another action is still running. Press STOP first.", "PorscheTool", MB_OK); return 0; }
            int cnt = (int)SendMessageA(g_hList, LB_GETSELCOUNT, 0, 0);
            if (cnt <= 0) { MessageBoxA(h, "Choose a function in the list first.", "PorscheTool", MB_OK); return 0; }
            std::vector<int> items(cnt); SendMessageA(g_hList, LB_GETSELITEMS, cnt, (LPARAM)items.data());
            Job j; j.mod = g_en.curMod; j.outWnd = g_hOut;
            for (int it : items) if (it >= 0 && it < (int)g_shown.size()) j.sel.push_back(g_shown[it]);
            if (j.sel.empty()) return 0;
            const pk::Module &m = g_en.pack.mods[g_en.curMod];
            if (id == ID_M_LIVE) {
                for (int ei : j.sel) { char c = g_en.entries[ei].cat; if (c == 'R' || c == 'A' || c == 'W') {
                    MessageBoxA(h, "LIVE repeats read functions only. For a routine, actuator test or write use RUN.", "PorscheTool", MB_OK); return 0; } }
                if (j.sel.size() == 1 && !askInputs(h, m.svc[g_en.entries[j.sel[0]].main], g_en.entries[j.sel[0]].name, j.in)) return 0;
                SetWindowTextA(g_hOut, ""); j.id = JOB_MOD_LIVE; startJob(j); return 0;
            }
            j.sel.resize(1); const Entry &e = g_en.entries[j.sel[0]];
            if (!askInputs(h, m.svc[e.main], e.name, j.in)) return 0;
            if (e.cat == 'R' || e.cat == 'A' || e.cat == 'W') {
                std::string q = (e.cat == 'W' ? "WRITE a new value into the control unit:\n\n" : e.cat == 'A' ? "Actuate a component:\n\n" : "Start a routine:\n\n")
                    + A(e.name) + "\n\n" + pt::safetyNote(e.cat, m.key, g_en.modName(m), e.name) + "\n\nFollow the workshop manual for this procedure.\n\nContinue?";
                if (MessageBoxA(h, q.c_str(), "Confirm", MB_YESNO | (e.cat == 'W' ? MB_ICONWARNING : MB_ICONQUESTION) | MB_DEFBUTTON2) != IDYES) return 0;
            }
            j.id = JOB_MOD_RUN; startJob(j);
            return 0; }
        return 0; }
    case WM_CLOSE: g_en.stop = true; DestroyWindow(h); return 0;
    case WM_DESTROY: g_hModWnd = nullptr; g_hList = nullptr; g_hOut = nullptr; return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}
static void openModuleWindow() {
    if (g_busy) { logLine("[BUSY] wait for the current action to finish (or press STOP)."); return; }
    int mi = selectedModule(); if (mi < 0) return;
    g_en.selectModule(mi); const pk::Module &m = g_en.pack.mods[mi];
    if (!g_hModWnd) g_hModWnd = CreateWindowA("PTModule", "", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 80, 800, 720, 0, 0, 0, 0);
    char t[256]; snprintf(t, sizeof t, "%s   [%03X/%03X]   %d functions", A(g_en.modName(m)).c_str(), m.ids[0].first, m.ids[0].second, (int)g_en.entries.size());
    SetWindowTextA(g_hModTitle, t); SetWindowTextA(g_hModWnd, (A(g_en.modName(m)) + " - PorscheTool").c_str());
    SetWindowTextA(g_hOut, "");
    fillList(); SetForegroundWindow(g_hModWnd);
}

// ---------------------------------------------------------------------------
// guided jobs window: pick a job, read what it does, one button runs it from start to finish
// ---------------------------------------------------------------------------
static void showJobDesc() {
    int k = (int)SendMessageA(g_hJobList, LB_GETCURSEL, 0, 0);
    if (k < 0 || k >= (int)g_en.pack.jobs.size()) { SetWindowTextA(g_hJobDesc, ""); return; }
    const pk::Job &j = g_en.pack.jobs[k];
    std::string t = j.desc + "\r\n\r\nSteps:";
    for (size_t i = 0; i < j.steps.size(); i++) t += "\r\n  " + std::to_string(i + 1) + ". " + j.steps[i].label + "   (" + g_en.modName(g_en.pack.mods[j.steps[i].mod]) + ")";
    SetWindowTextA(g_hJobDesc, A(t).c_str());
}
static LRESULT CALLBACK JobProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        HFONT big = CreateFontA(19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        HFONT mid = CreateFontA(17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        g_hJobList = CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 10, 10, 300, 230, h, (HMENU)IDC_J_LIST, 0, 0);
        SendMessageA(g_hJobList, WM_SETFONT, (WPARAM)mid, 1);
        g_hJobDesc = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 320, 10, 450, 180, h, (HMENU)IDC_J_DESC, 0, 0);
        SendMessageA(g_hJobDesc, WM_SETFONT, (WPARAM)mid, 1);
        HWND r = CreateWindowA("BUTTON", "RUN THIS JOB", WS_CHILD | WS_VISIBLE, 320, 198, 300, 42, h, (HMENU)ID_J_RUN, 0, 0);
        SendMessageA(r, WM_SETFONT, (WPARAM)big, 1);
        HWND s = CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, 630, 198, 140, 42, h, (HMENU)ID_J_STOP, 0, 0);
        SendMessageA(s, WM_SETFONT, (WPARAM)big, 1);
        g_hJobOut = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 250, 760, 300, h, (HMENU)IDC_J_OUT, 0, 0);
        SendMessageA(g_hJobOut, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        SendMessageA(g_hJobOut, WM_SETFONT, (WPARAM)f, 1);
        return 0; }
    case WM_SIZE: { RECT rc; GetClientRect(h, &rc);
        if (g_hJobOut) MoveWindow(g_hJobOut, 10, 250, rc.right - 20, rc.bottom - 260, TRUE);
        if (g_hJobDesc) MoveWindow(g_hJobDesc, 320, 10, rc.right - 330, 180, TRUE);
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_J_LIST && HIWORD(w) == LBN_SELCHANGE) { showJobDesc(); return 0; }
        if (id == ID_J_STOP) { g_en.stop = true; return 0; }
        if (id == ID_J_RUN || (id == IDC_J_LIST && HIWORD(w) == LBN_DBLCLK)) {
            if (g_busy) { MessageBoxA(h, "Another action is still running. Press STOP first.", "PorscheTool", MB_OK); return 0; }
            int k = (int)SendMessageA(g_hJobList, LB_GETCURSEL, 0, 0);
            if (k < 0 || k >= (int)g_en.pack.jobs.size()) { MessageBoxA(h, "Choose a job in the list first.", "PorscheTool", MB_OK); return 0; }
            const pk::Job &jb = g_en.pack.jobs[k];
            Job j; j.id = JOB_GUIDED; j.job = k; j.outWnd = g_hJobOut;
            for (auto &st : jb.steps) {                       // everything the user has to choose is asked before anything runs
                pk::Inputs v;
                if (Engine::askCount(st) > 0 && !askInputs(h, g_en.pack.mods[st.mod].svc[st.svc], st.label.empty() ? jb.title : st.label, v, &st)) return 0;
                j.jobIn.push_back(v);
            }
            std::string q = A(jb.title) + "\n\n" + g_en.jobNote(jb) + "\n\nFollow the workshop manual for this procedure.\n\nStart the job?";
            if (MessageBoxA(h, q.c_str(), "Confirm job", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
            SetWindowTextA(g_hJobOut, ""); startJob(j);
            return 0; }
        return 0; }
    case WM_CLOSE: g_en.stop = true; DestroyWindow(h); return 0;
    case WM_DESTROY: g_hJobWnd = nullptr; g_hJobList = g_hJobDesc = g_hJobOut = nullptr; return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}
static void openJobsWindow() {
    if (g_en.pack.jobs.empty()) { MessageBoxA(g_hMain, "This vehicle's data pack has no guided jobs yet.\nUse OPEN MODULE for the individual functions.", "Service jobs", MB_OK | MB_ICONINFORMATION); return; }
    if (!g_hJobWnd) g_hJobWnd = CreateWindowA("PTJobs", "Service jobs - PorscheTool", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 100, 800, 620, 0, 0, 0, 0);
    SendMessageA(g_hJobList, LB_RESETCONTENT, 0, 0);
    for (auto &j : g_en.pack.jobs) SendMessageA(g_hJobList, LB_ADDSTRING, 0, (LPARAM)A(j.title).c_str());
    SendMessageA(g_hJobList, LB_SETCURSEL, 0, 0); showJobDesc();
    SetForegroundWindow(g_hJobWnd);
}

// ---------------------------------------------------------------------------
// main window
// ---------------------------------------------------------------------------
static void selectPack(int i) { if (i >= 0 && i < (int)g_packs.size()) { SendMessageA(g_hVeh, CB_SETCURSEL, i, 0); loadPack(i); } }
static void runCli() {                              // scripted run: --scan / --clear-all / --run / --job
    if (!g_en.packOk) { logLine("[CLI] no vehicle data pack."); PostMessageA(g_hMain, WM_APP_DONE, 0, 0); return; }
    Job j;
    if (g_cliRun) {
        int mi = g_en.findModule(g_cliMod);
        if (mi < 0) { logLine("[CLI] module not found: " + g_cliMod); PostMessageA(g_hMain, WM_APP_DONE, 0, 0); return; }
        g_en.selectModule(mi);
        int ei = -1; for (int i = 0; i < (int)g_en.entries.size(); i++) if (g_en.entries[i].name.find(g_cliFn) != std::string::npos) { ei = i; break; }
        if (ei < 0) { logLine("[CLI] function not found: " + g_cliFn); PostMessageA(g_hMain, WM_APP_DONE, 0, 0); return; }
        j.id = JOB_MOD_RUN; j.mod = mi; j.sel = {ei}; j.in = g_cliIn;
    } else if (!g_cliJob.empty()) {
        int k = -1; for (int i = 0; i < (int)g_en.pack.jobs.size(); i++) if (g_en.pack.jobs[i].id == g_cliJob) k = i;
        if (k < 0) { logLine("[CLI] job not found in this pack: " + g_cliJob); PostMessageA(g_hMain, WM_APP_DONE, 0, 0); return; }
        j.id = JOB_GUIDED; j.job = k; j.jobIn.push_back(g_cliIn);
    } else j.id = g_autoJob;
    if (j.id == ID_READMOD || j.id == ID_CLEARMOD) j.mod = selectedModule();
    if (!startJob(j)) PostMessageA(g_hMain, WM_APP_DONE, 0, 0);
}
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = h;
        int x = 10, y = 10;
        g_hStatus = CreateWindowA("STATIC", "Adapter: checking...", WS_CHILD | WS_VISIBLE, x, y + 3, 420, 20, h, 0, 0, 0);
        CreateWindowA("BUTTON", "Detect", WS_CHILD | WS_VISIBLE, x + 440, y, 80, 24, h, (HMENU)ID_DETECT, 0, 0);
        y += 32;
        CreateWindowA("STATIC", "Vehicle:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hVeh = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x + 60, y, 350, 400, h, (HMENU)IDC_VEH, 0, 0);
        CreateWindowA("BUTTON", "AUTO-DETECT", WS_CHILD | WS_VISIBLE, x + 416, y - 1, 104, 26, h, (HMENU)ID_AUTOVEH, 0, 0);
        y += 34;
        HFONT big = CreateFontA(19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        HWND b1 = CreateWindowA("BUTTON", "SCAN ALL MODULES", WS_CHILD | WS_VISIBLE, x, y, 254, 50, h, (HMENU)ID_SCAN, 0, 0);
        HWND b2 = CreateWindowA("BUTTON", "CLEAR ALL FAULTS", WS_CHILD | WS_VISIBLE, x + 266, y, 254, 50, h, (HMENU)ID_CLEARALL, 0, 0);
        SendMessageA(b1, WM_SETFONT, (WPARAM)big, 1); SendMessageA(b2, WM_SETFONT, (WPARAM)big, 1);
        y += 56;
        HWND b4 = CreateWindowA("BUTTON", "SERVICE JOBS (service reset, brake bleeding, ...)", WS_CHILD | WS_VISIBLE, x, y, 520, 44, h, (HMENU)ID_JOBS, 0, 0);
        SendMessageA(b4, WM_SETFONT, (WPARAM)big, 1);
        y += 54;
        CreateWindowA("STATIC", "Module:", WS_CHILD | WS_VISIBLE, x, y + 4, 56, 16, h, 0, 0, 0);
        g_hMod = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x + 60, y, 460, 500, h, (HMENU)IDC_MOD, 0, 0);
        y += 30;
        HWND b3 = CreateWindowA("BUTTON", "OPEN MODULE (live data, tests, routines, coding)", WS_CHILD | WS_VISIBLE, x, y, 520, 40, h, (HMENU)ID_OPENMOD, 0, 0);
        SendMessageA(b3, WM_SETFONT, (WPARAM)big, 1);
        y += 46;
        CreateWindowA("BUTTON", "Read faults", WS_CHILD | WS_VISIBLE, x, y, 124, 26, h, (HMENU)ID_READMOD, 0, 0);
        CreateWindowA("BUTTON", "Clear faults", WS_CHILD | WS_VISIBLE, x + 132, y, 124, 26, h, (HMENU)ID_CLEARMOD, 0, 0);
        CreateWindowA("BUTTON", "Open last report", WS_CHILD | WS_VISIBLE, x + 264, y, 124, 26, h, (HMENU)ID_REPORT, 0, 0);
        CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE, x + 396, y, 124, 26, h, (HMENU)ID_STOP, 0, 0);
        y += 32;
        g_hRedact = CreateWindowA("BUTTON", "Hide the VIN serial number in saved reports", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, 400, 20, h, (HMENU)IDC_REDACT, 0, 0);
        if (g_en.redactVin) SendMessageA(g_hRedact, BM_SETCHECK, BST_CHECKED, 0);
        y += 26;
        g_hLog = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               x, y, 520, 400, h, (HMENU)IDC_LOG, 0, 0);
        SendMessageA(g_hLog, EM_SETLIMITTEXT, 8 * 1024 * 1024, 0);
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
        SendMessageA(g_hLog, WM_SETFONT, (WPARAM)f, 1);
        logLine("PorscheTool " PT_VERSION " - open-source diagnostics. Ignition ON, engine off, battery charger recommended.");
        if (g_demo) logLine("DEMO MODE: a simulated Panamera answers instead of a car. Nothing is sent to an adapter.");
        if (!g_replay.empty()) logLine("REPLAY MODE: answers come from the recorded session " + g_replay);
        if (sk::loaded()) logLine("Coding keys: " + std::to_string(sk::keys().size()) + " algorithms loaded.");
        findPacks();
        for (auto &p : g_packs) SendMessageA(g_hVeh, CB_ADDSTRING, 0, (LPARAM)(A(p.title) + (p.lang == "de" ? "  [original texts]" : "")).c_str());
        if (g_packs.empty()) logLine("[ERROR] No vehicle data found. Put the 'data' folder next to PorscheTool.exe.");
        else {
            int pi = 0;
            for (int i = 0; i < (int)g_packs.size(); i++)
                if (!g_cliVeh.empty() && g_cliVeh != "auto" && (g_packs[i].path.find("\\" + g_cliVeh + ".json") != std::string::npos ||
                                                                g_packs[i].path.find("\\" + g_cliVeh + "_") != std::string::npos)) { pi = i; break; }
            SendMessageA(g_hVeh, CB_SETCURSEL, pi, 0); loadPack(pi);
        }
        SetTimer(h, 1, 3000, 0);
        if (g_auto) { if (g_cliVeh == "auto") { Job j; j.id = ID_AUTOVEH; if (!startJob(j)) PostMessageA(h, WM_APP_DONE, 0, 0); } else runCli(); }
        else kickDetect();
        return 0; }
    case WM_TIMER: if (!g_auto) kickDetect(); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_hStatus) { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_devState == 1 ? RGB(0, 140, 0) : RGB(190, 0, 0)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_APP_DEVSTATE: {
        int prev = g_devState; g_devState = (int)w - 10;
        SetWindowTextA(g_hStatus, g_demo ? "Adapter: DEMO - simulated car" : !g_replay.empty() ? "Adapter: REPLAY of a recorded session" :
                       g_devState == 1 ? "Adapter: CONNECTED" : g_devState == 0 ? "Adapter: driver OK, NO DEVICE (check USB)" : "Adapter: J2534 driver NOT FOUND");
        InvalidateRect(g_hStatus, nullptr, TRUE);
        // first time the adapter is there: find out which car it is, so nobody has to pick it from the list
        if (g_devState == 1 && prev != 1 && !g_autoDetectTried && !g_auto && g_cliVeh.empty() && !g_busy && g_packs.size() > 1) {
            g_autoDetectTried = 1; Job j; j.id = ID_AUTOVEH; startJob(j); }
        return 0; }
    case WM_APP_VEHICLE: {
        int k = (int)w - 1;
        if (k >= 0 && k < (int)g_packs.size()) { if (k != g_curPack) selectPack(k); else logLine("Vehicle confirmed: " + g_en.pack.title); }
        if (g_auto && g_cliVeh == "auto") { g_cliVeh = "-"; if (k >= 0) { runCli(); return 0; } }
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_VEH && HIWORD(w) == CBN_SELCHANGE) {
            if (g_busy) { SendMessageA(g_hVeh, CB_SETCURSEL, g_curPack, 0); return 0; }
            g_autoDetectTried = 1; loadPack((int)SendMessageA(g_hVeh, CB_GETCURSEL, 0, 0)); return 0; }
        if (id == ID_DETECT) { kickDetect(); return 0; }
        if (id == ID_AUTOVEH) { g_autoDetectTried = 1; Job j; j.id = ID_AUTOVEH; startJob(j); return 0; }
        if (id == ID_STOP) { g_en.stop = true; logLine("[STOP] requested."); return 0; }
        if (id == ID_OPENMOD) { if (g_en.packOk) openModuleWindow(); return 0; }
        if (id == ID_JOBS) { if (g_en.packOk) openJobsWindow(); return 0; }
        if (id == ID_REPORT) {
            if (g_en.lastHtml.empty()) logLine("No report yet - press SCAN ALL MODULES first.");
            else ShellExecuteA(h, "open", g_en.lastHtml.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0; }
        if (id == ID_CLEARALL && MessageBoxA(h, "Clear the fault memory of EVERY control unit?\n\nRun SCAN first - the report keeps the faults.\nIgnition ON, engine OFF.",
                                            "Clear all faults", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_CLEARMOD && MessageBoxA(h, "Clear the fault memory of the selected module?", "Clear module", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
        if (id == ID_SCAN || id == ID_CLEARALL || id == ID_READMOD || id == ID_CLEARMOD) {
            if (g_devState != 1) logLine("[WARN] Adapter not detected - trying anyway.");
            Job j; j.id = id; j.mod = selectedModule(); startJob(j);
        }
        return 0; }
    case WM_APP_LOG: {
        HWND t = (HWND)w; char *s = (char*)l;
        if (t && IsWindow(t)) {
            int len = GetWindowTextLengthA(t); SendMessageA(t, EM_SETSEL, len, len);
            std::string line = crlf(s) + "\r\n"; SendMessageA(t, EM_REPLACESEL, 0, (LPARAM)line.c_str());
        }
        free(s); return 0; }
    case WM_APP_SETOUT: { char *s = (char*)l; if (g_hOut) SetWindowTextA(g_hOut, s); free(s); return 0; }
    case WM_APP_DONE:
        if (g_busy) return 0;                            // a follow-up action was started (auto-detect -> scripted job)
        logLine("[DONE]"); if (g_outWnd && IsWindow(g_outWnd)) post(g_outWnd, "[DONE]");
        if (g_auto) DestroyWindow(h);
        return 0;
    case WM_SIZE: if (g_hLog) { RECT rc; GetClientRect(h, &rc); MoveWindow(g_hLog, 10, 320, rc.right - 20, rc.bottom - 330, TRUE); } return 0;
    case WM_DESTROY: g_en.stop = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmd, int show) {
    {   // simple argument split (quotes group words)
        std::vector<std::string> av; std::string cur; bool q = false;
        for (const char *c = cmd ? cmd : ""; *c; c++) {
            if (*c == '"') { q = !q; continue; }
            if (*c == ' ' && !q) { if (!cur.empty()) av.push_back(cur); cur.clear(); } else cur += *c;
        }
        if (!cur.empty()) av.push_back(cur);
        auto nums = [&](size_t &i) { while (i + 1 < av.size() && av[i + 1].compare(0, 2, "--") != 0) { ++i; g_cliIn.add(atof(av[i].c_str()), U8(av[i])); } };
        for (size_t i = 0; i < av.size(); i++) {
            if (av[i] == "--scan") { g_auto = true; g_autoJob = ID_SCAN; }
            else if (av[i] == "--clear-all") { g_auto = true; g_autoJob = ID_CLEARALL; }
            else if (av[i] == "--demo") g_demo = true;
            else if (av[i] == "--redact-vin") g_en.redactVin = true;
            else if (av[i] == "--replay" && i + 1 < av.size()) g_replay = av[++i];
            else if (av[i] == "--vehicle" && i + 1 < av.size()) g_cliVeh = av[++i];
            else if (av[i] == "--job" && i + 1 < av.size()) { g_auto = true; g_cliJob = av[++i]; nums(i); }
            else if (av[i] == "--run" && i + 2 < av.size()) { g_auto = true; g_cliRun = true; g_cliMod = av[i + 1]; g_cliFn = U8(av[i + 2]); i += 2; nums(i); }
        }
    }
    InitializeCriticalSection(&g_graphLock);
    std::string dir = exeDir(), day = pt::stampNow().substr(0, 8);
    g_sessionLog = fopen((dir + "\\PorscheTool_log_" + day + ".txt").c_str(), "a");
    g_en.reportDir = dir; g_en.autoMode = g_auto;
    g_en.onLog = logLine; g_en.onOut = outLine; g_en.onSetOut = outSet; g_en.onLive = graphPush;
    if (g_demo) sim::installDemoKeys();              // the simulator's own test keys, never the real key file
    else sk::load(dir + "\\porschetool.keys");       // optional local coding key file
    if (!g_demo && g_replay.empty()) g_trace.open(dir + "\\PorscheTool_trace_" + day + ".ptrace");      // session recording, always on
    WNDCLASSA wc = {0}; wc.hInstance = hi; wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = WndProc; wc.lpszClassName = "PorscheToolWnd"; RegisterClassA(&wc);
    wc.lpfnWndProc = ModProc; wc.lpszClassName = "PTModule"; RegisterClassA(&wc);
    wc.lpfnWndProc = JobProc; wc.lpszClassName = "PTJobs"; RegisterClassA(&wc);
    wc.lpfnWndProc = InProc; wc.lpszClassName = "PTInput"; RegisterClassA(&wc);
    wc.lpfnWndProc = GraphProc; wc.lpszClassName = "PTGraph"; wc.hbrBackground = nullptr; RegisterClassA(&wc);
    HWND main = CreateWindowA("PorscheToolWnd", APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 880, 0, 0, hi, 0);
    ShowWindow(main, show); UpdateWindow(main);
    MSG m; while (GetMessage(&m, 0, 0, 0)) {
        if (g_hModWnd && IsDialogMessageA(g_hModWnd, &m)) continue;
        if (g_hJobWnd && IsDialogMessageA(g_hJobWnd, &m)) continue;
        TranslateMessage(&m); DispatchMessage(&m);
    }
    if (g_liveCsv) fclose(g_liveCsv);
    g_trace.close();
    if (g_sessionLog) fclose(g_sessionLog);
    return 0;
}
