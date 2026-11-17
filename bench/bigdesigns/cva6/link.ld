ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text.init : { *(.text.init) }
  .text : { *(.text*) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  .data : { *(.data*) *(.sdata*) *(.rodata*) *(.srodata*) }
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(16); . += 0x4000; _stack_top = .;
}
