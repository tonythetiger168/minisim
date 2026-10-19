// slang_dump.cpp - step 1 of the front end: parse + elaborate with slang,
// then walk the elaborated AST and list what the simulator must create.
// Written against recent slang (v6+/v7); header paths / names may need small
// tweaks for your version. Not compiled in the authoring environment.
//
// CMakeLists.txt:
//   cmake_minimum_required(VERSION 3.20)
//   project(slang_dump CXX)
//   set(CMAKE_CXX_STANDARD 20)
//   include(FetchContent)
//   FetchContent_Declare(slang GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
//                        GIT_TAG v7.0)
//   FetchContent_MakeAvailable(slang)
//   add_executable(slang_dump slang_dump.cpp)
//   target_link_libraries(slang_dump PRIVATE slang::slang)
//
// Test input counter.sv:
//   module counter(input logic clk, rst, output logic [3:0] cnt);
//     always_ff @(posedge clk) if (rst) cnt <= 0; else cnt <= cnt + 1;
//   endmodule
#include <iostream>
#include <memory>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/syntax/SyntaxTree.h"

using namespace slang;
using namespace slang::ast;
using namespace std;

// Visits every symbol in the elaborated design.
struct Dumper : ASTVisitor<Dumper, /*VisitStatements*/ false, /*VisitExpressions*/ false> {
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
    for (auto& d : comp.getAllDiagnostics()) engine.issue(d);
    string msgs = client->getString();
    if (!msgs.empty()) cerr << msgs;

    Dumper dumper;
    comp.getRoot().visit(dumper);
}
