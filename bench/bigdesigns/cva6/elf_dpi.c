// Minimal stand-in for CVA6's corev_apu/tb/dpi (which needs Spike's fesvr):
// ELF64 loader for read_elf/get_section/read_section_sv, symbol lookup for
// rvfi_tracer, and idle debug/JTAG ticks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#include "svdpi.h"
static unsigned char *img; static long imglen;
static int nsec, cur; static uint64_t sa[64], sl[64], so[64];
static Elf64_Sym *syms; static int nsym; static char *strtab;
void read_elf(const char *fn) {
  FILE *f = fopen(fn, "rb"); if (!f) { fprintf(stderr, "read_elf: cannot open %s\n", fn); exit(1); }
  fseek(f, 0, SEEK_END); imglen = ftell(f); fseek(f, 0, SEEK_SET);
  img = malloc(imglen); if (fread(img, 1, imglen, f) != (size_t)imglen) exit(1); fclose(f);
  Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
  Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
    sa[nsec] = ph[i].p_paddr; sl[nsec] = ph[i].p_filesz; so[nsec] = ph[i].p_offset; nsec++; }
  Elf64_Shdr *sh = (Elf64_Shdr *)(img + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_SYMTAB) {
    syms = (Elf64_Sym *)(img + sh[i].sh_offset); nsym = sh[i].sh_size / sizeof(Elf64_Sym);
    strtab = (char *)(img + sh[sh[i].sh_link].sh_offset); }
}
char get_section(long long *address, long long *len) {
  if (cur >= nsec) return 0;
  *address = sa[cur]; *len = sl[cur]; cur++; return 1;
}
void read_section_sv(long long address, const svOpenArrayHandle buffer) {
  for (int i = 0; i < nsec; i++) if (sa[i] == (uint64_t)address) {
    int n = svSize(buffer, 1);
    for (int j = 0; j < n && (uint64_t)j < sl[i]; j++) *(unsigned char *)svGetArrElemPtr1(buffer, j) = img[so[i] + j];
    return; }
}
char read_symbol(const char *name, unsigned long long *address) {
  for (int i = 0; i < nsym; i++) if (!strcmp(strtab + syms[i].st_name, name)) { *address = syms[i].st_value; return 0; }
  return 1;
}
int debug_tick(svBit *v, svBit rdy, int *a, int *o, int *d, svBit rv, svBit *rr, int rs, int rd) {
  *v = 0; *a = 0; *o = 0; *d = 0; *rr = 1; return 0; }
int jtag_tick(svBit *tck, svBit *tms, svBit *tdi, svBit *trstn, svBit tdo) {
  *tck = 0; *tms = 0; *tdi = 0; *trstn = 1; return 0; }
