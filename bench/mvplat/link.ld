/* link.ld - the benchmarks' layout: code at 0x80000000, data from the next
   4 KiB page (a simulator that translates code need not watch the code
   pages for stores), tohost, a 64 KiB stack. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text.init) *(.text .text.*) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : { *(.data .data.* .sdata .sdata.*) }
  .tohost : ALIGN(4096) { *(.tohost) }
  . = ALIGN(4096);
  .bss : { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); __bss_end = .; }
  . = ALIGN(16);
  . += 0x10000;
  __stack_top = .;
}
