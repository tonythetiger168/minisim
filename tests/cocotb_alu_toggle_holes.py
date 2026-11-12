import cocotb
from cocotb.triggers import Timer,ReadOnly
@cocotb.test()
async def result_msb(d):
    d.clk.value=0;d.reset_n.value=0;d.start.value=0;d.a.value=0;d.b.value=0;d.op.value=0
    await Timer(1,unit='ns');d.reset_n.value=1
    for a,b,op,expected in [(255,255,4,65025),(0,0,0,0)]:
        d.clk.value=0;d.start.value=1;d.a.value=a;d.b.value=b;d.op.value=op
        await Timer(1,unit='ns');d.clk.value=1;await ReadOnly()
        assert int(d.result.value)==expected and int(d.done.value)==1
        await Timer(1,unit='ns')
