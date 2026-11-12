"""Transaction functional coverage only; not line/FSM/toggle/SVA coverage."""
import argparse,hashlib,json
from pathlib import Path
CORNERS=(0,1,127,128,255)
def analyze(path):
    bins={f'op:{op}':0 for op in range(5)}
    bins.update({f'op:{op}:{operand}:{v}':0 for op in range(5) for operand in ('a','b') for v in CORNERS})
    count=0
    for line in path.read_text().splitlines():
        if not line.strip():continue
        row=json.loads(line)
        if row.get('kind')!='sample':raise ValueError('unexpected transaction kind')
        fields={}
        for part in row['payload'].split(','):
            key,value=part.split('=',1)
            if key in fields:raise ValueError('duplicate field')
            fields[key]=int(value)
        if set(fields)!={'op','a','b','result'}:raise ValueError('unexpected fields')
        op,a,b,result=(fields[x] for x in ('op','a','b','result'))
        if op not in range(5) or not 0<=a<=255 or not 0<=b<=255:raise ValueError('invalid stimulus')
        if result!={0:0,1:a+b,2:a&b,3:a^b,4:a*b}[op]:raise ValueError('incorrect ALU sample')
        count+=1;bins[f'op:{op}']+=1
        for operand,value in (('a',a),('b',b)):
            key=f'op:{op}:{operand}:{value}'
            if key in bins:bins[key]+=1
    if not count:raise ValueError('empty coverage input')
    holes=[key for key,hits in bins.items() if hits==0]
    stimuli=[]
    for key in holes:
        parts=key.split(':');item={'op':int(parts[1]),'a':0,'b':0}
        if len(parts)==4:item[parts[2]]=int(parts[3])
        if item not in stimuli:stimuli.append(item)
    return {'schema':'tinyalu-functional-v1','scope':'5 op bins plus op x individual operand corner bins; no joint a x b cross','samples':count,'total_bins':len(bins),'hit_bins':len(bins)-len(holes),'closed':not holes,'bins':bins,'holes':holes,'directed_stimuli':stimuli,'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
def main():
    p=argparse.ArgumentParser();p.add_argument('transactions',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    report=analyze(a.transactions);a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k] for k in ('samples','total_bins','hit_bins','closed')}))
if __name__=='__main__':main()
