# mspice: SPICE circuit simulation

`mspice` is the analog simulator of the minisim tool line: it reads a SPICE
netlist and runs DC, AC and transient analyses, in the manner of HSPICE,
PrimeSim or ngspice.

```
mspice amp.cir                 run the analyses in the netlist
mspice amp.cir -o out          also write out.<analysis>.csv with every computed point
mspice amp.cir -vcd w.vcd      transient waveforms as a VCD of real values (GTKWave, Surfer)
mspice amp.cir -threads 4      worker threads; the default is every logical CPU
mspice amp.cir -D rload=2k     set a .param from the command line
mspice amp.cir -q              print measurements and errors only
mspice amp.cir -stats          Newton iterations, accepted and rejected time steps, elapsed time
```

The exit status is 0 when every analysis ran and every `.measure` produced a
value, 1 otherwise, so a netlist with measurements is a test.

## Netlist

The first line is the title. `*` starts a comment line; `;` and ` $` start a
comment inside a line; `+` continues the line before. Everything is case
insensitive. Numbers take the usual suffixes (`f p n u m k meg g t`, `mil`)
and trailing unit letters are ignored (`10pF`, `2.2kohm`).

### Elements

| Element | Form |
|---|---|
| Resistor | `Rname n1 n2 value` |
| Capacitor | `Cname n1 n2 value [ic=v]` |
| Inductor | `Lname n1 n2 value [ic=i]` |
| Coupling | `Kname L1 L2 k` |
| Voltage source | `Vname n+ n- [dc] value [ac mag [phase]] [waveform]` |
| Current source | `Iname n+ n- [dc] value [ac mag [phase]] [waveform]` |
| Voltage-controlled voltage source | `Ename n+ n- nc+ nc- gain` |
| Voltage-controlled current source | `Gname n+ n- nc+ nc- gm` |
| Current-controlled current source | `Fname n+ n- Vcontrol gain` |
| Current-controlled voltage source | `Hname n+ n- Vcontrol r` |
| Diode | `Dname anode cathode model [area] [off]` |
| MOSFET | `Mname d g s b model [w= l= ad= as= pd= ps= m=]` |
| Bipolar transistor | `Qname c b e [substrate] model [area] [off]` |
| Subcircuit instance | `Xname nodes... subckt [name=value ...]` |

Waveforms: `pulse(v1 v2 td tr tf pw per)`, `sin(vo va freq td theta phase)`,
`pwl(t1 v1 t2 v2 ...)`, `exp(v1 v2 td1 tau1 td2 tau2)`.

### Models

| `.model` type | Parameters |
|---|---|
| `d` | `is n rs cjo vj m tt bv ibv fc eg xti` |
| `nmos`, `pmos` (level 1, Shichman-Hodges, Meyer gate capacitances) | `vto kp gamma phi lambda rd rs cbd cbs is pb cgso cgdo cgbo cj mj cjsw mjsw tox ld uo fc` |
| `nmos`, `pmos` with `level=14` or `level=54`: BSIM4 4.8.3 | every BSIM4 model parameter (`toxe vth0 u0 vsat k1 k2 rdsw ... rgatemod rbodymod rdsmod trnqsmod acnqsmod igcmod igbmod`); on the element line `w l nf m ad as pd ps nrd nrs sa sb sd sca scb scc sc min rbsb rbdb rbpb rbps rbpd delvto mulu0 xgw ngcon dtemp trnqsmod acnqsmod rbodymod rgatemod geomod rgeomod off` |
| `npn`, `pnp` (Gummel-Poon) | `is bf br nf nr vaf var ikf ikr ise ne isc nc rb rc re cje vje mje cjc vjc mjc tf tr fc eg xti` |

A model parameter mspice does not use is reported as a warning rather than
silently dropped. A MOSFET model with a `level` other than 1, 14 or 54 is
an error.

### BSIM4

`level=14` or `level=54` selects BSIM4. The model equations are not
mspice's: they are the BSIM4 4.8.3 code of the UC Berkeley BSIM Research
Group, which developed the model, used unmodified under the Educational
Community License 2.0 (`third_party/bsim4`, `THIRD_PARTY_NOTICES.md`). So a
model card that works in another simulator with BSIM4 4.8 means the same
here: all model and instance parameters are passed to that code by name,
and a name it does not know is a warning (model) or an error (element).
Its parameter checker prints its usual warnings to stderr.

mspice's part is the adapter (`src/spice_bsim4.cpp`): internal nodes (drain
and source resistance, gate resistance, body network, the NQS charge node)
become unknowns, charges are integrated with mspice's trapezoidal and
backward-Euler rule and watched by its step control, and AC uses the
model's own small-signal stamps. What is not connected yet: noise, the
safe-operating-area check, `ic=` on the element line, binned models
(`lmin lmax wmin wmax` selection) and printing a transistor's operating
point. A build without the model (`-DMINISIM_WITH_BSIM4=OFF`) reports
level 14/54 as unsupported.

### Parameters, expressions, hierarchy

```
.param rload=1k cload={2*10p}
.subckt rc in out r=1k c=1n
r1 in out {r}
c1 out 0 {c}
.ends
x1 a b rc r={rload} c='cload/2'
```

Expressions go in `{...}` or `'...'`: `+ - * / % ** ^`, comparisons, `&& || !`,
`?:`, and `sqrt exp ln log log10 abs sin cos tan asin acos atan atan2 sinh
cosh tanh min max pow sgn floor ceil int if db`, with `pi`. A `.param` may
refer to one defined later. Subcircuits nest, may define their own `.param`,
`.model` and `.subckt`, and take parameters with defaults. Inside a
subcircuit, node `n` of instance `x1` is `x1.n` and element `r1` is `x1.r1`.
`.global` nodes and node `0` (or `gnd`) are shared. `.include "file"` reads
another file.

### Corners, `.alter` and Monte Carlo

```
.lib 'models.lib' tt        * read section tt of models.lib
...
.alter slow corner          * run everything again with:
.lib 'models.lib' ss
.alter                      * and once more, on top of that, with:
r1 vdd d 5k
```

In a library file a section runs from `.lib name` to `.endl`; a section may
read others with `.lib 'file' name`. Each `.alter` block is one more run of
the whole deck. A line in the block replaces the line of the same name (an
element, a `.model`, a `.param`, a `.subckt`, an analysis of the same kind, a
`.measure`); a `.lib` line replaces what the earlier `.lib` of the same file
brought in; anything else is added. Blocks build on each other, as in HSPICE.
Output files of the k-th block get `.altk` before the extension.

`.step monte N` (or `sweep monte=N` at the end of an analysis line) runs N
Monte Carlo samples. In a sample these functions draw; otherwise they give
the nominal value:

| Function | Value |
|---|---|
| `gauss(nom, rel, sigma)` | normal, standard deviation `nom*rel/sigma` |
| `agauss(nom, abs, sigma)` | normal, standard deviation `abs/sigma` |
| `unif(nom, rel)` | uniform in `nom*(1 ± rel)` |
| `aunif(nom, abs)` | uniform in `nom ± abs` |

A top-level `.param` is drawn once per sample and so is the same everywhere
it is used (global variation). A `.param` inside a subcircuit is drawn once
per instance (local variation, mismatch). The draws depend only on
`.option seed=` (default 1) and the sample number, so the results are the same
at any thread count and sample k can be rerun alone. After the samples mspice
prints the mean, standard deviation, minimum and maximum of each `.measure`.

### Analyses

| Line | Analysis |
|---|---|
| `.op` | DC operating point |
| `.dc src start stop step [src2 start2 stop2 step2]` | DC sweep of one or two sources |
| `.ac dec\|oct\|lin n fstart fstop` | small-signal frequency response |
| `.tran tstep tstop [tstart [tmax]] [uic]` | transient |
| `.noise v(out[,ref]) src dec\|oct\|lin n fstart fstop` | noise at an output, and referred to the input source |
| `.tf v(out)\|i(vsrc) src` | DC small-signal gain, input and output resistance |
| `.sens v(out)\|i(vsrc) ...` | DC sensitivity of each output to every resistor and source |
| `.four freq out1 out2 ...` | harmonics and distortion over the last period of the `.tran` |

`.ic v(node)=value` holds nodes during the operating point (or, with `uic`,
is the starting state); `.nodeset v(node)=value` is a first guess.
`.temp` and `.option temp=` set the temperature.

`.step param name start stop step`, `.step param name list v1 v2 ...` and
`.step dec|oct param name start stop points` repeat every analysis for each
value. Several `.step` lines nest; the last one varies fastest.

`.noise` adds up the noise of every device at the output: thermal noise of
resistors (including the series resistances of the models), shot noise of
diode and bipolar currents, channel thermal noise `8kT gm/3` of the level-1
MOSFET, and flicker noise where the model gives `kf` (`af`, default 1): `kf
i^af / f` for diodes and base currents, `kf id^af / (f W Leff Cox^2)` for the
MOSFET, the formulas of ngspice. The columns are `onoise` and `inoise`, in
V/sqrt(Hz) (`inoise` in the unit of the input source). `.print noise onoise
inoise` prints them, `.measure noise` reads them, and the totals over the
band come out as `onoise_total` and `inoise_total` with the ten largest
contributors. One adjoint (transposed) solve per frequency gives the transfer
from every noise source at once, so the cost is that of an `.ac` sweep
however many devices there are; the frequencies are spread over the threads.

`.tf` prints `transfer_function`, `input_resistance` and `output_resistance`;
`.sens` prints one line per resistor and independent source: the change of
the output per ohm, volt or ampere. Both come from the linearized circuit at
the operating point; `.sens` takes a single adjoint solve.

`.four` reads each output on 4,096 evenly spaced points of the last period
and prints the DC value, nine harmonics as `magnitude * sin(k w t + phase)`
and the total harmonic distortion in percent, which is also a measurement
named `thd_<node>` (`thd_i_<source>` for a current).

### Outputs

`.print op|dc|ac|tran outputs...` prints a table. An output is `v(node)`,
`v(a,b)` or `i(element)` for a voltage source, inductor, E or H source. In
AC, add `m` (magnitude), `db`, `p` (phase in degrees), `r` or `i`:
`vdb(out)`, `vp(out)`, `im(v1)`. A transient table is printed on the `tstep`
grid by interpolation; `-o` writes the solver's own points. With no `.print`
for an analysis, `-o` holds every node voltage and source current.

`.measure` (or `.meas`):

```
.meas tran tpd   trig v(in) val=1.65 rise=1 targ v(out) val=1.65 fall=1
.meas tran t90   when v(out)=2.97 rise=1
.meas tran cross when v(a)=v(b)
.meas tran vend  find v(out) at=10n
.meas tran vx    find v(out) when v(in)=1.65 fall=last
.meas tran vmax  max v(out) from=1n to=5n       ; also min pp avg rms integ
.meas ac   f3db  when vdb(out)=-3
```

`rise=`, `fall=` and `cross=` take a count or `last`; `td=` skips the start.

### Options

`.option reltol= abstol= vntol= gmin= trtol= chgtol= itl1= itl4= temp= tnom=
gshunt=`, `method=trap|euler`, `bypass=0|1` (reuse the linearization of
devices whose voltages have not moved; on by default) and `parmin=` (the
number of instances of a device type from which its models are evaluated in
parallel; default 1024). `chgtol` (default 1e-14 C) is the smallest charge the
time step control resolves: with femtofarad nodes, as in standard cells, the
charges are below the default and the steps are then set by `tstep` alone, so
give `chgtol=1e-17` or so there.

## How it works

- **Equations**: modified nodal analysis, one unknown per node voltage and
  per branch current (voltage sources, inductors, E and H sources).
- **Linear solver** (`src/spice_lu.hpp`): sparse LU. The first factorization
  pivots (threshold pivoting on a minimum-degree order) and records the
  structure; later ones reuse it and only redo the numbers, falling back to
  a fresh pivot search when a pivot becomes too small. A linear circuit is
  factored once per step size, and then only its right-hand side is rebuilt.
- **Operating point**: Newton iteration with SPICE's junction and FET
  voltage limiting and a device-current convergence test. If a plain start
  fails, a shunt conductance on every node is stepped down adaptively, and
  then the sources are ramped up.
- **Transient**: trapezoidal integration, backward Euler for the first step
  after a waveform corner, the step chosen from the local truncation error
  of every charge and flux (SPICE's `trtol` test), and breakpoints at every
  corner of a source waveform.
- **Devices** (`src/spice_dev.hpp`) are stored by type, so the per-iteration
  loops walk contiguous memory. A device whose terminal voltages have not
  moved keeps its last linearization (bypass).
- **Pivot order**: unknowns fixed by a row with one entry (a supply node,
  through its voltage source) are eliminated first, then minimum degree.
  Without that the supply column gathers the product of the gains of a long
  chain of stages before it is reached.

## Threads

mspice uses every logical CPU by default; `-threads N` sets the count and
`-threads 1` turns threading off. Three things run in parallel:

- the runs of a `.step` (each builds its own circuit),
- the frequency points of an `.ac` (one pivot order, found at the first
  frequency, is shared by every worker),
- model evaluation, for a device type with at least `parmin` instances; for
  MOSFETs the matrix is assembled in parallel as well. The devices are cut
  into chunks; a matrix entry that only one chunk touches is summed by that
  chunk's thread, and the entries chunks share (a supply node, say) are
  summed afterwards on one thread, in device order.

Results are stored by index and matrices are summed in a fixed order, so
the output is identical, bit for bit, at any thread count. The test suite
checks this on every netlist.

## Tests and accuracy

`tests/spice/*.cir` each carry their expected `.measure` values, worked out
by hand, in `*>` comment lines. `tests/spice/check.py` checks them, checks
that the output is the same at 1, 3 and all threads, and, when ngspice is
installed (it is in CI), compares every measurement with ngspice: within 1%
by default, 2% for the MOSFET and BJT switching circuits. With tolerances
tightened in both simulators, an inverter's delays agree to five digits.
The `.noise`, `.tf`, `.sens` and `.four` tests carry numbers worked out by
hand and numbers from ngspice 42, whose noise analysis runs from a control
block that check.py does not write: each device's share of the output noise,
the transfer function, every sensitivity and the distortion agree to five or
six digits.

## Speed

`bench/spice/run.sh <mspice>` writes seven netlists and times mspice against
ngspice. On a 4-CPU cloud machine, ngspice 42, best of three, seconds:

| Netlist | mspice, 1 thread | mspice, 4 threads | ngspice |
|---|---|---|---|
| 20,000-section RC ladder, transient | 0.86 | 0.87 | 1.36 |
| 60 x 60 RC mesh, 401-point AC | 1.86 | 0.73 | 6.41 |
| the same mesh, 401-point noise analysis (7,140 noise sources) | 2.04 | 0.76 | 6.93 |
| 2,000 inverters in a chain (4,000 level-1 MOSFETs), transient | 1.03 | 0.60 | 1.75 |
| 101-stage ring oscillator, level 1, transient | 0.35 | 0.35 | 0.52 |
| 3 inverters with RC load, 64-value `.step`, transient | 0.58 | 0.17 | no `.step` |
| 2,000 inverters in a chain (4,000 BSIM4 transistors), transient | 3.18 | 2.08 | 5.82 |
| 101-stage ring oscillator, BSIM4, transient | 0.78 | 0.67 | 1.72 |

Both simulators give the same measurements on these. The ladder is linear
(one factorization, then solves), and the ring is under `parmin`, so
neither uses threads. On the chain, MOSFET evaluation and assembly take
0.81 s of the run on one thread and 0.36 s on four; what is left is the LU,
the convergence and step-size checks and the output, which are serial.

The two BSIM4 rows run the same model code in both simulators (delays agree
to 0.1%). mspice is ahead on one thread because a transistor whose voltages
have not moved is not evaluated again, including on the first iteration of
a time step. With threads the evaluations run in parallel; adding the
results to the matrix is serial and in netlist order (so any thread count
gives the same bits), and that, with the LU, is what limits the gain.

## Flat netlists for other tools

`src/spice_netlist.hpp` is the one SPICE reader of the tool line. Besides
the cards, it offers `spice::flatten(netlist, top)`: the elements of the top
level (or of the subcircuit `top`, with its ports) as primitives with flat
instance paths (`x1.x2.m1`) and flat net names, parameters evaluated to SI
numbers, `.global` nets and ground merged across the hierarchy, and
instances of subcircuits the netlist does not define (PDK device cells)
kept as they are. The comment above `flatten` states the rules.
`mspice -flatten [-top name] file.cir` prints the result, one element per
line: path, nodes, model, parameters.

## Not there yet

- BSIM3 and other MOSFET levels (2, 3, BSIM-CMG), the JFET; for BSIM4:
  noise, binned models, `ic=`; behavioral (`B`) sources, switches,
  transmission lines.
- Noise of BSIM4 transistors; `.sens` for model parameters and in AC;
  `.pz`; `.data` sweeps, `lot`/`dev` tolerances
  on `.model` lines, `.variation` blocks; `.dc` sweeps
  of a parameter or temperature; `i()` of a resistor or a device terminal.
- Temperature: the thermal voltage, the diode and BJT saturation current and
  the MOSFET mobility follow `temp`; other temperature coefficients do not.
- Gear integration; ngspice raw files (CSV and VCD are written).
- Mixed-signal co-simulation with msim (real-number models, Verilog-AMS).
