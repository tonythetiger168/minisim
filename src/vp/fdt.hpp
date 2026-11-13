// fdt.hpp - the device tree of a platform, as the flattened blob (DTB) that
// firmware and operating systems read at boot (Devicetree Specification
// v0.4, chapter 5).
//
// The tree is made from the platform itself: the harts, each RAM, the CLINT,
// the PLIC, the UARTs and the finisher, with the compatible strings OpenSBI
// and Linux already have drivers for (the ones QEMU's virt board uses).
// Devices with no standard binding (mvp model libraries, RTL) are left out.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "devices.hpp"

namespace vp {

// A writer for the flattened format: big-endian tokens in a structure block,
// property names in a strings block.
class Fdt {
public:
    void begin(const std::string& name) {
        tok(1);
        str(name);
    }
    void end() { tok(2); }
    void prop(const std::string& name, const std::vector<uint8_t>& v) {
        tok(3);
        tok((uint32_t)v.size());
        tok(nameOff(name));
        st_.insert(st_.end(), v.begin(), v.end());
        pad();
    }
    void propU32(const std::string& n, std::vector<uint32_t> cells) {
        std::vector<uint8_t> b;
        for (uint32_t c : cells) be(b, c);
        prop(n, b);
    }
    void propU64(const std::string& n, uint64_t v) { propU32(n, {(uint32_t)(v >> 32), (uint32_t)v}); }
    void propStr(const std::string& n, const std::vector<std::string>& ss) {
        std::vector<uint8_t> b;
        for (auto& s : ss) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }
        prop(n, b);
    }
    void propEmpty(const std::string& n) { prop(n, {}); }
    // reg = <address size> with two cells each.
    void reg(uint64_t base, uint64_t size) {
        propU32("reg", {(uint32_t)(base >> 32), (uint32_t)base, (uint32_t)(size >> 32), (uint32_t)size});
    }

    std::vector<uint8_t> blob(uint32_t bootCpu = 0) {
        tok(9);
        const uint32_t hdr = 40, rsv = 16;
        std::vector<uint8_t> out;
        uint32_t offStruct = hdr + rsv, offStrings = offStruct + (uint32_t)st_.size();
        uint32_t total = offStrings + (uint32_t)strs_.size();
        for (uint32_t v : {0xd00dfeedu, total, offStruct, offStrings, hdr, 17u, 16u, bootCpu,
                           (uint32_t)strs_.size(), (uint32_t)st_.size()})
            be(out, v);
        out.resize(out.size() + rsv, 0);  // an empty memory reservation map
        out.insert(out.end(), st_.begin(), st_.end());
        out.insert(out.end(), strs_.begin(), strs_.end());
        return out;
    }

private:
    static void be(std::vector<uint8_t>& b, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
    }
    void tok(uint32_t v) { be(st_, v); }
    void str(const std::string& s) {
        st_.insert(st_.end(), s.begin(), s.end());
        st_.push_back(0);
        pad();
    }
    void pad() { while (st_.size() % 4) st_.push_back(0); }
    uint32_t nameOff(const std::string& n) {
        auto it = names_.find(n);
        if (it != names_.end()) return it->second;
        uint32_t o = (uint32_t)strs_.size();
        strs_.insert(strs_.end(), n.begin(), n.end());
        strs_.push_back(0);
        names_[n] = o;
        return o;
    }
    std::vector<uint8_t> st_, strs_;
    std::map<std::string, uint32_t> names_;
};

// What the boot loader tells the kernel through /chosen.
struct BootArgs {
    std::string bootargs;
    uint64_t initrdStart = 0, initrdEnd = 0;  // both 0: no initrd
};

inline std::string hexName(const std::string& node, uint64_t a) {
    char b[64];
    snprintf(b, sizeof b, "%s@%llx", node.c_str(), (unsigned long long)a);
    return b;
}

// The device tree of `harts` harts of the given width on the devices of `bus`.
// `cpuHz` and `timebaseHz` are the hart clock and the mtime rate.
inline std::vector<uint8_t> makeDeviceTree(const Bus& bus, unsigned harts, bool rv64, uint64_t cpuHz,
                                           uint64_t timebaseHz, const BootArgs& boot) {
    Fdt f;
    const uint32_t intc0 = 1;  // phandles: hart h's interrupt controller is 1 + h
    const uint32_t plicPh = harts + 1, testPh = harts + 2;
    std::string xl = rv64 ? "rv64" : "rv32";

    f.begin("");
    f.propU32("#address-cells", {2});
    f.propU32("#size-cells", {2});
    f.propStr("compatible", {"minisim,mvplat"});
    f.propStr("model", {"minisim mvplat"});

    bool haveTest = false;
    std::string stdoutPath;
    for (auto& d : bus.devices())
        if (d->type == "uart16550") { stdoutPath = "/soc/" + hexName("serial", d->base); break; }
    f.begin("chosen");
    if (!boot.bootargs.empty()) f.propStr("bootargs", {boot.bootargs});
    if (!stdoutPath.empty()) f.propStr("stdout-path", {stdoutPath});
    if (boot.initrdEnd > boot.initrdStart) {
        f.propU64("linux,initrd-start", boot.initrdStart);
        f.propU64("linux,initrd-end", boot.initrdEnd);
    }
    f.end();

    for (auto& d : bus.devices()) {
        if (d->type != "ram") continue;
        f.begin(hexName("memory", d->base));
        f.propStr("device_type", {"memory"});
        f.reg(d->base, d->size);
        f.end();
    }

    f.begin("cpus");
    f.propU32("#address-cells", {1});
    f.propU32("#size-cells", {0});
    f.propU32("timebase-frequency", {(uint32_t)timebaseHz});
    for (unsigned h = 0; h < harts; h++) {
        f.begin(hexName("cpu", h));
        f.propStr("device_type", {"cpu"});
        f.propU32("reg", {h});
        f.propStr("status", {"okay"});
        f.propStr("compatible", {"riscv"});
        f.propStr("riscv,isa", {xl + "imafdc_zicntr_zicsr_zifencei"});
        f.propStr("riscv,isa-base", {xl + "i"});
        f.propStr("riscv,isa-extensions", {"i", "m", "a", "f", "d", "c", "zicntr", "zicsr", "zifencei"});
        f.propStr("mmu-type", {rv64 ? "riscv,sv39" : "riscv,sv32"});
        f.propU32("clock-frequency", {(uint32_t)cpuHz});
        f.begin("interrupt-controller");
        f.propU32("#interrupt-cells", {1});
        f.propEmpty("interrupt-controller");
        f.propStr("compatible", {"riscv,cpu-intc"});
        f.propU32("phandle", {intc0 + h});
        f.end();
        f.end();
    }
    f.end();  // cpus

    f.begin("soc");
    f.propU32("#address-cells", {2});
    f.propU32("#size-cells", {2});
    f.propStr("compatible", {"simple-bus"});
    f.propEmpty("ranges");
    for (auto& d : bus.devices()) {
        if (d->type == "clint") {
            f.begin(hexName("clint", d->base));
            f.propStr("compatible", {"sifive,clint0", "riscv,clint0"});
            f.reg(d->base, d->size);
            std::vector<uint32_t> ie;
            for (unsigned h = 0; h < harts; h++) for (uint32_t c : {intc0 + h, 3u, intc0 + h, 7u}) ie.push_back(c);
            f.propU32("interrupts-extended", ie);
            f.end();
        } else if (d->type == "plic") {
            auto* p = static_cast<const Plic*>(d.get());
            f.begin(hexName("plic", d->base));
            f.propStr("compatible", {"sifive,plic-1.0.0", "riscv,plic0"});
            f.reg(d->base, d->size);
            f.propU32("#address-cells", {0});
            f.propU32("#interrupt-cells", {1});
            f.propEmpty("interrupt-controller");
            f.propU32("riscv,ndev", {p->nsrc});
            std::vector<uint32_t> ie;
            for (unsigned h = 0; h < harts; h++) for (uint32_t c : {intc0 + h, 11u, intc0 + h, 9u}) ie.push_back(c);
            f.propU32("interrupts-extended", ie);
            f.propU32("phandle", {plicPh});
            f.end();
        } else if (d->type == "uart16550") {
            auto* u = static_cast<const Uart16550*>(d.get());
            f.begin(hexName("serial", d->base));
            f.propStr("compatible", {"ns16550a"});
            f.reg(d->base, d->size);
            f.propU32("clock-frequency", {3686400});
            if (u->shift) f.propU32("reg-shift", {u->shift});
            if (u->irq) {
                f.propU32("interrupt-parent", {plicPh});
                f.propU32("interrupts", {u->irq});
            }
            f.end();
        } else if (d->type == "finisher") {
            f.begin(hexName("test", d->base));
            f.propStr("compatible", {"sifive,test1", "sifive,test0", "syscon"});
            f.reg(d->base, d->size);
            f.propU32("phandle", {testPh});
            f.end();
            haveTest = true;
        }
    }
    f.end();  // soc
    if (haveTest) {
        f.begin("poweroff");
        f.propStr("compatible", {"syscon-poweroff"});
        f.propU32("regmap", {testPh});
        f.propU32("offset", {0});
        f.propU32("value", {0x5555});
        f.end();
        f.begin("reboot");
        f.propStr("compatible", {"syscon-reboot"});
        f.propU32("regmap", {testPh});
        f.propU32("offset", {0});
        f.propU32("value", {0x7777});
        f.end();
    }
    f.end();  // root
    return f.blob();
}

}  // namespace vp
