OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80200000;
  .text : { *(.text.init) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : { *(.data .data.* .sdata .sdata.*) }
  .tohost : ALIGN(8) { *(.tohost) }
  .bss : { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(8); __bss_end = .; }
  . = ALIGN(16);
  . += 0x10000;
  __stack_top = .;
}
