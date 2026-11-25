// pci.hpp - a PCI Express host bridge with ECAM configuration space
// (PCI Express Base 6.0, 7.2.2 "PCI Express Enhanced Configuration Access
// Mechanism"; Linux's and U-Boot's "pci-host-ecam-generic", the host QEMU's
// virt board has) and virtio devices behind it on the PCI transport
// (OASIS VIRTIO 1.2, 4.1 "Virtio Over PCI Bus", modern interface only).
//
// Bus 0 holds the devices, one function each, at device numbers 1 up (00.0
// is left empty, where QEMU has its host bridge). Each device has one 64-bit
// memory BAR (BAR0, 16 KiB) that the operating system assigns inside the
// host's memory window:
//
//   0x0000  common configuration (4.1.4.3)
//   0x1000  ISR status (4.1.4.5), read to clear
//   0x2000  device-specific configuration (4.1.4.6)
//   0x3000  notifications (4.1.4.4): queue n at 0x3000 + 4n
//
// Interrupts are INTx, level, as on a board without MSI: INTA of device d
// is PLIC source irq + d % 4 (the usual swizzle, given to the kernel as an
// interrupt-map), shared by the devices on that line.
//
// The virtio devices are the MMIO ones of virtio.hpp; this file only maps
// the PCI registers onto theirs, so a disk, a console or a random number
// source behaves the same on either transport.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "vp/bus.hpp"
#include "vp/virtio.hpp"

namespace vp {

class PcieHost : public Device {
public:
    // The memory window BARs are assigned in (the "ranges" of the host).
    uint64_t mmioBase = 0x40000000, mmioSize = 0x40000000;
    unsigned buses = 1;  // ECAM size: 1 MiB a bus
    unsigned irq = 0;    // INTA of device 0; four lines from here

    PcieHost() { type = "pcie"; }

    void addVirtio(std::unique_ptr<VirtioMmio> v) {
        auto f = std::make_unique<Fn>();
        f->host = this;
        f->dev = std::move(v);
        f->slot = (unsigned)fns_.size() + 1;
        f->init();
        fns_.push_back(std::move(f));
    }
    size_t count() const { return fns_.size(); }
    const VirtioMmio* virtio(size_t i) const { return fns_[i]->dev.get(); }
    unsigned slotOf(size_t i) const { return fns_[i]->slot; }

    // The memory window, a device of its own on the platform's bus.
    std::unique_ptr<Device> window() {
        auto w = std::make_unique<Window>();
        w->host = this;
        w->name = name + ".mmio";
        w->type = "pcie-mmio";
        w->base = mmioBase;
        w->size = mmioSize;
        return w;
    }

    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        Fn* f = fnAt(off);
        unsigned r = off & 0xfff;
        if (!f || r + n > 4096) { v = n == 8 ? ~0ull : (1ull << (8 * n)) - 1; return Acc::Ok; }
        v = 0;
        for (unsigned i = 0; i < n; i++) v |= (uint64_t)f->cfgByte(r + i) << (8 * i);
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        Fn* f = fnAt(off);
        unsigned r = off & 0xfff;
        if (!f || r + n > 4096) return Acc::Ok;
        for (unsigned i = 0; i < n; i++) f->cfgWrite(r + i, (uint8_t)(v >> (8 * i)));
        f->cfgWritten(r, n);
        return Acc::Ok;
    }
    void start() override {
        for (auto& f : fns_) {
            f->proxy.real = p;
            f->dev->p = &f->proxy;
            f->dev->irq = 1;  // any source: the proxy turns it into INTx
            f->dev->start();
        }
    }
    void save(Writer& w) const override {
        w.u64(fns_.size());
        for (auto& f : fns_) {
            w.bytes(f->cfg.data(), f->cfg.size());
            w.u32(f->qsel);
            for (uint16_t s : f->qsize) w.u32(s);
            w.u32(f->level);
            f->dev->save(w);
        }
    }
    void load(Reader& r) override {
        if (r.u64() != fns_.size()) throw std::runtime_error("the checkpoint's PCIe host has another device count");
        for (auto& f : fns_) {
            r.read(f->cfg.data(), f->cfg.size());
            f->qsel = r.u32();
            for (uint16_t& s : f->qsize) s = (uint16_t)r.u32();
            f->level = r.u32() != 0;
            f->dev->load(r);
        }
        for (auto& f : fns_) f->drive();
    }
    void finish() override { for (auto& f : fns_) f->dev->finish(); }
    bool live() const override {
        for (auto& f : fns_) if (f->dev->live()) return true;
        return false;
    }
    void advance(uint64_t now) override { for (auto& f : fns_) f->dev->advance(now); }
    uint64_t nextEventPs(uint64_t now) const override {
        uint64_t t = UINT64_MAX;
        for (auto& f : fns_) t = std::min(t, f->dev->nextEventPs(now));
        return t;
    }

private:
    static constexpr uint32_t BAR_SIZE = 0x4000;
    static constexpr unsigned MAXQ = 8;

    struct Fn;
    // What a virtio device behind the host sees as its platform: everything
    // goes to the real one but the interrupt, which becomes this function's
    // INTx level, ORed with the others on the same line.
    struct Proxy : Platform {
        Platform* real = nullptr;
        Fn* fn = nullptr;
        void setIrq(unsigned, bool level) override { fn->level = level; fn->drive(); }
        unsigned hartCount() const override { return real->hartCount(); }
        uint64_t tickPs() const override { return real->tickPs(); }
        uint64_t cyclePs() const override { return real->cyclePs(); }
        void requestExit(int c) override { real->requestExit(c); }
        void irqChanged() override { real->irqChanged(); }
        void out(const std::string& s) override { real->out(s); }
        bool masterRead(uint64_t a, uint8_t& v, const Device* d) override { return real->masterRead(a, v, d); }
        bool masterWrite(uint64_t a, uint8_t v, const Device* d) override { return real->masterWrite(a, v, d); }
        uint8_t* memSpan(uint64_t a, uint64_t n, bool w) override { return real->memSpan(a, n, w); }
    };

    struct Fn {
        PcieHost* host = nullptr;
        std::unique_ptr<VirtioMmio> dev;
        unsigned slot = 0;
        std::array<uint8_t, 256> cfg{};
        Proxy proxy;
        bool level = false;
        uint32_t qsel = 0;
        std::array<uint16_t, MAXQ> qsize{};

        void put16(unsigned o, uint16_t v) { cfg[o] = (uint8_t)v; cfg[o + 1] = (uint8_t)(v >> 8); }
        void put32(unsigned o, uint32_t v) { for (int i = 0; i < 4; i++) cfg[o + i] = (uint8_t)(v >> (8 * i)); }
        uint32_t get32(unsigned o) const { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)cfg[o + i] << (8 * i); return v; }
        uint16_t get16(unsigned o) const { return (uint16_t)(cfg[o] | cfg[o + 1] << 8); }

        // A vendor capability (4.1.4): type, BAR offset and length.
        void cap(unsigned at, unsigned next, uint8_t kind, uint32_t off, uint32_t len, unsigned clen) {
            cfg[at] = 0x09;  // vendor specific
            cfg[at + 1] = (uint8_t)next;
            cfg[at + 2] = (uint8_t)clen;
            cfg[at + 3] = kind;
            cfg[at + 4] = 0;  // BAR0
            put32(at + 8, off);
            put32(at + 12, len);
        }
        void init() {
            proxy.fn = this;
            const uint32_t id = dev->deviceIdPublic();
            put16(0x00, 0x1af4);           // Red Hat, Inc. (virtio)
            put16(0x02, (uint16_t)(0x1040 + id));
            put16(0x06, 0x0010);           // status: a capabilities list
            cfg[0x08] = 1;                 // revision 1: modern only
            uint32_t cls = id == 1 ? 0x020000 : id == 2 ? 0x010000 : id == 3 ? 0x078000 : 0x00ff00;
            cfg[0x09] = (uint8_t)cls; cfg[0x0a] = (uint8_t)(cls >> 8); cfg[0x0b] = (uint8_t)(cls >> 16);
            put32(0x10, 0x4);              // BAR0: memory, 64-bit
            put16(0x2c, 0x1af4);
            put16(0x2e, (uint16_t)id);
            cfg[0x34] = 0x40;              // capabilities
            cfg[0x3d] = 1;                 // INTA
            cap(0x40, 0x50, 1, 0x0000, 0x40, 16);    // common
            cap(0x50, 0x60, 3, 0x1000, 0x4, 16);     // ISR
            cap(0x60, 0x70, 4, 0x2000, 0x1000, 16);  // device
            cap(0x70, 0x00, 2, 0x3000, 4 * MAXQ, 20);  // notify
            put32(0x80, 4);                // notify_off_multiplier
            resetQueues();
        }
        void resetQueues() {
            qsel = 0;
            qsize.fill(0);
            Who w;
            for (unsigned q = 0; q < nq(); q++) {
                uint64_t v = 0;
                dev->write(0x030, 4, q, w);
                dev->read(0x034, 4, v, w);
                qsize[q] = (uint16_t)v;
            }
            dev->write(0x030, 4, 0, w);
        }
        unsigned nq() const { return std::min<unsigned>(dev->queueCount(), MAXQ); }

        uint8_t cfgByte(unsigned r) const {
            if (r >= cfg.size()) return 0;
            uint8_t v = cfg[r];
            if (r == 0x06) v = (uint8_t)((v & ~0x08) | (level ? 0x08 : 0));  // Interrupt Status
            return v;
        }
        // The writable bits. BAR0's low bits below its size read 0, so
        // writing all ones and reading back gives the size (7.5.1.2.1).
        void cfgWrite(unsigned r, uint8_t v) {
            if (r == 0x04) cfg[r] = v & 0x46;            // memory space, bus master, parity
            else if (r == 0x05) cfg[r] = v & 0x05;       // SERR#, INTx disable
            else if (r == 0x10) cfg[r] = 0x04;           // memory, 64-bit, not prefetchable
            else if (r == 0x11) cfg[r] = v & (uint8_t)~((BAR_SIZE - 1) >> 8);
            else if (r >= 0x12 && r < 0x18) cfg[r] = v;
            else if (r == 0x3c) cfg[r] = v;              // interrupt line
        }
        void cfgWritten(unsigned r, unsigned n) {
            if (r <= 0x05 && r + n > 0x04) drive();
        }
        uint64_t barAddr() const { return ((uint64_t)get32(0x14) << 32) | (get32(0x10) & ~0xfu); }
        bool memOn() const { return cfg[0x04] & 0x02; }

        // The INTx line, unless the command register disables it.
        void drive() { host->driveLine(slot % 4); }
        bool asserting() const { return level && !(cfg[0x05] & 0x04); }

        // BAR0 accesses, at offset o.
        Acc barRead(uint64_t o, unsigned n, uint64_t& v) {
            Who w;
            v = 0;
            if (o < 0x40) { v = common(o, n, false, 0); return Acc::Ok; }
            if (o >= 0x1000 && o < 0x1004) {  // ISR: read clears
                uint64_t s = 0;
                dev->read(0x060, 4, s, w);
                if (o == 0x1000) { v = s & 0xff; if (s) dev->write(0x064, 4, s, w); }
                return Acc::Ok;
            }
            if (o >= 0x2000 && o < 0x3000) return dev->read(0x100 + (o - 0x2000), n, v, w);
            return Acc::Ok;
        }
        Acc barWrite(uint64_t o, unsigned n, uint64_t v) {
            Who w;
            if (o < 0x40) { common(o, n, true, v); return Acc::Ok; }
            if (o >= 0x2000 && o < 0x3000) return dev->write(0x100 + (o - 0x2000), n, v, w);
            if (o >= 0x3000 && o < 0x3000 + 4 * MAXQ) {
                unsigned q = (unsigned)(o - 0x3000) / 4;
                if (q < nq()) dev->write(0x050, 4, q, w);
            }
            return Acc::Ok;
        }
        // The common configuration structure (4.1.4.3), field by field.
        uint64_t common(uint64_t o, unsigned n, bool wr, uint64_t v) {
            Who w;
            uint64_t t = 0;
            auto rd = [&](uint64_t reg) { uint64_t x = 0; dev->read(reg, 4, x, w); return x; };
            auto wrr = [&](uint64_t reg, uint64_t x) { dev->write(reg, 4, x & 0xffffffffu, w); };
            const bool qok = qsel < nq();
            switch (o) {
                case 0x00: if (wr) wrr(0x014, v); else { t = rd(0x014); } break;  // device_feature_select
                case 0x04: if (!wr) t = rd(0x010); break;                        // device_feature
                case 0x08: if (wr) wrr(0x024, v); break;                         // driver_feature_select
                case 0x0c: if (wr) wrr(0x020, v); break;                         // driver_feature
                case 0x10: t = 0xffff; break;                                    // config_msix_vector
                case 0x12: t = nq(); break;                                      // num_queues
                case 0x14:                                                       // device_status
                    if (wr) {
                        wrr(0x070, v & 0xff);
                        if ((v & 0xff) == 0) resetQueues();
                    } else t = rd(0x070) & 0xff;
                    break;
                case 0x15: t = rd(0x0fc) & 0xff; break;                          // config_generation
                case 0x16: if (wr) { qsel = (uint32_t)(v & 0xffff); wrr(0x030, qsel); } else t = qsel; break;
                case 0x18:                                                       // queue_size
                    if (wr) { if (qok) qsize[qsel] = (uint16_t)v; } else t = qok ? qsize[qsel] : 0;
                    break;
                case 0x1a: t = 0xffff; break;                                    // queue_msix_vector
                case 0x1c:                                                       // queue_enable
                    if (wr) { if (qok && (v & 1)) { wrr(0x038, qsize[qsel]); wrr(0x044, 1); } }
                    else t = qok ? rd(0x044) : 0;
                    break;
                case 0x1e: t = qok ? qsel : 0; break;                            // queue_notify_off
                case 0x20: case 0x24: case 0x28: case 0x2c: case 0x30: case 0x34: {  // queue addresses
                    static const uint64_t regs[6] = {0x080, 0x084, 0x090, 0x094, 0x0a0, 0x0a4};
                    unsigned k = (unsigned)(o - 0x20) / 4;
                    if (!qok) break;
                    if (wr) {
                        uint64_t& sh = addr[qsel][k / 2];
                        if (n == 8) { sh = v; wrr(regs[k], v); wrr(regs[k + 1], v >> 32); }
                        else { sh = (k & 1) ? (sh & 0xffffffffu) | (v << 32) : (sh & ~0xffffffffull) | (v & 0xffffffffu); wrr(regs[k], v); }
                    } else {
                        uint64_t sh = addr[qsel][k / 2];
                        t = n == 8 ? sh : (k & 1) ? sh >> 32 : sh & 0xffffffffu;
                    }
                    break;
                }
                case 0x38: t = 0; break;                                         // queue_notify_data
                case 0x3a: t = 0; break;                                         // queue_reset
                default: break;
            }
            (void)n;
            return t;
        }
        std::array<std::array<uint64_t, 3>, MAXQ> addr{};
    };

    struct Window : Device {
        PcieHost* host = nullptr;
        Acc read(uint64_t off, unsigned n, uint64_t& v, const Who& w) override {
            if (!w.serial) return Acc::Pending;
            uint64_t o;
            if (Fn* f = host->barFn(base + off, n, o)) return f->barRead(o, n, v);
            v = n == 8 ? ~0ull : (1ull << (8 * n)) - 1;
            return Acc::Ok;
        }
        Acc write(uint64_t off, unsigned n, uint64_t v, const Who& w) override {
            if (!w.serial) return Acc::Pending;
            uint64_t o;
            if (Fn* f = host->barFn(base + off, n, o)) return f->barWrite(o, n, v);
            return Acc::Ok;
        }
    };

    Fn* fnAt(uint64_t off) {
        unsigned bus = (unsigned)(off >> 20), dev = (unsigned)(off >> 15) & 31, fn = (unsigned)(off >> 12) & 7;
        if (bus != 0 || fn != 0) return nullptr;
        for (auto& f : fns_) if (f->slot == dev) return f.get();
        return nullptr;
    }
    Fn* barFn(uint64_t a, unsigned n, uint64_t& o) {
        for (auto& f : fns_) {
            if (!f->memOn()) continue;
            uint64_t b = f->barAddr();
            if (a >= b && a + n <= b + BAR_SIZE) { o = a - b; return f.get(); }
        }
        return nullptr;
    }
    void driveLine(unsigned line) {
        if (!irq || !p) return;
        bool on = false;
        for (auto& f : fns_) if (f->slot % 4 == line && f->asserting()) on = true;
        p->setIrq(irq + line, on);
    }

    std::vector<std::unique_ptr<Fn>> fns_;
};

}  // namespace vp
