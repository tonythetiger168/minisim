// a64gic.hpp - a GICv3 interrupt controller (Arm GIC Architecture
// Specification, IHI 0069): the distributor (GICD) for the shared peripheral
// interrupts, one redistributor (GICR) per processor element for its SGIs
// and PPIs, and the CPU interface, which is in the processor's ICC_*_EL1
// system registers (a64.hpp calls icc()).
//
// One security state (GICD_CTLR.DS = 1), affinity routing always on, no
// LPIs or ITS. Priorities have 5 bits. Group 0 and group 1 are both taken as
// IRQs (Linux puts everything in group 1). An SPI's level comes from
// Platform::setIrq: source n is SPI n, INTID 32 + n.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "a64.hpp"
#include "bus.hpp"

namespace vp {
namespace a64 {

class Gicv3 {
public:
    struct Irq {
        bool enabled = false, latched = false, active = false, level = false, edge = false, group1 = false;
        uint8_t prio = 0;
        uint64_t route = 0;  // SPIs: GICD_IROUTER (bit 31 IRM: any processor)
        bool pending() const { return latched || (!edge && level); }
    };
    static constexpr uint32_t SPURIOUS = 1023;

    Gicv3(std::vector<Cpu*> cpus, unsigned nspi) : cpus_(std::move(cpus)), spi_((nspi + 31) / 32 * 32), local_(cpus_.size()) {}
    unsigned spiCount() const { return (unsigned)spi_.size(); }

    // A shared peripheral interrupt's line (n from 0: INTID 32 + n).
    void setSpi(unsigned n, bool level) {
        if (n >= spi_.size()) return;
        Irq& q = spi_[n];
        if (level && !q.level && q.edge) q.latched = true;
        q.level = level;
        update();
    }
    // A PPI's line on one processor (the timer's).
    void setPpi(unsigned cpu, unsigned intid, bool level) {
        Irq& q = local_[cpu][intid];
        if (level == q.level) return;
        if (level && q.edge) q.latched = true;
        q.level = level;
        update();
    }

    // Which processors an SPI goes to: its IROUTER affinity, or with IRM
    // the lowest-numbered one that can take it.
    bool targets(const Irq& q, unsigned cpu) const {
        if (q.route >> 31 & 1) return true;
        return (q.route & 0xff) == cpus_[cpu]->id && ((q.route >> 8) & 0xff) == 0;
    }
    // The highest-priority pending, enabled interrupt for a processor, as
    // (priority << 16 | INTID); priority 0x100 when none.
    uint32_t best(unsigned cpu) const {
        uint32_t b = 0x100u << 16 | SPURIOUS;
        if (!(ctlr_ & 3)) return b;
        const auto& l = local_[cpu];
        for (unsigned i = 0; i < 32; i++) {
            const Irq& q = l[i];
            if (q.enabled && q.pending() && !q.active && (uint32_t)q.prio << 16 < (b & 0xffff0000u)) b = (uint32_t)q.prio << 16 | i;
        }
        for (unsigned i = 0; i < spi_.size(); i++) {
            const Irq& q = spi_[i];
            if (q.enabled && q.pending() && !q.active && targets(q, cpu) && (uint32_t)q.prio << 16 < (b & 0xffff0000u))
                b = (uint32_t)q.prio << 16 | (32 + i);
        }
        return b;
    }
    uint32_t running(const Cpu& c) const {
        uint32_t r = 0x100;
        for (uint32_t a : c.activeStack) r = std::min(r, a >> 16);
        return r;
    }
    // Recompute every processor's IRQ line.
    void update() {
        for (unsigned i = 0; i < cpus_.size(); i++) {
            Cpu& c = *cpus_[i];
            const uint32_t b = best(i), pr = b >> 16;
            c.irqLine = (c.igrpen1 & 1) && pr < 0x100 && pr < (c.pmr & 0xf8) && pr < running(c);
        }
    }

    // The CPU interface: reads and writes of ICC_*_EL1. False: not one.
    bool icc(Cpu& c, uint32_t reg, bool write, uint64_t& v) {
        const unsigned me = c.id;
        switch (reg) {
            case SR(3, 0, 4, 6, 0):  // ICC_PMR_EL1
                if (write) c.pmr = (uint32_t)v & 0xf8; else v = c.pmr;
                break;
            case SR(3, 0, 12, 12, 0): {  // ICC_IAR1_EL1
                if (write) return true;
                const uint32_t b = best(me), pr = b >> 16, id = b & 0xffff;
                if (!(c.igrpen1 & 1) || pr >= 0x100 || pr >= (c.pmr & 0xf8) || pr >= running(c)) { v = SPURIOUS; break; }
                Irq& q = irq(me, id);
                q.active = true;
                q.latched = false;
                c.activeStack.push_back(pr << 16 | id);
                v = id;
                break;
            }
            case SR(3, 0, 12, 12, 1): {  // ICC_EOIR1_EL1: drop the priority (and deactivate, EOImode 0)
                if (!write) { v = 0; break; }
                const uint32_t id = (uint32_t)v & 0xffffff;
                for (size_t k = c.activeStack.size(); k--;)
                    if ((c.activeStack[k] & 0xffff) == id) { c.activeStack.erase(c.activeStack.begin() + (long)k); break; }
                if (!(c.icc_ctlr & 2) && id < 32 + spi_.size()) irq(me, id).active = false;
                break;
            }
            case SR(3, 0, 12, 11, 1): {  // ICC_DIR_EL1
                if (write && v < 32 + spi_.size()) irq(me, (uint32_t)v).active = false;
                else if (!write) v = 0;
                break;
            }
            case SR(3, 0, 12, 12, 2):  // ICC_HPPIR1_EL1
                if (!write) { const uint32_t b = best(me); v = (b >> 16) < 0x100 ? (b & 0xffff) : SPURIOUS; }
                break;
            case SR(3, 0, 12, 11, 3):  // ICC_RPR_EL1
                if (!write) v = running(c) & 0xff;
                break;
            case SR(3, 0, 12, 12, 3):  // ICC_BPR1_EL1
                if (write) c.bpr1 = std::max<uint32_t>(2, (uint32_t)v & 7); else v = c.bpr1;
                break;
            case SR(3, 0, 12, 8, 3):  // ICC_BPR0_EL1
                if (!write) v = 2;
                break;
            case SR(3, 0, 12, 12, 4):  // ICC_CTLR_EL1: EOImode writable; PRIbits 4 (5 bits), IDbits 0 (16)
                if (write) c.icc_ctlr = (c.icc_ctlr & ~2u) | ((uint32_t)v & 2);
                else v = c.icc_ctlr | 4u << 8;
                break;
            case SR(3, 0, 12, 12, 5):  // ICC_SRE_EL1: SRE, DFB, DIB fixed at 1
                if (!write) v = 7;
                break;
            case SR(3, 0, 12, 12, 6):  // ICC_IGRPEN0_EL1
                if (!write) v = 0;
                break;
            case SR(3, 0, 12, 12, 7):  // ICC_IGRPEN1_EL1
                if (write) c.igrpen1 = (uint32_t)v & 1; else v = c.igrpen1;
                break;
            case SR(3, 0, 12, 8, 4): case SR(3, 0, 12, 9, 0):  // ICC_AP0R0_EL1, ICC_AP1R0_EL1
                if (!write) v = 0;
                break;
            case SR(3, 0, 12, 11, 5): case SR(3, 0, 12, 11, 6): case SR(3, 0, 12, 11, 7): {  // ICC_SGI1R, ASGI1R, SGI0R
                if (!write) { v = 0; break; }
                const unsigned id = (unsigned)(v >> 24) & 15;
                const bool all = v >> 40 & 1;
                const unsigned rs = (unsigned)(v >> 44) & 15;
                const unsigned aff1 = (unsigned)(v >> 16) & 0xff, aff2 = (unsigned)(v >> 32) & 0xff, aff3 = (unsigned)(v >> 48) & 0xff;
                for (unsigned t = 0; t < cpus_.size(); t++) {
                    const unsigned a0 = cpus_[t]->id;
                    bool hit = all ? t != me : (aff1 == 0 && aff2 == 0 && aff3 == 0 && a0 / 16 == rs && (v >> (a0 % 16) & 1));
                    if (hit) local_[t][id].latched = true;
                }
                break;
            }
            default: return false;
        }
        update();
        return true;
    }

    // GICD: the distributor's 64 KiB frame.
    Acc distRead(uint64_t off, unsigned n, uint64_t& v) {
        v = 0;
        if (off == 0x0000) v = ctlr_ | 1u << 6 | 1u << 4;  // DS, ARE
        else if (off == 0x0004) v = (spi_.size() / 32) | 9u << 19 | (uint32_t)(cpus_.size() - 1) << 5;  // ITLinesNumber, IDbits 10
        else if (off == 0x0008) v = 0;                                                                  // IIDR
        else if (off == 0xffe8) v = 0x3b;                                                               // PIDR2: GICv3
        else if (off >= 0x6000 && off < 0x8000) {                                                       // IROUTER
            const uint64_t id = (off - 0x6000) / 8;
            if (id >= 32 && id - 32 < spi_.size()) v = spi_[id - 32].route >> ((off & 7) * 8);
        } else if (off >= 0x0080 && off < 0x0c00 + 0x100 * 3) v = bankRead(off, n, nullptr);
        return Acc::Ok;
    }
    Acc distWrite(uint64_t off, unsigned n, uint64_t v) {
        if (off == 0x0000) ctlr_ = (uint32_t)v & 3;
        else if (off >= 0x6000 && off < 0x8000) {
            const uint64_t id = (off - 0x6000) / 8;
            if (id >= 32 && id - 32 < spi_.size()) {
                uint64_t& r = spi_[id - 32].route;
                if (n == 8) r = v;
                else if (off & 4) r = (r & 0xffffffffull) | (v & 0xffffffffull) << 32;
                else r = (r & ~0xffffffffull) | (v & 0xffffffffull);
                r &= 0xff00ffffffull | 1u << 31;
            }
        } else if (off >= 0x0080 && off < 0x0c00 + 0x100 * 3) bankWrite(off, n, v, nullptr);
        update();
        return Acc::Ok;
    }
    // GICR: two 64 KiB frames per processor (RD_base, SGI_base).
    Acc redistRead(unsigned cpu, uint64_t off, unsigned n, uint64_t& v) {
        v = 0;
        if (off < 0x10000) {
            switch (off) {
                case 0x0000: v = 0; break;  // CTLR
                case 0x0004: v = 0; break;  // IIDR
                case 0x0008: case 0x000c: {  // TYPER: affinity, processor number, Last
                    uint64_t t = (uint64_t)cpus_[cpu]->id << 32 | (uint64_t)cpu << 8 | (cpu + 1 == cpus_.size() ? 1u << 4 : 0);
                    v = off == 8 ? (n == 8 ? t : (uint32_t)t) : t >> 32;
                    break;
                }
                case 0x0014: v = waker_[cpu]; break;
                case 0xffe8: v = 0x3b; break;
            }
            return Acc::Ok;
        }
        off -= 0x10000;
        if (off >= 0x0080 && off < 0x0e00) v = bankRead(off, n, &local_[cpu]);
        return Acc::Ok;
    }
    Acc redistWrite(unsigned cpu, uint64_t off, unsigned n, uint64_t v) {
        if (off < 0x10000) {
            if (off == 0x0014) waker_[cpu] = ((uint32_t)v & 2) ? 6 : 0;  // ChildrenAsleep follows ProcessorSleep
            return Acc::Ok;
        }
        off -= 0x10000;
        if (off >= 0x0080 && off < 0x0e00) bankWrite(off, n, v, &local_[cpu]);
        update();
        return Acc::Ok;
    }

private:
    Irq& irq(unsigned cpu, uint32_t id) { return id < 32 ? local_[cpu][id] : spi_[id - 32]; }
    // The registers with a bit or a byte (or two bits) per interrupt:
    // the distributor's for SPIs (loc null; INTIDs below 32 read as zero),
    // a redistributor's SGI frame for its SGIs and PPIs.
    uint64_t bankRead(uint64_t off, unsigned n, std::array<Irq, 32>* loc) {
        uint64_t v = 0;
        for (unsigned b = 0; b < n; b++) v |= (uint64_t)byteRead(off + b, loc) << (8 * b);
        return v;
    }
    void bankWrite(uint64_t off, unsigned n, uint64_t v, std::array<Irq, 32>* loc) {
        for (unsigned b = 0; b < n; b++) byteWrite(off + b, (uint8_t)(v >> (8 * b)), loc);
    }
    Irq* at(unsigned id, std::array<Irq, 32>* loc) {
        if (loc) return id < 32 ? &(*loc)[id] : nullptr;
        return id >= 32 && id - 32 < spi_.size() ? &spi_[id - 32] : nullptr;
    }
    uint8_t byteRead(uint64_t o, std::array<Irq, 32>* loc) {
        uint8_t r = 0;
        if (o >= 0x400 && o < 0x800) {  // IPRIORITYR
            Irq* q = at((unsigned)(o - 0x400), loc);
            return q ? q->prio : 0;
        }
        if (o >= 0xc00 && o < 0xd00) {  // ICFGR: 2 bits each, bit 1 edge
            for (unsigned k = 0; k < 4; k++) {
                Irq* q = at((unsigned)((o - 0xc00) * 4 + k), loc);
                if (q && q->edge) r |= (uint8_t)(2u << (2 * k));
            }
            return r;
        }
        const uint64_t base = o & ~0x7full;  // the 0x80-byte register block
        const unsigned first = (unsigned)((o - base) * 8);
        for (unsigned k = 0; k < 8; k++) {
            Irq* q = at(first + k, loc);
            if (!q) continue;
            bool bit = false;
            switch (base) {
                case 0x080: bit = q->group1; break;
                case 0x100: case 0x180: bit = q->enabled; break;
                case 0x200: case 0x280: bit = q->pending(); break;
                case 0x300: case 0x380: bit = q->active; break;
                default: break;
            }
            if (bit) r |= (uint8_t)(1u << k);
        }
        return r;
    }
    void byteWrite(uint64_t o, uint8_t b, std::array<Irq, 32>* loc) {
        if (o >= 0x400 && o < 0x800) {
            Irq* q = at((unsigned)(o - 0x400), loc);
            if (q) q->prio = b & 0xf8;
            return;
        }
        if (o >= 0xc00 && o < 0xd00) {
            for (unsigned k = 0; k < 4; k++) {
                const unsigned id = (unsigned)((o - 0xc00) * 4 + k);
                Irq* q = at(id, loc);
                if (q && id >= 16) q->edge = b >> (2 * k + 1) & 1;  // SGIs are always edge
            }
            return;
        }
        const uint64_t base = o & ~0x7full;
        const unsigned first = (unsigned)((o - base) * 8);
        for (unsigned k = 0; k < 8; k++) {
            Irq* q = at(first + k, loc);
            if (!q) continue;
            const bool bit = b >> k & 1;
            switch (base) {
                case 0x080: q->group1 = bit; break;
                case 0x100: if (bit) q->enabled = true; break;
                case 0x180: if (bit) q->enabled = false; break;
                case 0x200: if (bit) q->latched = true; break;
                case 0x280: if (bit) q->latched = false; break;
                case 0x300: if (bit) q->active = true; break;
                case 0x380: if (bit) q->active = false; break;
                default: break;
            }
        }
    }

    std::vector<Cpu*> cpus_;
    std::vector<Irq> spi_;
    std::vector<std::array<Irq, 32>> local_;
    std::vector<uint32_t> waker_ = std::vector<uint32_t>(cpus_.size(), 6);
    uint32_t ctlr_ = 0;

public:
    // SGIs are edge-triggered; PPIs default to level.
    void reset() {
        for (auto& l : local_)
            for (unsigned i = 0; i < 16; i++) l[i].edge = true;
    }
};

// The two frames as bus devices.
class GicDist : public Device {
public:
    explicit GicDist(Gicv3& g) : g_(g) { type = "gicv3"; size = 0x10000; }
    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who&) override { return g_.distRead(off, n, v); }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who&) override { return g_.distWrite(off, n, v); }
private:
    Gicv3& g_;
};
class GicRedist : public Device {
public:
    GicRedist(Gicv3& g, unsigned ncpu) : g_(g) { type = "gicv3-redist"; size = 0x20000ull * ncpu; }
    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who&) override { return g_.redistRead((unsigned)(off >> 17), off & 0x1ffff, n, v); }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who&) override { return g_.redistWrite((unsigned)(off >> 17), off & 0x1ffff, n, v); }
private:
    Gicv3& g_;
};

}  // namespace a64
}  // namespace vp
