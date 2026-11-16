// 12.5: the case expression is evaluated exactly once, and the case item
// expressions are evaluated in order only until one matches. Also for
// case inside (12.5.4) and a selector with a side effect (pop_front).
//! icarus-skip: Icarus 13 cannot parse case inside with value ranges
module top;
  int calls;
  int q[$];
  function automatic int count(int v);
    calls++;
    return v;
  endfunction
  function automatic int pick(int x);
    case (x)
      count(1): return 10;
      count(2), count(3): return 20;
      default: return 30;
    endcase
  endfunction
  initial begin
    int r, hit;
    for (int v = 1; v <= 4; v++) begin
      calls = 0;
      r = pick(v);
      $display("pick(%0d)=%0d after %0d label calls", v, r, calls);
    end
    calls = 0;
    case (count(2))
      1: hit = 1;
      2: hit = 2;
      default: hit = 3;
    endcase
    $display("selector: hit %0d, %0d call", hit, calls);
    calls = 0;
    case (count(6)) inside
      [0:5]: hit = 1;
      [6:6]: hit = 2;
      default: hit = 3;
    endcase
    $display("inside: hit %0d, %0d call", hit, calls);
    q = '{2, 5, 7};
    case (q.pop_front())
      1: hit = 1;
      2: hit = 2;
      default: hit = 3;
    endcase
    $display("pop_front: hit %0d, %0d left", hit, q.size());
    casez (q.pop_front())
      4'b0001: hit = 1;
      4'b01?1: hit = 2;
      default: hit = 3;
    endcase
    $display("casez pop_front: hit %0d, %0d left", hit, q.size());
  end
endmodule
//> pick(1)=10 after 1 label calls
//> pick(2)=20 after 2 label calls
//> pick(3)=20 after 3 label calls
//> pick(4)=30 after 3 label calls
//> selector: hit 2, 1 call
//> inside: hit 2, 1 call
//> pop_front: hit 2, 2 left
//> casez pop_front: hit 2, 1 left
