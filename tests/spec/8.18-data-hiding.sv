// IEEE 1800-2023 8.18: local members are visible only inside the class;
// protected members also inside subclasses. Methods of the class can use
// them, including through another object of the same class.
//! icarus-skip: Icarus 13 does not find inherited methods called without this.
module top;
  class Account;
    local int balance;
    protected int fee = 2;
    function new(int b); balance = b; endfunction
    function int get(); return balance; endfunction
    function bit richer(Account o); return balance > o.balance; endfunction
  endclass
  class Savings extends Account;
    function new(int b); super.new(b); fee = 0; endfunction
    function int net(); return get() - fee; endfunction
  endclass
  Account a;
  Savings s;
  initial begin
    a = new(10);
    s = new(5);
    $display("%0d %0d %0d %0d", a.get(), s.net(), a.richer(s), s.richer(a));
  end
endmodule

//> 10 5 1 0
