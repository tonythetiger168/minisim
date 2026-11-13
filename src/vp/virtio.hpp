// virtio.hpp - virtio devices over MMIO (OASIS Virtual I/O Device (VIRTIO)
// Version 1.2: 4.2 "Virtio Over MMIO", 2.7 "Split Virtqueues", 5.2 "Block
// Device"), the interface Linux and U-Boot drive on QEMU's virt board.
//
// The register file is the version 2 (non-legacy) layout. Requests are served
// at once, when the driver writes QueueNotify: the descriptors are read from
// guest memory, the request is done, the used ring is written and the
// interrupt raised before the store that notified retires. A virtual
// platform's point is the software, and an instant disk keeps a run
// deterministic: the same instructions, the same cycle counts, every time.
//
//   virtio-blk  a disk backed by an image file (raw). Writes go to a private
//               copy unless the platform says "write": true.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "vp/bus.hpp"

namespace vp {

class VirtioMmio : public Device {
public:
    unsigned irq = 0;

    Acc read(uint64_t off, unsigned n, uint64_t& v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        v = 0;
        if (off >= 0x100) {
            uint64_t t = 0;
            for (unsigned i = 0; i < n; i++) t |= (uint64_t)configByte(off - 0x100 + i) << (8 * i);
            v = t;
            return Acc::Ok;
        }
        if (n != 4) return Acc::Fault;  // registers are 32-bit (4.2.2)
        const Queue& q = queues_[sel_ < queues_.size() ? sel_ : 0];
        bool qok = sel_ < queues_.size();
        switch (off) {
            case 0x000: v = 0x74726976; break;  // "virt"
            case 0x004: v = 2; break;           // version 2: not legacy
            case 0x008: v = deviceId(); break;
            case 0x00c: v = 0x4d56504c; break;  // vendor "LPVM"
            case 0x010: v = featSel_ < 2 ? (uint32_t)(features() >> (32 * featSel_)) : 0; break;
            case 0x034: v = qok ? QMAX : 0; break;
            case 0x044: v = qok && q.ready; break;
            case 0x060: v = intStatus_; break;
            case 0x070: v = status_; break;
            case 0x0fc: v = configGen_; break;
            default: v = 0; break;
        }
        return Acc::Ok;
    }
    Acc write(uint64_t off, unsigned n, uint64_t v, const Who& w) override {
        if (!w.serial) return Acc::Pending;
        if (off >= 0x100) {
            for (unsigned i = 0; i < n; i++) configWrite(off - 0x100 + i, (uint8_t)(v >> (8 * i)));
            return Acc::Ok;
        }
        if (n != 4) return Acc::Fault;
        uint32_t x = (uint32_t)v;
        Queue* q = sel_ < queues_.size() ? &queues_[sel_] : nullptr;
        switch (off) {
            case 0x014: featSel_ = x; break;
            case 0x020: if (drvSel_ < 2) driverFeat_ = (driverFeat_ & ~(0xffffffffull << (32 * drvSel_))) | ((uint64_t)x << (32 * drvSel_)); break;
            case 0x024: drvSel_ = x; break;
            case 0x030: sel_ = x; break;
            case 0x038: if (q) q->num = x; break;
            case 0x044: if (q) { q->ready = x & 1; if (q->ready) q->lastAvail = 0; } break;
            case 0x050: if (x < queues_.size()) serve(x); break;
            case 0x064: intStatus_ &= ~x; update(); break;
            case 0x070:
                if (x == 0) reset();
                else status_ = x;
                break;
            case 0x080: if (q) q->desc = (q->desc & ~0xffffffffull) | x; break;
            case 0x084: if (q) q->desc = (q->desc & 0xffffffffull) | (uint64_t)x << 32; break;
            case 0x090: if (q) q->avail = (q->avail & ~0xffffffffull) | x; break;
            case 0x094: if (q) q->avail = (q->avail & 0xffffffffull) | (uint64_t)x << 32; break;
            case 0x0a0: if (q) q->used = (q->used & ~0xffffffffull) | x; break;
            case 0x0a4: if (q) q->used = (q->used & 0xffffffffull) | (uint64_t)x << 32; break;
            default: break;
        }
        return Acc::Ok;
    }
    void start() override { update(); }
    void save(Writer& w) const override {
        w.u32(status_); w.u32(intStatus_); w.u32(sel_); w.u32(featSel_); w.u32(drvSel_); w.u32(configGen_);
        w.u64(driverFeat_);
        w.u64(queues_.size());
        for (auto& q : queues_) {
            w.u32(q.num); w.u32(q.ready); w.u32(q.lastAvail);
            w.u64(q.desc); w.u64(q.avail); w.u64(q.used);
        }
        saveDevice(w);
    }
    void load(Reader& r) override {
        status_ = r.u32(); intStatus_ = r.u32(); sel_ = r.u32(); featSel_ = r.u32(); drvSel_ = r.u32();
        configGen_ = r.u32();
        driverFeat_ = r.u64();
        if (r.u64() != queues_.size()) throw std::runtime_error("the checkpoint has another virtio queue count");
        for (auto& q : queues_) {
            q.num = r.u32(); q.ready = r.u32(); q.lastAvail = (uint16_t)r.u32();
            q.desc = r.u64(); q.avail = r.u64(); q.used = r.u64();
        }
        loadDevice(r);
        update();
    }
    uint64_t requests() const { return requests_; }

protected:
    static constexpr uint32_t QMAX = 256;
    static constexpr uint64_t F_VERSION_1 = 1ull << 32;

    // One chain of descriptors: what the driver gave, in order.
    struct Buf { uint64_t addr; uint32_t len; bool write; };

    explicit VirtioMmio(unsigned nqueues) : queues_(nqueues) { size = 0x200; }
    virtual uint32_t deviceId() const = 0;
    virtual uint64_t features() const = 0;
    virtual uint8_t configByte(uint64_t) const { return 0; }
    virtual void configWrite(uint64_t, uint8_t) {}
    // Serve one request; returns the bytes written into the driver's buffers.
    virtual uint32_t request(unsigned queue, const std::vector<Buf>& chain) = 0;
    virtual void saveDevice(Writer&) const {}
    virtual void loadDevice(Reader&) {}
    virtual void resetDevice() {}

    // Guest memory, through the platform (plain memory directly).
    bool readMem(uint64_t a, void* dst, uint64_t n) {
        if (uint8_t* p = this->p->memSpan(a, n, false)) { memcpy(dst, p, n); return true; }
        uint8_t* d = static_cast<uint8_t*>(dst);
        for (uint64_t i = 0; i < n; i++) if (!this->p->masterRead(a + i, d[i], this)) return false;
        return true;
    }
    bool writeMem(uint64_t a, const void* src, uint64_t n) {
        if (uint8_t* p = this->p->memSpan(a, n, true)) { memcpy(p, src, n); return true; }
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (uint64_t i = 0; i < n; i++) if (!this->p->masterWrite(a + i, s[i], this)) return false;
        return true;
    }
    template <class T> T rd(uint64_t a) { T v{}; readMem(a, &v, sizeof v); return v; }
    template <class T> void wr(uint64_t a, T v) { writeMem(a, &v, sizeof v); }

    // A device that has gone wrong (a bad descriptor) says so, as 2.1.2 asks,
    // and waits for a reset.
    void needsReset() {
        status_ |= 0x40;  // DEVICE_NEEDS_RESET
        intStatus_ |= 2;  // configuration change
        configGen_++;
        update();
    }

private:
    struct Queue {
        uint32_t num = 0, ready = 0;
        uint16_t lastAvail = 0;
        uint64_t desc = 0, avail = 0, used = 0;
    };

    void serve(unsigned qi) {
        Queue& q = queues_[qi];
        if (!q.ready || !q.num || q.num > QMAX || (q.num & (q.num - 1)) || (status_ & 0x40)) return;
        bool any = false;
        uint16_t availIdx = rd<uint16_t>(q.avail + 2);
        while (q.lastAvail != availIdx) {
            uint16_t head = rd<uint16_t>(q.avail + 4 + 2ull * (q.lastAvail % q.num));
            std::vector<Buf> chain;
            uint16_t i = head;
            bool bad = false;
            for (unsigned hops = 0;; hops++) {
                if (i >= q.num || hops >= q.num) { bad = true; break; }
                uint64_t d = q.desc + 16ull * i;
                uint64_t addr = rd<uint64_t>(d);
                uint32_t len = rd<uint32_t>(d + 8);
                uint16_t flags = rd<uint16_t>(d + 12), next = rd<uint16_t>(d + 14);
                if (flags & 4) { bad = true; break; }  // INDIRECT: not offered
                chain.push_back({addr, len, (flags & 2) != 0});
                if (!(flags & 1)) break;
                i = next;
            }
            if (bad) { needsReset(); return; }
            uint32_t written = request(qi, chain);
            requests_++;
            uint16_t usedIdx = rd<uint16_t>(q.used + 2);
            uint64_t e = q.used + 4 + 8ull * (usedIdx % q.num);
            wr<uint32_t>(e, head);
            wr<uint32_t>(e + 4, written);
            wr<uint16_t>(q.used + 2, (uint16_t)(usedIdx + 1));  // after the element (2.7.8.2)
            q.lastAvail++;
            any = true;
        }
        // VIRTQ_AVAIL_F_NO_INTERRUPT: the driver asks for no interrupt.
        if (any && !(rd<uint16_t>(q.avail) & 1)) { intStatus_ |= 1; update(); }
    }
    void reset() {
        status_ = 0; intStatus_ = 0; sel_ = 0; featSel_ = 0; drvSel_ = 0; driverFeat_ = 0;
        for (auto& q : queues_) q = Queue{};
        resetDevice();
        update();
    }
    void update() {
        if (irq && p) p->setIrq(irq, intStatus_ != 0);
    }

    std::vector<Queue> queues_;
    uint32_t status_ = 0, intStatus_ = 0, sel_ = 0, featSel_ = 0, drvSel_ = 0, configGen_ = 0;
    uint64_t driverFeat_ = 0;
    uint64_t requests_ = 0;
};

// virtio-blk (5.2): one request queue; capacity in 512-byte sectors.
class VirtioBlk : public VirtioMmio {
public:
    VirtioBlk() : VirtioMmio(1) { type = "virtio-blk"; }
    ~VirtioBlk() override {
        if (img_) munmap(img_, mapped_);
    }
    // Map the image: shared (writes reach the file) or private (writes stay in
    // the run). A size that is not a whole number of sectors is cut down.
    void open(const std::string& path, bool writeBack, bool readOnly) {
        int fd = ::open(path.c_str(), writeBack && !readOnly ? O_RDWR : O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open the disk image " + path);
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < 512) {
            ::close(fd);
            throw std::runtime_error("the disk image " + path + " is smaller than one sector");
        }
        bytes_ = (uint64_t)st.st_size & ~511ull;
        mapped_ = (size_t)st.st_size;
        void* m = mmap(nullptr, mapped_, PROT_READ | PROT_WRITE, writeBack && !readOnly ? MAP_SHARED : MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) throw std::runtime_error("cannot map the disk image " + path);
        img_ = static_cast<uint8_t*>(m);
        ro_ = readOnly;
        path_ = path;
    }
    uint64_t capacity() const { return bytes_ / 512; }
    bool readOnly() const { return ro_; }

protected:
    uint32_t deviceId() const override { return 2; }
    uint64_t features() const override {
        return F_VERSION_1 | (1ull << 9) /* FLUSH */ | (ro_ ? 1ull << 5 : 0) /* RO */;
    }
    uint8_t configByte(uint64_t off) const override {
        // capacity (le64) at 0; the rest (size_max, seg_max, geometry, ...)
        // belongs to features that are not offered and reads 0.
        uint64_t cap = capacity();
        if (off < 8) return (uint8_t)(cap >> (8 * off));
        if (off >= 20 && off < 24) return (uint8_t)(512u >> (8 * (off - 20)));  // blk_size (BLK_SIZE not offered)
        return 0;
    }
    uint32_t request(unsigned, const std::vector<Buf>& c) override {
        enum : uint32_t { IN = 0, OUT = 1, FLUSH = 4, GET_ID = 8 };
        enum : uint8_t { OK = 0, IOERR = 1, UNSUPP = 2 };
        // header (16 bytes, readable) ... status (1 byte, writable, last)
        if (c.size() < 2 || c.front().write || c.front().len < 16 || !c.back().write || c.back().len < 1) {
            needsReset();
            return 0;
        }
        uint32_t typ = rd<uint32_t>(c.front().addr);
        uint64_t sector = rd<uint64_t>(c.front().addr + 8);
        uint8_t st = OK;
        uint32_t written = 0;
        uint64_t pos = sector * 512;
        for (size_t i = 1; i + 1 < c.size() && st == OK; i++) {
            const Buf& b = c[i];
            if (typ == IN || typ == OUT) {
                if (b.write != (typ == IN) || pos + b.len > bytes_ || pos + b.len < pos) { st = IOERR; break; }
                if (typ == IN) {
                    if (!writeMem(b.addr, img_ + pos, b.len)) st = IOERR;
                    else written += b.len;
                } else if (ro_) {
                    st = IOERR;
                } else if (!readMem(b.addr, img_ + pos, b.len)) {
                    st = IOERR;
                }
                pos += b.len;
            } else if (typ == GET_ID) {
                char id[20] = {};
                strncpy(id, "mvplat-virtio-blk", sizeof id);
                uint32_t k = std::min<uint32_t>(b.len, sizeof id);
                if (!b.write || !writeMem(b.addr, id, k)) st = IOERR;
                else written += k;
            }
        }
        if (typ == FLUSH) {
            if (!ro_ && msync(img_, mapped_, MS_SYNC) != 0) st = IOERR;
        } else if (typ != IN && typ != OUT && typ != GET_ID) {
            st = UNSUPP;
        }
        wr<uint8_t>(c.back().addr, st);
        return written + 1;
    }
    // The whole image goes into a checkpoint, so a restored run sees the disk
    // as it was.
    void saveDevice(Writer& w) const override {
        w.u64(bytes_);
        w.bytes(img_, bytes_);
    }
    void loadDevice(Reader& r) override {
        if (r.u64() != bytes_) throw std::runtime_error("the checkpoint has a disk of another size");
        r.read(img_, bytes_);
    }

private:
    uint8_t* img_ = nullptr;
    uint64_t bytes_ = 0;
    size_t mapped_ = 0;
    bool ro_ = false;
    std::string path_;
};

}  // namespace vp
