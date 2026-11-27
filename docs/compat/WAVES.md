# Vendor waveform and transaction tasks

> Vendor compatibility layer: built only with `-DMINISIM_VENDOR_COMPAT=ON`
> (default OFF). Without it these task names are unknown, as any other
> unknown system task, and the native `$dumpfile` / `$dumpvars`, `msim -vcd-sva`,
> `-vcd-power` and the recording API (`$begin_transaction` and the rest, see
> [DEBUG.md](../DEBUG.md)) do the same work.

A testbench written for Verdi, VCS or Xcelium dumps with their own tasks.
msim compiles them and writes FST: the vendors' formats (FSDB, VPD, SHM)
are their own and closed, so the file of the same name with `.fst` is
written instead, and msim says so once:

    msim: wave.fsdb: FSDB, VPD and SHM are the vendors' own formats; writing wave.fst (open it with mdebug, GTKWave or Surfer)

| Tasks | What msim does |
|---|---|
| `$fsdbDumpfile`, `$vcdplusfile`, `$shm_open`, `$fsdbAutoSwitchDumpfile`, `$recordfile` | name the file (`.fst`); without one, `novas.fst`, `vcdplus.fst` or `waves.fst` |
| `$fsdbDumpvars`, `$fsdbDumpvarsES`, `$fsdbDumpSingle`, `$vcdpluson`, `$shm_probe`, `$recordvars` | as `$dumpvars`: a depth, instances or signals (by name or as strings, `"tb.dut"`); `+fsdbfile+NAME` names the file, `+all` and `+mda` add memories, `+sva` and `+power` add assertions and power states (`-vcd-sva`, `-vcd-power` above), other `+` options are accepted |
| `$fsdbDumpMDA`, `$fsdbDumpMem`, `$vcdplusmemon` | memories too, as `-vcd-arrays` |
| `$fsdbDumpon` / `off`, `$vcdpluson` / `off`, `$recordon` / `off` | as `$dumpon` / `$dumpoff` |
| `$fsdbDumpflush`, `$vcdplusflush` | as `$dumpflush` |
| `$fsdbDumpFinish`, `$vcdplusclose`, `$shm_close`, `$recordclose` | flush and stop |
| `$fsdbDumpSVA` | assertions in the waveform, as `-vcd-sva` |
| `$fsdbSuppress`, `$fsdbLog`, `$vcdplusautoflushon`, ... | accepted, nothing to do |

`msim -i` and `-do` take Verdi's UCLI commands the same way
([UCLI.md](UCLI.md)). `-vcd` / `-fst` on the command line wins over all of
them, as over `$dumpvars`.
mdebug, mlint and the other tools compile the same sources.

A Verdi PLI library loaded with `-vpi` that registers these
names itself keeps them. A second `$fsdbDumpvars` with more scopes after the
dump has started adds none (an FST's signals are fixed when it starts);
msim says so once.

## Verdi FSDB transaction tasks

The `$fsdbTrans_*` tasks are recorded as the native recording API is
(`transactions.log`, `msim -tr FILE`, read with `mdebug -tr`):

```systemverilog
p = $fsdbTrans_begin("tb.mon.seen", "burst");     // a stream by its name
v = $fsdbTrans_begin("tb.mon.seen", "beat", p);   // a child of p
$fsdbTrans_add_attribute(v, 8'h5a, "byte");
$fsdbTrans_set_label(v, "first");
$fsdbTrans_end(v);
```

`$fsdbTrans_add_relation` records a link, `$fsdbTrans_add_tag` is accepted
and ignored.
