// 16.12.17: a recursive property instanced in two places with different
// arguments. beats() checks the data beats of a write against a model value,
// one byte per beat; a retry restarts the beats from the first byte
// (beats(exp, 0)), any other beat goes on to the next byte (beats(exp, i+1)).
// Transaction 1 retries after two good beats and then sends all four: it
// passes. Transaction 2 sends a wrong second byte. Transaction 3 retries
// after one beat and then sends the second byte first: wrong, since the
// retry restarted the count.
//! icarus-skip: Icarus has no recursive properties
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic start = 0, beat = 0, retry = 0, last = 0;
  logic [7:0] data = 0;
  logic [31:0] model = 32'h11223344;

  property beats(local input logic [31:0] exp, local input logic [1:0] i);
    (beat || retry)[->1] |->
      ((beat |-> data == exp[31 - i * 8 -: 8]) and
       (if (retry) (1'b1 |=> beats(exp, 2'd0))
        else if (!last) (1'b1 |=> beats(exp, i + 2'd1))));
  endproperty
  property write_ok;
    logic [31:0] e;
    (start, e = model) |=> beats(e, 2'd0);
  endproperty
  a_write: assert property (@(posedge clk) write_ok) else $display("%0t fail", $time);

  // One row per clock edge: start, beat, retry, last, data.
  typedef struct { bit s, b, r, l; logic [7:0] d; } row_t;
  row_t rows[$] = '{
    '{1,0,0,0,8'h00},                                         // edge 1: transaction 1
    '{0,1,0,0,8'h11}, '{0,1,0,0,8'h22}, '{0,0,1,0,8'h00},     // two beats, retry
    '{0,1,0,0,8'h11}, '{0,0,0,0,8'h00}, '{0,1,0,0,8'h22},     // again, with a gap
    '{0,1,0,0,8'h33}, '{0,1,0,1,8'h44},                       // last beat: passes
    '{1,0,0,0,8'h00},                                         // edge 10: transaction 2
    '{0,1,0,0,8'h11}, '{0,1,0,0,8'h99},                       // edge 12: wrong byte
    '{0,0,0,0,8'h00},
    '{1,0,0,0,8'h00},                                         // edge 14: transaction 3
    '{0,1,0,0,8'h11}, '{0,0,1,0,8'h00},                       // one beat, retry
    '{0,1,0,0,8'h22},                                         // edge 17: should be 11 again
    '{0,0,0,0,8'h00}, '{0,0,0,0,8'h00}};
  initial begin
    foreach (rows[k]) begin
      @(negedge clk);
      {start, beat, retry, last, data} = {rows[k].s, rows[k].b, rows[k].r, rows[k].l, rows[k].d};
    end
    @(negedge clk);
    $display("done");
    $finish;
  end
endmodule
//> 125 fail
//> 175 fail
//> done
