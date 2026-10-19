// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `p` must outlive the simulation.
static Val rd(int s) { return S.get(s); }

Task exec(const Prog* p) {
    size_t pc = 0;
    while (pc < p->code.size()) {
        const Instr& i = p->code[pc];
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
                if (i.events.size() > 1) co_await WaitAny{i.events};
                else co_await Wait{i.sig, i.edge};
                pc++; break;
            case IK::WaitDelay: co_await Delay{i.delay}; pc++; break;
            case IK::Finish:    S.finished = true; pc++; break;
        }
    }
}
