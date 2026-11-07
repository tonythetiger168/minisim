// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `entry` must outlive the simulation.
// `rets` is a coroutine local. A #delay inside a callee hits co_await here,
// and the return address is still on `rets` when the process resumes.
static Val rd(int s) { return S.get(s); }

struct RetFrame { const Prog* prog; size_t pc; };

// One fork...join. Each branch reports when it ends; the parent resumes when
// every branch has ended (join) or the first one has (join_any).
struct JoinState {
    int left = 0;          // branches still running
    int join = 0;          // 0 join, 1 join_any, 2 join_none
    Handle parent{};
    bool waiting = false;  // parent is suspended on this fork
    bool ready() const { return join == 2 || left == 0 || (join == 1 && done > 0); }
    int done = 0;
    void branchEnded() {
        left--;
        done++;
        if (waiting && ready()) { waiting = false; S.active.push_back(parent); }
    }
};
struct JoinWait {
    shared_ptr<JoinState> js;
    bool await_ready() const { return js->ready(); }
    void await_suspend(Handle h) const { js->parent = h; js->waiting = true; }
    void await_resume() const {}
};

Task exec(const Prog* entry, shared_ptr<JoinState> joinOf = nullptr) {
    const Prog* p = entry;
    size_t pc = 0;
    vector<RetFrame> rets;
    while (p) {
        if (pc >= p->code.size()) {
            if (rets.empty()) goto done;
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
                if (i.events.size() > 1) co_await WaitAny{&i.events};
                else co_await Wait{i.sig, i.edge};
                pc++; break;
            case IK::WaitDelay: co_await Delay{i.delay}; pc++; break;
            case IK::Display:
                S.printSys(i.fmt, i.args, !i.nonl);
                if (S.finished) goto done;  // task ran; $finish already requested
                pc++;
                break;
            case IK::Strobe:
                S.strobe(i.fmt, i.args);
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Monitor:
                S.armMon(i.fmt, i.args);
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Finish:
                // Stop this process. The rest of this time slot still runs,
                // including NBA and postponed, so a $strobe/$monitor already
                // queued still prints. The next time does not start. Another
                // process already queued runs until its next system task;
                // that task itself runs (Icarus 13).
                S.finished = true;
                goto done;
            case IK::Call: {
                const Prog* callee = i.callee;
                rets.push_back({p, pc + 1});
                p = callee;
                pc = 0;
                break;
            }
            case IK::Fork: {
                auto js = make_shared<JoinState>();
                js->join = i.join;
                js->left = (int)i.branches.size();
                for (const Prog* b : i.branches) {
                    Task t = exec(b, js);
                    S.all.push_back(t.h);
                    S.active.push_back(t.h);
                }
                pc++;
                if (i.join != 2) co_await JoinWait{js};
                if (S.finished) goto done;
                break;
            }
            case IK::Ret:
                if (rets.empty()) goto done;
                {
                    RetFrame f = rets.back();
                    rets.pop_back();
                    p = f.prog;
                    pc = f.pc;
                }
                break;
        }
    }
done:
    if (joinOf) joinOf->branchEnded();
}
