// 3.12.1: the files of one msim command line are one compilation unit, so
// the macro and the $unit function here are seen by the files after it.
`define UNIT_W 6
function automatic int unit_twice(int x);
  return 2 * x;
endfunction
