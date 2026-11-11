# usage: sta -no_init -exit -threads N osta.tcl  (env: LIB, NET, SDC, OUT)
set t0 [clock milliseconds]
read_liberty $env(LIB)
read_verilog $env(NET)
link_design big
set t1 [clock milliseconds]
read_sdc $env(SDC)
# force a full timing update for both setup and hold
set ws [sta::worst_slack_cmd max]
set wh [sta::worst_slack_cmd min]
set t2 [clock milliseconds]
puts "OSTA_TIMES read_link_ms [expr {$t1-$t0}] sdc_update_ms [expr {$t2-$t1}] total_ms [expr {$t2-$t0}]"
puts "OSTA_WORST max $ws min $wh"
if {[info exists env(OUT)]} {
  report_checks -path_delay max -group_path_count 1000000 -endpoint_path_count 1 -format end -digits 4 > $env(OUT).max
  report_checks -path_delay min -group_path_count 1000000 -endpoint_path_count 1 -format end -digits 4 > $env(OUT).min
}
