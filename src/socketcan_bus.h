// Linux SocketCAN transport: ISO 15765-2 through the kernel's ISO-TP sockets (can-isotp, in mainline
// since Linux 5.10; "modprobe can-isotp"). Works with any SocketCAN interface: candleLight / CANable,
// PEAK, Kvaser, an MCP2515 hat ...   Bring the interface up first:
//     sudo ip link set can0 up type can bitrate 500000
// VW TP 2.0 units (behind the gateway) go through a raw CAN socket. K-line units of the older cars are not
// reachable this way (no K-line on a CAN interface).
#pragma once
#ifdef __linux__
#include "bus.h"
#include "tp20.h"
#include <cstring>
#include <linux/can.h>
#include <linux/can/isotp.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pt {

class SocketCanBus : public Bus, public RawCan {
    std::string ifname; int fd = -1, rawfd = -1; unsigned ifindex = 0;
    Tp20 tp{*this};
public:
    explicit SocketCanBus(const std::string &iface) : ifname(iface) {}
    ~SocketCanBus() override { close(); }
    bool open() override {
        ifindex = if_nametoindex(ifname.c_str());
        if (!ifindex) { error("[ERROR] CAN interface '" + ifname + "' not found. Bring it up: sudo ip link set " + ifname + " up type can bitrate 500000"); return false; }
        int s = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);
        if (s < 0) { error("[ERROR] the kernel has no ISO-TP sockets (try: sudo modprobe can-isotp)."); return false; }
        ::close(s); return true;
    }
    void close() override { if (tp.connected()) tp.close(); rawClose(); if (fd >= 0) { ::close(fd); fd = -1; } }
    bool rawOpen() override {
        if (rawfd >= 0) return true;
        rawfd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (rawfd < 0) { error("[ERROR] cannot open a raw CAN socket."); return false; }
        struct sockaddr_can a; memset(&a, 0, sizeof a); a.can_family = AF_CAN; a.can_ifindex = (int)ifindex;
        if (bind(rawfd, (struct sockaddr *)&a, sizeof a) < 0) { ::close(rawfd); rawfd = -1; error("[ERROR] cannot bind the raw CAN socket."); return false; }
        return true;
    }
    void rawClose() override { if (rawfd >= 0) { ::close(rawfd); rawfd = -1; } }
    bool sendFrame(const CanFrame &f) override {
        struct can_frame cf; memset(&cf, 0, sizeof cf); cf.can_id = f.id; cf.can_dlc = (uint8_t)std::min<size_t>(8, f.d.size());
        memcpy(cf.data, f.d.data(), cf.can_dlc);
        return rawfd >= 0 && write(rawfd, &cf, sizeof cf) == (ssize_t)sizeof cf;
    }
    bool recvFrame(CanFrame &f, unsigned timeoutMs) override {
        if (rawfd < 0) return false;
        struct pollfd pf = {rawfd, POLLIN, 0};
        if (poll(&pf, 1, (int)timeoutMs) <= 0) return false;
        struct can_frame cf; if (read(rawfd, &cf, sizeof cf) != (ssize_t)sizeof cf) return false;
        f.id = cf.can_id & CAN_SFF_MASK; f.d.assign(cf.data, cf.data + cf.can_dlc); return true;
    }
    bool target(uint32_t tx, uint32_t rx, int linkKind = LINK_CAN) override {
        if (tp.connected() && (linkKind != LINK_TP20 || tx != tp.address())) tp.close();
        Bus::target(tx, rx, linkKind);
        if (fd >= 0) { ::close(fd); fd = -1; }
        if (linkKind == LINK_TP20) { if (!rawOpen()) return false; if (!tp.connected()) tp.open((uint8_t)tx); return true; }
        if (linkKind != LINK_CAN) return true;                  // K-line unit: stays unreachable ("no answer")
        fd = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);
        if (fd < 0) { error("[ERROR] cannot create an ISO-TP socket."); return false; }
        struct can_isotp_options o; memset(&o, 0, sizeof o);
        o.flags = CAN_ISOTP_TX_PADDING; o.txpad_content = 0xAA;   // the control units expect full 8-byte frames
        setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &o, sizeof o);
        struct sockaddr_can a; memset(&a, 0, sizeof a);
        a.can_family = AF_CAN; a.can_ifindex = (int)ifindex;
        bool ext = tx > 0x7FF || rx > 0x7FF;
        a.can_addr.tp.tx_id = tx | (ext ? CAN_EFF_FLAG : 0); a.can_addr.tp.rx_id = rx | (ext ? CAN_EFF_FLAG : 0);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { error("[ERROR] cannot bind the ISO-TP socket on " + ifname + "."); close(); return false; }
        return true;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        if (kind == LINK_TP20) { tp.keepAlive(); return tp.sendMessage(p); }
        if (fd < 0 || p.empty()) return false;
        return write(fd, p.data(), p.size()) == (ssize_t)p.size();
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) override {
        if (kind == LINK_TP20) return tp.recvMessage(out, timeoutMs);
        if (fd < 0) return false;
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, (int)timeoutMs) <= 0 || !(pf.revents & POLLIN)) return false;
        uint8_t buf[4096]; ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        out.assign(buf, buf + n); return true;
    }
};

} // namespace pt
#endif
