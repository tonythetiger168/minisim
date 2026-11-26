"""Vendor coverage command lines for mcov (URG, IMC batch, vcover).

Part of the vendor compatibility layer: installed next to mcov only when
minisim is built with -DMINISIM_VENDOR_COMPAT=ON (docs/compat/COMPAT.md).
mcov loads it when it sits beside mcov and sets native_main to its own main.

With --compat=synopsys,
cadence or siemens (or -compat X), or started through a symlink named urg,
imc or vcover, mcov takes those tools' commands and runs its own engine:

    mcov --compat=synopsys -dir runs -dbname all.json -report urgReport -format both -elfile x.el -grade
    mcov --compat=cadence -execcmd "merge a.json b.json -out all.json; load all.json; report -detail -out r.txt"
    mcov --compat=cadence -exec cmds.tcl      # load, merge -out, report [-summary|-detail] [-html] -out, rank, exit
    mcov --compat=siemens merge all.json a.json b.json       # or merge -out all.json a.json b.json
    mcov --compat=siemens report -details -output r.txt all.json   # -html [-htmldir d]
    mcov --compat=siemens ranktest -rankfile rank.txt a.json b.json

Give mcov databases (JSON or UCIS XML, or a directory of them) where those
tools take their own binary databases, which are not read. Licence-queue
and 64-bit options are ignored, with a note.
"""
import os
import sys

native_main = None  # set by mcov when it loads this module

# Other vendors' coverage command lines (docs/compat/COMPAT.md): one engine, these
# only translate. --compat=synopsys|cadence|siemens (or -compat X) anywhere,
# or the name mcov was started as (a symlink the user makes) picks the
# vendor. Their binary databases are not read: give mcov databases (JSON or
# UCIS XML) where those tools take theirs.

COMPAT_ALIASES = {"urg": "synopsys", "imc": "cadence", "vcover": "siemens"}
COMPAT_NAMES = {"synopsys": "synopsys", "snps": "synopsys", "cadence": "cadence", "cdns": "cadence",
                "siemens": "siemens", "mentor": "siemens"}
# Options with no meaning here, for every vendor (licence queues, 32/64 bit).
COMPAT_IGNORED = {"-full64", "-licqueue", "-64bit", "-64", "-32", "-nocopyright", "-batch", "-nolog", "-nostdout"}


class CompatError(Exception):
    pass


def compat_detect(argv0, args):
    """(vendor or None, args without the -compat option)."""
    vendor, rest, i = None, [], 0
    while i < len(args):
        a = args[i]
        if a.startswith("--compat=") or a.startswith("-compat="):
            vendor = a.split("=", 1)[1]
        elif a in ("-compat", "--compat") and i + 1 < len(args):
            vendor = args[i + 1]
            i += 1
        else:
            rest.append(a)
        i += 1
    if vendor is not None:
        v = COMPAT_NAMES.get(vendor.lower())
        if v is None:
            raise CompatError(f"unknown vendor '{vendor}' (synopsys, cadence, siemens)")
        return v, rest
    base = os.path.basename(argv0 or "")
    for ext in (".py", ".exe"):
        if base.endswith(ext):
            base = base[:-len(ext)]
    return COMPAT_ALIASES.get(base), rest


def _note(msg):
    print(f"mcov: {msg}", file=sys.stderr)


def _db_paths(items):
    """Databases named on another tool's command line: files, or every .json / .xml database in a directory."""
    out = []
    for p in items:
        if os.path.isdir(p):
            found = sorted(q for q in (os.path.join(p, f) for f in os.listdir(p)) if _is_db(q))
            if not found:
                raise CompatError(f"{p}: a directory with no mcov database (.json or .xml) in it; "
                                  "vendor binary databases are not read")
            out += found
        elif not os.path.isfile(p):
            raise CompatError(f"{p}: no such database")
        else:
            out.append(p)
    return out


def _is_db(path):
    """An mcov database by its first bytes: msim-cov JSON or UCIS XML."""
    if not path.endswith((".json", ".xml")) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        head = f.read(512)
    return b'"msim-cov"' in head or b"UCIS" in head


def _read_list(path):
    with open(path, encoding="utf-8") as f:
        return [t for line in f for t in line.split("#", 1)[0].split()]


def _run(argv, out=None):
    """Run a native mcov command; its output to the file out if given."""
    if out is None:
        return native_main(argv, compat=False)
    import contextlib
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f, contextlib.redirect_stdout(f):
        return native_main(argv, compat=False)


def compat_urg(args):
    """URG: urg -dir a -dir b [-f list] [-dbname merged] [-report dir] [-format text|html|both]
    [-elfile x.el] [-grade] [-show tests]"""
    dirs, el, report, fmt, dbname, grade, i = [], [], "urgReport", "html", None, False, 0
    while i < len(args):
        a = args[i]
        val = lambda: args[i + 1] if i + 1 < len(args) else (_ for _ in ()).throw(CompatError(f"{a} needs a value"))
        if a == "-dir":
            dirs.append(val()); i += 1
        elif a == "-f":
            dirs += _read_list(val()); i += 1
        elif a == "-elfile":
            el.append(val()); i += 1
        elif a == "-report":
            report = val(); i += 1
        elif a == "-format":
            fmt = val().lower(); i += 1
        elif a == "-dbname":
            dbname = val(); i += 1
        elif a in ("-grade", "-grade_tests"):
            grade = True
            if i + 1 < len(args) and not args[i + 1].startswith("-"):
                i += 1       # the grading goal ("index", "quick", ...): ranking here is always greedy
        elif a in ("-metric", "-show", "-log", "-plan", "-hvp_no_score_missing", "-parallel", "-lca"):
            _note(f"ignored synopsys option {a} {val()}"); i += 1
        elif a in COMPAT_IGNORED or a in ("-noreport", "-warn"):
            if a == "-noreport":
                report = None
            else:
                _note(f"ignored synopsys option {a}")
        elif a.startswith("-"):
            raise CompatError(f"unknown urg option {a}")
        else:
            dirs.append(a)
        i += 1
    dbs = _db_paths(dirs)
    if not dbs:
        raise CompatError("no database: give -dir <database>")
    rc = 0
    if dbname:
        rc |= _run(["merge"] + dbs + ["-o", dbname])
    if grade:
        rc |= _run(["rank"] + dbs, os.path.join(report, "grades.txt") if report else None)
    if report:
        els = sum((["-elfile", e] for e in el), [])
        rc |= _run(["report"] + dbs + els + ["-detail", "-instances"], os.path.join(report, "dashboard.txt"))
        if fmt in ("html", "both"):
            rc |= _run(["report"] + dbs + els + ["-html", report], os.devnull)
        print(f"mcov: report in {report}")
    return rc


def _tcl_words(cmd):
    import shlex
    try:
        return shlex.split(cmd.replace("{", '"').replace("}", '"'))
    except ValueError as e:
        raise CompatError(f"cannot read command '{cmd}': {e}")


def compat_imc(args):
    """IMC batch: imc [-load db] -exec file.tcl | -execcmd "cmd; cmd"; commands load, merge, report, rank, exit."""
    cmds, loaded, i = [], [], 0
    while i < len(args):
        a = args[i]
        if a in ("-exec", "-execcmd", "-load", "-init") and i + 1 < len(args):
            v = args[i + 1]
            if a == "-exec":
                with open(v, encoding="utf-8") as f:
                    cmds += f.read().splitlines()
            elif a == "-execcmd":
                cmds += v.split(";")
            elif a == "-load":
                loaded += _db_paths([v])
            i += 1
        elif a.lower() in COMPAT_IGNORED or a.lower() in ("-gui", "-nogui"):
            _note(f"ignored cadence option {a}")
        else:
            raise CompatError(f"unknown imc option {a}")
        i += 1
    rc = 0
    for raw in cmds:
        for cmd in raw.split(";"):
            w = _tcl_words(cmd.split("#", 1)[0] if cmd.lstrip().startswith("#") else cmd)
            if not w:
                continue
            op, rest = w[0], w[1:]
            flags = lambda names: [x for x in rest if x in names]
            if op == "load":
                loaded = _db_paths([x for x in rest if not x.startswith("-")])
            elif op == "merge":
                out = None
                ins = []
                k = 0
                while k < len(rest):
                    if rest[k] == "-out" and k + 1 < len(rest):
                        out = rest[k + 1]; k += 1
                    elif rest[k] in ("-run_file", "-runfile") and k + 1 < len(rest):
                        ins += _read_list(rest[k + 1]); k += 1
                    elif rest[k].startswith("-"):
                        _note(f"ignored imc merge option {rest[k]}")
                    else:
                        ins.append(rest[k])
                    k += 1
                if not out:
                    raise CompatError("merge needs -out <database>")
                rc |= _run(["merge"] + _db_paths(ins or loaded) + ["-o", out])
                loaded = [out]
            elif op == "report":
                out, html_, k, extra = None, False, 0, []
                while k < len(rest):
                    x = rest[k]
                    if x == "-out" and k + 1 < len(rest):
                        out = rest[k + 1]; k += 1
                    elif x == "-html":
                        html_ = True
                    elif x in ("-detail", "-details"):
                        extra = ["-detail", "-instances"]
                    elif x == "-summary":
                        pass
                    elif x in ("-metrics", "-inst", "-type", "-grading") and k + 1 < len(rest):
                        _note(f"ignored imc report option {x} {rest[k + 1]}"); k += 1
                    else:
                        _note(f"ignored imc report option {x}")
                    k += 1
                if not loaded:
                    raise CompatError("report: no database loaded (load <database>)")
                if html_:
                    rc |= _run(["report"] + loaded + extra + ["-html", out or "imc_report"])
                else:
                    rc |= _run(["report"] + loaded + extra, out)
            elif op == "rank":
                out, ins, k = None, [], 0
                while k < len(rest):
                    if rest[k] == "-out" and k + 1 < len(rest):
                        out = rest[k + 1]; k += 1
                    elif rest[k] in ("-run_file", "-runfile") and k + 1 < len(rest):
                        ins += _read_list(rest[k + 1]); k += 1
                    elif rest[k].startswith("-"):
                        _note(f"ignored imc rank option {rest[k]}")
                    else:
                        ins.append(rest[k])
                    k += 1
                rc |= _run(["rank"] + _db_paths(ins or loaded) + (["-o", out] if out else []))
            elif op in ("exit", "quit"):
                return rc
            elif op in ("config", "set_message", "puts"):
                pass
            else:
                raise CompatError(f"imc command '{op}' is not supported")
    return rc


def compat_vcover(args):
    """vcover merge [-out out] out in... | report [-details] [-html] [-output out] db... | ranktest [-rankfile f] db..."""
    if not args:
        raise CompatError("vcover: give merge, report or ranktest")
    op, rest = args[0], args[1:]
    opts, pos, k = {}, [], 0
    takes = {"-out", "-output", "-outputstore", "-rankfile", "-testassociated", "-inputs", "-du", "-instance",
             "-htmldir", "-elfile"}
    while k < len(rest):
        x = rest[k]
        if x in takes and k + 1 < len(rest):
            opts.setdefault(x, []).append(rest[k + 1]); k += 1
        elif x.startswith("-"):
            opts[x] = [True]
        else:
            pos.append(x)
        k += 1
    for x in opts.get("-inputs", []):
        pos += _read_list(x)
    if op == "merge":
        out = (opts.get("-out") or [None])[0]
        if out is None:
            if len(pos) < 2:
                raise CompatError("vcover merge: give the output then the inputs, or -out")
            out, pos = pos[0], pos[1:]
        return _run(["merge"] + _db_paths(pos) + ["-o", out])
    if op == "report":
        extra = ["-detail", "-instances"] if "-details" in opts or "-detail" in opts else []
        els = sum((["-elfile", e] for e in opts.get("-elfile", [])), [])
        if "-html" in opts:
            d = (opts.get("-htmldir") or opts.get("-output") or ["covhtmlreport"])[0]
            return _run(["report"] + _db_paths(pos) + els + extra + ["-html", d])
        return _run(["report"] + _db_paths(pos) + els + extra, (opts.get("-output") or [None])[0])
    if op == "ranktest":
        rf = (opts.get("-rankfile") or [None])[0]
        return _run(["rank"] + _db_paths(pos), rf)
    raise CompatError(f"vcover {op} is not supported (merge, report, ranktest)")


def compat_main(vendor, args):
    try:
        if vendor == "synopsys":
            return compat_urg(args)
        if vendor == "cadence":
            return compat_imc(args)
        return compat_vcover(args)
    except CompatError as e:
        print(f"mcov: {e}", file=sys.stderr)
        return 2
