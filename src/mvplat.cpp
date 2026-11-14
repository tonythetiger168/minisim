// mvplat.cpp - a virtual platform: RISC-V harts, a memory map of peripheral
// models, and the software that runs on them, before the hardware exists.
//
//   mvplat prog.elf                         run on the built-in platform
//   mvplat -platform soc.json prog.elf      run on a platform described in JSON
//   mvplat prog.elf -harts 4                four harts, on every core
//   mvplat prog.elf -trace commits.log      a commit log, in spike's format
//
// The platform is RV32GC or RV64GC harts (machine, supervisor and user mode, PMP, Sv32, or Sv39/48/57) on a bus of
// RAM, ROM, a 16550 UART, a CLINT, a PLIC and a test finisher. Several harts
// run on several threads with the same result as on one (vp/machine.hpp says
// how). Options and device types this file does not know go to an extension
// library when one is installed (vp/ext.hpp). See docs/VPLAT.md.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "parallel.hpp"
#include "version.hpp"
#include "vp/ext.hpp"
#include "vp/fdt.hpp"
#include "vp/json.hpp"
#include "vp/machine.hpp"
#include "vp/virtio.hpp"

namespace fs = std::filesystem;
using namespace vp;

namespace {

// The built-in platform: the address map most RISC-V software already
// targets for a "virt"-style board.
const char* kBuiltin = R"({
  "name": "virt32",
  "harts": 1,
  "mhz": 100,
  "mtime_hz": 10000000,
  "devices": [
    {"type": "finisher",  "name": "test",  "base": "0x00100000"},
    {"type": "clint",     "name": "clint", "base": "0x02000000"},
    {"type": "plic",      "name": "plic",  "base": "0x0c000000", "sources": 32},
    {"type": "uart16550", "name": "uart0", "base": "0x10000000", "irq": 10},
    {"type": "ram",       "name": "dram",  "base": "0x80000000", "size": "128M"}
  ]
})";

Ext* loadExt(int argc, char** argv, const char* why);

// "52:54:00:12:34:56" into a virtio-net device's address.
void setMac(VirtioNet& v, const std::string& t, const std::string& where) {
    unsigned b[6];
    char tail;
    if (sscanf(t.c_str(), "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6)
        throw std::runtime_error(where + "\"mac\" is not six hex bytes with colons: '" + t + "'");
    for (int i = 0; i < 6; i++) v.mac[i] = (uint8_t)b[i];
}
Ext* g_ext = nullptr;
// The largest paging mode the device tree offers (the platform's "mmu" or
// -mmu): sv39, sv48 or sv57 on RV64. The harts take every mode either way.
std::string g_mmu = "sv39";

void usage() {
    std::cout <<
        "usage: mvplat [-platform FILE] PROGRAM [options]\n"
        "  PROGRAM            an RV32 or RV64 ELF executable (or -elf FILE)\n"
        "  -platform FILE     the platform in JSON (default: the built-in virt32 board)\n"
        "  -harts N           number of harts (overrides the platform)\n"
        "  -threads N         threads for the harts (default: every CPU; -j N too)\n"
        "  -quantum N         cycles each hart runs between synchronisations (default 1000)\n"
        "  -mhz F             hart clock in MHz (default 100)\n"
        "  -max-cycles N      stop after N cycles (exit code 4)\n"
        "  -input TEXT        characters the first UART receives\n"
        "  -trace FILE        commit log, one line per instruction (spike's format)\n"
        "Booting an operating system (as QEMU's -bios, -kernel, -initrd, -append, -dtb):\n"
        "  -bios FILE         firmware run first, in machine mode (OpenSBI fw_dynamic)\n"
        "  -kernel FILE       kernel (a raw Image or an ELF) the firmware starts in supervisor mode\n"
        "  -initrd FILE       initial RAM disk\n"
        "  -append TEXT       kernel command line\n"
        "  -dtb FILE          device tree blob to pass instead of the one made from the platform\n"
        "  -disk FILE         a raw disk image on a virtio-blk device (/dev/vda); writes stay in\n"
        "                     the run unless -disk-write (repeat for vdb, vdc, ...)\n"
        "  -disk-write        let writes reach the image files\n"
        "  -net user          a virtio-net interface whose built-in gateway 10.0.2.2 answers\n"
        "                     ARP and ping (repeat for eth1, ...)\n"
        "  -net dgram:LOCAL,REMOTE  a virtio-net interface on Unix datagram sockets: frames\n"
        "                     go to REMOTE, and come in on LOCAL (another platform's link)\n"
        "  -net-pcap FILE     write the first interface's frames to a pcap file\n"
        "  -vcon              a virtio console (Linux: console=hvc0); -input then goes to it\n"
        "  -dump-dtb FILE     write the platform's device tree blob and exit\n"
        "  -mmu MODE          the largest paging mode the device tree offers an RV64 OS:\n"
        "                     sv39 (default), sv48 or sv57\n"
        "  -vlen BITS         the vector registers' length: 128 (default), 256, 512 or 1024\n"
        "  -map               print the memory map\n"
        "  -q                 no summary line on stderr\n"
        "  -version           print the version\n"
        "Exit code: the program's own (finisher, HTIF tohost), 3 if every hart waits\n"
        "forever, 4 at -max-cycles, 2 for a usage or platform error.\n";
    if (g_ext) std::cout << g_ext->usage();
}

struct Options {
    std::string platform, elf, trace, input;
    std::string bios, kernel, initrd, append, dtb, dumpDtb;
    std::vector<std::string> disks, nets;
    std::string netPcap;
    bool vcon = false;
    std::string mmu;
    unsigned vlen = 0;
    bool diskWrite = false;
    bool haveAppend = false;
    int harts = -1;
    unsigned threads = par::defaultThreads();
    long long quantum = -1, maxCycles = 0;
    double mhz = 0;
    bool map = false, quiet = false;
    bool haveInput = false;
};

std::string exeDir() {
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
}

// The extension library (vp/ext.hpp), loaded the first time something asks
// for it. `why` names what asked, for the message when there is none.
int g_argc;
char** g_argv;
Ext* loadExt(int argc, char** argv, const char* why) {
    static bool tried = false;
    if (tried) return g_ext;
    tried = true;
    std::string path;
    if (const char* e = std::getenv("MVPLAT_EXT")) path = e;
    else {
        std::string d = exeDir();
        std::error_code ec;
        fs::path p = fs::path(d).parent_path() / "lib" / "minisim" / "libmvplat_ext.so";
        if (!d.empty() && fs::exists(p, ec)) path = p.string();
    }
    if (path.empty()) return nullptr;
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) throw std::runtime_error(std::string("cannot load the extension library for ") + why + ": " + dlerror());
    auto entry = (MvplatExtEntry)dlsym(h, MVPLAT_EXT_ENTRY);
    if (!entry) throw std::runtime_error(path + " is not an mvplat extension library");
    g_ext = entry(EXT_ABI, argc, argv);
    if (!g_ext) throw std::runtime_error(path + " was built for another version of mvplat");
    return g_ext;
}

void build(Machine& m, const json::Value& pl, const fs::path& dir, const Options& o) {
    auto rel = [&](const std::string& p) { return fs::path(p).is_absolute() ? p : (dir / p).string(); };
    if (pl.get("mmu")) g_mmu = pl.get("mmu")->str();
    if (!o.mmu.empty()) g_mmu = o.mmu;
    if (g_mmu != "sv39" && g_mmu != "sv48" && g_mmu != "sv57")
        throw std::runtime_error("mmu must be sv39, sv48 or sv57, not '" + g_mmu + "'");
    unsigned nh = pl.get("harts") ? (unsigned)pl.get("harts")->u64() : 1;
    if (o.harts > 0) nh = (unsigned)o.harts;
    if (nh < 1 || nh > 1024) throw std::runtime_error("harts must be 1 to 1024");
    double mhz = pl.get("mhz") ? pl.get("mhz")->num() : 100.0;
    if (o.mhz > 0) mhz = o.mhz;
    if (mhz <= 0 || mhz > 1e6) throw std::runtime_error("mhz must be above 0");
    m.cycle_ps = std::max<uint64_t>(1, (uint64_t)(1e6 / mhz + 0.5));
    double mt = pl.get("mtime_hz") ? pl.get("mtime_hz")->num() : 1e7;
    m.tick_ps = std::max<uint64_t>(1, (uint64_t)(1e12 / mt + 0.5));
    m.quantum = pl.get("quantum") ? pl.get("quantum")->u64() : 1000;
    if (o.quantum > 0) m.quantum = (uint64_t)o.quantum;
    if (!m.quantum) throw std::runtime_error("the quantum must be at least one cycle");
    uint64_t reset = pl.get("reset") ? pl.get("reset")->u64() : 0x80000000u;
    m.addHarts(nh, reset);
    unsigned vlen = pl.get("vlen") ? (unsigned)pl.get("vlen")->u64() : 128;
    if (o.vlen) vlen = o.vlen;
    if (vlen < 128 || vlen > Hart::VLEN_MAX || (vlen & (vlen - 1)))
        throw std::runtime_error("vlen must be 128, 256, 512 or 1024");
    for (auto& h : m.harts) h->vlen = vlen;

    const json::Value* devs = pl.get("devices");
    if (!devs || devs->kind != json::Value::Arr) throw std::runtime_error("the platform has no \"devices\" list");
    bool firstUart = true;
    for (auto& d : devs->a) {
        if (!d.get("type")) throw std::runtime_error(d.where() + "a device has no \"type\"");
        std::string type = d.get("type")->str();
        std::string name = d.get("name") ? d.get("name")->str() : type;
        auto base = [&]() {
            if (!d.get("base")) throw std::runtime_error(d.where() + "'" + name + "' has no \"base\"");
            return d.get("base")->u64();
        };
        std::unique_ptr<Device> dev;
        if (type == "ram" || type == "rom") {
            if (!d.get("size")) throw std::runtime_error(d.where() + "'" + name + "' has no \"size\"");
            auto r = std::make_unique<Ram>(d.get("size")->u64(), type == "ram");
            r->name = name;
            r->base = base();
            if (d.get("file")) r->loadFile(rel(d.get("file")->str()), d.get("offset") ? d.get("offset")->u64() : 0);
            dev = std::move(r);
        } else if (type == "uart16550" || type == "uart") {
            auto u = std::make_unique<Uart16550>();
            u->name = name;
            u->base = base();
            u->shift = d.get("shift") ? (unsigned)d.get("shift")->u64() : 0;
            u->size = d.get("size") ? d.get("size")->u64() : (8ull << u->shift);
            u->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            std::string in = d.get("input") ? d.get("input")->str() : "";
            if (firstUart && o.haveInput && !o.vcon) in = o.input;
            for (char c : in) u->rx.push_back((uint8_t)c);
            firstUart = false;
            dev = std::move(u);
        } else if (type == "clint") {
            auto c = std::make_unique<Clint>();
            c->name = name;
            c->base = base();
            m.clint = c.get();
            dev = std::move(c);
        } else if (type == "plic") {
            auto c = std::make_unique<Plic>();
            c->name = name;
            c->base = base();
            if (d.get("sources")) c->nsrc = (unsigned)d.get("sources")->u64();
            if (c->nsrc < 1 || c->nsrc > 1023) throw std::runtime_error(d.where() + "PLIC sources must be 1 to 1023");
            m.plic = c.get();
            dev = std::move(c);
        } else if (type == "finisher") {
            auto f = std::make_unique<Finisher>();
            f->name = name;
            f->base = base();
            dev = std::move(f);
        } else if (type == "virtio-blk") {
            auto v = std::make_unique<VirtioBlk>();
            v->name = name;
            v->base = base();
            v->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            if (!d.get("file")) throw std::runtime_error(d.where() + "'" + name + "' has no \"file\" (a raw disk image)");
            bool w = d.get("write") && d.get("write")->b;
            bool ro = d.get("readonly") && d.get("readonly")->b;
            v->open(rel(d.get("file")->str()), w || o.diskWrite, ro);
            dev = std::move(v);
        } else if (type == "virtio-console") {
            auto v = std::make_unique<VirtioConsole>();
            v->name = name;
            v->base = base();
            v->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            for (char c : d.get("input") ? d.get("input")->str() : std::string()) v->held.push_back((uint8_t)c);
            dev = std::move(v);
        } else if (type == "virtio-net") {
            auto v = std::make_unique<VirtioNet>();
            v->name = name;
            v->base = base();
            v->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            if (d.get("mac")) setMac(*v, d.get("mac")->str(), d.where());
            std::string be = d.get("backend") ? d.get("backend")->str() : "user";
            if (be == "dgram") {
                if (!d.get("local") || !d.get("remote"))
                    throw std::runtime_error(d.where() + "'" + name + "': the dgram back end needs \"local\" and \"remote\"");
                v->useDgram(rel(d.get("local")->str()), rel(d.get("remote")->str()));
            } else if (be != "user") {
                throw std::runtime_error(d.where() + "'" + name + "': unknown back end '" + be + "' (user, dgram)");
            }
            if (d.get("pcap")) v->capture(rel(d.get("pcap")->str()));
            dev = std::move(v);
        } else {
            Ext* x = loadExt(g_argc, g_argv, ("device type '" + type + "'").c_str());
            if (x && x->device(m, d, type, name, dir)) continue;
            throw std::runtime_error(d.where() + "unknown device type '" + type +
                                     "' (ram, rom, uart16550, clint, plic, finisher, virtio-blk, virtio-net, virtio-console" +
                                     (x ? "" : "; mvp and rtl need the mvplat extension library") + ")");
        }
        m.bus.add(std::move(dev));
    }
    // -disk: virtio-blk devices where QEMU's virt board has its virtio-mmio
    // slots (0x10001000 up, 0x1000 apart, PLIC sources 1 up), after any the
    // platform already has.
    unsigned slot = 0;
    for (const std::string& img : o.disks) {
        uint64_t b;
        do b = 0x10001000ull + 0x1000ull * slot++; while (m.bus.find(b) && slot < 64);
        auto v = std::make_unique<VirtioBlk>();
        v->name = "virtio" + std::to_string(slot - 1);
        v->base = b;
        v->irq = slot;
        if (m.plic && v->irq > m.plic->nsrc) throw std::runtime_error("-disk: no PLIC source left for " + img);
        v->open(img, o.diskWrite, false);
        m.bus.add(std::move(v));
    }
    // -net: virtio-net devices in the next slots.
    unsigned nic = 0;
    for (const std::string& spec : o.nets) {
        uint64_t b;
        do b = 0x10001000ull + 0x1000ull * slot++; while (m.bus.find(b) && slot < 64);
        auto v = std::make_unique<VirtioNet>();
        v->name = "net" + std::to_string(nic);
        v->base = b;
        v->irq = slot;
        v->mac[5] = (uint8_t)(0x56 + nic);
        if (m.plic && v->irq > m.plic->nsrc) throw std::runtime_error("-net: no PLIC source left");
        if (spec.rfind("dgram:", 0) == 0) {
            std::string rest = spec.substr(6);
            size_t c = rest.find(',');
            if (c == std::string::npos) throw std::runtime_error("-net dgram:LOCAL,REMOTE: no comma in '" + spec + "'");
            v->useDgram(rest.substr(0, c), rest.substr(c + 1));
        } else if (spec != "user") {
            throw std::runtime_error("-net: unknown back end '" + spec + "' (user, dgram:LOCAL,REMOTE)");
        }
        if (nic == 0 && !o.netPcap.empty()) v->capture(o.netPcap);
        m.bus.add(std::move(v));
        nic++;
    }
    if (!o.netPcap.empty() && !nic) throw std::runtime_error("-net-pcap needs -net");
    // -vcon: a virtio console in the next slot; -input goes to it.
    if (o.vcon) {
        uint64_t b;
        do b = 0x10001000ull + 0x1000ull * slot++; while (m.bus.find(b) && slot < 64);
        auto v = std::make_unique<VirtioConsole>();
        v->name = "vcon";
        v->base = b;
        v->irq = slot;
        if (m.plic && v->irq > m.plic->nsrc) throw std::runtime_error("-vcon: no PLIC source left");
        if (o.haveInput) for (char c : o.input) v->held.push_back((uint8_t)c);
        m.bus.add(std::move(v));
    }
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> b((size_t)in.tellg());
    in.seekg(0);
    in.read((char*)b.data(), (std::streamsize)b.size());
    if (!in) throw std::runtime_error("cannot read " + path);
    return b;
}

// Where OpenSBI is when -kernel is given with no -bios: $MVPLAT_BIOS, then
// the places distributions install the generic platform's fw_dynamic.
std::string defaultBios(bool rv64) {
    if (const char* e = std::getenv("MVPLAT_BIOS")) return e;
    std::vector<std::string> c = rv64
        ? std::vector<std::string>{"/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.elf",
                                   "/usr/share/opensbi/lp64/generic/firmware/fw_dynamic.elf"}
        : std::vector<std::string>{"/usr/share/opensbi/ilp32/generic/firmware/fw_dynamic.elf"};
    std::error_code ec;
    for (auto& p : c) if (fs::exists(p, ec)) return p;
    return "";
}

// Boot an operating system the way QEMU's virt board does: the firmware
// (OpenSBI fw_dynamic) runs from the start of RAM in machine mode with
//   a0 = hart id, a1 = device tree, a2 = struct fw_dynamic_info
// and starts the kernel in supervisor mode at RAM + 2 MiB (4 MiB on RV32).
// The device tree goes at the top of the first RAM on a 2 MiB boundary, the
// initrd just below it. Returns true when only -dump-dtb was asked for.
bool bootOs(Machine& m, const Options& o) {
    Device* ram = nullptr;
    for (auto& d : m.bus.devices())
        if (d->type == "ram" && (!ram || d->base < ram->base)) ram = d.get();
    if (!ram) throw std::runtime_error("the platform has no RAM to boot from");
    auto isElf = [](const std::vector<uint8_t>& b) { return b.size() >= 4 && !memcmp(b.data(), "\x7f" "ELF", 4); };

    bool rv64 = m.haveElf ? m.harts[0]->rv64 : true;  // -dump-dtb with a program: that program's width
    std::vector<uint8_t> kern;
    Elf kelf;
    bool kernElf = false;
    if (!o.kernel.empty()) {
        kern = readFile(o.kernel);
        if (isElf(kern)) { kelf = readElf(o.kernel); kernElf = true; rv64 = kelf.is64; }
    }
    std::string bios = o.bios;
    if (bios.empty() && !o.kernel.empty()) {
        bios = defaultBios(rv64);
        if (bios.empty())
            throw std::runtime_error("-kernel needs firmware: give -bios (OpenSBI's fw_dynamic.elf) or install opensbi");
    }
    uint64_t entry = ram->base;
    if (!bios.empty()) {
        std::vector<uint8_t> b = readFile(bios);
        if (isElf(b)) {
            Elf e = readElf(bios);
            // Position-independent firmware (OpenSBI's generic platform is
            // linked at 0) runs from the start of RAM.
            uint64_t lo = UINT64_MAX;
            for (auto& s : e.segs) lo = std::min(lo, s.addr);
            uint64_t off = lo != UINT64_MAX && !m.bus.find(lo) ? ram->base - lo : 0;
            for (auto& s : e.segs) m.poke(s.addr + off, s.data.data(), s.data.size());
            entry = e.entry + off;
            rv64 = e.is64;
        } else {
            m.poke(ram->base, b.data(), b.size());
        }
    }
    uint64_t kaddr = ram->base + (rv64 ? 0x200000 : 0x400000);
    if (kernElf) {
        for (auto& s : kelf.segs) m.poke(s.addr, s.data.data(), s.data.size());
        kaddr = kelf.entry;
    } else if (!kern.empty()) {
        m.poke(kaddr, kern.data(), kern.size());
    }

    BootArgs args;
    args.bootargs = o.append;
    args.mmu = g_mmu;
    std::vector<uint8_t> initrd;
    if (!o.initrd.empty()) {
        initrd = readFile(o.initrd);
        args.initrdEnd = 1;  // placeholders, so the tree has its final size
    }
    uint64_t hz = (uint64_t)(1e12 / (double)m.cycle_ps + 0.5), tb = (uint64_t)(1e12 / (double)m.tick_ps + 0.5);
    unsigned nh = (unsigned)m.harts.size();
    auto make = [&] { return makeDeviceTree(m.bus, nh, rv64, hz, tb, args); };
    std::vector<uint8_t> dtb = o.dtb.empty() ? make() : readFile(o.dtb);
    if (!o.dumpDtb.empty()) {
        std::ofstream out(o.dumpDtb, std::ios::binary);
        out.write((const char*)dtb.data(), (std::streamsize)dtb.size());
        if (!out) throw std::runtime_error("cannot write " + o.dumpDtb);
        if (o.kernel.empty() && o.bios.empty()) return true;
    }
    const uint64_t infoSize = 6 * 8;
    uint64_t end = ram->base + ram->size;
    if (dtb.size() + infoSize + 64 > (2u << 20) || ram->size < (8u << 20))
        throw std::runtime_error("RAM '" + ram->name + "' is too small to boot from");
    uint64_t dtbAddr = (end - dtb.size() - infoSize - 64) & ~(uint64_t)0x1fffff;
    uint64_t infoAddr = (dtbAddr + dtb.size() + 7) & ~(uint64_t)7;
    if (!initrd.empty()) {
        uint64_t start = (dtbAddr - initrd.size()) & ~(uint64_t)0xfff;
        if (dtbAddr < initrd.size() || start < kaddr + kern.size())
            throw std::runtime_error("the initrd does not fit in RAM '" + ram->name + "' above the kernel");
        m.poke(start, initrd.data(), initrd.size());
        args.initrdStart = start;
        args.initrdEnd = start + initrd.size();
        if (o.dtb.empty()) dtb = make();  // same size: the initrd properties are always there
        else throw std::runtime_error("-initrd with -dtb: put the initrd in the given device tree instead");
    }
    if (kaddr + kern.size() > dtbAddr) throw std::runtime_error("the kernel does not fit in RAM '" + ram->name + "'");
    m.poke(dtbAddr, dtb.data(), dtb.size());

    // struct fw_dynamic_info (OpenSBI include/sbi/fw_dynamic.h), version 2:
    // magic, version, next_addr, next_mode (1 = supervisor), options, boot_hart (-1: any).
    uint64_t info[6] = {0x4942534f, 2, kaddr, 1, 0, ~0ull};
    uint8_t ib[48];
    unsigned w = rv64 ? 8 : 4;
    for (unsigned i = 0; i < 6; i++) wrLE(ib + i * w, w, info[i]);
    m.poke(infoAddr, ib, 6 * w);
    // What -input types reaches the console once the kernel's driver is up.
    for (auto& d : m.bus.devices())
        if (d->type == "uart16550") {
            auto* u = static_cast<Uart16550*>(d.get());
            u->held.insert(u->held.end(), u->rx.begin(), u->rx.end());
            u->rx.clear();
            break;
        }
    for (auto& h : m.harts) {
        h->rv64 = rv64;
        h->pc = entry;
        h->x[10] = h->id;
        h->x[11] = dtbAddr;
        h->x[12] = bios.empty() ? 0 : infoAddr;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    g_argc = argc;
    g_argv = argv;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mvplat: " << s << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        auto integer = [&](const char* what) -> long long {
            std::string v = next(what);
            try { json::Value n; n.kind = json::Value::Num; n.s = v; return (long long)n.u64(); }
            catch (...) { std::cerr << "mvplat: " << s << ": '" << v << "' is not a number\n"; std::exit(2); }
        };
        if (s == "-h" || s == "-help" || s == "--help") {
            try { loadExt(argc, argv, "-h"); } catch (...) {}
            usage();
            return 0;
        }
        if (s == "-version" || s == "--version") {
            std::cout << "mvplat " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (par::threadsArg(argc, argv, i, o.threads, "mvplat")) continue;
        if (s == "-platform") o.platform = next("a JSON file");
        else if (s == "-elf") o.elf = next("an ELF file");
        else if (s == "-harts") o.harts = (int)integer("a number");
        else if (s == "-quantum") o.quantum = integer("a number of cycles");
        else if (s == "-mhz") { std::string v = next("a frequency"); o.mhz = atof(v.c_str()); }
        else if (s == "-max-cycles") o.maxCycles = integer("a number of cycles");
        else if (s == "-input") { o.input = next("text"); o.haveInput = true; }
        else if (s == "-trace") o.trace = next("a file");
        else if (s == "-bios") o.bios = next("a firmware file");
        else if (s == "-kernel") o.kernel = next("a kernel file");
        else if (s == "-initrd") o.initrd = next("a file");
        else if (s == "-append") { o.append = next("a command line"); o.haveAppend = true; }
        else if (s == "-dtb") o.dtb = next("a device tree blob");
        else if (s == "-dump-dtb") o.dumpDtb = next("a file");
        else if (s == "-disk") o.disks.push_back(next("a disk image"));
        else if (s == "-disk-write") o.diskWrite = true;
        else if (s == "-net") o.nets.push_back(next("user or dgram:LOCAL,REMOTE"));
        else if (s == "-net-pcap") o.netPcap = next("a file");
        else if (s == "-vcon") o.vcon = true;
        else if (s == "-mmu") o.mmu = next("sv39, sv48 or sv57");
        else if (s == "-vlen") o.vlen = (unsigned)std::stoul(next("128, 256, 512 or 1024"));
        else if (s == "-map") o.map = true;
        else if (s == "-q") o.quiet = true;
        else if (!s.empty() && s[0] == '-') {
            int used = 0;
            try {
                if (Ext* x = loadExt(argc, argv, s.c_str())) used = x->option(argc, argv, i);
            } catch (std::exception& e) { std::cerr << "mvplat: " << e.what() << "\n"; return 2; }
            if (!used) {
                bool paid = s == "-gdb" || s == "-gdb-portfile" || s == "-save" || s == "-restore" || s == "-profile";
                std::cerr << "mvplat: " << (paid && !g_ext ? s + " needs the mvplat extension library, which is not installed"
                                                           : "unknown option " + s + " (-h lists them)") << "\n";
                return 2;
            }
            i += used - 1;
        }
        else if (o.elf.empty()) o.elf = s;
        else { std::cerr << "mvplat: two programs: " << o.elf << " and " << s << "\n"; return 2; }
    }
    bool booting = !o.bios.empty() || !o.kernel.empty();
    if (booting && !o.elf.empty()) { std::cerr << "mvplat: give a program or -bios/-kernel, not both\n"; return 2; }
    if ((!o.initrd.empty() || o.haveAppend || !o.dtb.empty()) && !booting) {
        std::cerr << "mvplat: -initrd, -append and -dtb need -kernel or -bios\n";
        return 2;
    }
    if (o.elf.empty() && !booting && o.dumpDtb.empty() && !(g_ext && g_ext->needsNoProgram())) { usage(); return 2; }

    Machine m;
    FILE* trace = nullptr;
    try {
        std::string text = kBuiltin;
        fs::path dir = fs::current_path();
        if (!o.platform.empty()) {
            std::ifstream in(o.platform);
            if (!in) throw std::runtime_error("cannot read " + o.platform);
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            dir = fs::absolute(o.platform).parent_path();
        }
        json::Value pl;
        try { pl = json::parse(text); }
        catch (std::exception& e) { throw std::runtime_error((o.platform.empty() ? "the built-in platform" : o.platform) + ": " + e.what()); }
        build(m, pl, dir, o);
        m.threads = o.threads;
        m.maxCycles = (uint64_t)o.maxCycles;
        if (!o.elf.empty()) m.loadElf(o.elf);
        if (booting || !o.dumpDtb.empty()) {
            if (bootOs(m, o)) return 0;
        }
        if (o.map) {
            std::cout << "mvplat: " << m.harts.size() << " hart" << (m.harts.size() == 1 ? "" : "s") << ", "
                      << 1e6 / (double)m.cycle_ps << " MHz\n";
            for (auto& d : m.bus.devices()) {
                char b[160];
                snprintf(b, sizeof b, "  0x%08llx..0x%08llx  %-10s %s\n", (unsigned long long)d->base,
                         (unsigned long long)(d->base + d->size - 1), d->type.c_str(), d->name.c_str());
                std::cout << b;
            }
            std::cout.flush();
        }
        if (!o.trace.empty()) {
            trace = o.trace == "-" ? stdout : fopen(o.trace.c_str(), "w");
            if (!trace) throw std::runtime_error("cannot write " + o.trace);
            m.trace = trace;
        }
        if (g_ext) g_ext->prepare(m);
        m.start();
        if (g_ext) g_ext->started(m);
        m.run();
    } catch (std::exception& e) {
        fflush(stdout);
        std::cerr << "mvplat: " << e.what() << "\n";
        return 2;
    }
    fflush(stdout);
    if (trace && trace != stdout) fclose(trace);
    int code = m.exitCode();
    if (m.timeout) std::cerr << "mvplat: stopped at cycle " << m.cycles() << " (-max-cycles)\n";
    try {
        if (g_ext) code = g_ext->finished(m, code);
    } catch (std::exception& e) {
        std::cerr << "mvplat: " << e.what() << "\n";
        return 2;
    }
    if (!o.quiet) {
        uint64_t n = 0;
        for (auto& h : m.harts) n += h->instret;
        double s = m.seconds();
        char b[200];
        snprintf(b, sizeof b, "mvplat: %llu instructions, %llu cycles (%.3f ms simulated) in %.3f s, %.1f MIPS; exit code %d\n",
                 (unsigned long long)n, (unsigned long long)m.cycles(), (double)(m.cycles() * m.cycle_ps) / 1e9, s,
                 s > 0 ? (double)n / s / 1e6 : 0.0, code);
        std::cerr << b;
    }
    return code;
}
