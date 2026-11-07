// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `entry` must outlive the simulation.
// `rets` is a coroutine local. A #delay inside a callee hits co_await here,
// and the return address is still on `rets` when the process resumes.
static Val rd(int s) { return S.get(s); }

struct RetFrame { const Prog* prog; size_t pc; };

Task exec(const Prog* entry) {
    const Prog* p = entry;
    size_t pc = 0;
    vector<RetFrame> rets;
    while (p) {
        if (pc >= p->code.size()) {
            if (rets.empty()) co_return;
            RetFrame f = rets.back();
            rets.pop_back();
            p = f.prog;
            pc = f.pc;
            continue;
        }
        const Instr& i = p->code[pc];
        switch (i.k) {
            case IK::Assign: {
                Val v = eval(i.e, rd);
                if (i.drv >= 0) { S.drive(i.sig, i.drv, v, i.s0, i.s1); pc++; break; }
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
            case IK::Call: {
                const Prog* callee = i.callee;
                rets.push_back({p, pc + 1});
                p = callee;
                pc = 0;
                break;
            }
            case IK::Ret:
                if (rets.empty()) co_return;
                {
                    RetFrame f = rets.back();
                    rets.pop_back();
                    p = f.prog;
                    pc = f.pc;
                }
                break;
        }
    }
}
