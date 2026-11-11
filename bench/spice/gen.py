#!/usr/bin/env python3
"""Write the mspice benchmark netlists.

    gen.py <dir>

    ladder.cir   20,000-section RC ladder, transient (linear: one factorization)
    mesh.cir     60 x 60 resistor mesh with a capacitor on every node, 400-point AC
    chain.cir    2,000 CMOS inverters in a chain (4,000 level-1 MOSFETs), transient
    ring.cir     101-stage ring oscillator, transient
    mc.cir       an RC-loaded inverter repeated over a 64-value .step

Each ends with .measure lines, so the same file runs on mspice and (except
mc.cir, which uses .step) on ngspice, and the answers can be compared.
"""
import sys

MODELS = """.model nch nmos vto=0.7 kp=100u gamma=0.4 phi=0.65 lambda=0.02 tox=20n cgso=0.3n cgdo=0.3n cj=0.4m cjsw=0.3n pb=0.8
.model pch pmos vto=-0.8 kp=40u gamma=0.5 phi=0.65 lambda=0.03 tox=20n cgso=0.3n cgdo=0.3n cj=0.5m cjsw=0.3n pb=0.8
.subckt inv in out vdd
mp out in vdd vdd pch w=20u l=1u ad=40p as=40p pd=24u ps=24u
mn out in 0 0 nch w=10u l=1u ad=20p as=20p pd=14u ps=14u
.ends
"""


def ladder(n=20000):
    o = ["RC ladder, %d sections" % n, "v1 n0 0 pulse(0 1 0 1n 1n 1 2)"]
    for i in range(n):
        o.append(f"r{i} n{i} n{i+1} 10")
        o.append(f"c{i} n{i+1} 0 1p")
    o += [".tran 1n 200n", ".meas tran t50 when v(n50)=0.5 rise=1", f".meas tran vmid find v(n{n//100}) at=200n", ".end"]
    return o


def mesh(n=60):
    o = ["RC mesh %d x %d" % (n, n), "v1 in 0 dc 0 ac 1", "rin in n0_0 50"]
    for i in range(n):
        for j in range(n):
            if j + 1 < n:
                o.append(f"rh{i}_{j} n{i}_{j} n{i}_{j+1} 100")
            if i + 1 < n:
                o.append(f"rv{i}_{j} n{i}_{j} n{i+1}_{j} 100")
            o.append(f"c{i}_{j} n{i}_{j} 0 10f")
    o += [f"rl n{n-1}_{n-1} 0 1k", ".ac dec 100 1meg 10g",
          f".meas ac f3 when vdb(n{n-1}_{n-1})=-6 fall=1", f".meas ac lf find vm(n{n-1}_{n-1}) at=1meg", ".end"]
    return o


def chain(n=2000):
    o = ["Inverter chain, %d stages" % n, MODELS.rstrip(), "vdd vdd 0 3.3", "vin n0 0 pulse(0 3.3 1n 0.2n 0.2n 20n 40n)"]
    for i in range(n):
        o.append(f"x{i} n{i} n{i+1} vdd inv")
    o += [".tran 20p 12n", ".meas tran d10 trig v(n0) val=1.65 rise=1 targ v(n10) val=1.65 rise=1",
          ".meas tran d50 trig v(n0) val=1.65 rise=1 targ v(n50) val=1.65 rise=1", ".end"]
    return o


BSIM4 = """.model nch nmos level=54 version=4.8.3 toxe=1.8n vth0=0.4 u0=0.04 vsat=1.2e5 rdsw=150
+ k1=0.5 k2=-0.05 eta0=0.02 pclm=1.2 cgso=0.2n cgdo=0.2n cjs=1m cjd=1m cjsws=0.1n cjswd=0.1n
.model pch pmos level=54 version=4.8.3 toxe=1.9n vth0=-0.42 u0=0.012 vsat=1.0e5 rdsw=250
+ k1=0.5 k2=-0.03 eta0=0.02 pclm=1.2 cgso=0.2n cgdo=0.2n cjs=1m cjd=1m cjsws=0.1n cjswd=0.1n
.subckt inv a y vdd
mp y a vdd vdd pch w=400n l=50n ad=40f as=40f pd=1u ps=1u
mn y a 0 0 nch w=200n l=50n ad=20f as=20f pd=0.6u ps=0.6u
.ends
"""


def chain4(n=2000):
    o = ["BSIM4 inverter chain, %d stages" % n, BSIM4.rstrip(), "vdd vdd 0 1", "vin n0 0 pulse(0 1 100p 20p 20p 2n 4n)"]
    for i in range(n):
        o.append(f"x{i} n{i} n{i+1} vdd inv")
    o += [".tran 2p 1.6n", ".meas tran d10 trig v(n0) val=0.5 rise=1 targ v(n10) val=0.5 rise=1",
          ".meas tran d50 trig v(n0) val=0.5 rise=1 targ v(n50) val=0.5 rise=1", ".end"]
    return o


def ring4(n=101):
    o = ["BSIM4 ring oscillator, %d stages" % n, BSIM4.rstrip(), "vdd vdd 0 1"]
    for i in range(n):
        o.append(f"x{i} n{i} n{(i+1) % n} vdd inv")
    o += [".ic v(n0)=0", ".tran 5p 20n", ".meas tran period trig v(n0) val=0.5 rise=2 targ v(n0) val=0.5 rise=3", ".end"]
    return o


def ring(n=101):
    o = ["Ring oscillator, %d stages" % n, MODELS.rstrip(), "vdd vdd 0 3.3"]
    for i in range(n):
        o.append(f"x{i} n{i} n{(i+1) % n} vdd inv")
    o += [".ic v(n0)=0", ".tran 50p 100n", ".meas tran period trig v(n0) val=1.65 rise=2 targ v(n0) val=1.65 rise=3", ".end"]
    return o


def mc():
    o = ["Inverter delay over load capacitance", MODELS.rstrip(), "vdd vdd 0 3.3", ".param cl=10f",
         "vin in 0 pulse(0 3.3 1n 0.2n 0.2n 20n 40n)", "x1 in a vdd inv", "x2 a b vdd inv", "x3 b c vdd inv", "cl c 0 {cl}",
         "rl c d 1k", "cd d 0 {cl}",
         ".step param cl 10f 640f 10f", ".tran 5p 30n",
         ".meas tran tpd trig v(in) val=1.65 rise=1 targ v(d) val=1.65 fall=1", ".end"]
    return o


if __name__ == "__main__":
    d = sys.argv[1]
    for name, fn in (("ladder", ladder), ("mesh", mesh), ("chain", chain), ("ring", ring), ("mc", mc),
                     ("chain4", chain4), ("ring4", ring4)):
        open(f"{d}/{name}.cir", "w").write("\n".join(fn()) + "\n")
