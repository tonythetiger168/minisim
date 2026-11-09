// IEEE 1800-2023 21.4: the file name of $readmemh can be any string
// expression: a string parameter, an untyped parameter holding a string
// literal, or a string variable built at run time.
module top #(parameter string INIT_FILE = "data/mem.hex", parameter INIT2 = "data/mem.hex");
  logic [7:0] m [0:7], n [0:7], k [0:7];
  string f;
  initial begin
    $readmemh(INIT_FILE, m);
    $readmemh(INIT2, n);
    f = {"data/", "mem.hex"};
    $readmemh(f, k);
    $display("%h %h %h", m[0], n[1], k[3]);
  end
endmodule
//> 0a 0b ff
