// slang-tidy's checks as mlint rules.
//
// slang-tidy is the lint tool that ships with slang, our front end
// (tools/tidy in the slang tree, MIT license, see THIRD_PARTY_NOTICES.md).
// Its check sources are compiled into mlint unchanged (cmake/tools/
// mlint_tidy.cmake); this file runs them and turns what they report into
// mlint findings, so the rule table, severities, waivers, -json and
// -threads apply to them like to any other rule.
//
// Only the checks mlint has no rule of its own for are listed: NoCaseX is
// CASE003, AlwaysFFBlocking is L002, NoLatchesOnDesign is L001, and so on; and
// NoImplicitPortNameInPortConnection says what NoDotVarInPortConnection says,
// per module instead of per connection.
// One finding has one owner.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "TidyConfig.h"
#include "TidyFactory.h"
#include "lintcfg.hpp"
#include "lintir.hpp"
#include "parallel.hpp"
#include "slang/analysis/AnalysisManager.h"
#include "slang/diagnostics/DiagnosticEngine.h"

namespace linttidy {
using namespace lintir;

struct Map {
    const char* rule;    // the mlint rule
    const char* check;   // the slang-tidy check
};

// Fixed numbers: a check slang adds later gets the next number, none moves.
inline const std::vector<Map>& table() {
    static const std::vector<Map> t = {
        {"TIDY001", "OnlyAssignedOnReset"},
        {"TIDY002", "AlwaysFFAssignmentOutsideConditional"},
        {"TIDY003", "LoopBeforeResetCheck"},
        {"TIDY004", "UndrivenRange"},
        {"TIDY005", "StructFieldsInSameProcess"},
        {"TIDY006", "CastSignedIndex"},
        {"TIDY007", "UnusedSensitiveSignal"},
        {"TIDY008", "XilinxDoNotCareValues"},
        {"TIDY009", "NoDotStarInPortConnection"},
        {"TIDY010", "NoDotVarInPortConnection"},
        {"TIDY011", "OnlyANSIPortDecl"},
        {"TIDY012", "NoLegacyGenerate"},
        {"TIDY013", "AlwaysCombBlockNamed"},
        {"TIDY014", "TypedefEnums"},
        {"TIDY015", "TypedefStructUnion"},
        {"TIDY016", "EnumName"},
        {"TIDY017", "StructName"},
        {"TIDY018", "UnionName"},
        {"TIDY019", "CovergroupName"},
        {"TIDY020", "CoverpointName"},
        {"TIDY021", "CrossName"},
    };
    return t;
}

// The settings slang-tidy's checks read, under mlint's names (-set tidy.X=V).
inline const std::vector<const char*>& settings() {
    static const std::vector<const char*> k = {"tidy.clock",      "tidy.reset",      "tidy.reset_active_high",
                                               "tidy.enum",       "tidy.struct",     "tidy.union",
                                               "tidy.covergroup", "tidy.coverpoint", "tidy.cross"};
    return k;
}

inline bool anyEnabled(const Rules& rules) {
    for (auto& m : table())
        if (rules.enabled(m.rule)) return true;
    return false;
}

inline void configure(const lintcfg::Config& cfg, std::vector<std::string>& errors) {
    TidyConfig tc;
    auto& c = tc.getCheckConfigs();
    c.clkName = cfg.param("tidy.clock", "clk_i");
    c.resetName = cfg.param("tidy.reset", "rst_ni");
    std::string hi = cfg.param("tidy.reset_active_high", "false");
    c.resetIsActiveHigh = hi == "true" || hi == "1" || hi == "yes";
    auto pat = [&](const char* key, std::string& src, boost::regex& re) {
        std::string v = cfg.param(key, src.c_str());
        try {
            re = boost::regex(v);
            src = v;
        } catch (const std::exception&) {
            errors.push_back(std::string("set ") + key + ": '" + v + "' is not a regular expression");
        }
    };
    pat("tidy.enum", c.enumRegexString, c.enumRegexPattern);
    pat("tidy.struct", c.structRegexString, c.structRegexPattern);
    pat("tidy.union", c.unionRegexString, c.unionRegexPattern);
    pat("tidy.covergroup", c.covergroupRegexString, c.covergroupRegexPattern);
    pat("tidy.coverpoint", c.coverpointRegexString, c.coverpointRegexPattern);
    pat("tidy.cross", c.crossRegexString, c.crossRegexPattern);
    Registry::setConfig(tc);
}

// Runs every enabled check, each on its own thread slot (a check keeps its
// findings to itself), then reports in table order on this thread.
inline void run(Compilation& comp, const analysis::AnalysisManager& am, DiagnosticEngine& engine, Report& rep,
                unsigned threads) {
    Registry::setSourceManager(comp.getSourceManager());
    std::vector<const Map*> todo;
    std::vector<std::unique_ptr<TidyCheck>> checks;
    auto known = Registry::getRegisteredChecks();
    for (auto& m : table()) {
        if (!rep.rules.enabled(m.rule)) continue;
        if (std::find(known.begin(), known.end(), m.check) == known.end()) continue;   // not in this slang
        todo.push_back(&m);
        checks.push_back(Registry::create(m.check));
    }
    const RootSymbol& root = comp.getRoot();
    par::parallelFor(checks.size(), threads, [&](size_t i) { (void)checks[i]->check(root, am); });
    for (size_t i = 0; i < checks.size(); i++) {
        // By the code of the diagnostic, not of the check: some checks report
        // under a code they share with another.
        for (const Diagnostic& d : checks[i]->getDiagnostics()) {
            engine.setMessage(d.code, checks[i]->diagString());
            rep.add(todo[i]->rule, d.location, engine.formatMessage(d));
        }
    }
}

}  // namespace linttidy
