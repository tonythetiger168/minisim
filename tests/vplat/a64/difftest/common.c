#include <stdint.h>
extern void runall(void);
extern uint64_t outbuf[];
extern const uint64_t nblocks;
void emit(const char* s, unsigned n);
static char buf[256];
static unsigned pos;
static void flush(void) { emit(buf, pos); pos = 0; }
static void put(char c) { buf[pos++] = c; if (pos == sizeof buf) flush(); }
static void hex(uint64_t v) { for (int i = 60; i >= 0; i -= 4) put("0123456789abcdef"[(v >> i) & 15]); }
void dumpall(void) {
    runall();
    for (uint64_t b = 0; b < nblocks; b++) {
        uint64_t* o = outbuf + b * 96;
        for (int i = 0; i < 31; i++) { hex(o[i]); put(i == 30 ? '\n' : ' '); }
        for (int i = 0; i < 32; i++) { hex(o[32 + 2 * i + 1]); hex(o[32 + 2 * i]); put(i % 8 == 7 ? '\n' : ' '); }
    }
    flush();
}
