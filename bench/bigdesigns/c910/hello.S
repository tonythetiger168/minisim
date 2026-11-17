# OpenC910 smart_run bare-metal check: sum 1..100; on success write back 0x444333222,
# the value smart_run's tb.v watches on the integer/load write-back buses as "TEST PASS".
  .text
  .globl __start
__start:
  li t0, 0
  li t1, 1
  li t2, 101
1: add t0, t0, t1
  addi t1, t1, 1
  bne t1, t2, 1b
  li t3, 5050
  bne t0, t3, 2f
  li a0, 0x444333222
2: j 2b
