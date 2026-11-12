"""Experimental strict frontend for flat synthesis netlists, separate from baseline.

Extends the validated unsigned sequential subset with continuous wire drivers,
ternary expressions and escaped flattened names. No delayed/strength drivers,
combinational cycles, hierarchy, initialization, SVA or signed runtime expressions.
"""
import json
import re
from importlib.metadata import version

if __package__:
    from .pyslang_lower import Lowerer, Unsupported, kind
else:
    from pyslang_lower import Lowerer, Unsupported, kind


def refs(expression):
    if expression.get("op") == "ref": return {expression["name"]}
    found = set()
    for value in expression.values():
        if isinstance(value, dict): found |= refs(value)
    return found


class NetlistLowerer(Lowerer):
    def expression(self, node):
        if kind(node) == "ConditionalOp":
            if (len(node.conditions) != 1 or getattr(node.conditions[0], "pattern", None)
                    or node.type.isSigned or not node.type.isFourState):
                raise Unsupported("unsupported conditional expression")
            width = self.width(node.type)
            return {"op": "mux", "condition": self.expression(node.conditions[0].expr),
                    "left": {"op": "resize", "width": width, "operand": self.expression(node.left)},
                    "right": {"op": "resize", "width": width, "operand": self.expression(node.right)}}
        return super().expression(node)

    def lower(self):
        tops = self.compilation.getRoot().topInstances
        if any(d.isError() for d in self.compilation.getAllDiagnostics()):
            raise Unsupported("slang netlist compilation errors")
        if len(tops) != 1: raise Unsupported("one flat top required")
        top = tops[0]
        procedures, assignments = [], []
        for member in top.body:
            tag = kind(member)
            if tag == "Port": continue
            if tag in ("Variable", "Net"):
                if not re.fullmatch(r"[A-Za-z_$][A-Za-z_0-9.$]*", member.name):
                    raise Unsupported("unsupported flattened signal name")
                width = self.width(member.type)
                if member.type.isSigned or not member.type.isFourState or member.initializer is not None:
                    raise Unsupported("signed/two-state/initialized netlist signal")
                if member.type.hasFixedRange:
                    r = member.type.fixedRange
                    if r.right != 0 or r.left != width - 1: raise Unsupported("nonzero/ascending range")
                if tag == "Net" and str(member.netType.netKind).split(".")[-1] != "Wire":
                    raise Unsupported("non-wire net resolution")
                self.signals[member.name] = width
            elif tag == "ContinuousAssign": assignments.append(member)
            elif tag == "ProceduralBlock": procedures.append(member)
            else: raise Unsupported(f"netlist member {tag}")
        programs, dependencies = [], {}
        for assignment in assignments:
            expr = assignment.assignment
            try: strength = assignment.driveStrength
            except TypeError as error:
                raise Unsupported("explicit drive strength is not supported by this binding") from error
            if (assignment.delay is not None or strength != (None, None)
                    or kind(expr.left) != "NamedValue" or expr.isCompound):
                raise Unsupported("delayed/strength/non-whole-wire continuous driver")
            name = expr.left.symbol.name
            if name not in self.signals: raise Unsupported("nonlocal continuous destination")
            value = self.expression(expr.right)
            inputs = sorted(refs(value))
            dependencies.setdefault(name, set()).update(inputs)
            program = [{"kind": "Assign", "signal": name, "expr": value,
                        "driver": True, "line": self.location(expr)}]
            if inputs:
                program += [{"kind": "WaitEdge", "events": inputs}, {"kind": "Jmp", "target": 0}]
            programs.append(program)
        visiting, done = set(), set()
        def visit(name):
            if name in visiting: raise Unsupported("combinational cycle")
            if name in done: return
            visiting.add(name)
            for source in dependencies.get(name, set()): visit(source)
            visiting.remove(name); done.add(name)
        for name in dependencies: visit(name)
        written = set()
        for proc in procedures:
            if str(proc.procedureKind).split(".")[-1] not in ("Always", "AlwaysFF"):
                raise Unsupported("non-clocked netlist process")
            statement = proc.body
            if kind(statement) != "Timed" or kind(statement.timing) != "SignalEvent":
                raise Unsupported("single clock edge required")
            event = statement.timing
            edge = str(event.edge).split(".")[-1]
            if (edge not in ("PosEdge", "NegEdge") or event.iffCondition is not None
                    or kind(event.expr) != "NamedValue"):
                raise Unsupported("unsupported netlist clock event")
            clock = event.expr.symbol.name
            if self.signals.get(clock) != 1: raise Unsupported("clock must be scalar")
            instructions = [{"kind": "WaitEdge", "signal": clock,
                             "edge": "POS" if edge == "PosEdge" else "NEG"}]
            self.statement(statement.stmt, instructions)
            process_written = set()
            for instruction in instructions:
                if instruction["kind"] == "Nba":
                    if instruction["signal"] in dependencies:
                        raise Unsupported("mixed continuous/procedural writers")
                    process_written.add(instruction["signal"])
            if written & process_written: raise Unsupported("multiple procedural writers")
            written |= process_written
            instructions.append({"kind": "Jmp", "target": 0})
            programs.append(instructions)
        if not programs: raise Unsupported("empty netlist")
        return {"schema": "minisim-flat-netlist-ir-v1", "source": str(self.path),
                "source_sha256": self.digest, "pyslang_version": version("pyslang"),
                "top": top.name, "signals": self.signals, "programs": programs,
                "source_map": self.source_map}


def expression_cpp(expr):
    op = expr["op"]
    if op == "const": return f'konst(mk({expr["value"]}ULL, {expr["width"]}))'
    if op == "ref": return f'ref(d.signals.at({json.dumps(expr["name"])}))'
    if op == "resize": return f'resize({expression_cpp(expr["operand"])}, {expr["width"]})'
    if op == "mux":
        return f'mux({expression_cpp(expr["condition"])}, {expression_cpp(expr["left"])}, {expression_cpp(expr["right"])})'
    return f'bin(Op::{op}, {expression_cpp(expr["left"])}, {expression_cpp(expr["right"])})'


def header(model):
    lines = ["#pragma once", "struct GeneratedDesign { std::map<std::string,int> signals; std::vector<Prog> programs; };",
             "GeneratedDesign build_generated_design() { GeneratedDesign d;"]
    for name, width in model["signals"].items():
        quoted = json.dumps(name)
        lines.append(f'd.signals.emplace({quoted}, S.sig({quoted}, {width}));')
    for program in model["programs"]:
        lines.append("{ Prog p;")
        for instr in program:
            lines.append(f'{{ Instr i; i.k = IK::{instr["kind"]};')
            if "signal" in instr:
                lines.append(f'i.sig = d.signals.at({json.dumps(instr["signal"])});')
            if instr.get("driver"): lines.append("i.drv = S.newDriver(i.sig);")
            if "edge" in instr: lines.append(f'i.edge = {instr["edge"]};')
            if "events" in instr:
                events = ",".join('{d.signals.at(' + json.dumps(name) + '), ANY}' for name in instr["events"])
                lines.append(f'i.events = {{{events}}}; i.sig = i.events[0].first; i.edge = ANY;')
            if "expr" in instr: lines.append(f'i.e = {expression_cpp(instr["expr"])};')
            if "target" in instr: lines.append(f'i.target = {instr["target"]};')
            if "line" in instr: lines.append(f'i.line_bin = {json.dumps(instr["line"])};')
            lines.append("p.emit(i); }")
        lines.append("d.programs.push_back(std::move(p)); }")
    return "\n".join(lines + ["return d; }", ""])
