/* The test platform side of tests/pss/addr.pss: every permanent claim lies
   in a region of the address space, is aligned, and overlaps no other claim
   of its test. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t used[0x2000];
static int claims;

void claim(uint64_t addr, uint64_t size)
{
  int in_lo = addr >= 0x1000 && addr + size <= 0x1100;
  int in_hi = addr >= 0x1110 && addr + size <= 0x1150;
  if (claims++ % 5 == 0) memset(used, 0, sizeof used);   /* five claims a test */
  if (!(in_lo || in_hi) || addr % 16 || size < 16 || size > 48) {
    printf("ERROR: bad claim %x +%u\n", (unsigned)addr, (unsigned)size);
    return;
  }
  for (uint64_t i = 0; i < size; i++) {
    if (used[addr + i]) {
      printf("ERROR: claim %x +%u overlaps an earlier one\n", (unsigned)addr, (unsigned)size);
      return;
    }
    used[addr + i] = 1;
  }
  printf("claim %x +%u\n", (unsigned)addr, (unsigned)size);
}
