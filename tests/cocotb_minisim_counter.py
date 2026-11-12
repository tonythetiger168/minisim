"""Actual cocotb scheduler test for the experimental Minisim VPI host."""
import json
import os
from pathlib import Path
import cocotb
from cocotb.triggers import Timer,RisingEdge,ReadOnly

@cocotb.test()
async def counter(dut):
    rows=[];expected=0;previous=None
    async def edge_value():
        await RisingEdge(dut.clk)
        return str(dut.cnt.value)
    try:
        for i in range(96):
            dut.clk.value=0;reset=i in (0,37,38,58);dut.rst.value=int(reset)
            edge=cocotb.start_soon(edge_value())
            await Timer(5,unit='ns');dut.clk.value=1
            old=await edge
            if previous is not None:
                assert int(old,2)==previous,'RisingEdge observed downstream update too soon'
            await ReadOnly()
            expected=0 if reset else (expected+1)&15
            actual=int(dut.cnt.value)
            rows.append(dict(cycle=i,actual=actual,expected=expected))
            assert actual==expected,f'counter mismatch sample {i}: {actual} != {expected}'
            previous=actual
            await Timer(5,unit='ns')
    finally:
        Path(os.environ['MINISIM_TRACE_PATH']).write_text(json.dumps(rows,indent=2))
