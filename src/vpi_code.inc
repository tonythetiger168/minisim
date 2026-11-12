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
inline bool scopeCode(int type, const Scope& sc, std::vector<vpiHandle>& items) {
    if (type != vpiProcess && type != vpiContAssign) return false;
    eachMember(sc, [&](const Symbol& m) {
        if (type == vpiProcess && m.kind == SymbolKind::ProceduralBlock) items.push_back(H(processObj(m.as<ProceduralBlockSymbol>())));
        if (type == vpiContAssign && m.kind == SymbolKind::ContinuousAssign) items.push_back(H(contAssignObj(m.as<ContinuousAssignSymbol>())));
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
        return prop == vpiNetDeclAssign ? 0 : vpiUndefined;
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
                return e.as<RangeSelectExpression>().getSelectionKind() == RangeSelectionKind::IndexedUp ? vpiPosIndexed : vpiNegIndexed;
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
// The value of a constant (or constant-valued) expression.
inline bool codeValue(const Obj* o, Val& v) {
    if (o->kind != Obj::Expr) return false;
    const Expression& e = *static_cast<const Expression*>(o->node);
    EvalContext ctx(ASTContext(st().comp->getRoot(), LookupLocation::max));
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
