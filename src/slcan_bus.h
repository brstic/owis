// SLCAN (Lawicel serial-line CAN) transport for macOS and Linux: CANable / CANtact / USBtin and the many
// other USB-CAN adapters that show up as a serial port (/dev/cu.usbmodem... on macOS, /dev/ttyACM0 on Linux).
// ISO-TP and VW TP 2.0 run in user space on top of the frames. K-line units are not reachable this way.
//   setup:  C (close)  S6 (500 kbit/s)  O (open)
//   frames: t<id 3 hex><len><data hex>  /  T<id 8 hex><len><data hex>, each line ends with \r
#pragma once
#if defined(__APPLE__) || defined(__linux__)
#include "bus.h"
#include "isotp.h"
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace pt {

class SlcanBus : public Bus, public RawCan {
    std::string port; int fd = -1; std::string line;
    IsoTp iso{*this}; Tp20 tp{*this};
    bool cmd(const std::string &c) { std::string s = c + "\r"; return ::write(fd, s.data(), s.size()) == (ssize_t)s.size(); }
    static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
public:
    explicit SlcanBus(const std::string &p) : port(p) {}
    ~SlcanBus() override { close(); }
    bool open() override {
        fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) { error("[ERROR] cannot open the serial port " + port + " (adapter plugged in?)."); return false; }
        struct termios t; if (tcgetattr(fd, &t) == 0) {
            cfmakeraw(&t); cfsetspeed(&t, B115200); t.c_cflag |= CLOCAL | CREAD; tcsetattr(fd, TCSANOW, &t);
        }
        cmd(""); cmd(""); cmd("C"); cmd("S6"); cmd("O");          // close, 500 kbit/s, open
        sleepMs(50); char b[256]; while (::read(fd, b, sizeof b) > 0) {}   // drop the acknowledgements
        return true;
    }
    void close() override { if (fd >= 0) { if (tp.connected()) tp.close(); cmd("C"); ::close(fd); fd = -1; } }
    bool rawOpen() override { return fd >= 0; }
    void rawClose() override {}
    bool sendFrame(const CanFrame &f) override {
        char b[48]; bool ext = (f.id & 0x80000000u) || (f.id & 0x1FFFFFFF) > 0x7FF; uint32_t id = f.id & 0x1FFFFFFF;
        int n = ext ? snprintf(b, sizeof b, "T%08X%u", (unsigned)id, (unsigned)std::min<size_t>(8, f.d.size()))
                    : snprintf(b, sizeof b, "t%03X%u", (unsigned)id, (unsigned)std::min<size_t>(8, f.d.size()));
        std::string s(b, n);
        for (size_t i = 0; i < f.d.size() && i < 8; i++) { snprintf(b, sizeof b, "%02X", f.d[i]); s += b; }
        return cmd(s);
    }
    bool recvFrame(CanFrame &f, unsigned timeoutMs) override {
        uint32_t t0 = nowMs();
        for (;;) {
            size_t e = line.find('\r');
            while (e != std::string::npos) {
                std::string l = line.substr(0, e); line.erase(0, e + 1);
                if (!l.empty() && (l[0] == 't' || l[0] == 'T')) {
                    size_t il = l[0] == 't' ? 3 : 8; if (l.size() < il + 2) { e = line.find('\r'); continue; }
                    uint32_t id = 0; for (size_t i = 1; i <= il; i++) id = (id << 4) | (uint32_t)hexv(l[i]);
                    int n = hexv(l[il + 1]); if (n < 0 || n > 8 || l.size() < il + 2 + 2 * (size_t)n) { e = line.find('\r'); continue; }
                    f.id = id; f.d.clear();
                    for (int i = 0; i < n; i++) f.d.push_back((uint8_t)(hexv(l[il + 2 + 2 * i]) << 4 | hexv(l[il + 3 + 2 * i])));
                    return true;
                }
                e = line.find('\r');
            }
            uint32_t el = nowMs() - t0; if (el >= timeoutMs) return false;
            struct pollfd pf = {fd, POLLIN, 0};
            if (poll(&pf, 1, (int)(timeoutMs - el)) <= 0) return false;
            char b[512]; ssize_t r = ::read(fd, b, sizeof b);
            if (r > 0) line.append(b, (size_t)r);
        }
    }
    bool target(uint32_t tx, uint32_t rx, int linkKind = LINK_CAN) override {
        if (tp.connected() && (linkKind != LINK_TP20 || tx != tp.address())) tp.close();
        Bus::target(tx, rx, linkKind);
        if (linkKind == LINK_TP20) { if (!tp.connected()) tp.open((uint8_t)tx); }
        else iso.setIds(tx, rx);
        return fd >= 0;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        if (kind == LINK_TP20) { tp.keepAlive(); return tp.sendMessage(p); }
        if (kind == LINK_KLINE) return false;                    // no K-line on a CAN adapter
        return iso.send(p);
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) override {
        if (kind == LINK_TP20) return tp.recvMessage(out, timeoutMs);
        if (kind == LINK_KLINE) return false;
        return iso.recv(out, timeoutMs);
    }
};

} // namespace pt
#endif
