// IEEE 1800-2023 9.6.2: disable of a task ends every activation of it, and
// the tasks it called, and the caller goes on after the call; disable of a
// block inside a task moves every process running it past the block; what
// a disabled block forked ends with it; a task can disable itself.
module top;
  integer n;
  task automatic leaf(input integer id);
    #4 $display("%0t leaf %0d done", $time, id);
  endtask
  task automatic mid(input integer id);
    leaf(id);
    $display("%0t mid %0d after leaf", $time, id);
  endtask
  task automatic spin(input integer id);
    begin : body
      forever #3 n = n + id;
    end
    #id $display("%0t spin %0d left body", $time, id);
  endtask
  task selfie;
    #1 $display("%0t in selfie", $time);
    disable selfie;
    $display("never");
  endtask
  initial begin
    n = 0;
    fork mid(1); mid(2); join_none
    #2 disable mid;
    #1 $display("%0t mids gone", $time);
  end
  initial begin
    #10;
    fork spin(1); spin(2); join_none
    #7 disable spin.body;
    #3 $display("%0t n=%0d", $time, n);
  end
  initial begin
    #30;
    fork
      begin : a
        fork
          #5 $display("%0t grandchild", $time);
        join_none
        #10 $display("%0t a end", $time);
      end
      #2 disable a;
    join
    #10 $display("%0t after a", $time);
  end
  initial begin
    #50 selfie;
    $display("%0t back from selfie", $time);
  end
endmodule
//> 3 mids gone
//> 18 spin 1 left body
//> 19 spin 2 left body
//> 20 n=6
//> 42 after a
//> 51 in selfie
//> 51 back from selfie
