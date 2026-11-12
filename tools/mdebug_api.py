#!/usr/bin/env python3
"""Python interface to mdebug.

One mdebug process loads the design and the waveform once; every call below is
one line to its `-script -` input and one line of JSON back.

    from mdebug_api import Mdebug
    with Mdebug(["design.sv", "-top", "tb"], wave="run.vcd") as d:
        print(d.value("tb.u.state", 40)["value"])
        for drv in d.drivers("tb.u.state"):
            print(drv["file"], drv["line"], drv["text"])
        print("\n".join(d.active("tb.busy", at=40)))
"""
import json
import subprocess


class MdebugError(RuntimeError):
    pass


class Mdebug:
    def __init__(self, args, wave=None, mdebug="mdebug", threads=None):
        cmd = [mdebug] + list(args)
        if wave:
            cmd += ["-vcd", wave]
        if threads:
            cmd += ["-threads", str(threads)]
        cmd += ["-script", "-"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, *words):
        """Send one command; the answer as a dict, or MdebugError."""
        line = " ".join(str(w) for w in words if w is not None)
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        if not out:
            raise MdebugError("mdebug ended (exit %s) before answering '%s'" % (self.proc.poll(), line))
        ans = json.loads(out)
        if not ans.get("ok"):
            raise MdebugError(ans.get("error", "failed: " + line))
        return ans

    def drivers(self, sig):
        return self.ask("drivers", sig)["drivers"]

    def loads(self, sig):
        return self.ask("loads", sig)["loads"]

    def value(self, sig, at=None):
        return self.ask("value", sig, at)

    def changes(self, sig, t0=0, t1=None):
        return self.ask("changes", sig, t0, t1)["changes"]

    def active(self, sig, at=None, depth=3):
        if at is None:
            at = self.end()
        return self.ask("active", sig, at, depth)["text"]

    def trace(self, sig, depth=2):
        return self.ask("trace", sig, depth)["text"]

    def blocks(self, inst):
        return self.ask("blocks", inst)["text"]

    def fsm(self, name=None, at=None):
        return self.ask("fsm", name, at if name else None)["text"]

    def scopes(self):
        return self.ask("scopes")["scopes"]

    def signals(self, scope=None):
        return self.ask("signals", scope)["signals"]

    def end(self):
        return self.ask("end")["end"]

    def close(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.close()
            except OSError:
                pass
            self.proc.wait()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
