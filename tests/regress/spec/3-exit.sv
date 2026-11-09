//! exit: 1
// An elaboration error (undeclared name) must make msim exit with status 1.
module t; initial undeclared_name = 1; endmodule
