/* The test platform side of examples/pss/dma.pss for the C output: a memory
   and a DMA model behind the functions the scenarios call. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t mem[0x2000];

static void range(uint32_t addr, uint32_t size)
{
  if (addr < 0x1000 || addr + size > 0x2000 || addr % 4 || size % 4) {
    printf("ERROR: bad region %x +%u\n", (unsigned)addr, (unsigned)size);
    exit(1);
  }
}
void mem_fill(uint32_t addr, uint16_t size, uint8_t seed)
{
  range(addr, size);
  for (unsigned i = 0; i < size; i++) mem[addr + i] = (uint8_t)(seed + i);
}
void dma_copy(uint8_t chan, uint32_t src, uint32_t dst, uint16_t size)
{
  range(src, size);
  range(dst, size);
  if (chan > 3 || (src < dst + size && dst < src + size)) {
    printf("ERROR: bad copy on channel %u\n", (unsigned)chan);
    exit(1);
  }
  for (unsigned i = 0; i < size; i++) mem[dst + i] = mem[src + i];
}
uint32_t mem_sum(uint32_t addr, uint16_t size)
{
  uint32_t s = 0;
  range(addr, size);
  for (unsigned i = 0; i < size; i++) s += mem[addr + i];
  return s | 1;   /* the data depends on the order of overlapping copies; the test prints none of it */
}
