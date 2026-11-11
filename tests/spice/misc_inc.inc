* included by misc.cir: a model and a subcircuit that uses a global node
.model dz d is=1e-14
.subckt pullup out r=10k
r1 vcc out {r}
.ends
