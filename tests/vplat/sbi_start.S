# sbi_start.S - entry of the S-mode payload: a stack per hart, .bss cleared,
# then smain(hartid, dtb). _secondary is where HSM starts the other harts
# (a0 = hart id, a1 = the opaque value).
    .section .text.init
    .globl _start
_start:
    la    sp, __stack_top
    slli  t0, a0, 12
    sub   sp, sp, t0
    la    t0, __bss_start
    la    t1, __bss_end
1:  bgeu  t0, t1, 2f
    sd    zero, 0(t0)
    addi  t0, t0, 8
    j     1b
2:  call  smain
3:  j     3b
    .globl _secondary
_secondary:
    la    sp, __stack_top
    slli  t0, a0, 12
    sub   sp, sp, t0
    call  secondary
4:  j     4b
