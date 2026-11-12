// IEEE 1800-2023 20.15: $q_initialize, $q_add, $q_remove, $q_full and
// $q_exam manage FIFO (type 1) and LIFO (type 2) queues, with the status
// codes of Table 20-11 and the statistics of Table 20-10.
//! icarus-skip: the statistics of Icarus 13 differ in rounding
module top;
  integer st, job, inf, v, full;
  initial begin
    $q_initialize(1, 1, 3, st); $display("init %0d", st);
    $q_initialize(1, 1, 3, st); $display("dup %0d", st);
    $q_initialize(2, 3, 3, st); $display("type %0d", st);
    $q_initialize(2, 2, 0, st); $display("len %0d", st);
    $q_initialize(2, 2, 2, st); $display("lifo %0d", st);
    $q_add(9, 1, 1, st); $display("undef %0d", st);
    $q_remove(1, job, inf, st); $display("empty %0d", st);
    $q_add(1, 10, 100, st);
    #2 $q_add(1, 11, 101, st);
    #2 $q_add(1, 12, 102, st);
    full = $q_full(1, st); $display("full %0d %0d", full, st);
    $q_add(1, 13, 103, st); $display("over %0d", st);
    $q_exam(1, 1, v, st); $display("length %0d", v);
    $q_exam(1, 2, v, st); $display("interarrival %0d", v);
    $q_exam(1, 3, v, st); $display("max %0d", v);
    #6 $q_exam(1, 5, v, st); $display("longest %0d", v);
    $q_remove(1, job, inf, st); $display("fifo %0d %0d %0d", job, inf, st);
    #2 $q_remove(1, job, inf, st); $display("fifo %0d %0d %0d", job, inf, st);
    $q_exam(1, 4, v, st); $display("shortest %0d", v);
    $q_exam(1, 6, v, st); $display("average %0d", v);
    full = $q_full(1, st); $display("full %0d", full);
    $q_add(2, 1, 5, st); $q_add(2, 2, 6, st);
    $q_remove(2, job, inf, st); $display("lifo %0d %0d", job, inf);
  end
endmodule
//> init 0
//> dup 6
//> type 4
//> len 5
//> lifo 0
//> undef 2
//> empty 3
//> full 1 0
//> over 1
//> length 3
//> interarrival 2
//> max 3
//> longest 10
//> fifo 10 100 0
//> fifo 11 101 0
//> shortest 10
//> average 10
//> full 0
//> lifo 2 6
