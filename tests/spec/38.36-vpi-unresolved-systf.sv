// IEEE 1800-2023 38.36.3: cbUnresolvedSystf runs before elaboration for each
// system task or function name that is neither built in nor registered,
// once per name; user_data points to the name, which is also in
// cb_data->value (vpiStringVal). The
// callback may register the name with vpi_register_systf, and the call
// then runs as usual. Built-in names ($display) and names registered at
// startup ($known) do not run it.
//! icarus-skip: Icarus 13 does not run cbUnresolvedSystf
//! args: data/vpi/unresolved.c
module top;
  initial begin
    $known;
    $late(1);
    $late(2);
    $display("done");
  end
endmodule
//> unresolved: $late (user_data)
//> known
//> late
//> late
//> done
