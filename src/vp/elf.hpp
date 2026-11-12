// elf.hpp - reading a 32-bit little-endian RISC-V ELF executable: its loadable
// segments, its entry point and its symbols (for tohost, -profile and gdb).
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace vp {

struct Elf {
    struct Seg {
        uint64_t addr;
        std::vector<uint8_t> data;  // file bytes, then zeros up to memsz
    };
    uint64_t entry = 0;
    std::vector<Seg> segs;
    std::map<std::string, uint64_t> syms;          // name -> address
    std::map<uint64_t, std::pair<std::string, uint64_t>> funcs;  // address -> name, size

    // The function containing `a`, as "name+0x12", or "" when none does.
    std::string where(uint64_t a) const {
        auto it = funcs.upper_bound(a);
        if (it == funcs.begin()) return "";
        --it;
        uint64_t sz = it->second.second ? it->second.second : 1;
        if (a >= it->first + sz) return "";
        if (a == it->first) return it->second.first;
        char b[32];
        snprintf(b, sizeof b, "+0x%llx", (unsigned long long)(a - it->first));
        return it->second.first + b;
    }
};

inline Elf readElf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    auto need = [&](uint64_t off, uint64_t n) {
        if (off + n > f.size() || off + n < off) throw std::runtime_error(path + ": truncated ELF file");
    };
    need(0, 52);
    if (memcmp(f.data(), "\x7f" "ELF", 4) != 0) throw std::runtime_error(path + ": not an ELF file");
    if (f[4] != 1) throw std::runtime_error(path + ": a 64-bit ELF file; mvplat runs RV32 programs");
    if (f[5] != 1) throw std::runtime_error(path + ": not little-endian");
    auto u16 = [&](uint64_t o) { need(o, 2); return (uint32_t)(f[o] | f[o + 1] << 8); };
    auto u32 = [&](uint64_t o) {
        need(o, 4);
        return (uint32_t)f[o] | (uint32_t)f[o + 1] << 8 | (uint32_t)f[o + 2] << 16 | (uint32_t)f[o + 3] << 24;
    };
    if (u16(18) != 243) throw std::runtime_error(path + ": not a RISC-V ELF file");
    Elf e;
    e.entry = u32(24);
    uint32_t phoff = u32(28), shoff = u32(32);
    uint32_t phentsize = u16(42), phnum = u16(44), shentsize = u16(46), shnum = u16(48);
    for (uint32_t i = 0; i < phnum; i++) {
        uint64_t ph = phoff + (uint64_t)i * phentsize;
        if (u32(ph) != 1) continue;  // PT_LOAD
        uint32_t off = u32(ph + 4), paddr = u32(ph + 12), filesz = u32(ph + 16), memsz = u32(ph + 20);
        if (!memsz) continue;
        need(off, filesz);
        Elf::Seg s;
        s.addr = paddr;
        s.data.assign(f.begin() + off, f.begin() + off + filesz);
        s.data.resize(memsz, 0);
        e.segs.push_back(std::move(s));
    }
    // Symbols: every SYMTAB section, names from its linked string table.
    for (uint32_t i = 0; i < shnum && shoff; i++) {
        uint64_t sh = shoff + (uint64_t)i * shentsize;
        if (u32(sh + 4) != 2) continue;  // SHT_SYMTAB
        uint32_t off = u32(sh + 16), size = u32(sh + 20), link = u32(sh + 24), entsize = u32(sh + 36);
        uint64_t strsh = shoff + (uint64_t)link * shentsize;
        uint32_t stroff = u32(strsh + 16), strsize = u32(strsh + 20);
        need(stroff, strsize);
        if (!entsize) entsize = 16;
        for (uint32_t k = 0; k + entsize <= size; k += entsize) {
            uint64_t s = off + k;
            uint32_t name = u32(s), value = u32(s + 4), sz = u32(s + 8);
            uint8_t info = (need(s + 12, 1), f[s + 12]);
            if (name >= strsize) continue;
            const char* nm = reinterpret_cast<const char*>(&f[stroff + name]);
            size_t len = strnlen(nm, strsize - name);
            std::string n(nm, len);
            if (n.empty()) continue;
            unsigned type = info & 15;
            if (type == 1 || type == 2 || type == 0) e.syms.emplace(n, value);  // OBJECT, FUNC, NOTYPE
            if (type == 2) e.funcs[value] = {n, sz};
        }
    }
    return e;
}

}  // namespace vp
