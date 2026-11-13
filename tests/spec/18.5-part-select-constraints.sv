// IEEE 1800-2023 18.5 Constraint blocks: constraints on part-selects of a
// random variable (aligned addresses inside a window), with a dist on the
// variable an implication reads (18.5.3): every randomize() succeeds and the
// weights are followed.
//! icarus-skip: Icarus 13 has no constraints
class req;
  rand bit [1:0]  region;
  rand bit [31:0] addr;
  constraint c_region { region dist { 0 := 6, 1 := 2, 2 := 1, 3 := 1 }; }
  constraint c_addr   { addr[1:0] == 2'b00; addr[31:12] == 0;
                        region == 0 -> addr < 32'h3C0;
                        region == 1 -> addr inside {[32'h3C0:32'h3FC]};
                        region == 2 -> addr inside {[32'h400:32'h7FC]};
                        region == 3 -> addr >= 32'h800; }
endclass
module t;
  initial begin
    req r = new;
    int fail, bad, cnt[4];
    repeat (2000) begin
      if (!r.randomize()) fail++;
      else begin
        cnt[r.region]++;
        if (r.addr[1:0] != 0 || r.addr[31:12] != 0) bad++;
        if (r.region == 3 && r.addr < 32'h800) bad++;
        if (r.region == 0 && r.addr >= 32'h3C0) bad++;
      end
    end
    $display("fail=%0d bad=%0d", fail, bad);
    $display("weights %s", (cnt[0] > 1000 && cnt[1] > 300 && cnt[2] > 120 && cnt[3] > 120) ? "ok" : "off");
  end
endmodule
//> fail=0 bad=0
//> weights ok
