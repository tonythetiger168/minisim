import cocotb,json,os
from cocotb.triggers import Timer,ReadOnly
@cocotb.test()
async def wide_roundtrip(d):
    vectors=json.loads(open(os.environ['WIDE_VECTORS']).read());rows=[]
    d.clk.value=0;d.din.value=0
    for bits in vectors:
        d.clk.value=0;d.din.value=bits
        await Timer(1,unit='ns')
        assert str(d.din.value).lower()==bits,'64-bit input roundtrip'
        d.clk.value=1;await ReadOnly()
        actual=str(d.dout.value).lower();assert actual==bits,(actual,bits)
        rows.append(actual);await Timer(1,unit='ns')
    with open(os.environ['MINISIM_TRACE_PATH'],'w') as f:json.dump(rows,f)
