# msim -i UCLI commands (tests/msim_ucli.cmake)
stop -line 23 -file ucli.sv
run
where
next
step
step
print tb.sum
run 3
describe u1 q u1.q lane[1].r nope
scope u1
show -ports -value
describe W
get q -radix dec
scope -up
scope
stop -condition {q == 3 && en}
stop -absolute 47
stop
run
get q -radix bin
run
stop -delete all
set n 0
foreach s {clk en} { if {[get $s -radix dec] == 1} { incr n } }
puts "high: $n"
proc twice {} { run 5; run 5 }
twice
alias rr run 10
rr
senv time
search *lane*
stop -posedge clk -continue -command {puts "edge q=[get q -radix dec]"}
run 20
stop -delete all
listing 1
stop u1.q == 4'hc -once
run
stop
show -scopes
dump -file ucli.vcd
dump -add u1 -depth 1
run 10 ns
bogus
run -posedge clk
run -slot 3
time
