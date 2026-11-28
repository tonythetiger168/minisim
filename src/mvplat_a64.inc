// mvplat_a64.inc - mvplat's AArch64 platforms: the built-in board, a
// platform file with "arch": "aarch64", the device tree, loading a
// bare-metal ELF or booting a Linux arm64 Image, and the run. Part of
// mvplat.cpp (inside its anonymous namespace).

const char* kBuiltinA64 = R"({
  "arch": "aarch64",
  "name": "virt-arm64",
  "harts": 1,
  "devices": [
    {"type": "finisher", "base": "0x00100000"},
    {"type": "gicv3", "name": "gic", "base": "0x08000000", "redist": "0x080a0000", "spis": 96},
    {"type": "pl011", "name": "uart0", "base": "0x09000000", "irq": 1},
    {"type": "goldfish-rtc", "name": "rtc", "base": "0x09010000", "irq": 2},
    {"type": "ram", "name": "dram", "base": "0x40000000", "size": "1G"}
  ]
})";

// Is this file an AArch64 program: an ELF for machine 183, or a Linux arm64
// Image (the magic "ARM\x64" at offset 56)?
bool a64File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    uint8_t h[64] = {};
    in.read((char*)h, sizeof h);
    if (in.gcount() < 20) return false;
    if (!memcmp(h, "\x7f" "ELF", 4)) return (h[18] | h[19] << 8) == 183;
    return in.gcount() >= 60 && !memcmp(h + 56, "ARM\x64", 4);
}

struct A64Board {
    a64::Machine m;
    uint64_t gicBase = 0, gicRedist = 0;
    std::vector<std::pair<std::string, uint64_t>> virtio;  // for the device tree
};

void buildA64(A64Board& b, const json::Value& pl, const fs::path& dir, const Options& o) {
    auto rel = [&](const std::string& p) { return fs::path(p).is_absolute() ? p : (dir / p).string(); };
    a64::Machine& m = b.m;
    unsigned n = pl.get("harts") ? (unsigned)pl.get("harts")->u64() : 1;
    if (o.harts > 0) n = (unsigned)o.harts;
    if (n < 1 || n > 256) throw std::runtime_error("harts must be 1 to 256 on an AArch64 platform");
    double mhz = pl.get("mhz") ? pl.get("mhz")->num() : 100.0;
    if (o.mhz > 0) mhz = o.mhz;
    if (mhz <= 0 || mhz > 1e6) throw std::runtime_error("mhz must be above 0");
    m.cyclePs_ = std::max<uint64_t>(1, (uint64_t)(1e6 / mhz + 0.5));
    if (pl.get("timer_hz")) m.timerHz = pl.get("timer_hz")->u64();
    m.mt = o.mt && n > 1;
    m.threads = o.threads;
    m.quantum = pl.get("quantum") ? pl.get("quantum")->u64() : m.mt ? 20000 : 10000;
    if (o.quantum > 0) m.quantum = (uint64_t)o.quantum;
    m.addCpus(n);
    const json::Value* devs = pl.get("devices");
    if (!devs || devs->kind != json::Value::Arr) throw std::runtime_error("the platform has no \"devices\" list");
    unsigned spis = 96;
    for (auto& d : devs->a)
        if (d.get("type") && d.get("type")->str() == "gicv3" && d.get("spis")) spis = (unsigned)d.get("spis")->u64();
    m.makeGic(spis);
    bool firstUart = true, haveGic = false;
    for (auto& d : devs->a) {
        if (!d.get("type")) throw std::runtime_error(d.where() + "a device has no \"type\"");
        const std::string type = d.get("type")->str();
        const std::string name = d.get("name") ? d.get("name")->str() : type;
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
        } else if (type == "gicv3") {
            b.gicBase = base();
            if (!d.get("redist")) throw std::runtime_error(d.where() + "'" + name + "' has no \"redist\" (the redistributors' base)");
            b.gicRedist = d.get("redist")->u64();
            auto g = std::make_unique<a64::GicDist>(*m.gic);
            g->name = name;
            g->base = b.gicBase;
            g->p = &m;
            m.bus.add(std::move(g));
            auto r = std::make_unique<a64::GicRedist>(*m.gic, n);
            r->name = name + "-redist";
            r->base = b.gicRedist;
            r->p = &m;
            m.bus.add(std::move(r));
            haveGic = true;
            continue;
        } else if (type == "pl011") {
            auto u = std::make_unique<Pl011>();
            u->name = name;
            u->base = base();
            u->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            std::string in = d.get("input") ? d.get("input")->str() : "";
            if (firstUart && o.haveInput && !o.vcon) in = o.input;
            for (char ch : in) u->held.push_back((uint8_t)ch);
            if (firstUart && !o.vcon) {
                for (auto& [t, text] : o.inputAt) u->timed.push_back({(uint64_t)(t * 1e12), text});
                std::stable_sort(u->timed.begin(), u->timed.end(), [](auto& a, auto& c) { return a.first < c.first; });
                if (o.stdinInput) {
                    u->hostFd = 0;
                    ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
                }
            }
            firstUart = false;
            dev = std::move(u);
        } else if (type == "finisher") {
            auto f = std::make_unique<Finisher>();
            f->name = name;
            f->base = base();
            dev = std::move(f);
        } else if (type == "goldfish-rtc") {
            auto r = std::make_unique<GoldfishRtc>();
            r->name = name;
            r->base = base();
            r->irq = d.get("irq") ? (unsigned)d.get("irq")->u64() : 0;
            dev = std::move(r);
        } else if (type.rfind("virtio-", 0) == 0) {
            dev = makeVirtioDev(d, type, name, base(), o, dir);
            if (!dev) throw std::runtime_error(d.where() + "unknown device type '" + type + "'");
        } else if (type == "pcie") {
            auto h = std::make_unique<PcieHost>();
            h->name = name;
            h->base = base();
            h->buses = d.get("buses") ? (unsigned)d.get("buses")->u64() : 1;
            if (h->buses < 1 || h->buses > 256) throw std::runtime_error(d.where() + "PCIe buses must be 1 to 256");
            h->size = (uint64_t)h->buses << 20;
            h->mmioBase = d.get("mmio") ? d.get("mmio")->u64() : 0x10000000;
            h->mmioSize = d.get("mmio_size") ? d.get("mmio_size")->u64() : 0x2eff0000;
            if (!d.get("irq")) throw std::runtime_error(d.where() + "'" + name + "' has no \"irq\" (the first of four SPIs for INTA to INTD)");
            h->irq = (unsigned)d.get("irq")->u64();
            if (const json::Value* fs_ = d.get("devices")) {
                for (auto& cd : fs_->a) {
                    std::string ct = cd.get("type") ? cd.get("type")->str() : "";
                    std::string cn = cd.get("name") ? cd.get("name")->str() : name + "." + ct;
                    auto v = makeVirtioDev(cd, ct, cn, 0, o, dir);
                    if (!v) throw std::runtime_error(cd.where() + "a PCIe device is virtio-blk, virtio-net, virtio-console, virtio-rng or virtio-gpu, not '" + ct + "'");
                    if (h->count() >= 31) throw std::runtime_error(cd.where() + "at most 31 devices behind a PCIe host");
                    v->p = &m;
                    h->addVirtio(std::move(v));
                }
            }
            auto w = h->window();
            w->p = &m;
            m.bus.add(std::move(w));
            dev = std::move(h);
        } else {
            throw std::runtime_error(d.where() + "'" + name + "': type '" + type +
                                     "' is not one an AArch64 platform has (ram, rom, gicv3, pl011, finisher, goldfish-rtc, virtio-*, pcie)");
        }
        dev->p = &m;
        m.bus.add(std::move(dev));
    }
    if (!haveGic) throw std::runtime_error("an AArch64 platform needs a \"gicv3\"");
    // -disk, -net, -vcon: virtio-mmio devices from 0x0a000000, 0x1000 apart,
    // on SPIs 16 up, after any the platform already has
    unsigned slot = 0;
    auto next = [&](VirtioMmio& v, const char* what) {
        uint64_t a;
        do a = 0x0a000000ull + 0x1000ull * slot++; while (m.bus.find(a) && slot < 64);
        v.base = a;
        v.irq = 16 + slot - 1;
        if (v.irq >= spis) throw std::runtime_error(std::string(what) + ": no SPI left");
        v.p = &m;
    };
    for (const std::string& img : o.disks) {
        auto v = std::make_unique<VirtioBlk>();
        v->name = "virtio" + std::to_string(slot);
        next(*v, "-disk");
        v->open(img, o.diskWrite, false);
        m.bus.add(std::move(v));
    }
    unsigned nic = 0;
    for (const std::string& spec : o.nets) {
        auto v = std::make_unique<VirtioNet>();
        v->name = "net" + std::to_string(nic);
        next(*v, "-net");
        v->mac[5] = (uint8_t)(0x56 + nic);
        if (spec.rfind("dgram:", 0) == 0) {
            std::string rest = spec.substr(6);
            size_t cm = rest.find(',');
            if (cm == std::string::npos) throw std::runtime_error("-net dgram:LOCAL,REMOTE: no comma in '" + spec + "'");
            v->useDgram(rest.substr(0, cm), rest.substr(cm + 1));
        } else if (spec != "user") {
            throw std::runtime_error("-net: unknown back end '" + spec + "' (user, dgram:LOCAL,REMOTE)");
        }
        if (nic == 0 && !o.netPcap.empty()) v->capture(o.netPcap);
        m.bus.add(std::move(v));
        nic++;
    }
    if (!o.netPcap.empty() && !nic) throw std::runtime_error("-net-pcap needs -net");
    if (o.vcon) {
        auto v = std::make_unique<VirtioConsole>();
        v->name = "vcon";
        next(*v, "-vcon");
        if (o.haveInput) for (char ch : o.input) v->held.push_back((uint8_t)ch);
        m.bus.add(std::move(v));
    }
}

// The device tree Linux's arm64 kernel reads (the bindings in its
// Documentation/devicetree, the shape of QEMU's virt board).
std::vector<uint8_t> a64DeviceTree(A64Board& b, const std::string& bootargs, uint64_t initrdStart, uint64_t initrdEnd) {
    a64::Machine& m = b.m;
    Fdt f;
    const uint32_t GIC = 1, CLK = 2;
    f.begin("");
    f.propU32("#address-cells", {2});
    f.propU32("#size-cells", {2});
    f.propStr("compatible", {"minisim,mvplat-arm64"});
    f.propStr("model", {"minisim mvplat (AArch64)"});
    f.propU32("interrupt-parent", {GIC});

    f.begin("psci");
    f.propStr("compatible", {"arm,psci-1.0", "arm,psci-0.2"});
    f.propStr("method", {"hvc"});
    f.end();

    f.begin("cpus");
    f.propU32("#address-cells", {1});
    f.propU32("#size-cells", {0});
    for (auto& c : m.cpus) {
        f.begin("cpu@" + std::to_string(c->id));
        f.propStr("device_type", {"cpu"});
        f.propStr("compatible", {"arm,armv8"});
        f.propU32("reg", {c->id});
        f.propStr("enable-method", {"psci"});
        f.end();
    }
    f.end();

    f.begin("timer");
    f.propStr("compatible", {"arm,armv8-timer"});
    f.propU32("interrupts", {1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4});
    f.propEmpty("always-on");
    f.end();

    char nm[64];
    snprintf(nm, sizeof nm, "intc@%llx", (unsigned long long)b.gicBase);
    f.begin(nm);
    f.propStr("compatible", {"arm,gic-v3"});
    f.propU32("#interrupt-cells", {3});
    f.propU32("#address-cells", {2});
    f.propU32("#size-cells", {2});
    f.propEmpty("interrupt-controller");
    const uint64_t rs = 0x20000ull * m.cpus.size();
    f.propU32("reg", {(uint32_t)(b.gicBase >> 32), (uint32_t)b.gicBase, 0, 0x10000,
                      (uint32_t)(b.gicRedist >> 32), (uint32_t)b.gicRedist, (uint32_t)(rs >> 32), (uint32_t)rs});
    f.propU32("phandle", {GIC});
    f.end();

    f.begin("apb-pclk");
    f.propStr("compatible", {"fixed-clock"});
    f.propU32("#clock-cells", {0});
    f.propU32("clock-frequency", {24000000});
    f.propStr("clock-output-names", {"clk24mhz"});
    f.propU32("phandle", {CLK});
    f.end();

    std::string stdoutPath;
    for (auto& d : m.bus.devices()) {
        snprintf(nm, sizeof nm, "%llx", (unsigned long long)d->base);
        if (d->type == "ram") {
            f.begin(std::string("memory@") + nm);
            f.propStr("device_type", {"memory"});
            f.reg(d->base, d->size);
            f.end();
        } else if (d->type == "pl011") {
            auto* u = static_cast<Pl011*>(d.get());
            const std::string node = std::string("pl011@") + nm;
            f.begin(node);
            f.propStr("compatible", {"arm,pl011", "arm,primecell"});
            f.reg(d->base, d->size);
            f.propU32("interrupts", {0, u->irq, 4});
            f.propU32("clocks", {CLK, CLK});
            f.propStr("clock-names", {"uartclk", "apb_pclk"});
            f.end();
            if (stdoutPath.empty()) stdoutPath = "/" + node;
        } else if (d->type.rfind("virtio-", 0) == 0) {
            auto* v = static_cast<VirtioMmio*>(d.get());
            f.begin(std::string("virtio_mmio@") + nm);
            f.propStr("compatible", {"virtio,mmio"});
            f.reg(d->base, d->size);
            f.propU32("interrupts", {0, v->irq, 4});
            f.propEmpty("dma-coherent");
            f.end();
        } else if (d->type == "pcie") {
            // a generic ECAM host, one 32-bit memory window, INTx through an
            // interrupt-map (swizzled by slot) onto four SPIs
            auto* h = static_cast<PcieHost*>(d.get());
            f.begin(std::string("pcie@") + nm);
            f.propStr("compatible", {"pci-host-ecam-generic"});
            f.propStr("device_type", {"pci"});
            f.propU32("#address-cells", {3});
            f.propU32("#size-cells", {2});
            f.propU32("#interrupt-cells", {1});
            f.propU32("bus-range", {0, h->buses - 1});
            f.propU32("linux,pci-domain", {0});
            f.propEmpty("dma-coherent");
            f.reg(d->base, d->size);
            const uint32_t sp = h->mmioBase + h->mmioSize > (1ull << 32) ? 0x03000000u : 0x02000000u;
            f.propU32("ranges", {sp, (uint32_t)(h->mmioBase >> 32), (uint32_t)h->mmioBase, (uint32_t)(h->mmioBase >> 32),
                                 (uint32_t)h->mmioBase, (uint32_t)(h->mmioSize >> 32), (uint32_t)h->mmioSize});
            std::vector<uint32_t> im;
            for (uint32_t sl = 0; sl < 4; sl++)
                for (uint32_t pin = 1; pin <= 4; pin++)
                    for (uint32_t cell : {sl << 11, 0u, 0u, pin, GIC, 0u, 0u, 0u, h->irq + (sl + pin - 1) % 4, 4u}) im.push_back(cell);
            f.propU32("interrupt-map", im);
            f.propU32("interrupt-map-mask", {0x1800, 0, 0, 7});
            f.end();
        } else if (d->type == "goldfish-rtc") {
            auto* r = static_cast<GoldfishRtc*>(d.get());
            f.begin(std::string("rtc@") + nm);
            f.propStr("compatible", {"google,goldfish-rtc"});
            f.reg(d->base, d->size);
            f.propU32("interrupts", {0, r->irq, 4});
            f.end();
        }
    }
    f.begin("chosen");
    if (!bootargs.empty()) f.propStr("bootargs", {bootargs});
    if (!stdoutPath.empty()) f.propStr("stdout-path", {stdoutPath});
    if (initrdEnd) {
        f.propU64("linux,initrd-start", initrdStart);
        f.propU64("linux,initrd-end", initrdEnd);
    }
    f.end();
    f.end();
    return f.blob();
}

void pokeA64(a64::Machine& m, uint64_t a, const uint8_t* p, size_t n) {
    for (size_t k = 0; k < n;) {
        Device* d = m.bus.find(a + k);
        if (!d || !d->memory()) {
            char b[64];
            snprintf(b, sizeof b, "nothing to load into at 0x%llx", (unsigned long long)(a + k));
            throw std::runtime_error(b);
        }
        const size_t room = std::min<size_t>(n - k, d->base + d->size - (a + k));
        memcpy(d->memory() + (a + k - d->base), p + k, room);
        k += room;
    }
}

int runA64(const Options& o, const std::string& text, const fs::path& dir) {
    A64Board b;
    a64::Machine& m = b.m;
    m.quiet = o.quiet;
    json::Value pl = json::parse(text);
    buildA64(b, pl, dir, o);
    m.maxCycles = o.maxCycles > 0 ? (uint64_t)o.maxCycles : UINT64_MAX;
    Device* ram = nullptr;
    for (auto& d : m.bus.devices())
        if (d->type == "ram" && (!ram || d->base < ram->base)) ram = d.get();
    if (!ram) throw std::runtime_error("the platform has no RAM");
    a64::Cpu& c0 = *m.cpus[0];
    const std::string prog = !o.elf.empty() ? o.elf : o.kernel;
    std::vector<uint8_t> initrd;
    if (!o.initrd.empty()) initrd = readFile(o.initrd);
    uint64_t dtbAddr = 0;
    auto placeDtb = [&](uint64_t above) {
        // the device tree at the top of the first RAM, the initrd below it
        std::vector<uint8_t> probe = a64DeviceTree(b, o.append, 1, initrd.empty() ? 0 : 1);
        const uint64_t end = ram->base + std::min<uint64_t>(ram->size, 0x40000000ull);
        dtbAddr = (end - probe.size() - 64) & ~0xfffull;
        uint64_t is = 0, ie = 0;
        if (!initrd.empty()) {
            is = (dtbAddr - initrd.size()) & ~0xfffull;
            if (dtbAddr < initrd.size() || is < above) throw std::runtime_error("the initrd does not fit in RAM above the kernel");
            pokeA64(m, is, initrd.data(), initrd.size());
            ie = is + initrd.size();
        }
        std::vector<uint8_t> dtb = o.dtb.empty() ? a64DeviceTree(b, o.append, is, ie) : readFile(o.dtb);
        if (!o.dumpDtb.empty()) {
            std::ofstream out(o.dumpDtb, std::ios::binary);
            out.write((const char*)dtb.data(), (std::streamsize)dtb.size());
        }
        pokeA64(m, dtbAddr, dtb.data(), dtb.size());
    };
    if (prog.empty()) {
        if (!o.dumpDtb.empty()) { placeDtb(ram->base); return 0; }
        throw std::runtime_error("no program");
    }
    std::vector<uint8_t> f = readFile(prog);
    if (f.size() >= 4 && !memcmp(f.data(), "\x7f" "ELF", 4)) {
        Elf e = readElf(prog);
        uint64_t top = ram->base;
        for (auto& s : e.segs) { pokeA64(m, s.addr, s.data.data(), s.data.size()); top = std::max(top, s.addr + s.data.size()); }
        placeDtb(top);
        c0.pc = e.entry;
    } else {
        // Linux arm64 Image (Documentation/arch/arm64/booting.rst): at a
        // 2 MiB-aligned base + text_offset, x0 = the device tree
        uint64_t textOffset = 0, imageSize = f.size();
        memcpy(&textOffset, f.data() + 8, 8);
        memcpy(&imageSize, f.data() + 16, 8);
        if (!imageSize) imageSize = f.size();
        const uint64_t load = ((ram->base + 0x1fffff) & ~0x1fffffull) + textOffset;
        pokeA64(m, load, f.data(), f.size());
        placeDtb(load + std::max<uint64_t>(imageSize, f.size()));
        c0.pc = load;
    }
    c0.x[0] = dtbAddr;
    c0.off = false;
    m.program = prog;
    if (g_ext) g_ext->prepareA64(m);
    if (m.profile)
        for (auto& c : m.cpus) c->prof = std::make_unique<a64::Prof>();
    if (o.map) {
        std::cout << "mvplat: " << m.cpus.size() << " AArch64 processor" << (m.cpus.size() == 1 ? "" : "s") << ", "
                  << 1e6 / (double)m.cyclePs_ << " MHz\n";
        for (auto& d : m.bus.devices()) {
            char buf[160];
            snprintf(buf, sizeof buf, "  0x%08llx..0x%08llx  %-10s %s\n", (unsigned long long)d->base,
                     (unsigned long long)(d->base + d->size - 1), d->type.c_str(), d->name.c_str());
            std::cout << buf;
        }
        std::cout.flush();
    }
    const auto t0 = std::chrono::steady_clock::now();
    int code = m.run();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fflush(stdout);
    uint64_t cyc = 0;
    for (auto& c : m.cpus) cyc = std::max(cyc, c->cycle);
    if (!m.exit_ && cyc >= m.maxCycles) {
        std::cerr << "mvplat: stopped at cycle " << cyc << " (-max-cycles)\n";
        code = 4;
    }
    if (!o.quiet) {
        const uint64_t n = m.instructions();
        char buf[200];
        snprintf(buf, sizeof buf, "mvplat: %llu instructions, %llu cycles (%.3f ms simulated) in %.3f s, %.1f MIPS; exit code %d\n",
                 (unsigned long long)n, (unsigned long long)cyc, (double)(cyc * m.cyclePs_) / 1e9, s,
                 s > 0 ? (double)n / s / 1e6 : 0.0, code);
        std::cerr << buf;
    }
    if (g_ext) code = g_ext->finishedA64(m, code);
    return code;
}
