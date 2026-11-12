"""Shared real pyuvm scoreboard for cocotb/Icarus, Verilator and the Minisim counter VPI fixture."""
import random

import cocotb
from cocotb.triggers import ReadOnly, Timer
import pyuvm


class CounterScoreboard(pyuvm.uvm_component):
    def build_phase(self):
        self.expected = 0
        self.samples = 0
        self.wraps = 0
        self.resets = 0
        self.values = set()

    def sample(self, reset, actual):
        previous = self.expected
        self.expected = 0 if reset else (previous + 1) & 15
        self.samples += 1
        self.resets += bool(reset)
        self.wraps += not reset and previous == 15
        self.values.add(actual)
        assert actual == self.expected, (
            f"counter mismatch sample={self.samples} reset={reset}: "
            f"expected={self.expected} actual={actual}")

    def check_phase(self):
        assert self.samples == 96, f"incomplete run: {self.samples} samples"
        assert self.wraps >= 2, "wraparound was not exercised"
        assert self.resets >= 4, "insufficient reset checks"
        assert self.values == set(range(16)), "not all count values observed"
        self.logger.info("Scoreboard PASS: %d samples, %d wraps, %d reset samples",
                         self.samples, self.wraps, self.resets)


@pyuvm.test(timeout_time=2000, timeout_unit="ns")
class CounterTest(pyuvm.uvm_test):
    def build_phase(self):
        self.scoreboard = CounterScoreboard("scoreboard", self)

    async def run_phase(self):
        self.raise_objection()
        dut = cocotb.top
        dut.clk.value = 0
        dut.rst.value = 1
        rng = random.Random(168)
        # Two uninterrupted wraps, consecutive reset, reset after nonzero count,
        # then deterministic randomized reset timing.
        resets = [1] + [0] * 35 + [1, 1] + [0] * 20 + [1]
        resets += [int(rng.random() < 0.12) for _ in range(96 - len(resets))]
        for cycle, reset in enumerate(resets):
            dut.rst.value = reset
            await Timer(1, unit="ns")
            await ReadOnly()
            if cycle:
                # A synchronous reset must not change cnt without a rising edge.
                actual = int(dut.cnt.value)
                assert actual == self.scoreboard.expected, "counter changed without clock edge"
            await Timer(1, unit="ns")
            dut.clk.value = 1
            await Timer(1, unit="ns")
            await ReadOnly()
            assert dut.cnt.value.is_resolvable, "counter contains X/Z after clock edge"
            self.scoreboard.sample(reset, int(dut.cnt.value))
            await Timer(1, unit="ns")
            dut.clk.value = 0
        self.drop_objection()
