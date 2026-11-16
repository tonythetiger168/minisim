// IEEE 1800-2023 18.5.3, 18.5.6, 18.5.7, 18.5.13.2: a dist inside an if or on
// the right of -> weights the variable whenever the condition holds (also
// when the condition is a random variable fixed by a with clause), and the
// else branch's dist applies when it does not. `c -> disable soft x` only
// drops the soft constraint when c is true.
//! icarus-skip: Icarus does not support constraints
class C;
  rand bit [3:0] a[2];
  rand bit [3:0] y;
  rand bit sel;
  bit gate;
  constraint c1 {
    if (gate) { foreach (a[i]) a[i] dist { 4'd0 := 3, [4'd1 : 4'd4] := 1 }; }
    else { foreach (a[i]) a[i] dist { 4'd8 := 3, [4'd9 : 4'd12] := 1 }; }
  }
  constraint c2 { sel -> y dist { 4'd15 := 9, [4'd0 : 4'd8] := 1 }; }
endclass
class D;
  bit flag;
  rand bit [3:0] x;
  constraint s { soft x == 4'h5; }
  constraint o { flag -> disable soft x; flag -> x == 4'hc; }
endclass
module t;
  C c = new;
  D d = new;
  int zero, eight, fifteen, five, twelve;
  initial begin
    c.gate = 1;
    repeat (1000) begin void'(c.randomize() with { sel == 1; }); zero += c.a[0] == 0; fifteen += c.y == 15; end
    c.gate = 0;
    repeat (1000) begin void'(c.randomize()); eight += c.a[1] == 8; end
    // expected 3/7 = 429, 9/18 = 500 and 429; uniform gives 200, 111 and 200
    $display("if %s, else %s, -> %s", zero > 360 && zero < 500 ? "weighted" : "WRONG",
             eight > 360 && eight < 500 ? "weighted" : "WRONG", fifteen > 430 && fifteen < 570 ? "weighted" : "WRONG");
    d.flag = 0; repeat (10) begin void'(d.randomize()); five += d.x == 5; end
    d.flag = 1; repeat (10) begin void'(d.randomize()); twelve += d.x == 12; end
    $display("soft kept %0d/10, overridden %0d/10", five, twelve);
  end
endmodule
//> if weighted, else weighted, -> weighted
//> soft kept 10/10, overridden 10/10
