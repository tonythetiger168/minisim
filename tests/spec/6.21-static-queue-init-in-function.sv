// IEEE 1800-2023 6.21, 6.8: a static queue declared with an initializer in a
// function is initialized once and keeps its contents from call to call.
//! icarus-skip: Icarus does not support inside
module t;
  function automatic int next();
    static int q[$] = '{10, 20, 30};
    int v;
    v = q.pop_front();
    q.push_back(v + 1);
    return v;
  endfunction
  function bit known(string u);
    static string units[$] = '{"ps", "ns", "us"};
    return u inside {units};
  endfunction
  initial begin
    $display("%0d %0d %0d %0d %0d", next(), next(), next(), next(), next());
    $display("%0b %0b %0b", known("ns"), known("s"), known("us"));
  end
endmodule
//> 10 20 30 11 21
//> 1 0 1
