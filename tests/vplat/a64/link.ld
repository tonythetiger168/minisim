/* link.ld - mvplat's built-in AArch64 board: RAM at 0x40000000. */
ENTRY(_start)
SECTIONS {
  . = 0x40000000;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(16);
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  __bss_end = .;
  . = ALIGN(4096);
  __pt = .;           /* page tables for the MMU test */
  . += 0x10000;
  . = ALIGN(16);
  . += 0x40000;       /* stacks: 16 KiB per processor */
  __stack_top = .;
}
