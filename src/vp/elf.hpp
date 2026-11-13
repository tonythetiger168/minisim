// elf.hpp - reading a 32-bit or 64-bit little-endian RISC-V ELF executable: its loadable
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
    bool is64 = false;  // ELFCLASS64: the program is RV64
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
    if (f[4] != 1 && f[4] != 2) throw std::runtime_error(path + ": not a 32-bit or 64-bit ELF file");
    if (f[5] != 1) throw std::runtime_error(path + ": not little-endian");
    const bool w = f[4] == 2;  // ELF64
    auto u16 = [&](uint64_t o) { need(o, 2); return (uint32_t)(f[o] | f[o + 1] << 8); };
    auto u32 = [&](uint64_t o) {
        need(o, 4);
        return (uint32_t)f[o] | (uint32_t)f[o + 1] << 8 | (uint32_t)f[o + 2] << 16 | (uint32_t)f[o + 3] << 24;
    };
    auto u64 = [&](uint64_t o) { return (uint64_t)u32(o) | (uint64_t)u32(o + 4) << 32; };
    auto addr = [&](uint64_t o) { return w ? u64(o) : (uint64_t)u32(o); };  // an Elf_Addr / Elf_Off
    if (u16(18) != 243) throw std::runtime_error(path + ": not a RISC-V ELF file");
    Elf e;
    e.is64 = w;
    e.entry = addr(24);
    uint64_t phoff = addr(w ? 32 : 28), shoff = addr(w ? 40 : 32);
    unsigned eh = w ? 54 : 42;
    uint32_t phentsize = u16(eh), phnum = u16(eh + 2), shentsize = u16(eh + 4), shnum = u16(eh + 6);
    for (uint32_t i = 0; i < phnum; i++) {
        uint64_t ph = phoff + (uint64_t)i * phentsize;
        if (u32(ph) != 1) continue;  // PT_LOAD
        uint64_t off, paddr, filesz, memsz;
        if (w) { off = u64(ph + 8); paddr = u64(ph + 24); filesz = u64(ph + 32); memsz = u64(ph + 40); }
        else { off = u32(ph + 4); paddr = u32(ph + 12); filesz = u32(ph + 16); memsz = u32(ph + 20); }
        if (!memsz) continue;
        if (filesz > memsz || memsz > (1ull << 32)) throw std::runtime_error(path + ": a bad program header");
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
        uint64_t off = addr(sh + (w ? 24 : 16)), size = addr(sh + (w ? 32 : 20)), entsize = addr(sh + (w ? 56 : 36));
        uint32_t link = u32(sh + (w ? 40 : 24));
        uint64_t strsh = shoff + (uint64_t)link * shentsize;
        uint64_t stroff = addr(strsh + (w ? 24 : 16)), strsize = addr(strsh + (w ? 32 : 20));
        need(stroff, strsize);
        if (!entsize) entsize = w ? 24 : 16;
        for (uint64_t k = 0; k + entsize <= size; k += entsize) {
            uint64_t s = off + k;
            uint32_t name = u32(s);
            uint64_t value = w ? u64(s + 8) : u32(s + 4), sz = w ? u64(s + 16) : u32(s + 8);
            uint8_t info = (need(s + (w ? 4 : 12), 1), f[s + (w ? 4 : 12)]);
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
