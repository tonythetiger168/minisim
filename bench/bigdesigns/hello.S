# bare-metal: sum 1..100, then XiangShan good trap (nemu_trap = 0x6b, a0 = code)
  .section .text
  .globl _start
_start:
  li t0, 0
  li t1, 1
  li t2, 101
1: add t0, t0, t1
  addi t1, t1, 1
  bne t1, t2, 1b
  li t3, 5050
  sub a0, t0, t3
  .word 0x0000006b
2: j 2b
