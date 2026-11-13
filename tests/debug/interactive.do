time
signals cnt
print tb.cnt tb.head tb.none tb.b
stop tb.cnt == 3
stop tb.clk posedge
stop
run
print tb.head tb.head.next tb.head.next.id tb.head.tag tb.b.flag tb.head.zz
delete 2
run
print cnt
run -slot 2
time
force tb.en 0
run 30
print tb.cnt tb.en
release tb.en
deposit tb.cnt 8'h40
run 20
print tb.cnt
bogus
print tb.nope
run
