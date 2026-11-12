# crt0.S - the start of every test program: a stack per hart, .bss cleared by
# hart 0, then main(hartid). The value main returns goes to the test finisher.
    .section .text.init
    .globl _start
_start:
    csrr  a0, mhartid
    la    sp, __stack_top
    slli  t0, a0, 12          # 4 KiB of stack per hart
    sub   sp, sp, t0
    bnez  a0, 2f
    la    t0, __bss_start
    la    t1, __bss_end
1:  bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:  call  main
    call  exit
3:  j     3b
