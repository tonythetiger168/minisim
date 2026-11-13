// bus.hpp - the memory map of a virtual platform and the device interface.
//
// A device answers reads and writes relative to its base. Three answers:
// Ok, Fault (an access fault the CPU takes as a trap), and Pending, which
// means "not now": the access has side effects that must happen in a fixed
// order (a peripheral register while several harts run in parallel, or a
// register that lives in RTL on the other side of the msim bridge), so the
// hart stops before the instruction, and the platform repeats it when the
// device is ready. An instruction that got Pending has changed nothing.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vp {

enum class Acc : uint8_t { Ok, Fault, Pending };

// Who is asking: the hart and its own cycle count, so a device that reports
// time (the CLINT's mtime) answers the asking hart's time, not a global one.
// `serial` is true when the access may have side effects now (the platform
// is in its ordered phase, or there is only one hart).
struct Who {
    unsigned hart = 0;
    uint64_t cycle = 0;
    bool serial = true;
};

// Checkpoint streams: plain little-endian binary with section tags.
struct Writer {
    std::ostream& os;
    void u64(uint64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
    void u32(uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
    void bytes(const void* p, size_t n) { os.write(static_cast<const char*>(p), (std::streamsize)n); }
    void str(const std::string& s) { u64(s.size()); bytes(s.data(), s.size()); }
};
struct Reader {
    std::istream& is;
    uint64_t u64() { uint64_t v = 0; read(&v, 8); return v; }
    uint32_t u32() { uint32_t v = 0; read(&v, 4); return v; }
    void read(void* p, size_t n) {
        is.read(static_cast<char*>(p), (std::streamsize)n);
        if (!is) throw std::runtime_error("the checkpoint ends early");
    }
    std::string str() { std::string s(u64(), '\0'); read(s.data(), s.size()); return s; }
};

// What a device may ask of the platform it sits in.
struct Device;

struct Platform {
    virtual ~Platform() = default;
    // Drive interrupt source `src` (1-based, as the PLIC numbers them).
    virtual void setIrq(unsigned src, bool level) = 0;
    virtual unsigned hartCount() const = 0;
    // One mtime tick, in picoseconds.
    virtual uint64_t tickPs() const = 0;
    // One CPU cycle, in picoseconds.
    virtual uint64_t cyclePs() const = 0;
    // Stop the run with this exit code (the test finisher, HTIF, a model).
    virtual void requestExit(int code) = 0;
    // A device's interrupt outputs changed; recompute the harts' mip.
    virtual void irqChanged() = 0;
    // Console output, in the platform's order.
    virtual void out(const std::string& s) = 0;
    // An access from a bus master that is not a hart (DMA from RTL): one byte
    // at physical address `a`. False for a hole or a refusing device.
    virtual bool masterRead(uint64_t a, uint8_t& v, const Device* from) = 0;
    virtual bool masterWrite(uint64_t a, uint8_t v, const Device* from) = 0;
};

struct Device {
    std::string name, type;
    uint64_t base = 0, size = 0;
    // Bus statistics for -profile: accesses that reached the device.
    uint64_t nread = 0, nwrite = 0;
    Platform* p = nullptr;

    virtual ~Device() = default;
    virtual Acc read(uint64_t off, unsigned n, uint64_t& v, const Who& w) = 0;
    virtual Acc write(uint64_t off, unsigned n, uint64_t v, const Who& w) = 0;
    // Plain memory the CPU may read and write directly (RAM and ROM).
    virtual uint8_t* memory() { return nullptr; }
    virtual bool writable() const { return true; }
    // Time moved to now_ps (called between quanta, in device order).
    virtual void advance(uint64_t /*now_ps*/) {}
    // The earliest time, after now_ps, at which this device needs advance()
    // called, so a quantum can end on it. UINT64_MAX: none.
    virtual uint64_t nextEventPs(uint64_t /*now_ps*/) const { return UINT64_MAX; }
    // Does the device act on its own as time passes, so that the platform may
    // not skip time while every hart waits in WFI?
    virtual bool live() const { return false; }
    // Does the device's register side have to be accessed in order (true for
    // anything with side effects)? Plain memory says no.
    virtual bool ordered() const { return true; }
    // Checkpoints. A device that cannot save itself (a loaded model library,
    // the RTL bridge) says so and the checkpoint is refused.
    virtual bool checkpointable() const { return true; }
    virtual void save(Writer&) const {}
    virtual void load(Reader&) {}
    // Called once, after every device exists and before the first instruction.
    virtual void start() {}
    // Called once at the end of the run.
    virtual void finish() {}
};

// The map: devices sorted by base, with no overlaps.
class Bus {
public:
    void add(std::unique_ptr<Device> d) {
        if (!d->size) throw std::runtime_error("device '" + d->name + "' has size 0");
        if (d->base + d->size < d->base) throw std::runtime_error("device '" + d->name + "' wraps around");
        for (auto& e : devs_)
            if (d->base < e->base + e->size && e->base < d->base + d->size)
                throw std::runtime_error("device '" + d->name + "' at " + hex(d->base) + ".." +
                                         hex(d->base + d->size - 1) + " overlaps '" + e->name +
                                         "' at " + hex(e->base) + ".." + hex(e->base + e->size - 1));
        devs_.push_back(std::move(d));
        std::sort(devs_.begin(), devs_.end(), [](auto& a, auto& b) { return a->base < b->base; });
    }
    Device* find(uint64_t a) const {
        // A platform has tens of devices; a binary search over the sorted map.
        size_t lo = 0, hi = devs_.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            Device* d = devs_[mid].get();
            if (a < d->base) hi = mid;
            else if (a >= d->base + d->size) lo = mid + 1;
            else return d;
        }
        return nullptr;
    }
    Device* byName(const std::string& n) const {
        for (auto& d : devs_) if (d->name == n) return d.get();
        return nullptr;
    }
    const std::vector<std::unique_ptr<Device>>& devices() const { return devs_; }

    static std::string hex(uint64_t v) {
        char b[24];
        snprintf(b, sizeof b, "0x%08llx", (unsigned long long)v);
        return b;
    }

private:
    std::vector<std::unique_ptr<Device>> devs_;
};

}  // namespace vp
