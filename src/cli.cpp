// porschetool-cli: the PorscheTool engine on the command line, for Linux and macOS (and scripts).
// Same engine, data packs and safety rules as the Windows app.
//   Linux : any SocketCAN interface (--can can0)
//   any OS: --demo (simulated car) and --replay <trace> (a session recorded by the Windows app or by --record)
// Build:  c++ -std=c++17 -O2 -Isrc src/cli.cpp -o porschetool-cli       License: MIT.
#include "engine.h"
#include "sim.h"
#include "trace.h"
#include "socketcan_bus.h"
#include <filesystem>
#include <iostream>
#include <memory>

using namespace pt;

static void usage() {
    printf("PorscheTool %s - command line\n\n"
           "porschetool-cli [options] <command>\n\n"
           "Connection (one of):\n"
           "  --can <iface>      Linux SocketCAN interface, e.g. can0 (500 kbit/s, ISO-TP in the kernel)\n"
           "  --demo             simulated car, nothing is sent anywhere\n"
           "  --replay <file>    answer from a recorded session (.ptrace)\n"
           "Options:\n"
           "  --data <dir>       folder with the data packs (default: ./data)\n"
           "  --vehicle <id>     data pack to use (G1, 9x1, 992 ...); default: detected from the VIN\n"
           "  --record <file>    record the session to a trace file\n"
           "  --out <dir>        where reports are written (default: .)\n"
           "  --redact-vin       hide the VIN serial number in reports\n"
           "  --yes              do not ask before functions that change something\n\n"
           "Commands:\n"
           "  vehicles                         list the data packs\n"
           "  detect                           read the VIN and say which pack fits\n"
           "  modules                          control units of the vehicle\n"
           "  functions <module> [text]        functions of a control unit (filter by text)\n"
           "  jobs                             guided jobs of the vehicle\n"
           "  scan                             fault memory of every control unit, text + HTML report\n"
           "  clear-all                        clear every fault memory\n"
           "  faults <module>                  fault memory of one control unit, with status detail\n"
           "  clear <module>                   clear one control unit\n"
           "  run <module> \"<function>\" [v..]  run one function (values for its inputs in order)\n"
           "  live <module> \"<function>\" [n]   repeat a read function n times (default 10)\n"
           "  job <id> [v..]                   run a guided job (values for what it asks)\n", PT_VERSION);
}
static bool confirm(const std::string &what, const std::string &note, bool yes) {
    printf("\n%s\n\n%s\n\n", what.c_str(), note.c_str());
    if (yes) return true;
    printf("Follow the workshop manual for this procedure. Continue? [y/N] "); fflush(stdout);
    std::string a; std::getline(std::cin, a);
    return !a.empty() && (a[0] == 'y' || a[0] == 'Y');
}

int main(int argc, char **argv) {
    std::string data = "data", vehicle, can, replay, record, outDir = "."; bool demo = false, yes = false, redact = false;
    std::vector<std::string> cmd;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") data = next(); else if (a == "--vehicle") vehicle = next(); else if (a == "--can") can = next();
        else if (a == "--replay") replay = next(); else if (a == "--record") record = next(); else if (a == "--out") outDir = next();
        else if (a == "--demo") demo = true; else if (a == "--yes") yes = true; else if (a == "--redact-vin") redact = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else cmd.push_back(a);
    }
    if (cmd.empty()) { usage(); return 2; }

    std::vector<PackInfo> packs; std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(data, ec)) {
        PackInfo pi; if (e.path().extension() == ".json" && readPackHead(e.path().string(), pi)) packs.push_back(pi);
    }
    std::sort(packs.begin(), packs.end(), [](const PackInfo &a, const PackInfo &b) { return a.id != b.id ? a.id < b.id : a.lang > b.lang; });
    if (cmd[0] == "vehicles") {
        for (auto &p : packs) { std::string v; for (auto &x : p.vin) v += x + " ";
            printf("  %-8s %-3s %-52s VIN types: %s\n", p.id.c_str(), p.lang.c_str(), p.title.c_str(), v.c_str()); }
        if (packs.empty()) printf("no data packs in '%s' (see tools/make_packs.py)\n", data.c_str());
        return 0;
    }
    if (packs.empty()) { fprintf(stderr, "no data packs in '%s' - use --data <dir>\n", data.c_str()); return 2; }

    Engine en; en.reportDir = outDir; en.redactVin = redact; en.autoMode = false;
    en.onLog = [](const std::string &s) { printf("%s\n", s.c_str()); fflush(stdout); };

    // ---- connection
    std::unique_ptr<Bus> bus; TraceWriter tw; bool offline = cmd[0] == "modules" || cmd[0] == "functions" || cmd[0] == "jobs";
    if (demo) { bus.reset(new sim::SimBus()); sim::installDemoKeys(); }
    else if (!replay.empty()) { auto rb = new ReplayBus(); bus.reset(rb); if (!rb->load(replay)) { fprintf(stderr, "cannot read the trace %s\n", replay.c_str()); return 2; } }
#ifdef __linux__
    else if (!can.empty()) bus.reset(new SocketCanBus(can));
#endif
    if (!bus && !offline) {
#ifdef __linux__
        fprintf(stderr, "no connection: use --can <iface>, --demo or --replay <file>\n");
#else
        fprintf(stderr, "no connection: on this system use --demo or --replay <file>; a car is reached with --can on Linux or with the Windows app (J2534)\n");
#endif
        return 2;
    }
    if (bus) {
        bus->onError = [](const std::string &s) { fprintf(stderr, "%s\n", s.c_str()); };
        if (!demo && replay.empty()) sk::load("porschetool.keys");
        if (!record.empty() && tw.open(record)) tw.attach(*bus);
        if (!bus->open()) return 1;
    }

    // ---- vehicle
    int pi = -1;
    if (!vehicle.empty()) { for (int i = 0; i < (int)packs.size(); i++) if (packs[i].id == vehicle || packs[i].id + "_" + packs[i].lang == vehicle) { pi = i; break; } }
    else if (bus) { std::string how; pi = en.detectVehicle(*bus, packs, how); printf("Vehicle detection: %s\n", how.c_str()); }
    if (cmd[0] == "detect") { printf(pi >= 0 ? "Data pack: %s - %s\n" : "No data pack identified%s%s\n", pi >= 0 ? packs[pi].id.c_str() : "", pi >= 0 ? packs[pi].title.c_str() : ""); return pi >= 0 ? 0 : 1; }
    if (pi < 0) { fprintf(stderr, "vehicle not identified - choose one with --vehicle <id> (see 'vehicles')\n"); return 2; }
    std::string err;
    if (!en.loadPack(packs[pi].path, err)) { fprintf(stderr, "%s: %s\n", packs[pi].path.c_str(), err.c_str()); return 2; }
    printf("Vehicle: %s - %zu control units, %zu functions, %zu guided jobs\n", en.pack.title.c_str(), en.pack.mods.size(), en.functionCount(), en.pack.jobs.size());

    auto module = [&](const std::string &key) -> int {
        int mi = en.findModule(key);
        if (mi < 0) { std::string k = lowerAscii(key); for (int i = 0; i < (int)en.pack.mods.size() && mi < 0; i++)
            if (lowerAscii(en.pack.mods[i].key).find(k) != std::string::npos || lowerAscii(en.modName(en.pack.mods[i])).find(k) != std::string::npos) mi = i; }
        if (mi < 0) fprintf(stderr, "control unit not found: %s (see 'modules')\n", key.c_str());
        return mi;
    };
    auto entry = [&](int mi, const std::string &name) -> int {
        en.selectModule(mi); std::string k = lowerAscii(name); int part = -1;
        for (int i = 0; i < (int)en.entries.size(); i++) { std::string n = lowerAscii(en.entries[i].name); if (n == k) return i; if (part < 0 && n.find(k) != std::string::npos) part = i; }
        if (part < 0) fprintf(stderr, "function not found: %s (see 'functions %s')\n", name.c_str(), en.pack.mods[mi].key.c_str());
        return part;
    };
    auto nums = [&](size_t from) { pk::Inputs v; for (size_t i = from; i < cmd.size(); i++) v.add(atof(cmd[i].c_str()), cmd[i]); return v; };
    int rc = 0;
    const std::string &c = cmd[0];
    if (c == "modules") {
        for (auto &m : en.pack.mods) {
            if (m.ids.empty()) printf("  %-34s %-44s (sub-bus, not reachable directly)\n", m.key.c_str(), en.modName(m).c_str());
            else if (m.link == 1) printf("  %-34s %-44s K-line %02X\n", m.key.c_str(), en.modName(m).c_str(), m.ids[0].first);
            else printf("  %-34s %-44s %X/%X%s\n", m.key.c_str(), en.modName(m).c_str(), m.ids[0].first, m.ids[0].second, m.sk.empty() ? "" : "  coding key mapped");
        }
    } else if (c == "functions" && cmd.size() >= 2) {
        int mi = module(cmd[1]); if (mi < 0) return 2;
        en.selectModule(mi); std::string f = cmd.size() > 2 ? lowerAscii(cmd[2]) : "";
        static const char *tag[] = {"other", "live", "ident", "actuator", "routine", "read", "WRITE"}; static const char cc[] = {0, 'L', 'I', 'A', 'R', 'C', 'W'};
        for (auto &e : en.entries) { if (!f.empty() && lowerAscii(e.name).find(f) == std::string::npos) continue;
            int t = 0; for (int k = 1; k < 7; k++) if (cc[k] == e.cat) t = k;
            std::vector<std::pair<int, int>> in; en.pack.inputsOf(en.pack.mods[mi].svc[e.main], in);
            printf("  [%-8s] %s%s\n", tag[t], e.name.c_str(), in.empty() ? "" : ("   (" + std::to_string(in.size()) + " input(s))").c_str()); }
    } else if (c == "jobs") {
        for (auto &j : en.pack.jobs) { printf("  %-24s %s\n      %s\n", j.id.c_str(), j.title.c_str(), j.desc.c_str());
            for (auto &s : j.steps) printf("      - %s (%s)%s\n", s.label.c_str(), en.modName(en.pack.mods[s.mod]).c_str(),
                                           Engine::askCount(s) ? ("   asks " + std::to_string(Engine::askCount(s)) + " value(s)").c_str() : ""); }
        if (en.pack.jobs.empty()) printf("  (no guided jobs for this vehicle yet)\n");
    } else if (c == "scan") en.actScanAll(*bus);
    else if (c == "clear-all") { if (confirm("Clear the fault memory of EVERY control unit?", "Run 'scan' first - the report keeps the faults. Ignition ON, engine OFF.", yes)) en.actClearAll(*bus); }
    else if ((c == "faults" || c == "clear") && cmd.size() >= 2) {
        int mi = module(cmd[1]); if (mi < 0) return 2;
        if (c == "faults" || confirm("Clear the fault memory of " + en.modName(en.pack.mods[mi]) + "?", "Ignition ON, engine OFF.", yes)) rc = en.moduleFaults(*bus, mi, c == "clear") ? 0 : 1;
    } else if (c == "run" && cmd.size() >= 3) {
        int mi = module(cmd[1]); if (mi < 0) return 2;
        int ei = entry(mi, cmd[2]); if (ei < 0) return 2;
        const Entry &e = en.entries[ei]; const pk::Module &m = en.pack.mods[mi];
        std::vector<std::pair<int, int>> in; en.pack.inputsOf(m.svc[e.main], in); pk::Inputs v = nums(3);
        if (v.size() < in.size()) { fprintf(stderr, "'%s' needs %zu input value(s):\n", e.name.c_str(), in.size());
            for (auto &x : in) { const pk::Dop &d = en.pack.dops[x.second]; std::string o;
                if (d.cat == pk::C_TEXT) for (auto &s : d.sc) o += " " + pk::Pack::fmtNum(s.lo) + "=" + en.pack.S(s.text);
                fprintf(stderr, "  %s%s%s\n", en.pack.S(x.first).c_str(), d.unit >= 0 ? (" [" + en.pack.S(d.unit) + "]").c_str() : "", o.empty() ? "" : ("  :" + o).c_str()); }
            return 2; }
        if ((e.cat == 'R' || e.cat == 'A' || e.cat == 'W') &&
            !confirm(std::string(e.cat == 'W' ? "WRITE a new value: " : e.cat == 'A' ? "Actuate a component: " : "Start a routine: ") + e.name, safetyNote(e.cat, m.key, en.modName(m), e.name), yes)) return 1;
        if (e.cat == 'A' || e.cat == 'R') en.autoMode = true;          // no STOP button here: bounded hold / follow
        rc = en.runFunction(*bus, mi, ei, v) ? 0 : 1;
    } else if (c == "live" && cmd.size() >= 3) {
        int mi = module(cmd[1]); if (mi < 0) return 2;
        int ei = entry(mi, cmd[2]); if (ei < 0) return 2;
        int n = cmd.size() > 3 ? atoi(cmd[3].c_str()) : 10;
        en.onSetOut = [](const std::string &s) { std::string t; for (char ch : s) if (ch != '\r') t += ch; printf("%s", t.c_str()); fflush(stdout); };
        en.runLive(*bus, mi, {ei}, {}, n > 0 ? n : 10);
    } else if (c == "job" && cmd.size() >= 2) {
        const pk::Job *j = en.findJob(cmd[1]);
        if (!j) { fprintf(stderr, "job not found for this vehicle: %s (see 'jobs')\n", cmd[1].c_str()); return 2; }
        pk::Inputs v = nums(2); std::vector<pk::Inputs> asked; size_t k = 0; int need = 0;
        for (auto &s : j->steps) { int n = Engine::askCount(s); need += n; pk::Inputs a; for (int i = 0; i < n && k < v.size(); i++, k++) a.add(v[k], v.txt[k]); asked.push_back(a); }
        if ((int)v.size() < need) { fprintf(stderr, "job '%s' asks for %d value(s):\n", j->id.c_str(), need);
            for (auto &s : j->steps) { std::vector<std::pair<int, int>> in; en.pack.inputsOf(en.pack.mods[s.mod].svc[s.svc], in);
                for (size_t i = 0; i < in.size(); i++) if (i < s.kind.size() && s.kind[i] == pk::IN_ASK) { const pk::Dop &d = en.pack.dops[in[i].second]; std::string o;
                    if (d.cat == pk::C_TEXT) for (auto &sc : d.sc) o += " " + pk::Pack::fmtNum(sc.lo) + "=" + en.pack.S(sc.text);
                    fprintf(stderr, "  %s%s\n", en.pack.S(in[i].first).c_str(), o.empty() ? "" : ("  :" + o).c_str()); } }
            return 2; }
        if (!confirm("Job: " + j->title + "\n" + j->desc, en.jobNote(*j), yes)) return 1;
        rc = en.runJob(*bus, *j, asked) ? 0 : 1;
    } else { usage(); rc = 2; }
    if (bus) bus->close();
    return rc;
}
