// The SLCAN transport (macOS / Linux serial CAN adapters) end to end, without hardware: a pseudo-terminal
// plays the adapter, behind it the simulated car answers frame by frame (ISO-TP and VW TP 2.0).
//   c++ -std=c++17 -Isrc tests/test_slcan.cpp -o t -lpthread && ./t data/G1_de.json
#include "engine.h"
#include "sim.h"
#include "slcan_bus.h"
#include <atomic>
#include <thread>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
    int master = -1, slave = -1; char name[256];
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) { printf("no pty\n"); return 1; }
    struct termios t; tcgetattr(slave, &t); cfmakeraw(&t); tcsetattr(slave, TCSANOW, &t);
    sim::Car car; sim::IsoTpEcu iso(car); sim::Tp20Ecu tp(car);
    std::atomic<bool> run{true}; std::atomic<int> opened{0}; std::atomic<int> bitrate{0};
    std::thread dev([&]() {                                   // the "adapter": SLCAN text <-> CAN frames
        std::string buf; char b[512];
        auto put = [&](const pt::CanFrame &f) {
            char o[48]; int n = f.id > 0x7FF ? snprintf(o, sizeof o, "T%08X%u", f.id, (unsigned)f.d.size()) : snprintf(o, sizeof o, "t%03X%u", f.id, (unsigned)f.d.size());
            std::string s(o, n); for (uint8_t x : f.d) { snprintf(o, sizeof o, "%02X", x); s += o; } s += "\r";
            if (write(master, s.data(), s.size()) < 0) {}
        };
        while (run) {
            struct pollfd pf = {master, POLLIN, 0};
            if (poll(&pf, 1, 20) <= 0) continue;
            ssize_t r = read(master, b, sizeof b); if (r <= 0) continue; buf.append(b, (size_t)r);
            size_t e;
            while ((e = buf.find('\r')) != std::string::npos) {
                std::string l = buf.substr(0, e); buf.erase(0, e + 1);
                if (l == "O") opened = 1; else if (l == "C") opened = 0; else if (l == "S6") bitrate = 500;
                if (l.empty() || (l[0] != 't' && l[0] != 'T')) { if (write(master, "\r", 1) < 0) {} continue; }
                size_t il = l[0] == 't' ? 3 : 8; pt::CanFrame f; f.id = (uint32_t)strtoul(l.substr(1, il).c_str(), nullptr, 16);
                int n = l[il + 1] - '0'; for (int i = 0; i < n; i++) f.d.push_back((uint8_t)strtoul(l.substr(il + 2 + 2 * i, 2).c_str(), nullptr, 16));
                if (f.id == 0x200 || (!car.ecus.count(f.id) && f.id >= 0x740 && f.id < 0x780)) tp.handle(f); else iso.handle(f);
                while (!tp.out.empty()) { put(tp.out.front()); tp.out.pop_front(); }
                while (!iso.out.empty()) { put(iso.out.front()); iso.out.pop_front(); }
            }
        }
    });
    pt::SlcanBus bus(name); CHECK(bus.open()); pt::sleepMs(100);
    CHECK(opened == 1 && bitrate == 500);
    pt::Engine en; std::string err; en.quick = true; en.reportDir = "build";
    CHECK(en.loadPack(argc > 1 ? argv[1] : "data/G1_de.json", err));
    en.envOnScan = true; en.actScanAll(bus);
    int present = 0, faults = 0; std::string pasmEnv, doorIdent;
    for (auto &r : en.scan) { if (r.present) { present++; faults += (int)r.faults.size(); }
        if (r.req == 0x755 && !r.faults.empty()) pasmEnv = r.faults[0].env; if (r.req == 0x42) doorIdent = r.ident; }
    printf("SLCAN scan: %d units, %d faults, VIN %s\n  PASM: %s\n  door (TP 2.0): %s\n", present, faults, en.vin.c_str(), pasmEnv.c_str(), doorIdent.c_str());
    CHECK(present == 8 && faults == 4 && en.vin == sim::DEMO_VIN);                    // VIN = 20-byte multi-frame answer
    CHECK(pasmEnv.find("Mileage 123456 km") != std::string::npos);                    // 20-byte environment record (multi-frame)
    CHECK(doorIdent.find("Tuersteuergeraet") != std::string::npos);
    remove(en.lastTxt.c_str()); remove(en.lastHtml.c_str());
    bus.close(); run = false; dev.join();
    printf(fails ? "%d FAILURE(S)\n" : "ALL SLCAN TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
