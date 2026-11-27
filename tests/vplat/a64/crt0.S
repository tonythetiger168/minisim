// crt0.S - start-up for the AArch64 test programs: a stack per processor
// (by MPIDR_EL1.Aff0), FP/SIMD on, the vector table, BSS cleared by
// processor 0, then main(cpu). Processors that PSCI starts enter at
// _secondary with x0 = their number and call secondary(cpu).
    .section .text.start, "ax"
    .global _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    bl      setup
    cbnz    x1, 3f
    ldr     x5, =__bss_start
    ldr     x6, =__bss_end
1:  cmp     x5, x6
    b.hs    2f
    str     xzr, [x5], #8
    b       1b
2:  mov     x0, #0
    bl      main
    bl      exit
3:  wfi
    b       3b

    .global _secondary
_secondary:
    mov     x1, x0
    bl      setup
    mov     x0, x1
    bl      secondary
4:  wfi
    b       4b

// x1 = processor number; keeps x1
setup:
    ldr     x2, =__stack_top
    lsl     x3, x1, #14
    sub     x2, x2, x3
    mov     sp, x2
    mov     x4, #(3 << 20)
    msr     cpacr_el1, x4
    adr     x4, vectors
    msr     vbar_el1, x4
    isb
    ret

// The vector table: each entry saves the registers into a frame
// (x0-x30, ELR, SPSR) and calls trap(kind, frame); kind is the entry's
// number (0-15). trap may change the frame; ERET returns with it.
    .balign 2048
vectors:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .balign 128
    sub     sp, sp, #272
    stp     x0, x1, [sp, #0]
    mov     x0, #\n
    b       save
    .endr
save:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x30, x2, [sp, #240]
    str     x3, [sp, #256]
    mov     x1, sp
    bl      trap
    ldp     x30, x2, [sp, #240]
    ldr     x3, [sp, #256]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #272
    eret
