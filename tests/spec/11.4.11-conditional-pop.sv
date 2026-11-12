// IEEE 1800-2023 11.4.11: cond ? a : b evaluates the condition first and then
// only the chosen operand. A container method with a side effect in an arm
// (q.pop_front(), q.pop_back(): 7.10.2.4, 7.10.2.5) must not run before the
// condition, nor at all when its arm is not chosen.
module top;
  bit [7:0] q[$];
  bit [7:0] b;
  initial begin
    q.push_back(8'h5a);
    b = (q.size() > 0) ? q.pop_front() : 8'h00;
    $display("%h %0d", b, q.size());
    b = (q.size() > 0) ? q.pop_front() : 8'hee;
    $display("%h %0d", b, q.size());
    q.push_back(8'h11); q.push_back(8'h22);
    b = (q.size() > 5) ? q.pop_back() : 8'h33;
    $display("%h %0d", b, q.size());
    b = q.size() == 2 ? q.pop_back() : q.pop_front();
    $display("%h %0d", b, q.size());
  end
endmodule

//> 5a 0
//> ee 0
//> 33 2
//> 22 1
