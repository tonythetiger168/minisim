# Other simulators' interactive commands

> Vendor compatibility layer: built only with `-DMINISIM_VENDOR_COMPAT=ON`
> (default OFF). The native commands of `msim -i` and `msim -do` are in
> [DEBUG.md](../DEBUG.md#interactive-simulation-msim--i).

In a vendor-layer build `msim -i` and `-do` also take the names that the
xrun Tcl shell, VCS UCLI and Verdi use for the same commands:

| Command | Native command |
|---|---|
| `value NAME ...` | `print NAME ...` |
| `deposit SIG VALUE`, `deposit SIG = VALUE` | `change SIG VALUE` |
| `probe -create NAMES` | `dump -add NAMES` |
| `senv [NAME]` | `env [NAME]` |
| `checkpoint FILE` | `save FILE` |
| `fsdbDumpfile F`, `fsdbDumpvars [DEPTH] [SCOPES] [+all]`, `fsdbDumpon`, `fsdbDumpoff`, `fsdbDumpflush`, `fsdbDumpFinish`, `fsdbDumpMDA` | `dump -file F.fst`, `dump -add`, `dump -on` / `-off` / `-flush`; written as FST ([WAVES.md](WAVES.md)) |
| `database`, `simvision`, `dumptcf`, `assertion` | none: accepted from an xrun `-input` file with a note, and ignored |
