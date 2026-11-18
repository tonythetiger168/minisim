// IEEE 1800-2023 21.6: the user string of $value$plusargs may be any string
// expression. A prefix put together at run time, {name, "=%s"}, matches the
// plusarg it spells.
//! args: +TRACE_port1=trace.log +LEVEL_port1=3
module top;
  parameter string PORT = "port1";
  string file_name, which;
  int level;
  initial begin
    which = {"TRACE_", PORT};
    file_name = "none";
    level = -1;
    if ($value$plusargs({which, "=%s"}, file_name)) $display("file %s", file_name);
    if ($value$plusargs({"LEVEL_", PORT, "=%d"}, level)) $display("level %0d", level);
    if (!$value$plusargs({"MISSING_", PORT, "=%d"}, level)) $display("missing, level still %0d", level);
  end
endmodule
//> file trace.log
//> level 3
//> missing, level still 3
