import cocotb,json,os,random
from cocotb.triggers import Timer,ReadOnly
@cocotb.test()
async def alu(d):
    rng=random.Random(168);rows=[];expected=0
    for i in range(128):
        await Timer(1,unit='ns')
        reset=int(i%29!=0);start=int(i%7!=0);a=rng.randrange(256);b=rng.randrange(256);op=i%8
        d.clk.value=0;d.reset_n.value=reset;d.start.value=start;d.a.value=a;d.b.value=b;d.op.value=op
        await Timer(1,unit='ns');d.clk.value=1
        await ReadOnly()
        if not reset:expected=0
        elif start:expected={1:a+b,2:a&b,3:a^b,4:a*b}.get(op,0)
        actual=int(d.result.value);done=int(d.done.value)
        assert actual==expected and done==int(bool(reset and start)),f'ALU mismatch sample {i}'
        rows.append({'cycle':i,'result':actual,'done':done})
    assert len(rows)==128
    with open(os.environ['MINISIM_TRACE_PATH'],'w') as f:json.dump(rows,f)
