// 21.4.1, 21.5.3: $readmemh / $writememh with a dynamic array, a queue or
// an associative array. A dynamic array or queue keeps its size; loading an
// associative array creates an element at each address. $writememh writes
//! icarus-skip: Icarus 13 has no associative arrays with an int index
// @addresses only for an associative array.
module top;
  logic [7:0] d[];
  int q[$];
  bit [7:0] aa[int];
  int fd, i;
  string line;
  initial begin
    fd = $fopen("spec_21_4_1_in.hex", "w");
    $fdisplay(fd, "01\n02\n03\n@6\n0a\n0b");
    $fclose(fd);
    q = '{0, 0, 0, 0, 0, 0, 0, 0};
    $readmemh("spec_21_4_1_in.hex", q);
    $display("%p", q);
    aa[100] = 8'hff;
    $readmemh("spec_21_4_1_in.hex", aa);
    $display("%p", aa);
    d = new[4];
    $readmemh("spec_21_4_1_in.hex", d, 0, 2);
    $display("%p", d);
    $writememh("spec_21_4_1_d.hex", d, 0, 2);
    $writememh("spec_21_4_1_aa.hex", aa);
    fd = $fopen("spec_21_4_1_d.hex", "r");
    while ($fgets(line, fd)) $write("d: %s", line);
    $fclose(fd);
    fd = $fopen("spec_21_4_1_aa.hex", "r");
    while ($fgets(line, fd)) $write("aa: %s", line);
    $fclose(fd);
  end
endmodule
//> '{1, 2, 3, 0, 0, 0, 10, 11}
//> '{0:1, 1:2, 2:3, 6:10, 7:11, 100:255}
//> '{1, 2, 3, x}
//> d: 01
//> d: 02
//> d: 03
//> aa: @0
//> aa: 01
//> aa: 02
//> aa: 03
//> aa: @6
//> aa: 0a
//> aa: 0b
//> aa: @64
//> aa: ff
