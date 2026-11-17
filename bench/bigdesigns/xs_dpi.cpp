// Minimal DPI-C environment for XiangShan SimTop on msim (no DiffTest/NEMU):
// RAM from a raw .bin at 0x80000000 ($XS_BIN), the default flash boot stub,
// the trap event as the end of test, and no-op stubs for the other DiffTest probes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "svdpi.h"
static int xs_trace() { static int t = -1; if (t < 0) t = getenv("XS_TRACE") != 0; return t; }
static uint64_t *ram; static const uint64_t RAM_WORDS = (256ull << 20) / 8;   // 256 MB
static void ram_init() {
  if (ram) return;
  ram = (uint64_t *)calloc(RAM_WORDS, 8);
  const char *f = getenv("XS_BIN");
  if (!f) { fprintf(stderr, "xs_dpi: set XS_BIN\n"); exit(2); }
  FILE *fp = fopen(f, "rb"); if (!fp) { perror(f); exit(2); }
  size_t n = fread(ram, 1, RAM_WORDS * 8, fp); fclose(fp);
  fprintf(stderr, "xs_dpi: loaded %zu bytes from %s\n", n, f);
}
extern "C" long long difftest_ram_read(long long idx) { ram_init();
  if (xs_trace()) { static int n; if (n++ < 50) printf("xs_dpi: ram_read %llx\n", idx); } return (uint64_t)idx < RAM_WORDS ? ram[idx] : 0; }
extern "C" void difftest_ram_write(long long idx, long long data, long long mask) {
  ram_init(); if ((uint64_t)idx < RAM_WORDS) ram[idx] = (ram[idx] & ~mask) | (data & mask); }
extern "C" void flash_read(unsigned addr, unsigned long long *data) {
  static const uint64_t f[3] = {0x0010029b74446073ull, 0x3003307302a29313ull, 0x0002806701f29293ull};
  unsigned i = addr / 8; *data = i < 3 ? f[i] : 0;
  if (xs_trace()) printf("xs_dpi: flash_read %x -> %016llx\n", addr, *data); }
static uint64_t last_instr, last_cycle;
extern "C" void v_difftest_TrapEvent(uint8_t hasTrap, uint64_t cycle, uint64_t instr, uint8_t hasWFI, uint64_t code, uint64_t pc, uint8_t coreid) {
  last_instr = instr; last_cycle = cycle;
  if (hasTrap) {
    printf("xs_dpi: HIT %s TRAP at pc 0x%llx code %llu, instrCnt %llu cycleCnt %llu\n", code == 0 ? "GOOD" : "BAD",
           (unsigned long long)pc, (unsigned long long)code, (unsigned long long)instr, (unsigned long long)cycle);
    fflush(stdout); exit(code == 0 ? 0 : 1);
  }
}
extern "C" void xs_assert(long long line) { fprintf(stderr, "xs_dpi: assertion failed at line %lld\n", line); exit(3); }
extern "C" void xs_assert_v2(const char *file, long long line) { fprintf(stderr, "xs_dpi: assertion failed at %s:%lld (core cycle %llu)\n", file, line, (unsigned long long)last_cycle); if (!getenv("XS_NOSTOP")) exit(3); }
extern "C" int jtag_tick(svBit *tck, svBit *tms, svBit *tdi, svBit *trstn, svBit tdo) { *tck = 0; *tms = 0; *tdi = 0; *trstn = 1; return 0; }
extern "C" void sd_setaddr(int) {}
extern "C" void sd_read(int *d) { *d = 0; }
extern "C" long long amo_helper(char, long long, long long, char) { return 0; }
extern "C" char pte_helper(long long, long long, long long, char, char, long long, char, long long *pte, char *lvl, long long *p1, long long *p2, char *l1) { *pte = 0; *lvl = 0; *p1 = *p2 = 0; *l1 = 0; return 0; }
extern "C" void topdown_rob_info_dpic_288_48(unsigned, unsigned, const svBitVecVal *, svBitVecVal *) {}
extern "C" void topdown_iq_info_dpic_80(unsigned, const svBitVecVal *, svBitVecVal *) {}
extern "C" void topdown_iq_info_dpic_208(unsigned, const svBitVecVal *, svBitVecVal *) {}
extern "C" uint64_t xs_last_instr() { return last_instr; }
#include "dpic_stubs.inc"
