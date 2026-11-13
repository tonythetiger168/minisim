// mvplat.cpp - a virtual platform: RISC-V harts, a memory map of peripheral
// models, and the software that runs on them, before the hardware exists.
//
//   mvplat prog.elf                         run on the built-in platform
//   mvplat -platform soc.json prog.elf      run on a platform described in JSON
//   mvplat prog.elf -harts 4                four harts, on every core
//   mvplat prog.elf -trace commits.log      a commit log, in spike's format
//
// The platform is RV32IMAC harts (machine, supervisor and user mode, PMP, Sv32) on a bus of
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
#include "vp/json.hpp"
#include "vp/machine.hpp"

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
Ext* g_ext = nullptr;

void usage() {
    std::cout <<
        "usage: mvplat [-platform FILE] PROGRAM [options]\n"
        "  PROGRAM            an RV32 ELF executable (or -elf FILE)\n"
        "  -platform FILE     the platform in JSON (default: the built-in virt32 board)\n"
        "  -harts N           number of harts (overrides the platform)\n"
        "  -threads N         threads for the harts (default: every CPU; -j N too)\n"
        "  -quantum N         cycles each hart runs between synchronisations (default 1000)\n"
        "  -mhz F             hart clock in MHz (default 100)\n"
        "  -max-cycles N      stop after N cycles (exit code 4)\n"
        "  -input TEXT        characters the first UART receives\n"
        "  -trace FILE        commit log, one line per instruction (spike's format)\n"
        "  -map               print the memory map\n"
        "  -q                 no summary line on stderr\n"
        "  -version           print the version\n"
        "Exit code: the program's own (finisher, HTIF tohost), 3 if every hart waits\n"
        "forever, 4 at -max-cycles, 2 for a usage or platform error.\n";
    if (g_ext) std::cout << g_ext->usage();
}

struct Options {
    std::string platform, elf, trace, input;
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
    uint32_t reset = pl.get("reset") ? (uint32_t)pl.get("reset")->u64() : 0x80000000u;
    m.addHarts(nh, reset);

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
            if (firstUart && o.haveInput) in = o.input;
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
        } else {
            Ext* x = loadExt(g_argc, g_argv, ("device type '" + type + "'").c_str());
            if (x && x->device(m, d, type, name, dir)) continue;
            throw std::runtime_error(d.where() + "unknown device type '" + type +
                                     "' (ram, rom, uart16550, clint, plic, finisher" +
                                     (x ? "" : "; mvp and rtl need the mvplat extension library") + ")");
        }
        m.bus.add(std::move(dev));
    }
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
    if (o.elf.empty() && !(g_ext && g_ext->needsNoProgram())) { usage(); return 2; }

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
