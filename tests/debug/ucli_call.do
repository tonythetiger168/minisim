# msim -i call, drivers / loads, save / restore / restart (tests/msim_ucli.cmake)
drivers acc
loads clk
run 12
call twice 21
call addup(3, 4)
call bump 8'h10
print acc calls
call nope
call twice
save ck2
run 30
print acc
restore ck2
print acc calls
restart
print acc
run 20
print acc
