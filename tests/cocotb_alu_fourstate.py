import cocotb,json,os
from cocotb.triggers import Timer,ReadOnly
@cocotb.test()
async def fourstate(d):
    d.clk.value=0;d.reset_n.value=0;d.start.value=0;d.a.value=0;d.b.value=0;d.op.value=0
    await Timer(1,unit='ns');d.reset_n.value=1
    rows=[]
    for bits in ('xxxxxxxx','zzzzzzzz','10xz01z0','00000000','11111111'):
        for op in (2,3,4):
            d.clk.value=0;d.a.value=bits;d.b.value=0;d.op.value=op;d.start.value=1
            await Timer(1,unit='ns')
            assert str(d.a.value).lower()==bits,'VPI input X/Z roundtrip mismatch'
            d.clk.value=1;await ReadOnly()
            actual=str(d.result.value).lower()
            expected='0'*16 if op==2 else ('0'*8+bits.replace('z','x')) if op==3 else ('x'*16 if any(c in bits for c in 'xz') else '0'*16)
            assert actual==expected,(bits,op,actual,expected)
            assert int(d.done.value)==1
            rows.append(actual)
            await Timer(1,unit='ns')
    with open(os.environ['MINISIM_TRACE_PATH'],'w') as f:json.dump(rows,f)
