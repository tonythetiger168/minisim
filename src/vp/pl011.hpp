// pl011.hpp - a PrimeCell UART (PL011) as Linux's amba-pl011 driver and
// early console ("arm,pl011", "arm,primecell") use it, from the PL011
// Technical Reference Manual's register map. Transmission is instant, so
// the transmit FIFO is always empty and its interrupt is asserted while it
// is enabled. Received characters come from `held`, `timed` and `hostFd`,
// as with the 16550 (devices.hpp). Input that arrived before the receive
// interrupt was enabled is delivered 1 ms after that: Linux's driver empties
// the receive FIFO right after enabling it.
#pragma once

#include <unistd.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "bus.hpp"

namespace vp {

class Pl011 : public Device {
public:
    unsigned irq = 0;
    std::deque<uint8_t> rx, held;
    std::vector<std::pair<uint64_t, std::string>> timed;
    int hostFd = -1;

    Pl011() { type = "pl011"; size = 0x1000; }
    bool live() const override { return hostFd >= 0 || !timed.empty(); }
    uint64_t nextEventPs(uint64_t) const override { return timed.empty() ? UINT64_MAX : timed.front().first; }
    void advance(uint64_t now) override {
        now_ = now;
        // Before the driver enables the receive interrupt, input waits.
        std::deque<uint8_t>& q = (imsc_ & RX) ? rx : held;
        bool got = false;
        while (!timed.empty() && timed.front().first <= now) {
            for (char c : timed.front().second) q.push_back((uint8_t)c);
            timed.erase(timed.begin());
            got = true;
        }
        if (hostFd >= 0) {
            uint8_t b[256];
            ssize_t k = ::read(hostFd, b, sizeof b);
            if (k > 0) { q.insert(q.end(), b, b + k); got = true; }
            else if (k == 0) hostFd = -1;
        }
        if (got) update();
    }
    Acc read(uint64_t off, unsigned, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        v = 0;
        switch (off) {
            case 0x000:
                if (!rx.empty()) { v = rx.front(); rx.pop_front(); update(); }
                break;
            case 0x018: v = 0x80 | 0x40 * 0 | (rx.empty() ? 0x10 : 0); break;  // FR: TXFE, RXFE
            case 0x024: v = ibrd_; break;
            case 0x028: v = fbrd_; break;
            case 0x02c: v = lcr_; break;
            case 0x030: v = cr_; break;
            case 0x034: v = ifls_; break;
            case 0x038: v = imsc_; break;
            case 0x03c: v = ris(); break;
            case 0x040: v = ris() & imsc_; break;
            case 0x048: v = dmacr_; break;
            // PeriphID (0x00241011) and PrimeCell ID (0xb105f00d), a byte each
            case 0xfe0: v = 0x11; break;
            case 0xfe4: v = 0x10; break;
            case 0xfe8: v = 0x24; break;
            case 0xfec: v = 0x00; break;
            case 0xff0: v = 0x0d; break;
            case 0xff4: v = 0xf0; break;
            case 0xff8: v = 0x05; break;
            case 0xffc: v = 0xb1; break;
        }
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        switch (off) {
            case 0x000: p->out(std::string(1, (char)v)); tx_++; break;
            case 0x024: ibrd_ = (uint32_t)v & 0xffff; break;
            case 0x028: fbrd_ = (uint32_t)v & 0x3f; break;
            case 0x02c: lcr_ = (uint32_t)v & 0xff; break;
            case 0x030: cr_ = (uint32_t)v & 0xffff; break;
            case 0x034: ifls_ = (uint32_t)v & 0x3f; break;
            case 0x038:
                imsc_ = (uint32_t)v & 0x7ff;
                if ((imsc_ & RX) && !held.empty()) {
                    timed.insert(timed.begin(), {now_ + 1000000000ull, std::string(held.begin(), held.end())});
                    held.clear();
                }
                break;
            case 0x044: break;  // ICR: the conditions are levels here
            case 0x048: dmacr_ = (uint32_t)v & 7; break;
        }
        update();
        return Acc::Ok;
    }
    void start() override { update(); }
    uint64_t transmitted() const { return tx_; }

private:
    static constexpr uint32_t RX = 1u << 4, TX = 1u << 5, RT = 1u << 6;
    uint32_t ris() const { return (rx.empty() ? 0 : RX | RT) | TX; }
    void update() {
        if (irq) p->setIrq(irq, (ris() & imsc_) != 0);
    }
    uint32_t ibrd_ = 0, fbrd_ = 0, lcr_ = 0, cr_ = 0x300, ifls_ = 0x12, imsc_ = 0, dmacr_ = 0;
    uint64_t tx_ = 0, now_ = 0;
};

}  // namespace vp
