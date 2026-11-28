/* link-a64.ld - the AArch64 benchmarks' layout: code at 0x40000000 (the
   RAM of mvplat's arm board and of QEMU's virt machine), data from the next
   4 KiB page, the stacks crt0.S (tests/vplat/a64) expects. */
ENTRY(_start)
SECTIONS {
  . = 0x40000000;
  .text : { *(.text.start) *(.text .text.*) }
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  . = ALIGN(4096);
  __bss_start = .;
  .bss : { *(.bss .bss.*) *(COMMON) }
  . = ALIGN(16);
  __bss_end = .;
  . = ALIGN(4096);
  . += 0x40000;
  __stack_top = .;
}
