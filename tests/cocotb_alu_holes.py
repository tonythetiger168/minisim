import cocotb,json,os
from cocotb.triggers import Timer,ReadOnly
@cocotb.test()
async def fill_holes(d):
    plan=json.load(open(os.environ['MINISIM_HOLES_PLAN']))
    vectors=plan['directed_stimuli'];assert vectors
    d.clk.value=0;d.reset_n.value=0;d.start.value=0;d.a.value=0;d.b.value=0;d.op.value=0
    await Timer(1,unit='ns');d.reset_n.value=1
    rows=[]
    for i,item in enumerate(vectors):
        d.clk.value=0;d.start.value=1;d.a.value=item['a'];d.b.value=item['b'];d.op.value=item['op']
        await Timer(1,unit='ns');d.clk.value=1;await ReadOnly()
        op,a,b=item['op'],item['a'],item['b'];expected={0:0,1:a+b,2:a&b,3:a^b,4:a*b}[op]
        assert int(d.result.value)==expected and int(d.done.value)==1,f'hole stimulus mismatch {i}'
        rows.append({'kind':'sample','t':i,'payload':f'op={op},a={a},b={b},result={int(d.result.value)}'})
        await Timer(1,unit='ns')
    with open(os.environ['MINISIM_DIRECTED_TX'],'w') as f:
        for row in rows:f.write(json.dumps(row)+'\n')
