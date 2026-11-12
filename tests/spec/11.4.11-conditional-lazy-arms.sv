// IEEE 1800-2023 11.4.11: of the two arms of ?: only the one the condition
// picks is evaluated. A method call through a null handle in the arm not
// picked must not run, also when the ?: is a string, real or packed operand
// of a concatenation or another expression.
//! icarus-skip: Icarus 13 does not support a method call through a property (sp.get_name())
class P; function string get_name(); return "a_long_phase_name"; endfunction
  function real r(); return 2.5; endfunction
  function int n(); return 7; endfunction endclass
class S; P sp;
  function void go();
    real x;
    $display("%s", (sp != null ? sp.get_name() : "unknown"));
    $display("%s", {"Ending sequence library in phase ", (sp != null ? sp.get_name() : "unknown")});
    x = 1.0 + (sp != null ? sp.r() : 0.5);
    $display("%g %0d", x, {8'd1, (sp != null ? sp.n() : 3)});
    sp = new;
    $display("%s", {"Ending sequence library in phase ", (sp != null ? sp.get_name() : "unknown")});
    x = 1.0 + (sp != null ? sp.r() : 0.5);
    $display("%g %0d", x, {8'd1, (sp != null ? sp.n() : 3)});
  endfunction
endclass
module top; S s = new; initial s.go(); endmodule
//> unknown
//> Ending sequence library in phase unknown
//> 1.5 4294967299
//> Ending sequence library in phase a_long_phase_name
//> 3.5 4294967303
