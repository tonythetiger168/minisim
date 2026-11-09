// IEEE 1800-2023 6.16, 11.4.12.2, 11.4.13, 11.4.14, 6.20: indexing a string
// (read and write), foreach over a string, {n{s}} with a run-time n, string
// parameters, a concatenation of long literals given to a string argument,
// inside on strings, and {>>{q}} of a string queue.
//! icarus-skip: Icarus 13 does not support foreach over a string or string queues
module top;
  parameter string VERSION = "Accellera:1800.2:UVM";
  function automatic string scope_of(string full);
    int pos = full.len() - 1;
    byte c = full[pos];
    while (pos && c != "." && c != ":") c = full[--pos];
    return full.substr(0, pos - 1);
  endfunction
  function automatic string say(string s); return s; endfunction
  string s, dash, q[$];
  int n, stars;
  initial begin
    s = "a*b?c";
    foreach (s[i]) if (s[i] == "*" || s[i] == "?") stars++;
    $display("%0d %c %0d", stars, s[1], s.len());
    s[0] = "A";
    $display("%s", s);
    $display("%s", scope_of("top.u1.sub"));
    n = 5;
    dash = {n{"-"}};
    $display("[%s] %0d", dash, dash.len());
    $display("%s %0d", VERSION, VERSION.len());
    $display("%s", say({"Original and override type ", "arguments are identical"}));
    $display("%0d %0d", "RO" inside {"RW", "RO"}, s inside {"RO", "RC"});
    q = '{"ab", "cd", "e"};
    s = {>>{q}};
    $display("%s", s);
  end
endmodule

//> 2 * 5
//> A*b?c
//> top.u1
//> [-----] 5
//> Accellera:1800.2:UVM 20
//> Original and override type arguments are identical
//> 1 0
//> abcde
