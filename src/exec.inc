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
            case IK::Display:
                S.printSys(i.fmt, i.args);
                if (S.finished) co_return;  // task ran; $finish already requested
                pc++;
                break;
            case IK::Strobe:
                S.strobe(i.fmt, i.args);
                if (S.finished) co_return;
                pc++;
                break;
            case IK::Monitor:
                S.armMon(i.fmt, i.args);
                if (S.finished) co_return;
                pc++;
                break;
            case IK::Finish:
                // Stop this process. The rest of this time slot still runs,
                // including NBA and postponed, so a $strobe/$monitor already
                // queued still prints. The next time does not start. Another
                // process already queued runs until its next system task;
                // that task itself runs (Icarus 13).
                S.finished = true;
                co_return;
        }
    }
}
