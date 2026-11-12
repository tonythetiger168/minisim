// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `p` must outlive the simulation.
static Val rd(int s) { return S.get(s); }

Task exec(const Prog* p) {
    if (S.collect_coverage)
        for (const auto& instruction : p->code)
            if (!instruction.line_bin.empty() && !S.line_coverage.has(instruction.line_bin))
                S.line_coverage.add(instruction.line_bin);
    size_t pc = 0;
    uint64_t steps = 0;
    while (pc < p->code.size()) {
        if (steps == S.max_ir_instructions_per_resume) {
            S.execution_error = "IR instruction budget exceeded at time " + to_string(S.now) +
                                " pc " + to_string(pc);
            S.finished = true;
            co_return; // report through Sim::run, never throw through unhandled_exception
        }
        ++steps;
        const Instr& i = p->code[pc];
        if (S.collect_coverage && !i.line_bin.empty()) S.line_coverage.hit(i.line_bin);
        switch (i.k) {
            case IK::Assign: {
                Val v = eval(i.e, rd);
                if (i.drv >= 0) { S.drive(i.sig, i.drv, v); pc++; break; }
                S.set(i.sig, i.hi >= 0 ? v_insert(S.get(i.sig), v, i.hi, i.lo) : v);
                pc++; break;
            }
            case IK::Nba:       S.nba(i.sig, eval(i.e, rd), i.hi, i.lo); pc++; break;
            case IK::JmpIfNot:  pc = (truth(eval(i.e, rd)) == 1) ? pc + 1 : i.target; break;
            case IK::Jmp:       pc = i.target; break;
            case IK::WaitEdge:
                steps = 0;
                if (i.events.size() > 1) co_await WaitAny{i.events};
                else co_await Wait{i.sig, i.edge};
                pc++; break;
            case IK::WaitDelay: steps = 0; co_await Delay{i.delay}; pc++; break;
            case IK::Finish:    S.finished = true; co_return;
        }
    }
}
