#pragma once
// lintcfg.hpp - what mlint knows about its rules, and what the user said
// about them: the rule table (id, group, severity, on or off by default),
// the configuration (enable / disable / severity / set), and the waivers
// (a waiver file, and "// mlint: waive RULE" comments in the source).
//
// A configuration file is one directive per line ('#' starts a comment):
//
//   enable  NAME001 naming          # a rule, a group, or "all"
//   disable W002
//   only    width,latch             # everything else off
//   severity W001 error             # error | warning | info
//   set name.module ^[a-z][a-z0-9_]*$
//   waive W001 -file "rtl/legacy/*" -msg "*tmp*" -line 10-40 -reason "old IP"
//
// In the source:
//
//   assign y = a;   // mlint: waive W002 zero extension is intended
//   // mlint: waive W001,W004        (alone on a line: applies to the next line)
//   // mlint: off SYN002             ... // mlint: on SYN002
//   // synthesis translate_off       ... // synthesis translate_on   (SYN rules)
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lintcfg {

enum class Sev { Info, Warning, Error };

inline const char* sevName(Sev s) {
    return s == Sev::Error ? "error" : s == Sev::Warning ? "warning" : "info";
}

struct RuleInfo {
    const char* id;
    const char* group;
    Sev sev;
    bool on;             // on by default
    const char* brief;   // one line, for -list
    const char* detail;  // why it matters and what to do, for -explain
};

// The rule table. Rules of other layers (UPF, later CDC rules) that are not
// listed here still work: they are on, warnings, and their group is the
// letters of their name in lower case.
inline const std::vector<RuleInfo>& table() {
    static const std::vector<RuleInfo> t = {
        // ------------------------------------------------------------ assignments
        {"L001", "latch", Sev::Warning, true, "a value is not assigned on every path of an always_comb (a latch)",
         "An if with no else, or a case with no default, leaves the old value in place, which is a latch. "
         "Assign a default at the top of the block or cover every path."},
        {"L002", "assign", Sev::Warning, true, "a blocking assignment in an always_ff",
         "Other blocks on the same clock see the new or the old value depending on evaluation order. Use <=."},
        {"L003", "assign", Sev::Warning, true, "a non-blocking assignment in an always_comb",
         "The block's later statements read the old value, so simulation and synthesis disagree. Use =."},
        {"L004", "driver", Sev::Warning, true, "one value is assigned by more than one process",
         "Synthesis reports a multiple-driver error; simulation keeps the last write."},
        {"SEQ001", "assign", Sev::Warning, true, "a variable has both blocking and non-blocking assignments",
         "Mixing = and <= on one variable makes its update time depend on which statement ran. Pick one."},
        {"SEQ002", "assign", Sev::Warning, true, "a blocking assignment in an edge-triggered always, to a variable read elsewhere",
         "The old-style counterpart of L002: a blocking assignment in always @(posedge clk) races with the "
         "blocks that read the variable. A temporary used only inside the block is not reported."},
        {"SEQ003", "assign", Sev::Warning, true, "a non-blocking assignment in a combinational always @*",
         "The old-style counterpart of L003."},
        {"EXP001", "assign", Sev::Warning, true, "a variable is assigned to itself",
         "x = x does nothing; it is usually a typo for another name."},

        // ----------------------------------------------------------- latches, comb
        {"LAT001", "latch", Sev::Warning, true, "a value is not assigned on every path of a combinational always @* (a latch)",
         "The old-style counterpart of L001, for always @* and always @(a or b)."},
        {"LAT002", "latch", Sev::Info, false, "an always_latch: a latch by intent",
         "For flows that allow no latches at all. Off by default."},
        {"COMB001", "comb", Sev::Warning, true, "a signal read by a combinational always is missing from its sensitivity list",
         "Simulation does not re-evaluate the block when that signal changes, but synthesis builds the full "
         "logic: a simulation / synthesis mismatch. Use always_comb or @*."},
        {"COMB002", "comb", Sev::Warning, true, "a combinational loop",
         "A signal depends on itself through combinational logic only. Whole-word dependencies are followed "
         "through continuous assignments, combinational blocks and port connections; bit-select chains "
         "(c[i+1] from c[i]) are not reported."},

        // -------------------------------------------------------------------- case
        {"CASE001", "case", Sev::Warning, true, "a case in combinational logic has no default and does not cover every value",
         "Unlisted values keep the old output (a latch) or propagate nothing in simulation. Add a default, or "
         "use unique / priority when the missing values cannot happen."},
        {"CASE002", "case", Sev::Warning, true, "casex",
         "casex treats X and Z in the selector as wildcards too, so an unknown selector matches the first "
         "item silently. Use casez or case ... inside."},
        {"CASE003", "case", Sev::Warning, true, "an X or Z in an item of a plain case",
         "A plain case compares 4-state values exactly, so the item never matches in hardware. Use casez with ?."},
        {"CASE004", "case", Sev::Warning, true, "the same value appears in two items of one case",
         "The second item never runs."},

        // ------------------------------------------------------------------- width
        {"W001", "width", Sev::Warning, true, "an assignment truncates its right-hand side",
         "The value is wider than its target, so the upper bits are dropped. Select the bits you mean, or "
         "widen the target. Constants that fit and x = x + 1 are not reported."},
        {"W002", "width", Sev::Info, false, "an assignment extends a narrower right-hand side",
         "The value is zero- or sign-extended to the target. Usually intended; off by default."},
        {"W003", "width", Sev::Warning, true, "a port is connected to an expression of another width",
         "The connection is truncated or padded without notice."},
        {"W004", "width", Sev::Warning, true, "a comparison of operands of different widths",
         "The narrower operand is extended first, which is rarely what was meant when the widths differ by "
         "accident; a constant too wide for the other side can never match."},
        {"W005", "width", Sev::Warning, true, "a signed value is compared with an unsigned one",
         "The comparison is done unsigned, so a negative value compares as a large one."},
        {"W006", "width", Sev::Info, false, "a multi-bit value is used as a condition",
         "if (vec) is true when any bit is set; write vec != 0 to say so. Off by default."},
        {"W007", "width", Sev::Info, false, "a logical operator (!, &&, ||) is applied to a multi-bit value",
         "!vec is a 1-bit result, ~vec is bitwise: a common slip. Off by default."},
        {"W008", "width", Sev::Warning, true, "the two branches of ?: have different widths",
         "The narrower branch is extended to the wider one."},

        // ---------------------------------------------------------- synthesizability
        {"SYN001", "synth", Sev::Warning, true, "an initial block assigns design signals",
         "ASIC synthesis ignores initial blocks, so the gates start from an unknown value where simulation "
         "starts from this one. Use a reset. (Testbench modules are not checked.)"},
        {"SYN002", "synth", Sev::Warning, true, "a delay in design code",
         "Synthesis ignores #delays: simulation timing then differs from the gates."},
        {"SYN003", "synth", Sev::Warning, true, "a system task or function that cannot be synthesized",
         "$random, $time, file I/O, $finish and the like have no hardware meaning in an always block or a "
         "continuous assignment."},
        {"SYN004", "synth", Sev::Warning, true, "force, release, or a procedural assign / deassign",
         "Not synthesizable."},
        {"SYN005", "synth", Sev::Warning, true, "fork, wait, or an event trigger in design code",
         "Not synthesizable."},
        {"SYN006", "synth", Sev::Warning, true, "a real-typed signal in design code",
         "Real numbers are not synthesizable."},
        {"SYN007", "synth", Sev::Warning, true, "a sensitivity list mixes edges and levels",
         "@(posedge clk or d) matches neither a flop nor combinational logic."},
        {"SYN008", "synth", Sev::Warning, true, "a forever loop, or a loop with a timing control, in an always block",
         "Synthesis needs loops it can unroll at compile time."},
        {"SYN009", "synth", Sev::Warning, true, "== or != against a value with X or Z bits",
         "The result is X in simulation and constant in hardware. Use $isunknown in testbench code."},
        {"SYN010", "synth", Sev::Warning, true, "=== or !== in design code",
         "Hardware has no X to compare with; synthesis treats it as ==, simulation does not."},
        {"SYN011", "synth", Sev::Info, false, "an X is assigned",
         "Some guidelines forbid X assignments (X optimism hides bugs), others ask for them in default "
         "branches. Off by default."},
        {"SYN012", "synth", Sev::Warning, true, "an event control inside the body of an always block",
         "More than one @ in a block (an implicit state machine) is not synthesizable by most tools."},
        {"SYN013", "synth", Sev::Warning, true, "a dynamic type (string, queue, dynamic or associative array, class, event) in design code",
         "Not synthesizable."},
        {"SYN014", "synth", Sev::Info, true, "division, modulo or power by a value that is not a constant power of two",
         "Synthesizable by some tools but as a large, slow divider; usually a shared divider IP is meant."},
        {"SYN015", "synth", Sev::Warning, true, "defparam",
         "Deprecated (IEEE 1800-2023 annex C) and unsupported by many tools. Use #(.P(v))."},
        {"SYN016", "synth", Sev::Info, true, "a tri-state driver on a signal that is not an inout port",
         "Internal tri-state buses are not available in most ASIC and FPGA fabrics. Use a mux."},
        {"SYN017", "synth", Sev::Warning, true, "a hierarchical reference in design code",
         "a.b.c reaches into another module; synthesis works on one module at a time."},

        // ----------------------------------------------------------------- unused
        {"UNU001", "unused", Sev::Warning, true, "a signal is declared but never used",
         "Neither read nor driven in any instance. Names that start with 'unused' are not reported."},
        {"UNU002", "unused", Sev::Warning, true, "a signal is driven but never read",
         "Dead logic, or a missing connection."},
        {"UNU003", "unused", Sev::Warning, true, "a signal is read but never driven",
         "It is X (a variable) or Z (a net) for the whole simulation."},
        {"UNU004", "unused", Sev::Info, true, "an input port is never read", "The port can be removed, or a connection is missing."},
        {"UNU005", "unused", Sev::Warning, true, "an output port is never driven", "The port floats."},

        // ------------------------------------------------------------ connectivity
        {"CON001", "connect", Sev::Warning, true, "an input port of an instance is left unconnected",
         "The input floats (Z, read as X)."},
        {"CON002", "connect", Sev::Info, false, "an output port of an instance is left unconnected", "Off by default."},
        {"CON003", "connect", Sev::Warning, true, "ports connected by position",
         "A port added to the module later shifts every connection after it. Connect by name."},
        {"CON004", "connect", Sev::Warning, true, "an implicit net",
         "An undeclared name became a 1-bit wire: usually a typo. `default_nettype none makes it an error."},

        // ------------------------------------------------------------ clock, reset
        {"CLK001", "clock", Sev::Warning, true, "a clock is used as data",
         "A clock read by logic (other than a plain buffer assignment) makes a path that timing analysis "
         "treats specially. Clock gates and muxes belong in dedicated cells; waive this there."},
        {"CLK002", "clock", Sev::Info, false, "a flop on the falling edge of its clock", "Off by default."},
        {"CLK003", "clock", Sev::Warning, true, "one signal is used both as a clock and as a reset", ""},
        {"RST001", "reset", Sev::Warning, true, "one reset is used with both polarities",
         "Active high in one block and active low in another."},
        {"RST002", "reset", Sev::Warning, true, "one reset is used both asynchronously and synchronously", ""},
        {"RST003", "reset", Sev::Warning, true, "a flop in an asynchronous-reset block is not reset",
         "A register assigned in the else branch but not in the reset branch gets the reset as an enable: "
         "extra logic, and a hold path from the reset. Give it its own block or reset it."},
        {"RST004", "reset", Sev::Info, false, "a flop with no reset", "Off by default."},
        {"RST005", "reset", Sev::Warning, true, "the reset's edge does not match the level its block tests",
         "@(negedge rst_n) must go with if (!rst_n), and @(posedge rst) with if (rst)."},
        {"RST006", "reset", Sev::Warning, true, "an asynchronous reset to a value that is not constant",
         "Needs a flop with both set and reset, or is not synthesizable."},
        {"RST007", "reset", Sev::Warning, true, "an asynchronous block that does not start with if (reset)",
         "Synthesis recognises a flop with asynchronous reset only in the form "
         "if (reset) ... else ...; anything else may not map."},

        // ------------------------------------------------------------------ naming
        {"NAME001", "naming", Sev::Warning, false, "a module name does not match name.module", ""},
        {"NAME002", "naming", Sev::Warning, false, "a signal name does not match name.signal", ""},
        {"NAME003", "naming", Sev::Warning, false, "a parameter name does not match name.param", ""},
        {"NAME004", "naming", Sev::Warning, false, "a clock's name does not match name.clock", ""},
        {"NAME005", "naming", Sev::Warning, false, "a reset's name does not match name.reset", ""},
        {"NAME006", "naming", Sev::Warning, false, "an active-low reset's name does not match name.active_low", ""},
        {"NAME007", "naming", Sev::Warning, false, "an instance name does not match name.instance", ""},
        {"NAME008", "naming", Sev::Warning, false, "a port name does not match name.port_in / name.port_out", ""},
        {"NAME009", "naming", Sev::Warning, true, "two names in one scope differ only in case",
         "Easy to mix up, and a clash in case-insensitive tools (VHDL, some netlist formats)."},
        {"NAME010", "naming", Sev::Warning, false, "a name is longer than name.max_length", ""},
        {"NAME011", "naming", Sev::Info, true, "a generate block has no label",
         "Its hierarchical names are genblk1, genblk2, ... and change when a block is added above it."},
        {"NAME012", "naming", Sev::Warning, false, "a name is a VHDL keyword", "A problem in mixed-language flows."},
        {"NAME013", "naming", Sev::Warning, false, "a typedef name does not match name.typedef", ""},
        {"NAME014", "naming", Sev::Warning, false, "a macro name does not match name.macro", "Verible rule macro-name-style."},

        // ------------------------------------------------------------------- style
        {"NAME015", "naming", Sev::Warning, false, "an interface name does not match name.interface", "Verible rule interface-name-style."},
        {"NAME016", "naming", Sev::Warning, false, "a constraint name does not match name.constraint", "Verible rule constraint-name-style."},
        {"NAME017", "naming", Sev::Info, false, "a parameter named for what it disables", "Verible rule positive-meaning-parameter-name."},
        {"NAME018", "naming", Sev::Info, false, "a register written in always_ff does not match name.dff",
         "Verible rule dff-name-style (output side). Default suffixes _q, _ff, _reg, _r."},
        {"NAME019", "naming", Sev::Info, false, "a declaration hides one of the same name in an enclosing scope",
         "Verible rule instance-shadowing."},
        {"STY001", "style", Sev::Info, true, "more than one module in a file", ""},
        {"STY002", "style", Sev::Info, false, "a file's name is not its module's name", ""},
        {"STY003", "style", Sev::Info, false, "a plain always where always_ff / always_comb / always_latch says more", ""},
        {"STY004", "style", Sev::Info, false, "`endif with no comment", "Verible rule endif-comment."},
        {"STY005", "style", Sev::Info, false, "a package in a file that is not named after it", "Verible rule package-filename."},
        {"STY006", "style", Sev::Info, false, "a function or task with no lifetime (automatic or static)", "Verible rule explicit-function-lifetime, explicit-task-lifetime."},
        {"STY007", "style", Sev::Info, false, "a parameter with no type", "Verible rule explicit-parameter-storage-type."},
        {"STY008", "style", Sev::Info, false, "a packed dimension that counts up", "Verible rule packed-dimensions-range-ordering."},
        {"STY009", "style", Sev::Info, false, "an unpacked dimension that counts down", "Verible rule unpacked-dimensions-range-ordering."},
        {"STY010", "style", Sev::Info, false, "a string literal carried over the line with a backslash", "Verible rule forbid-line-continuations."},
        {"STY011", "style", Sev::Info, false, "a semicolon after a UVM macro", "Verible rule uvm-macro-semicolon."},
        {"STY012", "style", Sev::Info, false, "disable of a name that is not an enclosing begin block", "Verible rule disable-statement."},
        {"STY013", "style", Sev::Info, false, "an if, else, loop, always or initial body with no begin ... end",
         "Verible rule explicit-begin. else if chains are fine."},
        {"STY014", "style", Sev::Info, false, "parameter declared outside a #(...) list (module body, package, class body)",
         "Verible rule proper-parameter-declaration: use localparam, or move it into the parameter list."},
        {"STY015", "style", Sev::Info, false, "a hex or binary format with no 0x/0b prefix, or %d after one",
         "Verible rule numeric-format-string-style."},
        {"FUNC001", "style", Sev::Info, true, "a function reads or writes a signal that is not one of its arguments",
         "always @* does not see signals read inside a function, and the function cannot be reused."},

#ifdef MINISIM_TIDY
        // -------------------------------------------------------------------- tidy
        // slang-tidy's checks (linttidy.hpp); the detail names the check.
        {"TIDY001", "tidy", Sev::Warning, true, "a register is assigned in the reset branch only", "slang-tidy check OnlyAssignedOnReset."},
        {"TIDY002", "tidy", Sev::Warning, true, "an always_ff with a reset assigns a register outside the if / else of the reset", "slang-tidy check AlwaysFFAssignmentOutsideConditional."},
        {"TIDY003", "tidy", Sev::Warning, true, "a for loop in front of the reset test of an always_ff", "slang-tidy check LoopBeforeResetCheck."},
        {"TIDY004", "tidy", Sev::Warning, true, "some bits of a signal are never driven", "slang-tidy check UndrivenRange."},
        {"TIDY005", "tidy", Sev::Warning, true, "fields of one struct are assigned in different processes", "slang-tidy check StructFieldsInSameProcess."},
        {"TIDY006", "tidy", Sev::Warning, true, "a signed cast used as an index", "slang-tidy check CastSignedIndex."},
        {"TIDY007", "tidy", Sev::Warning, false, "a signal in a sensitivity list is not used in the block", "slang-tidy check UnusedSensitiveSignal."},
        {"TIDY008", "tidy", Sev::Warning, false, "'d or '? literals (not accepted by some FPGA tools)", "slang-tidy check XilinxDoNotCareValues."},
        {"TIDY009", "tidy", Sev::Warning, false, "a .* port connection", "slang-tidy check NoDotStarInPortConnection."},
        {"TIDY010", "tidy", Sev::Warning, false, "a .name port connection with no expression", "slang-tidy check NoDotVarInPortConnection."},
        {"TIDY011", "tidy", Sev::Warning, false, "a non-ANSI port declaration", "slang-tidy check OnlyANSIPortDecl."},
        {"TIDY012", "tidy", Sev::Warning, false, "generate / endgenerate keywords or a genvar declared outside its loop", "slang-tidy check NoLegacyGenerate."},
        {"TIDY013", "tidy", Sev::Warning, false, "an always_comb whose begin has no label", "slang-tidy check AlwaysCombBlockNamed."},
        {"TIDY014", "tidy", Sev::Warning, false, "an enum declared without a typedef", "slang-tidy check TypedefEnums."},
        {"TIDY015", "tidy", Sev::Warning, false, "a struct or union declared without a typedef", "slang-tidy check TypedefStructUnion."},
        {"TIDY016", "tidy", Sev::Warning, false, "an enum type name does not match tidy.enum", "slang-tidy check EnumName."},
        {"TIDY017", "tidy", Sev::Warning, false, "a struct type name does not match tidy.struct", "slang-tidy check StructName."},
        {"TIDY018", "tidy", Sev::Warning, false, "a union type name does not match tidy.union", "slang-tidy check UnionName."},
        {"TIDY019", "tidy", Sev::Warning, false, "a covergroup name does not match tidy.covergroup", "slang-tidy check CovergroupName."},
        {"TIDY020", "tidy", Sev::Warning, false, "a coverpoint name does not match tidy.coverpoint", "slang-tidy check CoverpointName."},
        {"TIDY021", "tidy", Sev::Warning, false, "a cross name does not match tidy.cross", "slang-tidy check CrossName."},
#endif
        // ------------------------------------------------- source text and syntax
        // lintsrc.hpp. The rule set follows Verible's; the detail names its rule.
        {"FMT001", "format", Sev::Info, false, "a line longer than fmt.line_length", "Verible rule line-length."},
        {"FMT002", "format", Sev::Info, false, "a tab character", "Verible rule no-tabs."},
        {"FMT003", "format", Sev::Info, false, "trailing whitespace", "Verible rule no-trailing-spaces."},
        {"FMT004", "format", Sev::Info, false, "the file does not end with a newline", "Verible rule posix-eof."},
        {"SUS001", "suspicious", Sev::Info, false, "a stray semicolon (x = 1;; or endfunction;)", "Verible rule forbid-consecutive-null-statements."},
        {"SUS002", "suspicious", Sev::Warning, false, "void'() around randomize() or uvm_hdl_read()", "Verible rule void-cast."},
        {"SUS003", "suspicious", Sev::Info, false, "$psprintf, $random, $srandom or a $dist_ function",
         "Verible rule invalid-system-task-function."},
        {"SUS004", "suspicious", Sev::Info, false, "$test$plusargs: +NAME=0 still reads as given",
         "Verible rule plusarg-assignment: read the value with $value$plusargs."},
        {"W009", "width", Sev::Info, false, "a sized binary literal with fewer digits than its width (8'b101)", "Verible rule undersized-binary-literal."},
        // ------------------------------------------------------------------ formal
        // lintformal.hpp: answered by enumerating the values the logic can take.
        {"FML001", "formal", Sev::Warning, true, "an if condition or case item that can never be true where it stands",
         "Dead code: the enclosing conditions or the earlier branches rule the branch out, for every value of the "
         "signals involved. Proved by enumeration up to formal.max_bits free bits; else and default branches are "
         "not reported (defensive code)."},
        {"FML002", "formal", Sev::Warning, true, "an index or part select that can fall outside its array or vector",
         "Some value the index can take on a path that reaches the select is outside the declared range: the read "
         "gives X (or the default) and the write is lost. The message gives one such value."},
        {"FML003", "formal", Sev::Warning, true, "a register that never leaves its reset value",
         "Every assignment after reset gives it the value it already has: a constant, so the logic it feeds is "
         "fixed too."},
        // ---------------------------------------------------------------- soc flow
        // lintsoc.hpp: the top checked against block abstracts (-abstract).
        {"ABS001", "soc", Sev::Warning, true, "a block output the block never drives is connected at the top",
         "The block's abstract says nothing inside drives the output, so whatever reads it at the top floats."},
        {"ABS002", "soc", Sev::Info, true, "the top drives a block input the block never reads",
         "Logic at the top feeds an input the block ignores. Tie it off, or check the block uses it."},
        {"ABS003", "soc", Sev::Warning, true, "a block clock input is left open or tied to a constant",
         "The block clocks registers on this input."},
        {"ABS004", "soc", Sev::Warning, true, "a block was signed off with errors or warnings not waived",
         "From the counts in its abstract (-abstract-out at block level)."},
        // ---------------------------------------------------------------- soc flow
        // lintsoc.hpp: the top checked against block abstracts (-abstract).
        {"ABS001", "soc", Sev::Warning, true, "a block output the block never drives is connected at the top",
         "The block's abstract says nothing inside drives the output, so whatever reads it at the top floats."},
        {"ABS002", "soc", Sev::Info, true, "the top drives a block input the block never reads",
         "Logic at the top feeds an input the block ignores. Tie it off, or check the block uses it."},
        {"ABS003", "soc", Sev::Warning, true, "a block clock input is left open or tied to a constant",
         "The block clocks registers on this input."},
        {"ABS004", "soc", Sev::Warning, true, "a block was signed off with errors or warnings not waived",
         "From the counts in its abstract (-abstract-out at block level)."},
        // --------------------------------------------------------------------- fsm
        {"FSM001", "fsm", Sev::Warning, true, "a state that no transition and no reset leads to",
         "The state is named or tested, but the machine can never be in it: dead logic, or a transition that was forgotten."},
        {"FSM002", "fsm", Sev::Warning, true, "a state with no way out",
         "Once there the machine stays until reset. Fine for a final state that is meant; waive it then."},
        {"FSM003", "fsm", Sev::Warning, true, "the case on a state register has no default and does not use every encoding",
         "An encoding that is not a state (after a glitch or an upset) is held forever; add a default that goes to a safe state."},
        {"FSM004", "fsm", Sev::Warning, true, "a state register with no reset state",
         "The machine starts in an unknown state; assign a state in a reset branch."},

        // --------------------------------------------------------------------- cdc
        {"CDC001", "cdc", Sev::Warning, true, "a clock-domain crossing with no synchronizer, or only one flop", ""},
        {"CDC002", "cdc", Sev::Warning, true, "a multi-bit bus crossing through a synchronizer, not Gray coded", ""},
        {"CDC003", "cdc", Sev::Warning, true, "combinational logic in front of a synchronizer (a glitch can be captured)", ""},
        {"CDC004", "cdc", Sev::Warning, true, "separately synchronized signals of one domain meet again (reconvergence)", ""},
        {"CDC005", "cdc", Sev::Warning, true, "one signal synchronized more than once into the same domain", ""},
        {"CDC006", "cdc", Sev::Warning, true, "with -sdc: a flop's clock is not a clock in the SDC file", ""},
        {"CDC007", "cdc", Sev::Warning, true, "a clock read as data by logic", ""},
        {"CDC008", "cdc", Sev::Warning, true, "a clock made by combinational logic of a clock and other signals (glitch)", ""},
        {"CDC009", "cdc", Sev::Warning, true, "a one-cycle pulse from a faster clock into a slower one through a plain synchronizer (SDC periods)", ""},
        {"CDC010", "cdc", Sev::Warning, true, "a Gray-coded bus that a formal search finds changing more than one bit in one clock (-cdc-formal)", ""},
        {"RDC001", "rdc", Sev::Warning, true, "an asynchronous reset generated in another clock domain", ""},
        {"RDC002", "rdc", Sev::Warning, true, "one asynchronous reset used directly in several clock domains", ""},
        {"RDC003", "rdc", Sev::Warning, true, "a flop feeds a flop with a different asynchronous reset, in one clock domain", ""},
        {"RDC004", "rdc", Sev::Warning, false, "a reset flop feeds a flop with no asynchronous reset", ""},
        {"RDC005", "rdc", Sev::Warning, true, "a reset synchronizer of one flop", ""},
        {"RDC006", "rdc", Sev::Warning, true, "an asynchronous reset made by combinational logic of several flops", ""},
    };
    return t;
}

inline const RuleInfo* findRule(std::string_view id) {
    static const std::unordered_map<std::string_view, const RuleInfo*> ix = [] {
        std::unordered_map<std::string_view, const RuleInfo*> m;
        for (auto& r : table()) m[r.id] = &r;
        return m;
    }();
    auto it = ix.find(id);
    return it == ix.end() ? nullptr : it->second;
}

inline std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

inline std::string groupOf(std::string_view id) {
    if (auto* r = findRule(id)) return r->group;
    std::string g;
    for (char c : id)
        if (std::isalpha((unsigned char)c)) g += (char)std::tolower((unsigned char)c);
    return g;
}

// '*' any run, '?' one character.
inline bool glob(std::string_view pat, std::string_view s) {
    size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
            p++, i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// Words of a line; "..." keeps spaces, '#' ends the line.
inline std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
        if (i >= line.size() || line[i] == '#') break;
        std::string w;
        if (line[i] == '"') {
            i++;
            while (i < line.size() && line[i] != '"') w += line[i++];
            if (i < line.size()) i++;
        } else {
            while (i < line.size() && !std::isspace((unsigned char)line[i])) w += line[i++];
        }
        out.push_back(std::move(w));
    }
    return out;
}

inline std::vector<std::string> splitList(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || std::isspace((unsigned char)c)) {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

struct Waiver {
    std::vector<std::string> rules;  // ids, groups, or "*"
    std::string file, msg, reason;   // globs; empty matches all
    size_t line0 = 0, line1 = 0;     // 0: any line
    std::string origin;              // "waivers.cfg:12"
    mutable size_t hits = 0;
};

struct Config {
    // enable / disable / only, in the order given: the last word wins.
    struct Op {
        enum Kind { Enable, Disable, Only } kind;
        std::string what;  // rule, group, or "all"
    };
    std::vector<Op> ops;
    std::vector<std::pair<std::string, Sev>> sevs;
    std::map<std::string, std::string> params;
    std::vector<Waiver> waivers;
    std::vector<std::string> errors;

    static bool names(const std::string& what, std::string_view id) {
        if (what == "all" || what == "*") return true;
        if (what == id) return true;
        return lower(what) == groupOf(id);
    }

    bool enabled(std::string_view id) const {
        const RuleInfo* r = findRule(id);
        bool on = r ? r->on : true;
        for (const Op& op : ops) {
            bool hit = names(op.what, id);
            if (op.kind == Op::Only)
                on = false;  // an "only" list is a run of Only ops followed by Enable ops
            else if (hit)
                on = op.kind == Op::Enable;
        }
        return on;
    }

    Sev severity(std::string_view id) const {
        const RuleInfo* r = findRule(id);
        Sev s = r ? r->sev : Sev::Warning;
        for (auto& [what, sv] : sevs)
            if (names(what, id)) s = sv;
        return s;
    }

    std::string param(const std::string& key, const char* dflt) const {
        auto it = params.find(key);
        return it == params.end() ? std::string(dflt) : it->second;
    }

    void only(std::string_view list) {
        ops.push_back({Op::Only, ""});
        for (auto& w : splitList(list)) ops.push_back({Op::Enable, w});
    }

    bool known(const std::string& what) const {
        if (what == "all" || what == "*" || findRule(what)) return true;
        std::string g = lower(what);
        if (g == "upf" || g == "rdc") return true;
        for (auto& r : table())
            if (g == r.group) return true;
        // A rule of another layer (UPF001, a later CDC or RDC rule): the
        // letters of a known family, then digits.
        size_t i = 0;
        while (i < what.size() && std::isupper((unsigned char)what[i])) i++;
        if (i == 0 || i == what.size() || !std::all_of(what.begin() + (long)i, what.end(), [](char c) { return std::isdigit((unsigned char)c); }))
            return false;
        std::string fam = what.substr(0, i);
        if (fam == "UPF" || fam == "CDC" || fam == "RDC") return true;
        for (auto& r : table())
            if (std::string_view(r.id).substr(0, i) == fam && !std::isalpha((unsigned char)r.id[i])) return true;
        return false;
    }

    static bool parseSev(const std::string& s, Sev& out) {
        std::string l = lower(s);
        if (l == "error") out = Sev::Error;
        else if (l == "warning" || l == "warn") out = Sev::Warning;
        else if (l == "info" || l == "note") out = Sev::Info;
        else return false;
        return true;
    }

    // One directive. `origin` names the place for messages.
    void directive(const std::vector<std::string>& w, const std::string& origin) {
        if (w.empty()) return;
        auto bad = [&](const std::string& why) { errors.push_back(origin + ": " + why); };
        const std::string& cmd = w[0];
        if (cmd == "enable" || cmd == "disable") {
            if (w.size() < 2) return bad(cmd + " needs a rule or a group");
            for (size_t i = 1; i < w.size(); i++)
                for (auto& x : splitList(w[i])) {
                    if (!known(x)) bad("unknown rule or group '" + x + "'");
                    ops.push_back({cmd == "enable" ? Op::Enable : Op::Disable, x});
                }
        } else if (cmd == "only") {
            std::string all;
            for (size_t i = 1; i < w.size(); i++) all += w[i] + ",";
            for (auto& x : splitList(all))
                if (!known(x)) bad("unknown rule or group '" + x + "'");
            only(all);
        } else if (cmd == "severity") {
            Sev s;
            if (w.size() != 3 || !parseSev(w[2], s)) return bad("severity RULE error|warning|info");
            if (!known(w[1])) bad("unknown rule or group '" + w[1] + "'");
            sevs.push_back({w[1], s});
        } else if (cmd == "set") {
            if (w.size() != 3) return bad("set NAME VALUE");
            params[w[1]] = w[2];
        } else if (cmd == "waive") {
            if (w.size() < 2) return bad("waive RULE[,RULE] [-file GLOB] [-line N[-M]] [-msg GLOB] [-reason TEXT]");
            Waiver wv;
            wv.origin = origin;
            wv.rules = splitList(w[1]);
            for (auto& x : wv.rules)
                if (!known(x)) bad("unknown rule or group '" + x + "'");
            for (size_t i = 2; i < w.size(); i++) {
                if (i + 1 >= w.size()) return bad(w[i] + " needs a value");
                const std::string& k = w[i];
                const std::string& v = w[++i];
                if (k == "-file") wv.file = v;
                else if (k == "-msg") wv.msg = v;
                else if (k == "-reason" || k == "-comment") wv.reason = v;
                else if (k == "-line") {
                    size_t dash = v.find('-');
                    wv.line0 = (size_t)std::atoll(v.c_str());
                    wv.line1 = dash == std::string::npos ? wv.line0 : (size_t)std::atoll(v.c_str() + dash + 1);
                    if (!wv.line0 || wv.line1 < wv.line0) return bad("bad -line '" + v + "'");
                } else {
                    return bad("unknown waive option '" + k + "'");
                }
            }
            waivers.push_back(std::move(wv));
        } else {
            bad("unknown directive '" + cmd + "'");
        }
    }

    bool load(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            errors.push_back("cannot read '" + path + "'");
            return false;
        }
        std::string line;
        for (size_t n = 1; std::getline(in, line); n++) directive(words(line), path + ":" + std::to_string(n));
        return true;
    }
};

// ------------------------------------------------------- waivers in the source

// The "// mlint:" comments of one file, and the translate_off regions.
struct SourceMarks {
    struct LineWaive {
        std::vector<std::string> rules;
        std::string reason;
    };
    struct Region {
        size_t from, to;  // lines, inclusive
        std::vector<std::string> rules;  // empty: every rule
    };
    std::map<size_t, std::vector<LineWaive>> lines;
    std::vector<Region> regions;

    static bool ruleList(std::string_view& rest, std::vector<std::string>& rules) {
        // Leading words that look like rules (W001, naming, *), comma or space separated
        // only while they are followed by a comma.
        size_t i = 0;
        while (i < rest.size() && rest[i] == ' ') i++;
        size_t j = i;
        while (j < rest.size() && !std::isspace((unsigned char)rest[j])) j++;
        rules = splitList(rest.substr(i, j - i));
        rest = rest.substr(j);
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == ':' || rest[0] == '-')) rest.remove_prefix(1);
        return !rules.empty();
    }

    void scan(const std::string& path) {
        std::ifstream in(path);
        if (!in) return;
        std::string line;
        std::map<std::string, size_t> open;  // rule ("" = all, "synth") -> line it was turned off
        size_t n = 0;
        while (std::getline(in, line)) {
            n++;
            size_t c = line.find("//");
            if (c == std::string::npos) continue;
            std::string_view cm(line);
            cm.remove_prefix(c + 2);
            while (!cm.empty() && cm[0] == ' ') cm.remove_prefix(1);
            bool alone = line.find_first_not_of(" \t") == c;
            auto starts = [&](std::string_view p) {
                if (cm.substr(0, p.size()) != p) return false;
                cm.remove_prefix(p.size());
                while (!cm.empty() && cm[0] == ' ') cm.remove_prefix(1);
                return true;
            };
            if (starts("mlint:")) {
                if (starts("waive")) {
                    LineWaive w;
                    if (!ruleList(cm, w.rules)) continue;
                    w.reason = std::string(cm);
                    while (!w.reason.empty() && std::isspace((unsigned char)w.reason.back())) w.reason.pop_back();
                    lines[alone ? n + 1 : n].push_back(std::move(w));
                } else if (starts("off")) {
                    std::vector<std::string> rules;
                    ruleList(cm, rules);
                    if (rules.empty()) rules.push_back("");
                    for (auto& r : rules) open.emplace(r, n);
                } else if (starts("on")) {
                    std::vector<std::string> rules;
                    ruleList(cm, rules);
                    if (rules.empty())
                        for (auto& [r, from] : open) regions.push_back({from, n, r.empty() ? std::vector<std::string>{} : std::vector<std::string>{r}});
                    if (rules.empty()) open.clear();
                    for (auto& r : rules) {
                        auto it = open.find(r);
                        if (it == open.end()) continue;
                        regions.push_back({it->second, n, {r}});
                        open.erase(it);
                    }
                }
            } else if (starts("synthesis") || starts("synopsys") || starts("pragma")) {
                if (starts("translate_off")) open.emplace("synth", n);
                else if (starts("translate_on")) {
                    auto it = open.find("synth");
                    if (it != open.end()) {
                        regions.push_back({it->second, n, {"synth"}});
                        open.erase(it);
                    }
                }
            }
        }
        for (auto& [r, from] : open) regions.push_back({from, n + 1, r.empty() ? std::vector<std::string>{} : std::vector<std::string>{r}});
    }

    // Is this rule waived at this line? `reason` gets the comment's text.
    bool waived(std::string_view rule, size_t line, std::string& reason) const {
        auto it = lines.find(line);
        if (it != lines.end())
            for (auto& w : it->second)
                for (auto& r : w.rules)
                    if (Config::names(r, rule)) {
                        reason = w.reason.empty() ? "waived in the source" : w.reason;
                        return true;
                    }
        for (auto& g : regions) {
            if (line < g.from || line > g.to) continue;
            if (g.rules.empty()) {
                reason = "mlint: off";
                return true;
            }
            for (auto& r : g.rules)
                if (Config::names(r, rule)) {
                    reason = r == "synth" ? "translate_off region" : "mlint: off " + r;
                    return true;
                }
        }
        return false;
    }
};

inline std::string jsonStr(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

}  // namespace lintcfg
