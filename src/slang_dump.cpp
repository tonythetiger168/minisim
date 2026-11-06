// slang_dump.cpp - parse and elaborate with slang (v12.1), lower the design
// (including module instances), then list the signals and processes.
#include <iostream>
#include <memory>
#include <stdexcept>

#include "lower.hpp"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/syntax/SyntaxTree.h"

using namespace slang;
using namespace slang::ast;
using namespace std;

// Visits every symbol in the elaborated design.
struct Dumper : ASTVisitor<Dumper> {
    void handle(const InstanceBodySymbol& body) {
        cout << "module " << body.name << "\n";
        visitDefault(body);  // recurse into members
    }
    // Registers / variables -> become Sim::sig(name, width)
    void handle(const VariableSymbol& v) {
        cout << "  var  " << v.name << " : " << v.getType().toString()
             << " (" << v.getType().getBitWidth() << " bits)\n";
    }
    // Wires -> also signals
    void handle(const NetSymbol& n) {
        cout << "  net  " << n.name << " : " << n.getType().toString() << "\n";
    }
    // always / always_ff / initial -> become coroutine Tasks
    void handle(const ProceduralBlockSymbol& p) {
        cout << "  proc " << toString(p.procedureKind) << "\n";
        // Next step: walk p.getBody() (a Statement tree) and lower it:
        //   TimedStatement -> co_await Delay / Wait
        //   ConditionalStatement -> if
        //   ExpressionStatement w/ AssignmentExpression -> set() or nba()
    }
    // assign -> process sensitive to every input of the RHS
    void handle(const ContinuousAssignSymbol&) { cout << "  assign\n"; }
};

int main(int argc, char** argv) {
    if (argc < 2) { cerr << "usage: slang_dump file.sv\n"; return 1; }

    auto tree = syntax::SyntaxTree::fromFile(argv[1]);
    if (!tree) { cerr << "cannot read " << argv[1] << "\n"; return 1; }

    Compilation comp;
    comp.addSyntaxTree(tree.value());

    // Print any parse / elaboration errors.
    DiagnosticEngine engine(*comp.getSourceManager());
    auto client = make_shared<TextDiagnosticClient>();
    engine.addClient(client);
    bool errors = false;
    for (auto& d : comp.getAllDiagnostics()) {
        engine.issue(d);
        if (d.isError()) errors = true;
    }
    string msgs = client->getString();
    if (!msgs.empty()) cerr << msgs;
    if (errors) return 1;

    try {
        int n = 0;
        lower(comp, [&](const std::string&, int, bool) { return n++; }, [](int) { return 0; });
    } catch (const std::exception& ex) {
        cerr << ex.what() << "\n";
        return 1;
    }

    Dumper dumper;
    comp.getRoot().visit(dumper);
    return 0;
}
