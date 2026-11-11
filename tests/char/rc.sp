* An inverter with a known answer: an ideal inverting source behind a
* resistor. Into a load C the output is an RC exponential, so for a fast
* input edge the 50% delay is R C ln 2 and the 20-80% transition R C ln 4.
.subckt rcinv a y vdd vss
e1 n vss vdd a 1
r1 n y 10k
.ends
