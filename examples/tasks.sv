// Module-level function and task. Compared with Icarus Verilog 13.
// FUNC/NAME/SUM/DISP: a function result is the value of the call.
// SEXT/ZEXT/SLT: signedness is the formal and return types.
// XZ/ADDX/PASSZ/SEXTZ: X and Z go through the call.
// TASK/INOUT/NAMED: output and inout are copied back after the call.
// NEST: a function may call another function.
// COUNT/EARLY: return leaves the task, not the caller's loop.
// STEP is static (initializer once). ASTEP is automatic (every call).
// STICKY keeps a static local across calls.
// D0/IN/MID/OUT/D5: #5 inside a task suspends the caller.
module tasks;
  logic [7:0] y, s, n, named, count, i;
  logic [3:0] d, pz;
  logic signed [7:0] ws;

  function logic [7:0] add(input logic [7:0] a, input logic [7:0] b);
    return a + b;
  endfunction

  function logic [7:0] addn(input logic [7:0] a, input logic [7:0] b);
    addn = a + b;
  endfunction

  function logic [7:0] inc(input logic [7:0] a);
    return a + 8'h01;
  endfunction

  function logic [7:0] twice(input logic [7:0] a);
    return inc(inc(a));
  endfunction

  function logic signed [7:0] sext(input logic signed [3:0] a);
    return a;
  endfunction

  function logic [7:0] zext(input logic [3:0] a);
    return a;
  endfunction

  function logic slt(input logic signed [3:0] a, input logic signed [3:0] b);
    return a < b;
  endfunction

  function logic [3:0] ident(input logic [3:0] a);
    return a;
  endfunction

  function logic [7:0] sticky(input logic [7:0] a);
    logic [7:0] t;
    if (a == 0) t = 8'h05;
    return t;
  endfunction

  function logic [7:0] step(input logic [7:0] a);
    static logic [7:0] t = 8'h01;
    t = t + a;
    return t;
  endfunction

  function automatic logic [7:0] astep(input logic [7:0] a);
    logic [7:0] t = 8'h01;
    t = t + a;
    return t;
  endfunction

  task addt(input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
    y = a + b;
  endtask

  task bump(inout logic [7:0] a);
    a = a + 8'h01;
  endtask

  task early(output logic [7:0] y);
    y = 8'h01;
    return;
    y = 8'hff;
  endtask

  task pass(input logic [3:0] a, output logic [3:0] b);
    b = a;
  endtask

  task later(output logic [3:0] y);
    $display("IN");
    y = 4'h1;
    #5;
    y = 4'ha;
    $display("OUT");
  endtask

  initial begin
    y = add(8'h0f, 8'h01);
    $display("FUNC %b", y);
    y = addn(8'h03, 8'h04);
    $display("NAME %b", y);
    s = add(8'h03, 8'h04) + add(8'h10, 8'h01);
    $display("SUM %b", s);
    $display("DISP %b", add(8'h0f, 8'h01));
    if (add(8'h01, 8'h01) == 8'h02) $display("IF 1");
    $display("NEST %b", twice(8'h05));
    ws = sext(4'sb1000);
    $display("SEXT %b", ws);
    y = zext(4'b1000);
    $display("ZEXT %b", y);
    $display("SLT %b", slt(4'sb1000, 4'sb0001));
    $display("SLT0 %b", slt(4'sb0111, 4'sb0001));
    $display("XZ %b", ident(4'b10xz));
    $display("ADDX %b", ident(4'b0001) + ident(4'bxxxx));
    $display("SEXTZ %b", sext(4'bz000));
    $display("STICKY %b", sticky(8'h00));
    $display("STICKY2 %b", sticky(8'h01));
    $display("STEP %b", step(8'h01));
    $display("STEP2 %b", step(8'h01));
    $display("ASTEP %b", astep(8'h01));
    $display("ASTEP2 %b", astep(8'h01));
    addt(8'h10, 8'h01, y);
    $display("TASK %b", y);
    y = 8'h04;
    bump(y);
    $display("INOUT %b", y);
    addt(.b(8'h01), .a(8'h02), .y(named));
    $display("NAMED %b", named);
    pass(4'bz10x, pz);
    $display("PASSZ %b", pz);
    count = 0;
    for (i = 0; i < 4; i = i + 1) begin
      early(n);
      count = count + 1;
    end
    $display("COUNT %b", count);
    $display("EARLY %b", n);
    d = 4'h0;
    $display("D0 %b", d);
    later(d);
    $display("D5 %b", d);
  end

  initial begin
    #1 $display("MID");
  end
endmodule
