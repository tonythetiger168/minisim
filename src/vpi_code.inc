// Included by vpi.inc inside namespace vpi.
// ---- processes, continuous assignments, statements and expressions ----
// IEEE 1800-2023 37.32 (processes), 37.33 (assignment), 37.35 (expressions),
// 37.36 to 37.51 (statements, timing controls, case items), 37.40
// (continuous assignments). Objects come from the elaborated slang AST: a
// statement or expression object stands for one node, and knows the object
// it was reached from (vpiParent).

inline Obj* codeObj(Obj::Kind k, int type, const void* node, const Obj* parent) {
    auto& slot = st().codeObjs[{node, (int)k}];
    if (!slot) {
        slot = mkObj(k, type);
        slot->node = node;
        slot->parent = parent;
    }
    return slot;
}
inline int lineOf(slang::SourceRange r) {
    if (!srcMgr() || !r.start().valid()) return 0;
    return (int)srcMgr()->getLineNumber(srcMgr()->getFullyOriginalLoc(r.start()));
}
inline std::string fileOf(slang::SourceRange r) {
    if (!srcMgr() || !r.start().valid()) return "";
    return std::string(srcMgr()->getFileName(srcMgr()->getFullyOriginalLoc(r.start())));
}
inline std::string textOf(slang::SourceRange r) {
    if (!srcMgr() || !r.start().valid() || !r.end().valid()) return "";
    auto a = srcMgr()->getFullyOriginalLoc(r.start()), b = srcMgr()->getFullyOriginalLoc(r.end());
    if (a.buffer() != b.buffer() || b.offset() < a.offset()) return "";
    std::string_view t = srcMgr()->getSourceText(a.buffer());
    return std::string(t.substr(a.offset(), b.offset() - a.offset()));
}
inline const Expression* stripConv(const Expression* e) {
    while (e && e->kind == ExpressionKind::Conversion && e->as<ConversionExpression>().isImplicit())
        e = &e->as<ConversionExpression>().operand();
    return e;
}
inline int unaryOp(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::Plus: return vpiPlusOp;
        case UnaryOperator::Minus: return vpiMinusOp;
        case UnaryOperator::BitwiseNot: return vpiBitNegOp;
        case UnaryOperator::BitwiseAnd: return vpiUnaryAndOp;
        case UnaryOperator::BitwiseOr: return vpiUnaryOrOp;
        case UnaryOperator::BitwiseXor: return vpiUnaryXorOp;
        case UnaryOperator::BitwiseNand: return vpiUnaryNandOp;
        case UnaryOperator::BitwiseNor: return vpiUnaryNorOp;
        case UnaryOperator::BitwiseXnor: return vpiUnaryXNorOp;
        case UnaryOperator::LogicalNot: return vpiNotOp;
        case UnaryOperator::Preincrement: return vpiPreIncOp;
        case UnaryOperator::Predecrement: return vpiPreDecOp;
        case UnaryOperator::Postincrement: return vpiPostIncOp;
        case UnaryOperator::Postdecrement: return vpiPostDecOp;
    }
    return 0;
}
inline int binaryOp(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::Add: return vpiAddOp;
        case BinaryOperator::Subtract: return vpiSubOp;
        case BinaryOperator::Multiply: return vpiMultOp;
        case BinaryOperator::Divide: return vpiDivOp;
        case BinaryOperator::Mod: return vpiModOp;
        case BinaryOperator::BinaryAnd: return vpiBitAndOp;
        case BinaryOperator::BinaryOr: return vpiBitOrOp;
        case BinaryOperator::BinaryXor: return vpiBitXorOp;
        case BinaryOperator::BinaryXnor: return vpiBitXNorOp;
        case BinaryOperator::Equality: return vpiEqOp;
        case BinaryOperator::Inequality: return vpiNeqOp;
        case BinaryOperator::CaseEquality: return vpiCaseEqOp;
        case BinaryOperator::CaseInequality: return vpiCaseNeqOp;
        case BinaryOperator::GreaterThanEqual: return vpiGeOp;
        case BinaryOperator::GreaterThan: return vpiGtOp;
        case BinaryOperator::LessThanEqual: return vpiLeOp;
        case BinaryOperator::LessThan: return vpiLtOp;
        case BinaryOperator::WildcardEquality: return vpiWildEqOp;
        case BinaryOperator::WildcardInequality: return vpiWildNeqOp;
        case BinaryOperator::LogicalAnd: return vpiLogAndOp;
        case BinaryOperator::LogicalOr: return vpiLogOrOp;
        case BinaryOperator::LogicalImplication: return vpiImpliesOp;
        case BinaryOperator::LogicalEquivalence: return vpiEqOp;   // a <-> b: no operator of its own in annex K
        case BinaryOperator::LogicalShiftLeft: return vpiLShiftOp;
        case BinaryOperator::LogicalShiftRight: return vpiRShiftOp;
        case BinaryOperator::ArithmeticShiftLeft: return vpiArithLShiftOp;
        case BinaryOperator::ArithmeticShiftRight: return vpiArithRShiftOp;
        case BinaryOperator::Power: return vpiPowerOp;
    }
    return 0;
}
// The operator of an expression that is a vpiOperation, else 0.
inline int opTypeOf(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::UnaryOp: return unaryOp(e.as<UnaryExpression>().op);
        case ExpressionKind::BinaryOp: return binaryOp(e.as<BinaryExpression>().op);
        case ExpressionKind::ConditionalOp: return vpiConditionOp;
        case ExpressionKind::Inside: return vpiInsideOp;
        case ExpressionKind::Concatenation: return vpiConcatOp;
        case ExpressionKind::Replication: return vpiMultiConcatOp;
        case ExpressionKind::Streaming:
            return e.as<StreamingConcatenationExpression>().isFixedSize() || e.as<StreamingConcatenationExpression>().getSliceSize() ? vpiStreamRLOp : vpiStreamLROp;
        case ExpressionKind::Conversion: return vpiCastOp;
        case ExpressionKind::Assignment: return vpiAssignmentOp;
        case ExpressionKind::MinTypMax: return vpiMinTypMaxOp;
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern: return vpiAssignmentPatternOp;
        case ExpressionKind::ReplicatedAssignmentPattern: return vpiMultiAssignmentPatternOp;
        default: return 0;
    }
}
// Operands of an operation, in source order.
inline std::vector<const Expression*> operandsOf(const Expression& e) {
    std::vector<const Expression*> r;
    switch (e.kind) {
        case ExpressionKind::UnaryOp: r.push_back(&e.as<UnaryExpression>().operand()); break;
        case ExpressionKind::BinaryOp:
            r.push_back(&e.as<BinaryExpression>().left());
            r.push_back(&e.as<BinaryExpression>().right());
            break;
        case ExpressionKind::ConditionalOp: {
            const auto& c = e.as<ConditionalExpression>();
            for (auto& cond : c.conditions) r.push_back(cond.expr);
            r.push_back(&c.left());
            r.push_back(&c.right());
            break;
        }
        case ExpressionKind::Inside:
            r.push_back(&e.as<InsideExpression>().left());
            for (auto* x : e.as<InsideExpression>().rangeList()) r.push_back(x);
            break;
        case ExpressionKind::Concatenation:
            for (auto* x : e.as<ConcatenationExpression>().operands()) r.push_back(x);
            break;
        case ExpressionKind::Replication:
            r.push_back(&e.as<ReplicationExpression>().count());
            r.push_back(&e.as<ReplicationExpression>().concat());
            break;
        case ExpressionKind::Streaming:
            for (auto& s : e.as<StreamingConcatenationExpression>().streams()) r.push_back(s.operand);
            break;
        case ExpressionKind::Conversion: r.push_back(&e.as<ConversionExpression>().operand()); break;
        case ExpressionKind::Assignment:
            r.push_back(&e.as<AssignmentExpression>().left());
            r.push_back(&e.as<AssignmentExpression>().right());
            break;
        case ExpressionKind::MinTypMax:
            r.push_back(&e.as<MinTypMaxExpression>().min());
            r.push_back(&e.as<MinTypMaxExpression>().typ());
            r.push_back(&e.as<MinTypMaxExpression>().max());
            break;
        case ExpressionKind::SimpleAssignmentPattern:
        case ExpressionKind::StructuredAssignmentPattern:
        case ExpressionKind::ReplicatedAssignmentPattern:
            for (auto* x : static_cast<const AssignmentPatternExpressionBase&>(e).elements()) r.push_back(x);
            break;
        default: break;
    }
    return r;
}
inline int constTypeOf(const Expression& e) {
    switch (e.kind) {
        case ExpressionKind::RealLiteral: return vpiRealConst;
        case ExpressionKind::TimeLiteral: return vpiTimeConst;
        case ExpressionKind::StringLiteral: return vpiStringConst;
        case ExpressionKind::UnbasedUnsizedIntegerLiteral: return vpiBinaryConst;
        case ExpressionKind::IntegerLiteral: {
            std::string t = textOf(e.sourceRange);
            auto q = t.find('\'');
            if (q == std::string::npos) return vpiDecConst;
            size_t k = q + 1;
            if (k < t.size() && (t[k] == 's' || t[k] == 'S')) k++;
            char b = k < t.size() ? (char)std::tolower((unsigned char)t[k]) : 'd';
            return b == 'b' ? vpiBinaryConst : b == 'o' ? vpiOctConst : b == 'h' ? vpiHexConst : vpiDecConst;
        }
        default: return 0;
    }
}
// The object for an expression: a net, variable, word, bit or parameter
// it names (an object of its own), else a vpiOperation, vpiConstant,
// vpiFuncCall / vpiSysFuncCall, vpiBitSelect or vpiPartSelect.
inline Obj* exprNode(const Expression* e, const Obj* parent) {
    e = stripConv(e);
    if (!e) return nullptr;
    if (e->kind == ExpressionKind::NamedValue || e->kind == ExpressionKind::HierarchicalValue) {
        const Symbol& sym = e->as<ValueExpressionBase>().symbol;
        if (sym.kind == SymbolKind::Parameter) return paramObj(&sym);
        if (Obj* v = valueObj(&sym)) return v;
    }
    if (e->kind == ExpressionKind::ElementSelect || e->kind == ExpressionKind::RangeSelect)
        if (Obj* v = exprObj(e)) return v;   // constant indices: the word, bit or part itself
    int type = 0;
    switch (e->kind) {
        case ExpressionKind::IntegerLiteral:
        case ExpressionKind::RealLiteral:
        case ExpressionKind::TimeLiteral:
        case ExpressionKind::StringLiteral:
        case ExpressionKind::UnbasedUnsizedIntegerLiteral: type = vpiConstant; break;
        case ExpressionKind::Call: {
            const auto& c = e->as<CallExpression>();
            type = c.isSystemCall() ? vpiSysFuncCall : c.thisClass() ? vpiMethodFuncCall : vpiFuncCall;
            break;
        }
        case ExpressionKind::ElementSelect: type = vpiBitSelect; break;
        case ExpressionKind::RangeSelect:
            type = e->as<RangeSelectExpression>().getSelectionKind() == RangeSelectionKind::Simple ? vpiPartSelect : vpiIndexedPartSelect;
            break;
        default:
            if (opTypeOf(*e)) type = vpiOperation;
            else if (e->kind == ExpressionKind::NamedValue || e->kind == ExpressionKind::HierarchicalValue) type = vpiRefObj;
            break;
    }
    if (!type) return nullptr;
    return codeObj(Obj::Expr, type, e, parent);
}

// ---- statements ----
inline int timedType(const TimingControl& t) {
    switch (t.kind) {
        case TimingControlKind::Delay:
        case TimingControlKind::Delay3:
        case TimingControlKind::OneStepDelay:
        case TimingControlKind::CycleDelay: return vpiDelayControl;
        case TimingControlKind::RepeatedEvent: return vpiRepeatControl;
        default: return vpiEventControl;
    }
}
inline int stmtType(const Statement& s) {
    switch (s.kind) {
        case StatementKind::List: return vpiBegin;   // an unnamed begin-end of several statements
        case StatementKind::Block: {
            const auto& b = s.as<BlockStatement>();
            bool named = b.blockSymbol && !b.blockSymbol->name.empty();
            if (b.blockKind == StatementBlockKind::Sequential) return named ? vpiNamedBegin : vpiBegin;
            return named ? vpiNamedFork : vpiFork;
        }
        case StatementKind::ExpressionStatement: {
            const Expression& e = *stripConv(&s.as<ExpressionStatement>().expr);
            if (e.kind == ExpressionKind::Call) {
                const auto& c = e.as<CallExpression>();
                return c.isSystemCall() ? vpiSysTaskCall : c.thisClass() ? vpiMethodTaskCall : vpiTaskCall;
            }
            return vpiAssignment;   // an assignment, or ++ / --
        }
        case StatementKind::Return: return vpiReturn;
        case StatementKind::Continue: return vpiContinue;
        case StatementKind::Break: return vpiBreak;
        case StatementKind::Disable: return vpiDisable;
        case StatementKind::Conditional: return s.as<ConditionalStatement>().ifFalse ? vpiIfElse : vpiIf;
        case StatementKind::Case:
        case StatementKind::PatternCase: return vpiCase;
        case StatementKind::ForLoop: return vpiFor;
        case StatementKind::RepeatLoop: return vpiRepeat;
        case StatementKind::ForeachLoop: return vpiForeachStmt;
        case StatementKind::WhileLoop: return vpiWhile;
        case StatementKind::DoWhileLoop: return vpiDoWhile;
        case StatementKind::ForeverLoop: return vpiForever;
        case StatementKind::Timed: return timedType(s.as<TimedStatement>().timing);
        case StatementKind::DisableFork: return vpiDisableFork;
        case StatementKind::Wait: return vpiWait;
        case StatementKind::WaitFork: return vpiWaitFork;
        case StatementKind::WaitOrder: return vpiOrderedWait;
        case StatementKind::EventTrigger: return vpiEventStmt;
        case StatementKind::ProceduralAssign: return s.as<ProceduralAssignStatement>().isForce ? vpiForce : vpiAssignStmt;
        case StatementKind::ProceduralDeassign: return s.as<ProceduralDeassignStatement>().isRelease ? vpiRelease : vpiDeassign;
        case StatementKind::Empty: return vpiNullStmt;
        default: return 0;
    }
}
// The statements of a block, in order (declarations left out).
inline void blockItems(const Statement& s, std::vector<const Statement*>& out) {
    if (s.kind == StatementKind::List) {
        for (auto* x : s.as<StatementList>().list) blockItems(*x, out);
        return;
    }
    if (s.kind == StatementKind::VariableDeclaration || s.kind == StatementKind::Empty) return;
    out.push_back(&s);
}
// The object for a statement. An assertion statement is the assertion
// object of clause 39; a declaration is not a statement.
inline Obj* stmtNode(const Statement* s, const Obj* parent) {
    if (!s) return nullptr;
    if (s->kind == StatementKind::ImmediateAssertion || s->kind == StatementKind::ConcurrentAssertion) {
        for (size_t k = 0; k < asserts().size(); k++)
            if (asserts()[k].stmt == s && !asserts()[k].hidden) return assertObj((int)k);
        return nullptr;
    }
    // A one-statement list is that statement; so is the block slang makes
    // around a for loop that declares its variables.
    for (;;) {
        if (s->kind == StatementKind::List && s->as<StatementList>().list.size() == 1) { s = s->as<StatementList>().list[0]; continue; }
        if (s->kind == StatementKind::Block) {
            const auto& b = s->as<BlockStatement>();
            std::vector<const Statement*> in;
            blockItems(b.body, in);
            if (b.blockKind == StatementBlockKind::Sequential && b.blockSymbol && b.blockSymbol->name.empty() && in.size() == 1 &&
                in[0]->kind == StatementKind::ForLoop && in[0]->sourceRange.start() == s->sourceRange.start()) {
                s = in[0];
                continue;
            }
        }
        break;
    }
    int type = stmtType(*s);
    if (!type) return nullptr;
    return codeObj(Obj::Stmt, type, s, parent);
}
// A named block's scope (37.3) and its statement object share the name.
inline const StatementBlockSymbol* blockSym(const Obj* o) {
    if (o->kind != Obj::Stmt) return nullptr;
    const auto* s = static_cast<const Statement*>(o->node);
    return s->kind == StatementKind::Block ? s->as<BlockStatement>().blockSymbol : nullptr;
}
// The process or continuous assignment a statement or expression belongs to.
inline const Obj* ownerOf(const Obj* o) {
    while (o && (o->kind == Obj::Stmt || o->kind == Obj::Expr || o->kind == Obj::CaseItem || o->kind == Obj::ExprStmt)) o = o->parent;
    return o;
}
inline Obj* processObj(const ProceduralBlockSymbol& p) {
    auto& slot = st().symObj[&p];
    if (!slot) {
        int t = p.procedureKind == ProceduralBlockKind::Initial ? vpiInitial
              : p.procedureKind == ProceduralBlockKind::Final ? vpiFinal : vpiAlways;
        slot = mkObj(Obj::Process, t);
        slot->sym = &p;
    }
    return slot;
}
inline Obj* contAssignObj(const ContinuousAssignSymbol& c) {
    auto& slot = st().symObj[&c];
    if (!slot) {
        slot = mkObj(Obj::ContAssign, vpiContAssign);
        slot->sym = &c;
    }
    return slot;
}
inline const Symbol* codeSym(const Obj* o) {
    const Obj* w = ownerOf(o);
    return w ? w->sym : nullptr;
}
inline bool isCode(const Obj* o) {
    return o->kind == Obj::Process || o->kind == Obj::ContAssign || o->kind == Obj::Stmt || o->kind == Obj::Expr ||
           o->kind == Obj::CaseItem || o->kind == Obj::ExprStmt;
}
inline slang::SourceRange rangeOfCode(const Obj* o) {
    switch (o->kind) {
        case Obj::Stmt: return static_cast<const Statement*>(o->node)->sourceRange;
        case Obj::Expr: return static_cast<const Expression*>(o->node)->sourceRange;
        case Obj::ExprStmt:
            if (o->idx == 1) {
                const auto* v = static_cast<const VariableSymbol*>(o->node);
                return {v->location, v->getInitializer() ? v->getInitializer()->sourceRange.end() : v->location};
            }
            return static_cast<const Expression*>(o->node)->sourceRange;
        case Obj::CaseItem: {
            const auto* it = static_cast<const CaseStatement::ItemGroup*>(o->node);
            if (o->idx < 0) return static_cast<const Statement*>(o->node)->sourceRange;   // default
            return it->expressions.empty() ? it->stmt->sourceRange : it->expressions[0]->sourceRange;
        }
        default: return {o->sym->location, o->sym->location};
    }
}

// vpi_handle from a code object (37.32 to 37.51).
inline vpiHandle codeHandle(int type, const Obj* o) {
    using EK = ExpressionKind;
    if (type == vpiModule || type == vpiInstance) {
        const Symbol* s = codeSym(o);
        return s ? H(moduleObj(instOf(s->getParentScope()))) : nullptr;
    }
    if (type == vpiScope) {
        if (const StatementBlockSymbol* b = blockSym(o); b && !b->name.empty()) return H(scopeObj(*b));
        const Symbol* s = codeSym(o);
        return s ? H(scopeOfSym(*s)) : nullptr;
    }
    bool sel = o->kind == Obj::Expr && (o->type == vpiBitSelect || o->type == vpiPartSelect || o->type == vpiIndexedPartSelect);
    if (type == vpiParent && !sel) return H(const_cast<Obj*>(o->parent));
    if (o->kind == Obj::Process) {
        const auto& p = o->sym->as<ProceduralBlockSymbol>();
        return type == vpiStmt ? H(stmtNode(&p.getBody(), o)) : nullptr;
    }
    if (o->kind == Obj::ContAssign && o->sym->kind == SymbolKind::Net) {   // wire w = expr; (vpiNetDeclAssign)
        const auto& n = o->sym->as<NetSymbol>();
        if (type == vpiLhs) return H(valueObj(&n));
        if (type == vpiRhs) return H(exprNode(n.getInitializer(), o));
        if (type == vpiDelay) {
            const TimingControl* t = n.getDelay();
            if (t && t->kind == TimingControlKind::Delay) return H(exprNode(&t->as<DelayControl>().expr, o));
            if (t && t->kind == TimingControlKind::Delay3) return H(exprNode(&t->as<Delay3Control>().expr1, o));
        }
        return nullptr;
    }
    if (o->kind == Obj::ContAssign) {
        const Expression& a = *stripConv(&o->sym->as<ContinuousAssignSymbol>().getAssignment());
        if (a.kind != EK::Assignment) return nullptr;
        if (type == vpiLhs) return H(exprNode(&a.as<AssignmentExpression>().left(), o));
        if (type == vpiRhs) return H(exprNode(&a.as<AssignmentExpression>().right(), o));
        if (type == vpiDelay) {
            const TimingControl* t = o->sym->as<ContinuousAssignSymbol>().getDelay();
            if (t && t->kind == TimingControlKind::Delay) return H(exprNode(&t->as<DelayControl>().expr, o));
            if (t && t->kind == TimingControlKind::Delay3) return H(exprNode(&t->as<Delay3Control>().expr1, o));
        }
        return nullptr;
    }
    if (o->kind == Obj::ExprStmt && o->idx == 1) {   // for (int k = 0; ...): k = 0
        const auto& v = *static_cast<const VariableSymbol*>(o->node);
        if (type == vpiLhs) return H(valueObj(&v));
        if (type == vpiRhs) return H(exprNode(v.getInitializer(), o));
        return nullptr;
    }
    if (o->kind == Obj::ExprStmt) {   // a for loop's initialization or step: an assignment
        const Expression& e = *stripConv(static_cast<const Expression*>(o->node));
        if (e.kind == EK::Assignment) {
            if (type == vpiLhs) return H(exprNode(&e.as<AssignmentExpression>().left(), o));
            if (type == vpiRhs) return H(exprNode(&e.as<AssignmentExpression>().right(), o));
        } else if (e.kind == EK::UnaryOp) {
            if (type == vpiLhs) return H(exprNode(&e.as<UnaryExpression>().operand(), o));
            if (type == vpiRhs) return H(exprNode(&e, o));
        }
        return nullptr;
    }
    if (o->kind == Obj::CaseItem) {
        if (type != vpiStmt) return nullptr;
        if (o->idx < 0) return H(stmtNode(static_cast<const Statement*>(o->node), o));
        return H(stmtNode(static_cast<const CaseStatement::ItemGroup*>(o->node)->stmt, o));
    }
    if (o->kind == Obj::Expr) {
        const Expression& e = *static_cast<const Expression*>(o->node);
        if (e.kind == EK::ElementSelect) {
            const auto& x = e.as<ElementSelectExpression>();
            if (type == vpiIndex) return H(exprNode(&x.selector(), o));
            if (type == vpiParent || type == vpiExpr) return H(exprNode(&x.value(), o));
        }
        if (e.kind == EK::RangeSelect) {
            const auto& x = e.as<RangeSelectExpression>();
            bool idx = x.getSelectionKind() != RangeSelectionKind::Simple;
            if (type == vpiParent || type == vpiExpr) return H(exprNode(&x.value(), o));
            if (type == (idx ? vpiBaseExpr : vpiLeftRange)) return H(exprNode(&x.left(), o));
            if (type == (idx ? vpiWidthExpr : vpiRightRange)) return H(exprNode(&x.right(), o));
        }
        return nullptr;
    }
    // A statement.
    const Statement& s = *static_cast<const Statement*>(o->node);
    auto lhsRhs = [&](const Expression& a) -> vpiHandle {
        const Expression* x = stripConv(&a);
        if (x->kind != EK::Assignment) return nullptr;
        const auto& as = x->as<AssignmentExpression>();
        if (type == vpiLhs) return H(exprNode(&as.left(), o));
        if (type == vpiRhs) return H(exprNode(&as.right(), o));
        if ((type == vpiDelayControl || type == vpiEventControl) && as.timingControl)
            return nullptr;   // an intra-assignment timing control has no object of its own here
        return nullptr;
    };
    switch (s.kind) {
        case StatementKind::ExpressionStatement: {
            const Expression& e = *stripConv(&s.as<ExpressionStatement>().expr);
            if (e.kind == EK::UnaryOp && type == vpiLhs) return H(exprNode(&e.as<UnaryExpression>().operand(), o));
            if (e.kind == EK::UnaryOp && type == vpiRhs) return H(exprNode(&e, o));
            return lhsRhs(e);
        }
        case StatementKind::ProceduralAssign: return lhsRhs(s.as<ProceduralAssignStatement>().assignment);
        case StatementKind::ProceduralDeassign:
            return type == vpiLhs || type == vpiExpr ? H(exprNode(&s.as<ProceduralDeassignStatement>().lvalue, o)) : nullptr;
        case StatementKind::Conditional: {
            const auto& c = s.as<ConditionalStatement>();
            if (type == vpiCondition) return H(exprNode(c.conditions[0].expr, o));
            if (type == vpiStmt) return H(stmtNode(&c.ifTrue, o));
            if (type == vpiElseStmt) return H(stmtNode(c.ifFalse, o));
            return nullptr;
        }
        case StatementKind::Case:
            return type == vpiCondition ? H(exprNode(&s.as<CaseStatement>().expr, o)) : nullptr;
        case StatementKind::PatternCase:
            return type == vpiCondition ? H(exprNode(&s.as<PatternCaseStatement>().expr, o)) : nullptr;
        case StatementKind::WhileLoop:
            if (type == vpiCondition) return H(exprNode(&s.as<WhileLoopStatement>().cond, o));
            return type == vpiStmt ? H(stmtNode(&s.as<WhileLoopStatement>().body, o)) : nullptr;
        case StatementKind::DoWhileLoop:
            if (type == vpiCondition) return H(exprNode(&s.as<DoWhileLoopStatement>().cond, o));
            return type == vpiStmt ? H(stmtNode(&s.as<DoWhileLoopStatement>().body, o)) : nullptr;
        case StatementKind::RepeatLoop:
            if (type == vpiCondition) return H(exprNode(&s.as<RepeatLoopStatement>().count, o));
            return type == vpiStmt ? H(stmtNode(&s.as<RepeatLoopStatement>().body, o)) : nullptr;
        case StatementKind::ForeverLoop: return type == vpiStmt ? H(stmtNode(&s.as<ForeverLoopStatement>().body, o)) : nullptr;
        case StatementKind::ForeachLoop:
            if (type == vpiVariables) return H(exprNode(&s.as<ForeachLoopStatement>().arrayRef, o));
            return type == vpiStmt ? H(stmtNode(&s.as<ForeachLoopStatement>().body, o)) : nullptr;
        case StatementKind::ForLoop: {
            const auto& f = s.as<ForLoopStatement>();
            if (type == vpiCondition) return H(exprNode(f.stopExpr, o));
            if (type == vpiStmt) return H(stmtNode(&f.body, o));
            if (type == vpiForInitStmt && !f.loopVars.empty()) {
                Obj* x = codeObj(Obj::ExprStmt, vpiAssignment, f.loopVars[0], o);
                x->idx = 1;
                return H(x);
            }
            if (type == vpiForInitStmt && !f.initializers.empty()) return H(codeObj(Obj::ExprStmt, vpiAssignment, f.initializers[0], o));
            if (type == vpiForIncStmt && !f.steps.empty()) return H(codeObj(Obj::ExprStmt, vpiAssignment, f.steps[0], o));
            return nullptr;
        }
        case StatementKind::Timed: {
            const auto& t = s.as<TimedStatement>();
            if (type == vpiStmt) return H(stmtNode(&t.stmt, o));
            if (type == vpiDelay && t.timing.kind == TimingControlKind::Delay) return H(exprNode(&t.timing.as<DelayControl>().expr, o));
            if (type == vpiCondition && t.timing.kind == TimingControlKind::SignalEvent)
                return H(exprNode(&t.timing.as<SignalEventControl>().expr, o));
            return nullptr;
        }
        case StatementKind::Wait:
            if (type == vpiCondition) return H(exprNode(&s.as<WaitStatement>().cond, o));
            return type == vpiStmt ? H(stmtNode(&s.as<WaitStatement>().stmt, o)) : nullptr;
        case StatementKind::Return:
            return type == vpiCondition || type == vpiExpr ? H(exprNode(s.as<ReturnStatement>().expr, o)) : nullptr;
        case StatementKind::Disable:
            return type == vpiExpr ? H(exprNode(&s.as<DisableStatement>().target, o)) : nullptr;
        case StatementKind::EventTrigger:
            return type == vpiNamedEvent || type == vpiExpr ? H(exprNode(&s.as<EventTriggerStatement>().target, o)) : nullptr;
        default: return nullptr;
    }
}

// vpi_iterate from a code object; false: not a relation of it.
inline bool codeIterate(int type, const Obj* o, std::vector<vpiHandle>& items) {
    auto add = [&](Obj* x) { if (x) items.push_back(H(x)); };
    if (o->kind == Obj::Stmt) {
        const Statement& s = *static_cast<const Statement*>(o->node);
        if (type == vpiStmt && (s.kind == StatementKind::Block || s.kind == StatementKind::List)) {
            std::vector<const Statement*> list;
            blockItems(s.kind == StatementKind::Block ? s.as<BlockStatement>().body : s, list);
            for (auto* x : list) add(stmtNode(x, o));
            return true;
        }
        if (type == vpiCaseItem && s.kind == StatementKind::Case) {
            const auto& c = s.as<CaseStatement>();
            for (size_t k = 0; k < c.items.size(); k++) {
                Obj* it = codeObj(Obj::CaseItem, vpiCaseItem, &c.items[k], o);
                it->idx = (int)k;
                add(it);
            }
            if (c.defaultCase) {
                Obj* it = codeObj(Obj::CaseItem, vpiCaseItem, c.defaultCase, o);
                it->idx = -1;
                add(it);
            }
            return true;
        }
        if (type == vpiArgument && s.kind == StatementKind::ExpressionStatement) {
            const Expression& e = *stripConv(&s.as<ExpressionStatement>().expr);
            if (e.kind != ExpressionKind::Call) return true;
            for (auto* a : e.as<CallExpression>().arguments()) add(exprNode(a, o));
            return true;
        }
        if ((type == vpiForInitStmt || type == vpiForIncStmt) && s.kind == StatementKind::ForLoop) {
            const auto& f = s.as<ForLoopStatement>();
            if (type == vpiForInitStmt)
                for (auto* v : f.loopVars) {
                    Obj* x = codeObj(Obj::ExprStmt, vpiAssignment, v, o);
                    x->idx = 1;
                    add(x);
                }
            for (auto* x : type == vpiForInitStmt ? f.initializers : f.steps) add(codeObj(Obj::ExprStmt, vpiAssignment, x, o));
            return true;
        }
        return false;
    }
    if (o->kind == Obj::CaseItem && type == vpiExpr) {
        if (o->idx >= 0)
            for (auto* x : static_cast<const CaseStatement::ItemGroup*>(o->node)->expressions) add(exprNode(x, o));
        return true;
    }
    if (o->kind == Obj::Expr) {
        const Expression& e = *static_cast<const Expression*>(o->node);
        if (type == vpiOperand && o->type == vpiOperation) {
            for (auto* x : operandsOf(e)) add(exprNode(x, o));
            return true;
        }
        if (type == vpiArgument && e.kind == ExpressionKind::Call) {
            for (auto* a : e.as<CallExpression>().arguments()) add(exprNode(a, o));
            return true;
        }
    }
    return false;
}
// Processes and continuous assignments of a scope (37.10, 37.32, 37.40).
inline Obj* netDeclAsgObj(const NetSymbol& n);   // vpi_param.inc
inline bool scopeCode(int type, const Scope& sc, std::vector<vpiHandle>& items) {
    if (type != vpiProcess && type != vpiContAssign) return false;
    eachMember(sc, [&](const Symbol& m) {
        if (type == vpiProcess && m.kind == SymbolKind::ProceduralBlock) items.push_back(H(processObj(m.as<ProceduralBlockSymbol>())));
        if (type == vpiContAssign && m.kind == SymbolKind::ContinuousAssign) items.push_back(H(contAssignObj(m.as<ContinuousAssignSymbol>())));
        if (type == vpiContAssign && m.kind == SymbolKind::Net && m.as<NetSymbol>().getInitializer())
            items.push_back(H(netDeclAsgObj(m.as<NetSymbol>())));   // a net declaration assignment (37.40)
    });
    return true;
}
// vpi_get of a code object; vpiUndefined - 1: not answered here.
inline int64_t codeGet(int prop, const Obj* o) {
    switch (prop) {
        case vpiType: return o->type;
        case vpiLineNo: return lineOf(rangeOfCode(o));
        default: break;
    }
    if (o->kind == Obj::Process) {
        if (prop != vpiAlwaysType || o->type != vpiAlways) return vpiUndefined;
        switch (o->sym->as<ProceduralBlockSymbol>().procedureKind) {
            case ProceduralBlockKind::AlwaysComb: return vpiAlwaysComb;
            case ProceduralBlockKind::AlwaysFF: return vpiAlwaysFF;
            case ProceduralBlockKind::AlwaysLatch: return vpiAlwaysLatch;
            default: return vpiAlways;
        }
    }
    if (o->kind == Obj::ContAssign) {
        return prop == vpiNetDeclAssign ? (o->sym->kind == SymbolKind::Net ? 1 : 0) : vpiUndefined;
    }
    if (o->kind == Obj::Expr) {
        const Expression& e = *static_cast<const Expression*>(o->node);
        switch (prop) {
            case vpiOpType: return o->type == vpiOperation ? opTypeOf(e) : vpiUndefined;
            case vpiConstType: return o->type == vpiConstant ? constTypeOf(e) : vpiUndefined;
            case vpiSize: return e.type ? (int64_t)e.type->getBitWidth() : vpiUndefined;
            case vpiSigned: return e.type && e.type->isSigned();
            case vpiIndexedPartSelectType:
                if (o->type != vpiIndexedPartSelect) return vpiUndefined;
                return e.as<RangeSelectExpression>().getSelectionKind() == slang::ast::RangeSelectionKind::IndexedUp ? vpiPosIndexed : vpiNegIndexed;
            case vpiFuncType:
                if (e.kind != ExpressionKind::Call || !e.type) return vpiUndefined;
                return e.type->isFloating() ? vpiRealFunc : e.type->isSigned() ? vpiSizedSignedFunc : vpiSizedFunc;
            default: return vpiUndefined;
        }
    }
    if (o->kind == Obj::Stmt) {
        const Statement& s = *static_cast<const Statement*>(o->node);
        switch (prop) {
            case vpiBlocking:
                if (s.kind == StatementKind::ExpressionStatement) {
                    const Expression& e = *stripConv(&s.as<ExpressionStatement>().expr);
                    return e.kind == ExpressionKind::Assignment ? !e.as<AssignmentExpression>().isNonBlocking() : 1;
                }
                return vpiUndefined;
            case vpiCaseType:
                if (s.kind != StatementKind::Case) return vpiUndefined;
                switch (s.as<CaseStatement>().condition) {
                    case CaseStatementCondition::WildcardXOrZ: return vpiCaseX;
                    case CaseStatementCondition::WildcardJustZ: return vpiCaseZ;
                    default: return vpiCaseExact;
                }
            case vpiQualifier: {
                UniquePriorityCheck c = UniquePriorityCheck::None;
                if (s.kind == StatementKind::Case) c = s.as<CaseStatement>().check;
                else if (s.kind == StatementKind::Conditional) c = s.as<ConditionalStatement>().check;
                else return vpiUndefined;
                int q = c == UniquePriorityCheck::Priority ? vpiPriorityQualifier
                      : c == UniquePriorityCheck::Unique || c == UniquePriorityCheck::Unique0 ? vpiUniqueQualifier : vpiNoQualifier;
                if (s.kind == StatementKind::Case && s.as<CaseStatement>().condition == CaseStatementCondition::Inside) q |= vpiInsideQualifier;
                return q;
            }
            case vpiJoinType:
                if (s.kind != StatementKind::Block) return vpiUndefined;
                switch (s.as<BlockStatement>().blockKind) {
                    case StatementBlockKind::JoinAny: return vpiJoinAny;
                    case StatementBlockKind::JoinNone: return vpiJoinNone;
                    case StatementBlockKind::JoinAll: return vpiJoin;
                    default: return vpiUndefined;
                }
            default: return vpiUndefined;
        }
    }
    if (o->kind == Obj::ExprStmt && prop == vpiBlocking) return 1;
    return vpiUndefined;
}
// vpi_get_str of a code object; true: answered (r null for "no such string").
inline bool codeGetStr(int prop, const Obj* o, std::string& r, bool& have) {
    have = false;
    if (prop == vpiFile) { r = fileOf(rangeOfCode(o)); have = true; return true; }
    if (prop == vpiDecompile && (o->kind == Obj::Expr || o->kind == Obj::ExprStmt)) {
        r = textOf(rangeOfCode(o));
        have = true;
        return true;
    }
    if (prop == vpiName || prop == vpiFullName) {
        if (const StatementBlockSymbol* b = blockSym(o); b && !b->name.empty()) {
            r = prop == vpiName ? std::string(b->name) : b->getHierarchicalPath();
            have = true;
            return true;
        }
        if (o->kind == Obj::Expr && static_cast<const Expression*>(o->node)->kind == ExpressionKind::Call) {
            r = std::string(static_cast<const Expression*>(o->node)->as<CallExpression>().getSubroutineName());
            have = true;
            return true;
        }
        if (o->kind == Obj::Expr && o->type == vpiRefObj) {   // a reference by name (a class property, a loop variable)
            const Symbol& sym = static_cast<const Expression*>(o->node)->as<ValueExpressionBase>().symbol;
            r = prop == vpiName ? std::string(sym.name) : sym.getHierarchicalPath();
            have = true;
            return true;
        }
        if (o->kind == Obj::Stmt) {
            const Statement& s = *static_cast<const Statement*>(o->node);
            if (s.kind == StatementKind::ExpressionStatement) {
                const Expression& e = *stripConv(&s.as<ExpressionStatement>().expr);
                if (e.kind == ExpressionKind::Call) {
                    r = std::string(e.as<CallExpression>().getSubroutineName());
                    have = true;
                }
            }
        }
        return true;
    }
    return prop != vpiType;
}
// A value as a slang constant of type t: integral (4-state bits kept),
// real, shortreal or string.
inline bool valConst(const Val& v, const Type& t0, slang::ConstantValue& cv) {
    const Type& t = t0.getCanonicalType();
    if (t.isFloating()) {
        double d;
        uint64_t b = v.v;
        std::memcpy(&d, &b, 8);
        if (t.as<FloatingType>().floatKind == FloatingType::ShortReal) cv = slang::ConstantValue(slang::shortreal_t((float)d));
        else cv = slang::ConstantValue(slang::real_t(d));
        return true;
    }
    if (t.isString()) {
        Val s = v;
        s.str = true;
        cv = slang::ConstantValue(v_str(s));
        return true;
    }
    if (!t.isIntegral()) return false;
    int w = (int)t.getBitWidth();
    if (w <= 0) return false;
    std::string lit = std::to_string(w) + (t.isSigned() ? "'sb" : "'b");
    for (int i = w - 1; i >= 0; i--) {
        int b = bitAt(v, i);
        lit += b == 0 ? '0' : b == 1 ? '1' : b == 3 ? 'z' : 'x';
    }
    cv = slang::SVInt::fromString(lit);
    return true;
}
// The value an object has now as a slang constant: a variable or net, or a
// one-dimensional fixed-size array of them.
inline bool objConst(Obj* x, const Type& t0, slang::ConstantValue& cv) {
    const Type& t = t0.getCanonicalType();
    if (!x->arr) return valConst(readObj(x), t, cv);
    if (t.kind != SymbolKind::FixedSizeUnpackedArrayType || !t.getArrayElementType()) return false;
    const Type& et = t.getArrayElementType()->getCanonicalType();
    if (et.isUnpackedArray()) return false;
    slang::ConstantRange r = t.getFixedRange();
    std::vector<slang::ConstantValue> el(r.width());
    for (int32_t i = r.lower(); i <= r.upper(); i++) {
        Obj* w = wordObj(x, i);
        size_t k = (size_t)(r.left <= r.right ? i - r.left : r.left - i);   // slang keeps the leftmost element first
        if (!w || !valConst(readObj(w), et, el[k])) return false;
    }
    cv = slang::ConstantValue(std::move(el));
    return true;
}
// The value of an expression now (37.35, 38.15): slang evaluates it with
// each variable or net it reads set to the value that variable has now.
// Parameters and enum constants are constants; an expression that reads an
// automatic variable, a class property or a hierarchical name has no value
// here.
inline bool codeValue(const Obj* o, Val& v) {
    if (o->kind != Obj::Expr) return false;
    const Expression& e = *static_cast<const Expression*>(o->node);
    EvalContext ctx(ASTContext(st().comp->getRoot(), LookupLocation::max), EvalFlags::IsScript);
    bool ok = true;
    std::set<const ValueSymbol*> seen;
    auto vis = slang::ast::makeVisitor([&](auto&, const NamedValueExpression& n) {
        const ValueSymbol& s = n.symbol;
        if (s.kind != SymbolKind::Net && s.kind != SymbolKind::Variable) return;
        if (!seen.insert(&s).second) return;
        if (s.kind == SymbolKind::Variable && s.as<VariableSymbol>().lifetime == VariableLifetime::Automatic) { ok = false; return; }
        Obj* x = valueObj(&s);
        slang::ConstantValue cv;
        if (!x || !objConst(x, s.getType(), cv)) { ok = false; return; }
        ctx.createLocal(&s, std::move(cv));
    });
    e.visit(vis);
    if (!ok) return false;
    slang::ConstantValue cv = e.eval(ctx);
    return cv && constVal(cv, v);
}
inline const char* codeTypeName(int type) {
    static const std::map<int, const char*> names = {
        {vpiInitial, "vpiInitial"}, {vpiAlways, "vpiAlways"}, {vpiFinal, "vpiFinal"}, {vpiContAssign, "vpiContAssign"},
        {vpiBegin, "vpiBegin"}, {vpiFork, "vpiFork"}, {vpiAssignment, "vpiAssignment"}, {vpiTaskCall, "vpiTaskCall"},
        {vpiSysTaskCall, "vpiSysTaskCall"}, {vpiMethodTaskCall, "vpiMethodTaskCall"}, {vpiReturn, "vpiReturn"},
        {vpiContinue, "vpiContinue"}, {vpiBreak, "vpiBreak"}, {vpiDisable, "vpiDisable"}, {vpiIf, "vpiIf"},
        {vpiIfElse, "vpiIfElse"}, {vpiCase, "vpiCase"}, {vpiCaseItem, "vpiCaseItem"}, {vpiFor, "vpiFor"},
        {vpiRepeat, "vpiRepeat"}, {vpiForeachStmt, "vpiForeachStmt"}, {vpiWhile, "vpiWhile"}, {vpiDoWhile, "vpiDoWhile"},
        {vpiForever, "vpiForever"}, {vpiDelayControl, "vpiDelayControl"}, {vpiEventControl, "vpiEventControl"},
        {vpiRepeatControl, "vpiRepeatControl"}, {vpiDisableFork, "vpiDisableFork"}, {vpiWait, "vpiWait"},
        {vpiWaitFork, "vpiWaitFork"}, {vpiOrderedWait, "vpiOrderedWait"}, {vpiEventStmt, "vpiEventStmt"},
        {vpiForce, "vpiForce"}, {vpiAssignStmt, "vpiAssignStmt"}, {vpiRelease, "vpiRelease"}, {vpiDeassign, "vpiDeassign"},
        {vpiNullStmt, "vpiNullStmt"}, {vpiOperation, "vpiOperation"}, {vpiConstant, "vpiConstant"},
        {vpiFuncCall, "vpiFuncCall"}, {vpiSysFuncCall, "vpiSysFuncCall"}, {vpiMethodFuncCall, "vpiMethodFuncCall"},
        {vpiBitSelect, "vpiBitSelect"}, {vpiIndexedPartSelect, "vpiIndexedPartSelect"}, {vpiRefObj, "vpiRefObj"},
        {vpiFsm, "vpiFsm"}, {vpiFsmHandle, "vpiFsmHandle"}, {vpiSchedEvent, "vpiSchedEvent"}};
    auto it = names.find(type);
    return it == names.end() ? nullptr : it->second;
}

// ---- cbStmt (38.36.1) ----
// A statement's code is found by its source line: the first instruction of
// each run of instructions of that line, in every process and subroutine,
// calls the callback. Two statements on one line share it.
inline void onStmt(const Instr& i) {
    auto& s = st();
    auto it = s.stmtCbs.find({i.file, i.line});
    if (it == s.stmtCbs.end()) return;
    auto list = it->second;   // a callback may register or remove others
    for (Cb* c : list) {
        if (!c->live) continue;
        s_cb_data d = c->d;
        s_vpi_time t = c->t;
        d.obj = H(c->obj);
        d.time = c->hasT ? &t : nullptr;
        d.value = nullptr;
        if (c->hasT && t.type != vpiSuppressTime) fillTime(&t, c->obj, S.now);
        d.cb_rtn(&d);
    }
}
inline bool markStmt(const Obj* o, Cb* c) {
    slang::SourceRange r = rangeOfCode(o);
    if (!srcMgr() || !r.start().valid()) return false;
    auto ol = srcMgr()->getFullyOriginalLoc(r.start());
    uint32_t file = covFileId(std::string(srcMgr()->getFullPath(ol.buffer()).string()));
    uint32_t line = (uint32_t)srcMgr()->getLineNumber(ol);
    bool any = false;
    for (const Prog* p : st().progs)
        for (size_t k = 0; k < p->code.size(); k++) {
            const Instr& i = p->code[k];
            if (i.file != file || i.line != line) continue;
            if (k == 0 || p->code[k - 1].file != file || p->code[k - 1].line != line) i.stmtCb = true;
            any = true;
        }
    if (!any) return false;
    st().stmtCbs[{file, line}].push_back(c);
    g_stmtHook = onStmt;
    g_hook = true;
    return true;
}
// ---- Drivers and loads of a net or variable (37.16, 37.17) ----
// A continuous assignment drives the variables its left side names and loads
// the ones its right side reads; a port of a module instance drives the
// signal its connection names when it is an output, and loads it when it is
// an input (inout: both). vpiLocalDriver / vpiLocalLoad stay inside the
// module of the signal; vpiDriver / vpiLoad follow port connections through
// the hierarchy (the whole simulated net) and give the continuous
// assignments and the ports that are not just a whole-signal connection.
inline Obj* primObj(const PrimitiveInstanceSymbol& g);
inline Obj* primTermObj(const Obj* p, int k);
inline int primTermDir(const PrimitiveInstanceSymbol& g, int k);
// Which bits of `s` a driver or load must touch to count: all (lo < 0), or
// bits lo..hi counted from the right end of its declared range (a vpiNetBit
// or part select handle, 37.16 detail 18).
struct Bits { int64_t lo = -1, hi = -1; };
inline std::optional<int64_t> constOf(const Expression& e, const Symbol* s) {
    slang::ConstantValue cv;
    if (const slang::ConstantValue* c = e.getConstant()) cv = *c;
    else {
        const Scope* sc = s->getParentScope();
        if (!sc) return std::nullopt;
        slang::ast::EvalContext ctx(slang::ast::ASTContext(*sc, slang::ast::LookupLocation::max));
        cv = e.eval(ctx);
    }
    if (!cv.isInteger()) return std::nullopt;
    return cv.integer().as<int64_t>();
}
// Does the select `e` (an element or range select straight on `s`) touch
// bits b? Unknown (a non-constant index, a multi-dimensional select): yes.
inline bool selTouches(const Expression& e, const Symbol* s, Bits b) {
    if (b.lo < 0) return true;
    const Expression& v = e.kind == ExpressionKind::ElementSelect ? e.as<ElementSelectExpression>().value() : e.as<RangeSelectExpression>().value();
    const Type& t = v.type->getCanonicalType();
    if (!t.isIntegral() || !t.hasFixedRange() || t.getArrayElementType() && t.getArrayElementType()->getBitstreamWidth() != 1) return true;
    std::optional<int64_t> a, c;
    if (e.kind == ExpressionKind::ElementSelect) a = c = constOf(e.as<ElementSelectExpression>().selector(), s);
    else {
        const auto& rs = e.as<RangeSelectExpression>();
        a = constOf(rs.left(), s);
        c = constOf(rs.right(), s);
        if (a && c && rs.getSelectionKind() == slang::ast::RangeSelectionKind::IndexedUp) c = *a + *c - 1;
        else if (a && c && rs.getSelectionKind() == slang::ast::RangeSelectionKind::IndexedDown) c = *a - *c + 1;
    }
    if (!a || !c) return true;
    auto r = t.getFixedRange();
    auto off = [&](int64_t i) { return r.left >= r.right ? i - r.right : r.right - i; };
    int64_t x = std::min(off(*a), off(*c)), y = std::max(off(*a), off(*c));
    return !(y < b.lo || x > b.hi);
}
inline bool onSym(const Expression& v, const Symbol* s) {
    return (v.kind == ExpressionKind::NamedValue || v.kind == ExpressionKind::HierarchicalValue) && &v.as<ValueExpressionBase>().symbol == s;
}
inline const Symbol* rootOf(const Expression* e) {
    while (e) {
        switch (e->kind) {
            case ExpressionKind::Conversion: e = &e->as<ConversionExpression>().operand(); break;
            case ExpressionKind::Assignment: e = &e->as<AssignmentExpression>().left(); break;
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: return &e->as<ValueExpressionBase>().symbol;
            default: return nullptr;
        }
    }
    return nullptr;
}
// Does the target expression `e` (the left side of an assignment) write `s`
// (bits b of it)?
inline bool writes(const Expression& e, const Symbol* s, Bits b = {}) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue: return &e.as<ValueExpressionBase>().symbol == s;
        case ExpressionKind::Conversion: return writes(e.as<ConversionExpression>().operand(), s, b);
        case ExpressionKind::Assignment: return writes(e.as<AssignmentExpression>().left(), s, b);
        case ExpressionKind::ElementSelect: {
            const auto& v = e.as<ElementSelectExpression>().value();
            return onSym(v, s) ? selTouches(e, s, b) : writes(v, s, b);
        }
        case ExpressionKind::RangeSelect: {
            const auto& v = e.as<RangeSelectExpression>().value();
            return onSym(v, s) ? selTouches(e, s, b) : writes(v, s, b);
        }
        case ExpressionKind::MemberAccess: return writes(e.as<MemberAccessExpression>().value(), s, b);
        case ExpressionKind::Concatenation:
            for (auto* x : e.as<ConcatenationExpression>().operands())
                if (writes(*x, s, b)) return true;
            return false;
        default: return false;
    }
}
// Does `e` read `s` (bits b of it) anywhere?
inline bool reads(const Expression& e, const Symbol* s, Bits b = {}) {
    bool hit = false;
    auto v = slang::ast::makeVisitor(
        [&](auto&, const NamedValueExpression& n) { if (&n.symbol == s) hit = true; },
        [&](auto&, const HierarchicalValueExpression& n) { if (&n.symbol == s) hit = true; },
        [&](auto& self, const ElementSelectExpression& x) {
            if (onSym(x.value(), s)) {
                if (selTouches(x, s, b)) hit = true;
                x.selector().visit(self);
            } else self.visitDefault(x);
        },
        [&](auto& self, const RangeSelectExpression& x) {
            if (onSym(x.value(), s)) {
                if (selTouches(x, s, b)) hit = true;
                x.left().visit(self);
                x.right().visit(self);
            } else self.visitDefault(x);
        });
    e.visit(v);
    return hit;
}
inline const InstanceBodySymbol* bodyOf(const Symbol* s) {
    for (const Scope* sc = s->getParentScope(); sc; sc = sc->asSymbol().getParentScope())
        if (sc->asSymbol().kind == SymbolKind::InstanceBody) return &sc->asSymbol().as<InstanceBodySymbol>();
    return nullptr;
}
inline Obj* portOf(const PortSymbol& p) {
    const auto* b = bodyOf(&p);
    int k = 0;
    if (b)
        for (const Symbol* q : b->getPortList()) {
            if (q == &p) break;
            k++;
        }
    return portObj(&p, k);
}
// The continuous assignments and instance ports inside the module of `body`
// (not inside its child instances) that drive (drv) or load `s`.
// `skipWhole`: leave out ports whose connection is the whole of `s`.
inline void localConn(const InstanceBodySymbol& body, const Symbol* s, bool drv, bool skipWhole, std::vector<Obj*>& out, Bits b = {}) {
    auto v = slang::ast::makeVisitor(
        [&](auto&, const ContinuousAssignSymbol& c) {
            const auto& a = c.getAssignment();
            if (a.kind != ExpressionKind::Assignment) return;
            const auto& ae = a.as<AssignmentExpression>();
            if (drv ? writes(ae.left(), s, b) : reads(ae.right(), s, b)) out.push_back(contAssignObj(c));
        },
        [&](auto&, const PrimitiveInstanceSymbol& g) {   // a gate, switch or UDP terminal (37.46)
            auto conns = g.getPortConnections();
            for (size_t k = 0; k < conns.size(); k++) {
                if (!conns[k]) continue;
                int dir = primTermDir(g, (int)k);
                bool in = dir == vpiInput || dir == vpiInout, outp = dir == vpiOutput || dir == vpiInout;
                if (drv ? outp && writes(*conns[k], s, b) : in && reads(*conns[k], s, b)) out.push_back(primTermObj(primObj(g), (int)k));
            }
        },
        [&](auto&, const InstanceSymbol& inst) {   // a child instance: its port connections, not its body
            for (const PortConnection* pc : inst.getPortConnections()) {
                if (pc->port.kind != SymbolKind::Port) continue;
                const auto& p = pc->port.as<PortSymbol>();
                const Expression* e = pc->getExpression();
                if (!e) continue;
                bool in = p.direction == ArgumentDirection::In || p.direction == ArgumentDirection::InOut || p.direction == ArgumentDirection::Ref;
                bool outp = p.direction == ArgumentDirection::Out || p.direction == ArgumentDirection::InOut || p.direction == ArgumentDirection::Ref;
                bool hit = drv ? outp && writes(*e, s, b) : in && reads(*e, s, b);
                if (!hit) continue;
                if (skipWhole && rootOf(e) == s) continue;
                out.push_back(portOf(p));
            }
        },
        [&](auto&, const ProceduralBlockSymbol&) {},
        [&](auto&, const SubroutineSymbol&) {});
    body.visit(v);
}
// The nets joined to `s` by whole-signal port connections, `s` first.
inline std::vector<const Symbol*> simNet(const Symbol* s) {
    std::vector<const Symbol*> set{s};
    for (size_t k = 0; k < set.size(); k++) {
        const Symbol* x = set[k];
        auto add = [&](const Symbol* y) {
            if (y && std::find(set.begin(), set.end(), y) == set.end()) set.push_back(y);
        };
        const InstanceBodySymbol* b = bodyOf(x);
        if (!b) continue;
        // up: a port of this module whose inside is x
        for (const Symbol* q : b->getPortList()) {
            if (q->kind != SymbolKind::Port || q->as<PortSymbol>().internalSymbol != x) continue;
            const InstanceSymbol* inst = b->parentInstance;
            const PortConnection* pc = inst ? inst->getPortConnection(q->as<PortSymbol>()) : nullptr;
            if (pc) add(rootOf(pc->getExpression()));
        }
        // down: a child instance port connected to the whole of x
        auto v = slang::ast::makeVisitor(
            [&](auto&, const InstanceSymbol& inst) {
                for (const PortConnection* pc : inst.getPortConnections())
                    if (pc->port.kind == SymbolKind::Port && rootOf(pc->getExpression()) == x) add(pc->port.as<PortSymbol>().internalSymbol);
            },
            [&](auto&, const ProceduralBlockSymbol&) {}, [&](auto&, const SubroutineSymbol&) {});
        b->visit(v);
    }
    return set;
}
// The force and assign statements of the design by source line, with the
// procedure or task they are in (lazily, once).
struct HoldStmt { const ProceduralAssignStatement* st; const Symbol* in; };
inline const std::map<std::pair<uint32_t, uint32_t>, HoldStmt>& holdStmts() {
    static std::map<std::pair<uint32_t, uint32_t>, HoldStmt> m;
    static bool built = false;
    if (built || !st().comp || !srcMgr()) return m;
    built = true;
    const Symbol* in = nullptr;
    auto inner = slang::ast::makeVisitor([&](auto&, const ProceduralAssignStatement& p) {
        auto ol = srcMgr()->getFullyOriginalLoc(p.sourceRange.start());
        uint32_t file = covFileId(std::string(srcMgr()->getFullPath(ol.buffer()).string()));
        m.emplace(std::make_pair(file, (uint32_t)srcMgr()->getLineNumber(ol)), HoldStmt{&p, in});
    });
    auto outer = slang::ast::makeVisitor(
        [&](auto&, const ProceduralBlockSymbol& p) { in = &p; p.getBody().visit(inner); },
        [&](auto&, const SubroutineSymbol& f) { in = &f; f.getBody().visit(inner); });
    st().comp->getRoot().visit(outer);
    return m;
}
// The force (and, for a variable, assign) statements in effect on signal
// `o` that drive it, or (load) that read it, as statement objects.
inline void holdConn(const Obj* o, bool drv, bool local, Bits bits, std::vector<Obj*>& out) {
    const InstanceBodySymbol* home = bodyOf(o->sym);
    bool net = o->sym->kind == SymbolKind::Net;
    for (auto& [key, src] : st().holdSrcs) {
        auto [sg, force, hi, lo] = key;
        auto it = holdStmts().find(src);
        if (it == holdStmts().end()) continue;
        const HoldStmt& h = it->second;
        if (local && bodyOf(h.in) != home) continue;
        const auto& a = h.st->assignment;
        if (a.kind != ExpressionKind::Assignment) continue;
        if (drv) {   // a net: forces only (detail 15); a variable: forces and assigns (37.21)
            if (sg != o->sig || (net && !force)) continue;
            if (bits.lo >= 0 && hi >= 0 && (hi < bits.lo || lo > bits.hi)) continue;
        } else if (!reads(a.as<AssignmentExpression>().right(), o->sym, bits))
            continue;
        Obj* so = stmtNode(h.st, h.in->kind == SymbolKind::ProceduralBlock ? processObj(h.in->as<ProceduralBlockSymbol>()) : nullptr);
        if (so && std::find(out.begin(), out.end(), so) == out.end()) out.push_back(so);
    }
}
inline bool netConn(int type, const Obj* o, std::vector<vpiHandle>& items) {
    if (type != vpiDriver && type != vpiLoad && type != vpiLocalDriver && type != vpiLocalLoad && type != vpiPortInst) return false;
    // A bit or part select of a vector: the drivers and loads that touch
    // those bits (37.16 detail 18).
    Bits bits;
    if (o->kind == Obj::Bit && o->parent && o->parent->kind == Obj::Signal && type != vpiPortInst) {
        bits = {o->off, o->off + o->w - 1};
        o = o->parent;
    }
    if (o->kind != Obj::Signal || !o->sym || !ValueSymbol::isKind(o->sym->kind)) return false;
    const InstanceBodySymbol* b = bodyOf(o->sym);
    if (!b) return true;
    std::vector<Obj*> out;
    if (type == vpiPortInst) {   // the ports of instances this signal connects to (37.16)
        auto v = slang::ast::makeVisitor(
            [&](auto&, const InstanceSymbol& inst) {
                for (const PortConnection* pc : inst.getPortConnections())
                    if (pc->port.kind == SymbolKind::Port && pc->getExpression() && reads(*pc->getExpression(), o->sym))
                        out.push_back(portOf(pc->port.as<PortSymbol>()));
            },
            [&](auto&, const ProceduralBlockSymbol&) {}, [&](auto&, const SubroutineSymbol&) {});
        b->visit(v);
    } else if (type == vpiLocalDriver || type == vpiLocalLoad) {
        // The ports of its own module count (37.16 detail 17): an input or
        // inout drives it, an output or inout loads it.
        for (const Symbol* q : b->getPortList()) {
            if (q->kind != SymbolKind::Port || q->as<PortSymbol>().internalSymbol != o->sym) continue;
            auto d = q->as<PortSymbol>().direction;
            bool in = d == ArgumentDirection::In || d == ArgumentDirection::InOut, outp = d == ArgumentDirection::Out || d == ArgumentDirection::InOut;
            if (type == vpiLocalDriver ? in : outp) out.push_back(portOf(q->as<PortSymbol>()));
        }
        localConn(*b, o->sym, type == vpiLocalDriver, false, out, bits);
        holdConn(o, type == vpiLocalDriver, true, bits, out);
    } else {
        holdConn(o, type == vpiDriver, false, bits, out);
        for (const Symbol* x : simNet(o->sym))
            if (const auto* xb = bodyOf(x)) localConn(*xb, x, type == vpiDriver, true, out, bits);   // whole connections: the same bits
    }
    for (Obj* x : out)
        if (std::find_if(items.begin(), items.end(), [&](vpiHandle h) { return O(h) == x; }) == items.end()) items.push_back(H(x));
    return true;
}
