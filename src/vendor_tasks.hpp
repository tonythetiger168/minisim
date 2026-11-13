#pragma once
// vendor_tasks.hpp - the waveform system tasks of the commercial simulators,
// known to slang so that a testbench written for them compiles:
//   Synopsys Verdi FSDB  $fsdbDumpfile, $fsdbDumpvars, $fsdbDumpon / off, ...
//   Synopsys VCS VPD     $vcdpluson / off, $vcdplusfile, $vcdplusflush, ...
//   Cadence SHM          $shm_open, $shm_probe, $shm_close, $recordvars, ...
// Each is a task with any arguments; an instance name as an argument stays
// the instance (as in $dumpvars). The lowering (lower.hpp lowerVendorDump)
// maps them onto msim's own dump (VCD or FST, 21.7). A name a VPI library
// registers itself (a real Verdi PLI) wins: declare() runs after VPI's.
#include <memory>
#include <string>
#include "slang/ast/Compilation.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/syntax/AllSyntax.h"

namespace vendor {

inline const std::vector<std::string>& waveTasks() {
    static const std::vector<std::string> v = {
        "$fsdbDumpfile", "$fsdbDumpvars", "$fsdbDumpon", "$fsdbDumpoff", "$fsdbDumpflush", "$fsdbDumpFinish",
        "$fsdbDumpMDA", "$fsdbDumpSVA", "$fsdbDumpvarsByFile", "$fsdbAutoSwitchDumpfile", "$fsdbSwitchDumpfile",
        "$fsdbDumpvarsES", "$fsdbDumpSingle", "$fsdbDumpvarsToFile", "$fsdbSuppress", "$fsdbLog", "$fsdbDumpMem",
        "$fsdbDumpClassMethod", "$fsdbDumpClassObject",
        "$vcdpluson", "$vcdplusoff", "$vcdplusfile", "$vcdplusflush", "$vcdplusclose", "$vcdplusmemon",
        "$vcdplusmemoff", "$vcdplusautoflushon", "$vcdplusautoflushoff", "$vcdplusdeltacycleon", "$vcdplusglitchon",
        "$shm_open", "$shm_probe", "$shm_close", "$shm_save", "$recordvars", "$recordfile", "$recordon", "$recordoff",
        "$recordclose"};
    return v;
}

class WaveTask : public slang::ast::SystemSubroutine {
public:
    explicit WaveTask(const std::string& n) : SystemSubroutine(n, slang::ast::SubroutineKind::Task) {}
    bool allowEmptyArgument(size_t) const final { return true; }
    const slang::ast::Expression& bindArgument(size_t argIndex, const slang::ast::ASTContext& context,
                                               const slang::syntax::ExpressionSyntax& syntax, const Args& args) const final {
        using namespace slang::ast;
        // A name is the instance, module or signal it names, as in $dumpvars.
        if (slang::syntax::NameSyntax::isKind(syntax.kind))
            return ArbitrarySymbolExpression::fromSyntax(context.getCompilation(), syntax.as<slang::syntax::NameSyntax>(), context);
        return SystemSubroutine::bindArgument(argIndex, context, syntax, args);
    }
    const slang::ast::Type& checkArguments(const slang::ast::ASTContext& context, const Args&, slang::SourceRange,
                                           const slang::ast::Expression*) const final {
        return context.getCompilation().getVoidType();
    }
    slang::ConstantValue eval(slang::ast::EvalContext& context, const Args&, slang::SourceRange range,
                              const slang::ast::CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }
};

// Transaction recording (expr.hpp trrec, lower.hpp trCall): Questa's API
// and Verdi's FSDB transaction tasks. The three that make a stream or a
// transaction are functions giving its handle (an integer); the rest are
// tasks. Arguments are ordinary expressions.
inline const std::vector<std::string>& trFunctions() {
    static const std::vector<std::string> v = {"$create_transaction_stream", "$begin_transaction", "$fsdbTrans_begin"};
    return v;
}
inline const std::vector<std::string>& trTasks() {
    static const std::vector<std::string> v = {"$add_attribute", "$end_transaction", "$free_transaction", "$add_relation",
                                               "$add_color", "$fsdbTrans_add_attribute", "$fsdbTrans_set_label",
                                               "$fsdbTrans_end", "$fsdbTrans_add_relation", "$fsdbTrans_add_tag"};
    return v;
}

class TrCall : public slang::ast::SystemSubroutine {
public:
    TrCall(const std::string& n, bool fn)
        : SystemSubroutine(n, fn ? slang::ast::SubroutineKind::Function : slang::ast::SubroutineKind::Task), fn(fn) {}
    bool allowEmptyArgument(size_t) const final { return true; }
    const slang::ast::Type& checkArguments(const slang::ast::ASTContext& context, const Args&, slang::SourceRange,
                                           const slang::ast::Expression*) const final {
        return fn ? context.getCompilation().getIntegerType() : context.getCompilation().getVoidType();
    }
    slang::ConstantValue eval(slang::ast::EvalContext& context, const Args&, slang::SourceRange range,
                              const slang::ast::CallExpression::SystemCallInfo&) const final {
        notConst(context, range);
        return nullptr;
    }

private:
    bool fn;
};

inline void declare(slang::ast::Compilation& comp) {
    for (const std::string& n : waveTasks())
        if (!comp.getSystemSubroutine(n)) comp.addSystemSubroutine(std::make_shared<WaveTask>(n));
    for (const std::string& n : trFunctions())
        if (!comp.getSystemSubroutine(n)) comp.addSystemSubroutine(std::make_shared<TrCall>(n, true));
    for (const std::string& n : trTasks())
        if (!comp.getSystemSubroutine(n)) comp.addSystemSubroutine(std::make_shared<TrCall>(n, false));
}

}   // namespace vendor
