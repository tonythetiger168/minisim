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
//   virtio-net  an Ethernet interface: a built-in gateway that answers ARP and
//               ping, or Unix datagram sockets to another platform.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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
            case 0x050: if (x < queues_.size() && usable(x)) notify(x); break;
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
    // The driver notified queue `qi`. By default every chain it made
    // available is served now, through request(); a device that keeps
    // buffers for later (a network device's receive queue) overrides this
    // and uses pop/push/kick itself.
    virtual void notify(unsigned qi) {
        uint16_t head;
        std::vector<Buf> chain;
        while (pop(qi, head, chain)) {
            uint32_t written = request(qi, chain);
            if (status_ & 0x40) return;
            push(qi, head, written);
        }
        kick(qi);
    }
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

    // Is queue `qi` set up, with a size that is a power of 2 (2.7)?
    bool usable(unsigned qi) const {
        const Queue& q = queues_[qi];
        return q.ready && q.num && q.num <= QMAX && !(q.num & (q.num - 1)) && !(status_ & 0x40);
    }
    // The next chain the driver made available on queue `qi`, if any. A bad
    // chain sets DEVICE_NEEDS_RESET and gives none.
    bool pop(unsigned qi, uint16_t& head, std::vector<Buf>& chain) {
        if (!usable(qi)) return false;
        Queue& q = queues_[qi];
        if (q.lastAvail == rd<uint16_t>(q.avail + 2)) return false;
        head = rd<uint16_t>(q.avail + 4 + 2ull * (q.lastAvail % q.num));
        chain.clear();
        uint16_t i = head;
        for (unsigned hops = 0;; hops++) {
            if (i >= q.num || hops >= q.num) { needsReset(); return false; }
            uint64_t d = q.desc + 16ull * i;
            uint64_t addr = rd<uint64_t>(d);
            uint32_t len = rd<uint32_t>(d + 8);
            uint16_t flags = rd<uint16_t>(d + 12), next = rd<uint16_t>(d + 14);
            if (flags & 4) { needsReset(); return false; }  // INDIRECT: not offered
            chain.push_back({addr, len, (flags & 2) != 0});
            if (!(flags & 1)) break;
            i = next;
        }
        q.lastAvail++;
        return true;
    }
    // Give a chain back through the used ring, with the bytes written into it.
    void push(unsigned qi, uint16_t head, uint32_t written) {
        Queue& q = queues_[qi];
        uint16_t usedIdx = rd<uint16_t>(q.used + 2);
        uint64_t e = q.used + 4 + 8ull * (usedIdx % q.num);
        wr<uint32_t>(e, head);
        wr<uint32_t>(e + 4, written);
        wr<uint16_t>(q.used + 2, (uint16_t)(usedIdx + 1));  // after the element (2.7.8.2)
        q.pushed = true;
        requests_++;
    }
    // Interrupt for what push() gave back, unless the driver asked for none
    // (VIRTQ_AVAIL_F_NO_INTERRUPT).
    void kick(unsigned qi) {
        Queue& q = queues_[qi];
        if (!q.pushed) return;
        q.pushed = false;
        if (!(rd<uint16_t>(q.avail) & 1)) { intStatus_ |= 1; update(); }
    }
    // Copy `n` bytes into a chain's writable buffers, in order.
    uint32_t fill(const std::vector<Buf>& chain, const uint8_t* src, uint32_t n) {
        uint32_t done = 0;
        for (const Buf& b : chain) {
            if (!b.write || done == n) continue;
            uint32_t k = std::min(b.len, n - done);
            if (!writeMem(b.addr, src + done, k)) break;
            done += k;
        }
        return done;
    }
    // Gather a chain's readable buffers.
    std::vector<uint8_t> gather(const std::vector<Buf>& chain) {
        std::vector<uint8_t> out;
        for (const Buf& b : chain) {
            if (b.write) continue;
            size_t o = out.size();
            out.resize(o + b.len);
            if (!readMem(b.addr, out.data() + o, b.len)) { out.resize(o); break; }
        }
        return out;
    }

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
        bool pushed = false;
    };

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


// virtio-net (5.1): queue 0 receives, queue 1 transmits; the MAC address is
// in the configuration space. What the guest sends goes to a back end:
//   user   a built-in gateway at 10.0.2.2 (MAC 52:54:00:12:35:02) that
//          answers ARP and ICMP echo, so a guest can bring its interface up
//          and ping, with nothing on the host (deterministic);
//   dgram  Unix datagram sockets, one frame per datagram: bind LOCAL, send
//          to REMOTE, so two platforms (or a program on the host) share a
//          link. Frames that arrive are taken between quanta.
// Every frame both ways can also go to a pcap file.
class VirtioNet : public VirtioMmio {
public:
    VirtioNet() : VirtioMmio(2) { type = "virtio-net"; }
    ~VirtioNet() override {
        if (fd_ >= 0) { ::close(fd_); ::unlink(local_.c_str()); }
        if (pcap_) fclose(pcap_);
    }
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

    void useDgram(const std::string& local, const std::string& remote) {
        if (local.size() >= sizeof(sockaddr_un{}.sun_path) || remote.size() >= sizeof(sockaddr_un{}.sun_path))
            throw std::runtime_error("virtio-net: a socket path is too long");
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("virtio-net: cannot make a socket");
        ::unlink(local.c_str());
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        memcpy(a.sun_path, local.c_str(), local.size());
        if (::bind(fd_, (sockaddr*)&a, sizeof a) != 0) throw std::runtime_error("virtio-net: cannot bind " + local);
        local_ = local;
        remote_ = remote;
    }
    void capture(const std::string& path) {
        pcap_ = fopen(path.c_str(), "wb");
        if (!pcap_) throw std::runtime_error("cannot write " + path);
        // pcap global header: version 2.4, LINKTYPE_ETHERNET, microseconds
        uint32_t h[6] = {0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
        fwrite(h, 4, 6, pcap_);
    }
    bool dgram() const { return fd_ >= 0; }
    uint64_t sent() const { return sent_; }
    uint64_t received() const { return received_; }

    bool live() const override { return fd_ >= 0; }
    uint64_t nextEventPs(uint64_t now) const override {
        // Look at the socket every 100 us of simulated time.
        return fd_ >= 0 ? now + 100000000ull : UINT64_MAX;
    }
    void advance(uint64_t now) override {
        now_ = now;
        if (fd_ < 0) return;
        uint8_t b[65536];
        for (;;) {
            ssize_t n = ::recv(fd_, b, sizeof b, 0);
            if (n <= 0) break;
            if (n >= 14) incoming(std::vector<uint8_t>(b, b + n));
        }
        deliver();
    }
    bool checkpointable() const override { return fd_ < 0; }

protected:
    static constexpr uint32_t HDR = 12;  // struct virtio_net_hdr with VERSION_1
    uint32_t deviceId() const override { return 1; }
    uint64_t features() const override { return F_VERSION_1 | (1ull << 5) /* MAC */ | (1ull << 16) /* STATUS */; }
    uint8_t configByte(uint64_t off) const override {
        if (off < 6) return mac[off];
        if (off == 6) return 1;  // status: VIRTIO_NET_S_LINK_UP
        return 0;
    }
    uint32_t request(unsigned, const std::vector<Buf>&) override { return 0; }
    void notify(unsigned qi) override {
        if (qi == 1) {
            uint16_t head;
            std::vector<Buf> chain;
            while (pop(1, head, chain)) {
                std::vector<uint8_t> pkt = gather(chain);
                push(1, head, 0);
                if (pkt.size() > HDR) send(std::vector<uint8_t>(pkt.begin() + HDR, pkt.end()));
            }
            kick(1);
        }
        deliver();
    }
    void resetDevice() override { rx_.clear(); }
    void saveDevice(Writer& w) const override {
        w.u64(rx_.size());
        for (auto& f : rx_) w.str(std::string(f.begin(), f.end()));
    }
    void loadDevice(Reader& r) override {
        rx_.clear();
        for (uint64_t n = r.u64(); n; n--) {
            std::string f = r.str();
            rx_.emplace_back(f.begin(), f.end());
        }
    }

private:
    void record(const std::vector<uint8_t>& f) {
        if (!pcap_) return;
        uint64_t us = now_ / 1000000;
        uint32_t h[4] = {(uint32_t)(us / 1000000), (uint32_t)(us % 1000000), (uint32_t)f.size(), (uint32_t)f.size()};
        fwrite(h, 4, 4, pcap_);
        fwrite(f.data(), 1, f.size(), pcap_);
        fflush(pcap_);
    }
    void incoming(std::vector<uint8_t> f) {
        record(f);
        if (rx_.size() < 1024) rx_.push_back(std::move(f));
    }
    // Frames waiting for the guest go into its receive buffers while there are
    // some; the rest wait for the driver to add more (it notifies queue 0).
    void deliver() {
        uint16_t head;
        std::vector<Buf> chain;
        while (!rx_.empty() && pop(0, head, chain)) {
            std::vector<uint8_t> b(HDR, 0);
            b[10] = 1;  // num_buffers
            b.insert(b.end(), rx_.front().begin(), rx_.front().end());
            rx_.pop_front();
            push(0, head, fill(chain, b.data(), (uint32_t)b.size()));
            received_++;
        }
        kick(0);
    }
    void send(std::vector<uint8_t> f) {
        record(f);
        sent_++;
        if (fd_ >= 0) {
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            memcpy(a.sun_path, remote_.c_str(), remote_.size());
            ::sendto(fd_, f.data(), f.size(), 0, (sockaddr*)&a, sizeof a);  // a lost frame is a lost frame
            return;
        }
        gateway(f);
    }

    // The user back end's gateway: ARP for its address, and ICMP echo.
    static constexpr uint8_t GW_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};
    static constexpr uint8_t GW_IP[4] = {10, 0, 2, 2};
    static uint16_t csum(const uint8_t* p, size_t n) {
        uint32_t s = 0;
        for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
        if (n & 1) s += (uint32_t)p[n - 1] << 8;
        while (s >> 16) s = (s & 0xffff) + (s >> 16);
        return (uint16_t)~s;
    }
    void gateway(const std::vector<uint8_t>& f) {
        if (f.size() < 14) return;
        uint16_t et = (uint16_t)(f[12] << 8 | f[13]);
        if (et == 0x0806 && f.size() >= 42 && f[20] == 0 && f[21] == 1 && !memcmp(&f[38], GW_IP, 4)) {
            std::vector<uint8_t> r(42);
            memcpy(&r[0], &f[6], 6);       // to the asker
            memcpy(&r[6], GW_MAC, 6);
            r[12] = 0x08; r[13] = 0x06;
            memcpy(&r[14], &f[14], 6);     // htype, ptype, hlen, plen
            r[20] = 0; r[21] = 2;          // reply
            memcpy(&r[22], GW_MAC, 6);
            memcpy(&r[28], GW_IP, 4);
            memcpy(&r[32], &f[22], 10);    // the asker's MAC and IP
            incoming(std::move(r));
            return;
        }
        if (et != 0x0800 || f.size() < 34) return;
        unsigned ihl = (f[14] & 15) * 4;
        size_t tot = (size_t)(f[16] << 8 | f[17]);
        if (ihl < 20 || 14 + tot > f.size() || tot < ihl + 8) return;
        if (f[23] != 1 || memcmp(&f[30], GW_IP, 4)) return;  // ICMP to the gateway
        size_t ic = 14 + ihl;
        if (f[ic] != 8) return;                                 // echo request
        std::vector<uint8_t> r(f.begin(), f.begin() + 14 + tot);
        memcpy(&r[0], &f[6], 6);
        memcpy(&r[6], GW_MAC, 6);
        memcpy(&r[26], &f[30], 4);  // source: the gateway
        memcpy(&r[30], &f[26], 4);  // destination: the asker
        r[22] = 64;                 // TTL
        r[24] = r[25] = 0;
        uint16_t c = csum(&r[14], ihl);
        r[24] = (uint8_t)(c >> 8); r[25] = (uint8_t)c;
        r[ic] = 0;                  // echo reply
        r[ic + 2] = r[ic + 3] = 0;
        c = csum(&r[ic], tot - ihl);
        r[ic + 2] = (uint8_t)(c >> 8); r[ic + 3] = (uint8_t)c;
        incoming(std::move(r));
    }

    std::deque<std::vector<uint8_t>> rx_;
    int fd_ = -1;
    std::string local_, remote_;
    FILE* pcap_ = nullptr;
    uint64_t now_ = 0, sent_ = 0, received_ = 0;
};

}  // namespace vp
