"""Fail-closed experimental pyslang -> Minisim IR -> generated C++ frontend.

Supported: one flat module, unsigned 1..64-bit signals, explicit edge-list always/always_ff,
begin/end, if/else, whole-variable NBA, known integer literals, unsigned conversions,
and unsigned + / - / == / != expressions. Everything else raises Unsupported.
The output header must follow minisim.cpp (with MINISIM_NO_DEMO) in one translation unit.
"""
import argparse
import hashlib
import json
from importlib.metadata import version
from pathlib import Path
import re

from pyslang.ast import Compilation
from pyslang import SVInt
from pyslang.syntax import SyntaxTree


class Unsupported(ValueError):
    pass


def kind(node):
    return str(node.kind).split(".")[-1]


def identifier(name):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
        raise Unsupported(f"unsupported identifier: {name!r}")
    return name


class Lowerer:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.tree = SyntaxTree.fromFile(str(self.path))
        self.compilation = Compilation()
        self.compilation.addSyntaxTree(self.tree)
        self.manager = self.tree.sourceManager
        self.digest = hashlib.sha256(self.path.read_bytes()).hexdigest()
        self.signals = {}
        self.parameters = {}
        self.source_map = {}

    def location(self, node):
        loc = node.sourceRange.start
        if self.manager.isMacroLoc(loc):
            raise Unsupported("macro source mapping is not supported")
        filename = Path(str(self.manager.getFileName(loc))).resolve()
        if filename != self.path:
            raise Unsupported("included source files are not supported")
        line = self.manager.getLineNumber(loc)
        key = f"line:f{self.digest[:16]}:{line}"
        self.source_map.setdefault(key, {"file": str(filename), "line": line})
        return key

    def width(self, typ):
        if not typ.isSimpleBitVector or not 1 <= typ.bitWidth <= 64:
            raise Unsupported(f"unsupported integral type: {typ}")
        return typ.bitWidth

    def expression(self, node):
        tag = kind(node)
        width = self.width(node.type)
        if tag == "NamedValue" and kind(node.symbol) == "Parameter":
            name = node.symbol.name
            if name not in self.parameters:
                raise Unsupported("parameter reference outside supported top")
            return {"op": "const", "value": self.parameters[name]["value"], "width": width}
        constant = node.constant
        if constant is not None and isinstance(constant.value, SVInt):
            value = constant.value
            if value.hasUnknown or (node.type.isSigned and value.isNegative()):
                raise Unsupported("unknown/negative constant expression")
            return {"op": "const", "value": int(value), "width": width}
        if tag == "IntegerLiteral":
            if node.value.hasUnknown or (node.type.isSigned and node.value.isNegative()):
                raise Unsupported("unknown/negative integer literal")
            return {"op": "const", "value": int(node.value), "width": width}
        if node.type.isSigned:
            raise Unsupported("signed expression")
        if not node.type.isFourState:
            raise Unsupported("two-state runtime expression")
        if tag == "NamedValue":
            name = node.symbol.name
            if name not in self.signals:
                raise Unsupported(f"non-module signal reference: {name}")
            return {"op": "ref", "name": name}
        if tag == "Conversion":
            operand = node.operand
            return {"op": "resize", "width": width, "operand": self.expression(operand)}
        if tag == "UnaryOp":
            operations = {"LogicalNot": "LogNot", "BitwiseNot": "Not"}
            op = str(node.op).split(".")[-1]
            if op not in operations:
                raise Unsupported(f"unary operator {node.op}")
            return {"op": operations[op], "operand": self.expression(node.operand)}
        if tag == "BinaryOp":
            operations = {"Add": "Add", "Subtract": "Sub", "Equality": "Eq",
                          "Inequality": "Ne", "BinaryAnd": "And",
                          "BinaryXor": "Xor", "BinaryOr": "Or", "Multiply": "Mul"}
            op = str(node.op).split(".")[-1]
            if op not in operations:
                raise Unsupported(f"binary operator {op}")
            return {"op": operations[op], "left": self.expression(node.left),
                    "right": self.expression(node.right)}
        raise Unsupported(f"expression {tag}")

    def statement(self, node, instructions):
        tag = kind(node)
        if tag == "Block":
            self.statement(node.body, instructions)
        elif tag == "List":
            for child in node.list:
                self.statement(child, instructions)
        elif tag == "Empty":
            return
        elif tag == "Conditional":
            if str(node.check).split(".")[-1] not in ("None", "None_"):
                raise Unsupported("unique/priority conditional checks")
            if len(node.conditions) != 1 or node.conditions[0].pattern is not None:
                raise Unsupported("pattern/multiple-condition if")
            branch = {"kind": "JmpIfNot", "expr": self.expression(node.conditions[0].expr),
                      "line": self.location(node)}
            instructions.append(branch)
            self.statement(node.ifTrue, instructions)
            jump = {"kind": "Jmp"}
            instructions.append(jump)
            branch["target"] = len(instructions)
            if node.ifFalse is not None:
                self.statement(node.ifFalse, instructions)
            jump["target"] = len(instructions)
        elif tag == "Case":
            if (str(node.condition).split(".")[-1] != "Normal" or
                    str(node.check).split(".")[-1] not in ("None", "None_")):
                raise Unsupported("only ordinary case without unique/priority supported")
            selector = self.expression(node.expr)
            exits = []
            for group in node.items:
                match = None
                for label in group.expressions:
                    value = self.expression(label)
                    if value["op"] != "const":
                        raise Unsupported("case label must be known constant")
                    test = {"op": "CaseEq", "left": selector, "right": value}
                    match = test if match is None else {"op": "Or", "left": match, "right": test}
                if match is None:
                    raise Unsupported("empty case item")
                branch = {"kind": "JmpIfNot", "expr": match, "line": self.location(node)}
                instructions.append(branch)
                self.statement(group.stmt, instructions)
                jump = {"kind": "Jmp"}; instructions.append(jump); exits.append(jump)
                branch["target"] = len(instructions)
            if node.defaultCase is not None:
                self.statement(node.defaultCase, instructions)
            for jump in exits:
                jump["target"] = len(instructions)
        elif tag == "ExpressionStatement":
            expr = node.expr
            if (kind(expr) != "Assignment" or not expr.isNonBlocking or expr.isCompound
                    or expr.timingControl is not None or kind(expr.left) != "NamedValue"):
                raise Unsupported("only untimed whole-variable NBA assignment is supported")
            name = expr.left.symbol.name
            if name not in self.signals:
                raise Unsupported(f"assignment to non-module signal {name}")
            instructions.append({"kind": "Nba", "signal": name,
                                 "expr": self.expression(expr.right), "line": self.location(node)})
        else:
            raise Unsupported(f"statement {tag}")

    def lower(self):
        tops = self.compilation.getRoot().topInstances
        errors = [str(d.code) for d in self.compilation.getAllDiagnostics() if d.isError()]
        if errors:
            raise Unsupported("slang compilation errors: " + ", ".join(errors))
        if len(tops) != 1:
            raise Unsupported("exactly one top module required")
        top = tops[0]
        procedures = []
        for member in top.body:
            tag = kind(member)
            if tag == "Port":
                continue
            if tag == "Parameter":
                if (self.manager.isMacroLoc(member.location) or
                        Path(str(self.manager.getFileName(member.location))).resolve() != self.path):
                    raise Unsupported("included or macro parameter declaration")
                value = member.value.value
                width = self.width(member.type)
                if not isinstance(value, SVInt) or value.hasUnknown or (member.type.isSigned and value.isNegative()):
                    raise Unsupported("parameter must be a known non-negative integral constant")
                name = identifier(member.name)
                self.parameters[name] = {"value": int(value), "width": width,
                                         "signed": member.type.isSigned, "local": member.isLocalParam}
                continue
            if tag in ("Variable", "Net"):
                if (self.manager.isMacroLoc(member.location) or
                        Path(str(self.manager.getFileName(member.location))).resolve() != self.path):
                    raise Unsupported("included or macro signal declaration")
                name = identifier(member.name)
                width = self.width(member.type)
                if (member.type.isSigned or not member.type.isFourState
                        or member.initializer is not None):
                    raise Unsupported("signed/two-state signal or declaration initializer")
                if member.type.hasFixedRange:
                    r = member.type.fixedRange
                    if r.right != 0 or r.left != width - 1:
                        raise Unsupported("only descending [N-1:0] packed ranges supported")
                self.signals[name] = width
            elif tag == "ProceduralBlock":
                procedures.append(member)
            else:
                raise Unsupported(f"module member {tag}")
        if not procedures:
            raise Unsupported("module has no executable process")
        programs = []
        for proc in procedures:
            if str(proc.procedureKind).split(".")[-1] not in ("Always", "AlwaysFF"):
                raise Unsupported("only always/always_ff processes supported")
            timed = proc.body
            if kind(timed) != "Timed" or kind(timed.timing) not in ("SignalEvent", "EventList"):
                raise Unsupported("single clock edge required")
            events = list(timed.timing.events) if kind(timed.timing) == "EventList" else [timed.timing]
            sensitivity = []
            for event in events:
                if kind(event) != "SignalEvent":
                    raise Unsupported("unsupported event list member")
                edge = str(event.edge).split(".")[-1]
                if (edge not in ("PosEdge", "NegEdge") or event.iffCondition is not None
                        or kind(event.expr) != "NamedValue"):
                    raise Unsupported("unsupported clock event")
                clock = event.expr.symbol.name
                if self.signals.get(clock) != 1:
                    raise Unsupported("clock must be one-bit module signal")
                item = {"signal": clock, "edge": "POS" if edge == "PosEdge" else "NEG"}
                if item not in sensitivity:
                    sensitivity.append(item)
            if not sensitivity:
                raise Unsupported("empty event list")
            wait = {"kind": "WaitEdge", **sensitivity[0]}
            if len(sensitivity) > 1:
                wait["events"] = sensitivity
            instructions = [wait]
            self.statement(timed.stmt, instructions)
            instructions.append({"kind": "Jmp", "target": 0})
            programs.append(instructions)
        return {"schema": "minisim-pyslang-ir-v1", "source_sha256": self.digest,
                "pyslang_version": version("pyslang"),
                "source": str(self.path), "top": identifier(top.name),
                "signals": self.signals, "parameters": self.parameters, "programs": programs, "source_map": self.source_map}


def cpp_expression(expr):
    op = expr["op"]
    if op in ("LogNot", "Not"):
        return f'un(Op::{op}, {cpp_expression(expr["operand"])})'
    if op == "const":
        return f'konst(mk({expr["value"]}ULL, {expr["width"]}))'
    if op == "ref":
        return f'ref(d.signals.at({json.dumps(expr["name"])}))'
    if op == "resize":
        return f'resize({cpp_expression(expr["operand"])}, {expr["width"]})'
    return f'bin(Op::{op}, {cpp_expression(expr["left"])}, {cpp_expression(expr["right"])})'


def cpp_header(model):
    lines = ["// Generated from pyslang elaborated AST; do not edit.", "#pragma once",
             "struct GeneratedDesign { std::map<std::string, int> signals; std::vector<Prog> programs; };",
             "GeneratedDesign build_generated_design() { GeneratedDesign d;"]
    for name, width in model["signals"].items():
        quoted = json.dumps(name)
        lines.append(f"d.signals.emplace({quoted}, S.sig({quoted}, {width}));")
    for program in model["programs"]:
        lines.append("{ Prog p;")
        for instr in program:
            lines.append(f'{{ Instr i; i.k = IK::{instr["kind"]};')
            if "signal" in instr:
                lines.append(f'i.sig = d.signals.at({json.dumps(instr["signal"])});')
            if "edge" in instr:
                lines.append(f'i.edge = {instr["edge"]};')
            for event in instr.get("events", []):
                lines.append(f'i.events.emplace_back(d.signals.at({json.dumps(event["signal"])}), {event["edge"]});')
            if "expr" in instr:
                lines.append(f'i.e = {cpp_expression(instr["expr"])};')
            if "target" in instr:
                lines.append(f'i.target = {instr["target"]};')
            if "line" in instr:
                lines.append(f'i.line_bin = {json.dumps(instr["line"])};')
            lines.append("p.emit(i); }")
        lines.append("d.programs.push_back(std::move(p)); }")
    return "\n".join(lines + ["return d; }", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        model = Lowerer(args.source).lower()
    except Unsupported as error:
        parser.exit(2, f"UNSUPPORTED: {error}\n")
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "design.json").write_text(json.dumps(model, indent=2))
    (args.output / "design.hpp").write_text(cpp_header(model))
    print(f'Lowered {model["top"]}: {len(model["programs"])} process(es), '
          f'{len(model["source_map"])} source line bins')


if __name__ == "__main__":
    main()
