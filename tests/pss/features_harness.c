/* The platform side of features.pss for the C output. */
#include <stdint.h>
#include <stdio.h>

void reg_write(uint32_t addr, uint32_t data)
{
  printf("write %x %x\n", (unsigned)addr, (unsigned)data);
}
uint32_t reg_read(uint32_t addr)
{
  return addr ^ 0x5a;
}
