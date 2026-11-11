// The mdebug viewer: one HTML page, the debug database spliced in at
// /*MDEBUG_DATA*/null. No network, no dependencies.
static const char kViewer[] = R"MDV(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>mdebug</title>
<style>
:root{--bg:#fbfbfa;--panel:#f1f1ee;--line:#d9d8d2;--fg:#1f1f1c;--dim:#75746d;--acc:#b4552d;--hi:#fde9b8;--sel:#e4ebf7;
--kw:#7a3e9d;--cm:#7d8a77;--num:#1c6b8a;--w1:#1c7c4a;--wx:#c0392b;--wz:#b58900}
@media(prefers-color-scheme:dark){:root{--bg:#1c1c1a;--panel:#252523;--line:#3a3a36;--fg:#e6e4dc;--dim:#98968d;--acc:#e08a5f;
--hi:#5a4a1c;--sel:#2c3a52;--kw:#c79be6;--cm:#7f8f78;--num:#7cc3e0;--w1:#5fd08f;--wx:#ff7b6b;--wz:#e5c04a}}
*{box-sizing:border-box}html,body{height:100%;margin:0}
body{background:var(--bg);color:var(--fg);font:13px/1.45 system-ui,sans-serif;display:grid;
grid-template:"top top top top" auto "left src src right" 1fr "wave wave sch sch" var(--waveh,210px)/250px 1fr 220px 380px}
header{grid-area:top;display:flex;gap:12px;align-items:center;padding:6px 10px;border-bottom:1px solid var(--line);background:var(--panel)}
header b{color:var(--acc)}header input{flex:0 1 320px;padding:4px 8px;border:1px solid var(--line);border-radius:4px;background:var(--bg);color:var(--fg)}
header span{color:var(--dim)}button{font:inherit;padding:2px 8px;border:1px solid var(--line);border-radius:4px;background:var(--bg);color:var(--fg);cursor:pointer}
button:hover{border-color:var(--acc)}
#left{grid-area:left;border-right:1px solid var(--line);display:flex;flex-direction:column;min-height:0}
#src{grid-area:src;overflow:auto;min-width:0;font:12px/1.5 ui-monospace,Menlo,Consolas,monospace}
#right{grid-area:right;border-left:1px solid var(--line);overflow:auto;padding:8px 10px;min-height:0}
#wave{grid-area:wave;border-top:1px solid var(--line);display:flex;flex-direction:column;min-height:0;background:var(--panel)}
h3{margin:0;padding:5px 10px;font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--dim);background:var(--panel);border-bottom:1px solid var(--line)}
#right h3{margin:10px -10px 6px;border-top:1px solid var(--line)}#right h3:first-child{margin-top:-8px;border-top:0}
#tree,#sigs{overflow:auto;flex:1;min-height:0;padding:4px 0}#sigs{flex:1.4;border-top:1px solid var(--line)}
.row{padding:1px 10px;cursor:pointer;white-space:nowrap;display:flex;gap:8px}.row:hover{background:var(--panel)}
.row.on{background:var(--sel)}.row i{color:var(--dim);font-style:normal;margin-left:auto;font-family:ui-monospace,monospace;font-size:11px}
.row small{color:var(--dim)}
.ln{display:flex;white-space:pre}.ln>u{flex:0 0 52px;text-align:right;padding-right:10px;color:var(--dim);text-decoration:none;user-select:none}
.ln.hit>u{border-right:3px solid #3a9a5c}.ln.miss>u{border-right:3px solid #d0453a;color:#d0453a}.ln.drv{background:var(--hi)}.ln.load{background:var(--sel)}.ln.cur{outline:1px solid var(--acc)}
.id{cursor:pointer;border-bottom:1px dotted var(--dim)}.id:hover,.id.on{background:var(--sel);color:var(--acc)}
.id em{font-style:normal;color:var(--w1);font-size:10px;margin-left:2px}
.kw{color:var(--kw)}.cm{color:var(--cm)}.nu{color:var(--num)}
.fname{position:sticky;top:0;background:var(--panel);border-bottom:1px solid var(--line);padding:3px 10px;font-family:system-ui;color:var(--dim);z-index:1}
.d{padding:4px 6px;border:1px solid var(--line);border-radius:4px;margin-bottom:5px;cursor:pointer;background:var(--bg)}
.d:hover{border-color:var(--acc)}.d code{display:block;font:11.5px ui-monospace,monospace;white-space:pre-wrap;word-break:break-all}
.d small{color:var(--dim)}.d .w{color:var(--kw)}
.d.act{border-color:var(--w1);border-width:2px}.d.off{opacity:.5}
.tr{font:11.5px ui-monospace,monospace;white-space:nowrap;overflow-x:auto;padding-bottom:4px}.tr div{cursor:pointer;padding:1px 0}.tr div:hover{color:var(--acc)}
.tr .a{color:var(--dim)}.tr .v{color:var(--w1)}.tr .q{color:var(--wz)}
#name{font:600 13px ui-monospace,monospace;word-break:break-all}#meta{color:var(--dim)}
#schp{grid-area:sch;border-top:1px solid var(--line);border-left:1px solid var(--line);overflow:auto;min-height:0;min-width:0}#sch{padding:6px 10px}
svg{display:block}svg text{font:11px ui-monospace,monospace;fill:var(--fg)}
svg .n rect{fill:var(--bg);stroke:var(--dim)}svg .n.me rect{stroke:var(--acc);stroke-width:2}svg .n{cursor:pointer}
svg .b rect{fill:var(--panel);stroke:var(--fg)}svg .b text.bn{fill:var(--fg);font-weight:600}svg text.net{cursor:pointer;fill:var(--acc)}svg .b text{fill:var(--dim)}svg .b{cursor:pointer}
svg path{fill:none;stroke:var(--dim)}svg .st circle{fill:var(--bg);stroke:var(--fg)}svg .st.me circle{stroke:var(--acc);stroke-width:2.5}svg .st.rs circle{stroke-dasharray:4 2}svg .st text{text-anchor:middle}
svg path.tr{cursor:pointer;stroke:var(--fg);stroke-width:1.6}svg path.tr.off{stroke:var(--dim);opacity:.45}svg path.tr.on{stroke:var(--acc)}svg marker path{fill:var(--dim);stroke:none}
svg path.c{stroke-dasharray:3 3}svg .tri{fill:none;stroke:var(--fg)}
#wbar{display:flex;gap:8px;align-items:center;padding:3px 10px;border-bottom:1px solid var(--line);color:var(--dim)}
#wc{flex:1;min-height:0;position:relative;overflow-y:auto}canvas{display:block;width:100%}
.none{color:var(--dim);padding:4px 0}
</style></head><body>
<header><b>mdebug</b><input id="q" placeholder="find a signal (tb.u.q)" list="ql"><datalist id="ql"></datalist>
<span id="stat"></span><span style="margin-left:auto" id="tcur"></span></header>
<div id="left"><h3>Hierarchy</h3><div id="tree"></div><h3>Signals</h3><div id="sigs"></div></div>
<div id="src"></div>
<div id="right"><h3>Signal</h3><div id="name" class="none">Click a signal in the source or the list.</div><div id="meta"></div>
<h3>Active driver at the cursor</h3><div id="act"><div class="none">With a waveform: the assignment that gave the signal its value, and what caused it.</div></div>
<h3>Drivers</h3><div id="drv"></div><h3>Loads</h3><div id="lod"></div></div>
<div id="schp"><h3>Schematic</h3><div id="sch"><div class="none">Select a signal to draw its drivers and loads.</div></div></div>
<div id="wave"><div id="wbar"><span>Waves</span><button id="prev" title="previous change of the selected signal">&#9664; change</button>
<button id="next" title="next change of the selected signal">change &#9654;</button><button id="cone" title="add the inputs of the selected signal">+ inputs</button>
<button id="clr">clear</button><button id="fit">fit</button><span id="winfo"></span></div><div id="wc"><canvas id="cv"></canvas></div></div>
<script>
const DB=/*MDEBUG_DATA*/null;
const $=id=>document.getElementById(id),esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
// Served live (mdebug -serve): a signal's waves are fetched the first time they are looked at.
const LIVE=DB.live?new Proxy({},{get(t,k){if(typeof k!='string'||!/^\d+$/.test(k))return undefined;
  if(!(k in t)){try{const x=new XMLHttpRequest;x.open('GET','wave?i='+k,false);x.send();t[k]=x.status==200?JSON.parse(x.responseText):null}catch(e){t[k]=null}}return t[k]||undefined}}):null;
const S=DB.signals,D=DB.drivers,W=LIVE||DB.waves||null,END=DB.end||0;
const byPath=new Map(S.map((s,i)=>[s.path,i])),drvOf=S.map(()=>[]),loadOf=S.map(()=>[]);
D.forEach((d,i)=>{drvOf[d.sig].push(i);new Set([...d.reads,...d.ctl]).forEach(s=>loadOf[s].push(i))});
const scopeOf=p=>p.slice(0,p.lastIndexOf('.')),leaf=p=>p.slice(p.lastIndexOf('.')+1);
const sigsIn=new Map();S.forEach((s,i)=>{const k=scopeOf(s.path);(sigsIn.get(k)||sigsIn.set(k,[]).get(k)).push(i)});
const st={scope:DB.scopes.length?DB.scopes[0].path:'',sig:-1,file:-1,t:END,rows:[],t0:0,t1:Math.max(END,1)};
const KW=new Set('module endmodule input output inout logic wire reg assign always always_ff always_comb always_latch initial begin end if else case casez casex endcase default for while posedge negedge or parameter localparam generate endgenerate genvar function endfunction task endtask int bit integer typedef enum struct packed signed unsigned interface endinterface package endpackage import return'.split(' '));
function hex(b){if(b.length==1)return b;if(/[xz]/.test(b))return /^x+$/.test(b)?'x':/^z+$/.test(b)?'z':b.length+"'b"+b;
  let h='';for(let i=b.length;i>0;i-=4)h=parseInt(b.slice(Math.max(0,i-4),i),2).toString(16)+h;return h}
function chAt(i,t){const c=W&&W[i];if(!c||!c.length)return -1;let lo=0,hi=c.length;while(lo<hi){const m=(lo+hi)>>1;if(c[m][0]<=t)lo=m+1;else hi=m}return lo-1}
function val(i,t){const k=chAt(i,t);return k<0?null:hex(W[i][k][1])}
// ---- the active-driver trace: conditions evaluated on waveform values.
// A value is {w,v,s}; anything with x or z in it is null (undecided).
const M=w=>(1n<<BigInt(w))-1n,mk=(w,v,s)=>({w,v:v&M(w),s:!!s}),sg=x=>x.s&&(x.v>>BigInt(x.w-1))&1n?x.v-(1n<<BigInt(x.w)):x.v,b1=c=>mk(1,c?1n:0n,0);
function bitsAt(i,t){const k=chAt(i,t);return k<0?null:W[i][k][1]}
function ev(e,t){if(!e)return null;switch(e[0]){
  case's':{const b=bitsAt(e[1],t);return !b||/[xz]/.test(b)?null:mk(b.length,BigInt('0b'+b),e[2])}
  case'c':return /[xz]/.test(e[1])?null:mk(e[1].length,BigInt('0b'+e[1]),e[2]);
  case'cv':{const v=ev(e[1],t);if(!v)return null;v.s=e[3];return mk(e[2],sg(v),e[4])}
  case'u':{const v=ev(e[2],t);if(!v)return null;const all=v.v==M(v.w);let p=0n;for(let x=v.v;x;x>>=1n)p^=x&1n;
    switch(e[1]){case'Plus':return v;case'Minus':return mk(v.w,-v.v,v.s);case'BitwiseNot':return mk(v.w,~v.v,v.s);
    case'BitwiseAnd':return b1(all);case'BitwiseNand':return b1(!all);case'BitwiseOr':return b1(v.v);case'BitwiseNor':case'LogicalNot':return b1(!v.v);
    case'BitwiseXor':return b1(p);case'BitwiseXnor':return b1(!p)}return null}
  case'b':{const l=ev(e[2],t),r=ev(e[3],t);if(!l||!r)return null;const w=Math.max(l.w,r.w),s=l.s&&r.s,a=s?sg(l):l.v,b=s?sg(r):r.v;
    switch(e[1]){case'Add':return mk(w,a+b,s);case'Subtract':return mk(w,a-b,s);case'Multiply':return mk(w,a*b,s);
    case'Divide':return b?mk(w,a/b,s):null;case'Mod':return b?mk(w,a%b,s):null;
    case'BinaryAnd':return mk(w,l.v&r.v,s);case'BinaryOr':return mk(w,l.v|r.v,s);case'BinaryXor':return mk(w,l.v^r.v,s);case'BinaryXnor':return mk(w,~(l.v^r.v),s);
    case'Equality':case'CaseEquality':return b1(a==b);case'Inequality':case'CaseInequality':return b1(a!=b);
    case'GreaterThanEqual':return b1(a>=b);case'GreaterThan':return b1(a>b);case'LessThanEqual':return b1(a<=b);case'LessThan':return b1(a<b);
    case'LogicalAnd':return b1(l.v&&r.v);case'LogicalOr':return b1(l.v||r.v);
    case'LogicalShiftLeft':case'ArithmeticShiftLeft':return r.v>4096n?mk(l.w,0n,l.s):mk(l.w,l.v<<r.v,l.s);
    case'LogicalShiftRight':return mk(l.w,r.v>4096n?0n:l.v>>r.v,l.s);
    case'ArithmeticShiftRight':return mk(l.w,r.v>4096n?(sg(l)<0n?-1n:0n):(l.s?sg(l):l.v)>>r.v,l.s)}return null}
  case'?:':{const c=ev(e[1],t);if(!c)return null;return ev(c.v?e[2]:e[3],t)}
  case'bit':{const v=ev(e[1],t),i=ev(e[2],t);if(!v||!i)return null;const n=Number(sg(i)),p=e[3]>=e[4]?n-e[4]:e[4]-n;return p<0||p>=v.w?null:b1((v.v>>BigInt(p))&1n)}
  case'rng':{const v=ev(e[1],t),A=ev(e[2],t),B=ev(e[3],t);if(!v||!A||!B)return null;let hi=Number(sg(A)),lo=Number(sg(B));const desc=e[5]>=e[6];
    if(e[4]==1){lo=Number(sg(A));hi=lo+Number(B.v)-1;if(!desc)[hi,lo]=[lo,hi]}else if(e[4]==2){hi=Number(sg(A));lo=hi-Number(B.v)+1;if(!desc)[hi,lo]=[lo,hi]}
    const ph=desc?hi-e[6]:e[6]-hi,pl=desc?lo-e[6]:e[6]-lo;return pl<0||ph>=v.w||ph<pl?null:mk(ph-pl+1,v.v>>BigInt(pl),0)}
  case'fld':{const v=ev(e[1],t);return !v||e[2]+e[3]>v.w?null:mk(e[3],v.v>>BigInt(e[2]),e[4])}
  case'cat':{let w=0,x=0n;for(const o of e[1]){const v=ev(o,t);if(!v)return null;x=(x<<BigInt(v.w))|v.v;w+=v.w}return mk(w,x,0)}
  case'rep':{const n=ev(e[1],t),v=ev(e[2],t);if(!n||!v||n.v>4096n)return null;let w=0,x=0n;for(let k=0n;k<n.v;k++){x=(x<<BigInt(v.w))|v.v;w+=v.w}return w?mk(w,x,0):null}}
  return null}
function itemHit(c,j,sel,t){for(const e of c.items[j]){
    const k=e&&e[0]=='cv'&&e[1][0]=='c'?e[1]:e;
    if(c.k!='n'&&k&&k[0]=='c'&&/[xz]/.test(k[1])){const pat=k[1].padStart(sel.w,'0').slice(-sel.w),b=sel.v.toString(2).padStart(sel.w,'0');
      let m=true;for(let i=0;i<sel.w;i++){const p=pat[i];if(p=='z'||(p=='x'&&c.k=='x'))continue;if(p!=b[i]){m=false;break}}if(m)return true;continue}
    const v=ev(e,t);if(!v)return null;if((v.s&&sel.s?sg(v)==sg(sel):v.v==sel.v))return true}return false}
function holds(g,t){if(g[0]=='if'||g[0]=='else'){const v=ev(g[1],t);if(!v)return null;return g[0]=='if'?!!v.v:!v.v}
  if(g[0]=='case'){const c=DB.cases[g[1]];if(c.k=='?')return null;const sel=ev(c.sel,t);if(!sel)return null;const upto=g[2]<0?c.items.length:g[2];
    for(let j=0;j<upto;j++){const m=itemHit(c,j,sel,t);if(m===null)return null;if(m)return false}return g[2]<0?true:itemHit(c,g[2],sel,t)}
  return null}
// The drivers of sig that ran for its value at time t: [{d,state,ts}], state 1 yes, -1 undecided.
// pt: [lo,width,name], the bits of sig that matter (a struct field that was read).
function activeOf(sig,t,pt){const k=chAt(sig,t);if(k<0)return {tc:0,hits:[]};const seen=W[sig][k][1];
  if(pt&&pt[0]==0&&pt[1]>=seen.length)pt=null;const cut=j=>{const b=W[sig][j][1];return b.slice(Math.max(0,b.length-pt[0]-pt[1]),b.length-pt[0])};
  let pi=k;if(pt)while(pi>0&&cut(pi-1)==cut(pi))pi--;const tc=W[sig][pi][0];
  let hits=drvOf[sig].map(i=>{const d=D[i],ts=d.seq&&tc>0?tc-1:tc;let st=1;for(const g of d.g){const h=holds(g,ts);if(h===null){if(st==1)st=-1}else if(!h){st=0;break}}return {d:i,state:st,ts}});
  if(!/[xz]/.test(seen)){const want=BigInt('0b'+seen);let match=false;hits.forEach(h=>{if(!h.state||!D[h.d].rhs)return;const v=ev(D[h.d].rhs,h.ts);if(!v)return;
    if(mk(seen.length,sg(v),0).v==want)match=true;else h.diff=1});if(match)hits.forEach(h=>{if(h.diff)h.state=0})}
  const cov=n=>{const a=D[hits[n].d];if(a.blk<0)return false;const later=[];for(let j=n+1;j<hits.length;j++){const b=D[hits[j].d];if(hits[j].state!=1||b.blk!=a.blk)continue;
    if(b.whole)return true;if(b.lo>=0)later.push([b.lo,b.lo+b.w]);else if(a.lo<0&&!a.whole)return true}
    if(a.lo<0)return false;later.sort((x,y)=>x[0]-y[0]);let at=a.lo;for(const[l,e]of later){if(l>at)break;at=Math.max(at,e)}return at>=a.lo+a.w};
  hits=hits.filter((h,n)=>{const d=D[h.d];return h.state&&!(h.state==1&&cov(n))&&!(pt&&d.lo>=0&&(d.lo+d.w<=pt[0]||d.lo>=pt[0]+pt[1]))});return {tc,hits,pt,v:pt?cut(k):seen}}
function why(sig,t,depth,ind,seen,out,pt){const pad='&nbsp;'.repeat(ind*2),k=chAt(sig,t);let nm=esc(S[sig].path);
  if(!W[sig]){out.push(`<div data-s="${sig}">${pad}${nm} <span class="a">(not in the waveform)</span></div>`);return}
  const a=activeOf(sig,t,pt);if(a.pt)nm+=esc(a.pt[2]);out.push(`<div data-s="${sig}" data-t="${a.tc}">${pad}${nm} = <span class="v">${k<0?'x':hex(a.v)}</span> <span class="a">@${t}${a.tc!=t?' (since '+a.tc+')':''}</span></div>`);
  if(!drvOf[sig].length){out.push(`<div class="a">${pad}&nbsp;&nbsp;no driver: an input or a testbench value</div>`);return}
  const key=sig+'@'+a.tc+(a.pt?'#'+a.pt[0]+':'+a.pt[1]:'');if(seen.has(key)){out.push(`<div class="a">${pad}&nbsp;&nbsp;(shown above)</div>`);return}seen.add(key);
  if(!a.hits.length)out.push(`<div class="a">${pad}&nbsp;&nbsp;no assignment's conditions hold at ${a.tc}</div>`);
  a.hits.forEach(h=>{const d=D[h.d],f=DB.files[d.file];
    out.push(`<div data-d="${h.d}">${pad}&nbsp;&nbsp;<span class="${h.state==1?'v':'q'}">${h.state==1?'&larr;':'??'}</span> ${esc(d.text)} <span class="a">${esc(f?f.name.replace(/.*\//,''):'?')}:${d.line}${d.when?' when '+esc(d.when):''}</span></div>`);
    if(depth<=0)return;const follow=d.reads.slice();
    if(!d.seq)d.ctl.forEach(c=>{const kk=chAt(c,a.tc);if(kk>=0&&W[c][kk][0]==a.tc&&a.tc>0&&!follow.includes(c))follow.push(c)});
    follow.forEach(r=>{if(!d.seq&&W[r]&&follow.length>1){const kk=chAt(r,a.tc);if(kk<0||W[r][kk][0]!=a.tc){out.push(`<div data-s="${r}" data-t="${a.tc}" class="a">${pad}&nbsp;&nbsp;&nbsp;&nbsp;${esc(S[r].path)} = ${val(r,a.tc)} (unchanged)</div>`);return}}
      why(r,h.ts,depth-1,ind+2,seen,out,d.part&&d.part[r])})})}
function tree(){const kids=new Map();DB.scopes.forEach(s=>{const p=s.path.includes('.')?scopeOf(s.path):'';(kids.get(p)||kids.set(p,[]).get(p)).push(s)});
  let h='';const walk=(p,dep)=>(kids.get(p)||[]).forEach(s=>{h+=`<div class="row${s.path==st.scope?' on':''}" data-p="${esc(s.path)}" style="padding-left:${10+dep*14}px">${esc(leaf(s.path))} <small>${esc(s.def)}</small></div>`;walk(s.path,dep+1)});
  walk('',0);$('tree').innerHTML=h}
function sigList(){const l=sigsIn.get(st.scope)||[];$('sigs').innerHTML=l.map(i=>{const v=W?val(i,st.t):null;
  return `<div class="row${i==st.sig?' on':''}" data-s="${i}">${esc(leaf(S[i].path))}<i>${v===null?esc(S[i].type):v}</i></div>`}).join('')||'<div class="row none">no signals here</div>'}
function source(scroll){const sc=DB.scopes.find(s=>s.path==st.scope);const f=st.file>=0?st.file:(sc?sc.file:-1);
  if(f<0||!DB.files[f]){$('src').innerHTML='<div class="fname">no source</div>';return}
  st.file=f;const drv=new Set,lod=new Set,CL=DB.cov&&DB.cov.lines[f];
  if(st.sig>=0){drvOf[st.sig].forEach(i=>D[i].file==f&&drv.add(D[i].line));loadOf[st.sig].forEach(i=>D[i].file==f&&lod.add(D[i].line))}
  let lo=1,hi=1e9;if(sc&&sc.file==f){lo=sc.line;const L=DB.files[f].text.split('\n');for(let k=lo;k<=L.length;k++)if(/\bend(module|interface|program)\b/.test(L[k-1])){hi=k;break}}
  const pre=st.scope+'.';let h=`<div class="fname">${esc(DB.files[f].name)} &middot; scope ${esc(st.scope)}</div>`;
  DB.files[f].text.split('\n').forEach((ln,n)=>{let code=ln,cm='';const c=ln.indexOf('//');if(c>=0){code=ln.slice(0,c);cm=ln.slice(c)}
    let o='',p=0;code.replace(/[A-Za-z_][A-Za-z0-9_$]*|\d[\w']*/g,(m,at)=>{o+=esc(code.slice(p,at));p=at+m.length;
      const s=n+1>=lo&&n+1<=hi?byPath.get(pre+m):undefined;
      if(/^\d/.test(m))o+=`<span class="nu">${esc(m)}</span>`;else if(KW.has(m))o+=`<span class="kw">${m}</span>`;
      else if(s!==undefined){const v=W?val(s,st.t):null;o+=`<span class="id${s==st.sig?' on':''}" data-s="${s}">${m}${v===null?'':`<em>${v}</em>`}</span>`}
      else o+=esc(m);return m});
    o+=esc(code.slice(p))+(cm?`<span class="cm">${esc(cm)}</span>`:'');
    const cv=CL&&CL[n+1];h+=`<div class="ln${drv.has(n+1)?' drv':lod.has(n+1)?' load':''}${n+1==scroll?' cur':''}${cv===undefined?'':cv?' hit':' miss'}" id="L${n+1}"${cv===undefined?'':` title="${cv?'line ran '+cv+(cv==1?' time':' times'):'line never ran'}"`}><u>${n+1}</u><span>${o}</span></div>`});
  $('src').innerHTML=h;if(scroll){const e=$('L'+scroll);e&&e.scrollIntoView({block:'center'})}}
function card(i,showT){const d=D[i],f=DB.files[d.file];return `<div class="d" data-d="${i}"><code>${esc(d.text)}</code><small>${esc(f?f.name:'?')}:${d.line} &middot; ${esc(d.kind)}${showT?' &rarr; '+esc(S[d.sig].path):''}</small>${d.when?`<small><br>when <span class="w">${esc(d.when)}</span></small>`:''}</div>`}
function panel(){if(st.sig<0)return;const s=S[st.sig],v=W?val(st.sig,st.t):null;$('name').className='';$('name').textContent=s.path;
  $('meta').textContent=s.type+(v===null?'':'  =  '+v+'  @'+st.t)+'  ·  '+(DB.files[s.file]?DB.files[s.file].name:'?')+':'+s.line+(DB.cov&&DB.cov.stuck[st.sig]?'  ·  coverage: '+DB.cov.stuck[st.sig]:'');
  let act=null;if(W&&W[st.sig]){act=activeOf(st.sig,st.t);const out=[];why(st.sig,st.t,4,0,new Set,out);$('act').innerHTML='<div class="tr">'+out.join('')+'</div>'}
  else $('act').innerHTML='<div class="none">'+(W?'This signal is not in the waveform.':'Run mdebug with -vcd to see it.')+'</div>';
  const cls=i=>!act?'':act.hits.some(h=>h.d==i&&h.state==1)?' act':act.hits.some(h=>h.d==i)?'':' off';
  $('drv').innerHTML=drvOf[st.sig].map(i=>card(i).replace('class="d"','class="d'+cls(i)+'"')).join('')||'<div class="none">No driver: an input or a testbench value.</div>';
  $('lod').innerHTML=loadOf[st.sig].map(i=>card(i,1)).join('')||'<div class="none">Nothing reads it.</div>';fsmOf.has(st.sig)?fsmView():schematic()}
// Inputs -> assignments -> the signal -> assignments that read it -> what they drive.
function schematic(){const me=st.sig,dv=drvOf[me].slice(0,12),ld=loadOf[me].slice(0,12);
  const ins=[],outs=[];dv.forEach(i=>[...D[i].reads,...D[i].ctl].forEach(s=>ins.includes(s)||ins.push(s)));ld.forEach(i=>outs.includes(D[i].sig)||outs.push(D[i].sig));
  const X=[4,128,238,362,472],WN=104,WB=90,H=22,G=8,rows=Math.max(ins.length,dv.length,ld.length,outs.length,1),ht=rows*(H+G)+G;
  const y=(k,n)=>G+(ht-2*G-n*(H+G)+G)/2+k*(H+G);let o='';
  const cut=(t,n)=>t.length>n?'…'+t.slice(t.length-n+1):t;
  const node=(s,x,yy,cls)=>`<g class="n ${cls||''}" data-s="${s}"><title>${esc(S[s].path)}</title><rect x="${x}" y="${yy}" width="${WN}" height="${H}" rx="10"/><text x="${x+8}" y="${yy+15}">${esc(cut(S[s].path,15))}</text></g>`;
  const box=(i,x,yy)=>{const d=D[i],f=DB.files[d.file];return `<g class="b" data-d="${i}"><title>${esc(d.text)}</title><rect x="${x}" y="${yy}" width="${WB}" height="${H}"/>${d.seq?`<path class="tri" d="M${x} ${yy+H-9}l7 4l-7 4"/>`:''}<text x="${x+10}" y="${yy+15}">${esc(cut((f?f.name.replace(/.*\//,''):'?')+':'+d.line,12))}</text></g>`};
  const wire=(x1,y1,x2,y2,c)=>`<path ${c?'class="c"':''} d="M${x1} ${y1+H/2}C${(x1+x2)/2} ${y1+H/2} ${(x1+x2)/2} ${y2+H/2} ${x2} ${y2+H/2}"/>`;
  const ym=y(0,1);
  dv.forEach((i,k)=>{const yb=y(k,dv.length);D[i].reads.forEach(s=>o+=wire(X[0]+WN,y(ins.indexOf(s),ins.length),X[1],yb));
    D[i].ctl.forEach(s=>D[i].reads.includes(s)||(o+=wire(X[0]+WN,y(ins.indexOf(s),ins.length),X[1],yb,1)));o+=wire(X[1]+WB,yb,X[2],ym)+box(i,X[1],yb)});
  ld.forEach((i,k)=>{const yb=y(k,ld.length);o+=wire(X[2]+WN,ym,X[3],yb,!D[i].reads.includes(me))+wire(X[3]+WB,yb,X[4],y(outs.indexOf(D[i].sig),outs.length))+box(i,X[3],yb)});
  ins.forEach((s,k)=>o+=node(s,X[0],y(k,ins.length)));outs.forEach((s,k)=>o+=node(s,X[4],y(k,outs.length)));o+=node(me,X[2],ym,'me');
  $('sch').innerHTML=`<svg viewBox="0 0 ${X[4]+WN+4} ${ht}" width="${X[4]+WN+4}">${o}</svg><div class="none">Solid: data. Dashed: a condition. Click a signal to move the view.</div>`}
// A state register: its states in a ring, the transitions the source has,
// the state at the cursor, and (with waves) which transitions ran.
const fsmOf=new Map((DB.fsms||[]).map(f=>[f.sig,f]));
function fsmView(){const f=fsmOf.get(st.sig),n=f.states.length,R=Math.max(60,n*17),cx=R+60,cy=R+30,pos=new Map,nm=new Map(f.states.map(s=>[s[0],s[1]]));
  f.states.forEach((s,k)=>{const a=-Math.PI/2+2*Math.PI*k/n;pos.set(s[0],[cx+R*Math.cos(a),cy+R*Math.sin(a)])});
  const c=W&&W[st.sig]||[],num=b=>/[xz]/.test(b)?-1:Number(BigInt('0b'+b)),took=new Map;
  for(let k=1;k<c.length;k++){const key=num(c[k-1][1])+'>'+num(c[k][1]);took.set(key,(took.get(key)||0)+1)}
  const k=chAt(st.sig,st.t),cur=k<0?-1:num(c[k][1]),cut=(t,m)=>t.length>m?t.slice(0,m-1)+'…':t;
  let o='<defs><marker id="ah" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0L8 4L0 8z"/></marker></defs>';
  f.trans.forEach(t=>{const a=pos.get(t[0]),b=pos.get(t[1]);if(!a||!b)return;const dx=b[0]-a[0],dy=b[1]-a[1],L=Math.hypot(dx,dy)||1,ux=dx/L,uy=dy/L,r=19,
    x1=a[0]+ux*r,y1=a[1]+uy*r,x2=b[0]-ux*r,y2=b[1]-uy*r,mx=(x1+x2)/2-uy*L*.14,my=(y1+y2)/2+ux*L*.14,nn=took.get(t[0]+'>'+t[1])||0;
    o+=`<path class="tr${W&&W[st.sig]?(nn?' on':' off'):''}${t[2]?' c':''}" data-f="${t[3]}" data-l="${t[4]}" marker-end="url(#ah)" d="M${x1} ${y1}Q${mx} ${my} ${x2} ${y2}"><title>${esc(nm.get(t[0]))} → ${esc(nm.get(t[1]))}${t[2]?' (reset)':''}${W&&W[st.sig]?', taken '+nn+(nn==1?' time':' times'):''}</title></path>`});
  f.states.forEach(s=>{const[x,y]=pos.get(s[0]);o+=`<g class="st${s[0]==cur?' me':''}${s[2]?' rs':''}"><title>${esc(s[1])} = ${s[0]}${s[2]?' (reset state)':''}</title><circle cx="${x}" cy="${y}" r="19"/><text x="${x}" y="${y+4}">${esc(cut(s[1],6))}</text></g>`});
  const bad=cur>=0&&!nm.has(cur);
  $('sch').innerHTML=`<svg viewBox="0 0 ${2*cx} ${2*cy}" width="${2*cx}">${o}</svg><div class="none">State machine ${esc(leaf(S[st.sig].path))}${cur>=0?': '+esc(nm.get(cur)||String(cur))+' at '+st.t:''}${bad?' (not a state)':''}. ${W&&W[st.sig]?'Bright: a transition that ran. Faint: never taken. ':''}Dashed: reset. Click an arrow for its line.</div>`}
function blocks(){const P=st.scope,ch=DB.scopes.filter(s=>s.path.includes('.')&&scopeOf(s.path)==P);
  if(!ch.length){$('sch').innerHTML='<div class="none">No instances inside '+esc(P)+'. Select a signal to draw its drivers and loads.</div>';return}
  const ix=new Map(ch.map((s,k)=>[s.path,k])),B=ch.map(s=>({s,ins:[],outs:[],lv:0})),src=new Map,use=new Map;
  D.forEach(d=>{if(d.kind!='port')return;const ts=scopeOf(S[d.sig].path);
    if(ix.has(ts)){if(d.reads.some(r=>S[r].path.startsWith(ts+'.')))return;const nets=d.reads.filter(r=>scopeOf(S[r].path)==P);B[ix.get(ts)].ins.push({p:d.sig,nets});nets.forEach(n=>(use.get(n)||use.set(n,[]).get(n)).push(ix.get(ts)))}
    else if(ts==P)d.reads.forEach(r=>{const b=ix.get(scopeOf(S[r].path));if(b===undefined)return;let o=B[b].outs.find(o=>o.p==r);if(!o)B[b].outs.push(o={p:r,nets:[]});o.nets.push(d.sig);src.set(d.sig,{b,p:r})})});
  for(let pass=0;pass<B.length;pass++){let ch2=0;B.forEach((b,k)=>b.ins.forEach(i=>i.nets.forEach(n=>{const f=src.get(n);if(f&&f.b!=k&&B[f.b].lv+1>b.lv&&B[f.b].lv+1<B.length){b.lv=B[f.b].lv+1;ch2=1}})));if(!ch2)break}
  const BW=120,GX=150,PH=15,LM=130,top=[],pos=new Map;let o='',H=0,Wd=0;
  B.forEach((b,k)=>{const h=Math.max(b.ins.length,b.outs.length,1)*PH+24,x=LM+b.lv*(BW+GX),y=(top[b.lv]||8);top[b.lv]=y+h+14;b.x=x;b.y=y;b.h=h;H=Math.max(H,y+h+8);Wd=Math.max(Wd,x+BW+LM);
    b.ins.forEach((i,n)=>pos.set(i.p,[x,y+26+n*PH]));b.outs.forEach((q,n)=>pos.set(q.p,[x+BW,y+26+n*PH]))});
  const cut=(t,n)=>t.length>n?t.slice(0,n-1)+'…':t,lab=(s,x,y,a)=>`<text class="net" data-s="${s}" x="${x}" y="${y+3}" text-anchor="${a}"><title>${esc(S[s].path)}</title>${esc(cut(leaf(S[s].path),17))}</text>`;
  B.forEach((b,k)=>{b.ins.forEach(i=>{const[x,y]=pos.get(i.p);let lone=!i.nets.length;i.nets.forEach(n=>{const f=src.get(n);if(f){const[a,c]=pos.get(f.p);o+=`<path d="M${a} ${c}C${a+GX/2} ${c} ${x-GX/2} ${y} ${x} ${y}"><title>${esc(S[n].path)}</title></path>`}else lone=n+1});
      if(lone)o+=`<path d="M${x-10} ${y}H${x}"/>`+(lone===true?'':lab(lone-1,x-13,y,'end'))});
    b.outs.forEach(q=>{const[x,y]=pos.get(q.p),n=q.nets.find(n=>!(use.get(n)||[]).length);o+=n===undefined?(q.nets.length?lab(q.nets[0],x+4,y-5,'start'):''):`<path d="M${x} ${y}h10"/>`+lab(n,x+13,y,'start')});
    o+=`<g class="b" data-p="${esc(b.s.path)}"><rect x="${b.x}" y="${b.y}" width="${BW}" height="${b.h}"/><text class="bn" x="${b.x+BW/2}" y="${b.y+13}" text-anchor="middle">${esc(cut(leaf(b.s.path)+' : '+b.s.def,19))}</text>`+
      b.ins.map(i=>`<text x="${b.x+4}" y="${pos.get(i.p)[1]+3}">${esc(cut(leaf(S[i.p].path),8))}</text>`).join('')+b.outs.map(q=>`<text x="${b.x+BW-4}" y="${pos.get(q.p)[1]+3}" text-anchor="end">${esc(cut(leaf(S[q.p].path),8))}</text>`).join('')+'</g>'});
  $('sch').innerHTML=`<svg viewBox="0 0 ${Wd} ${H}" width="${Wd}">${o}</svg><div class="none">Instances inside ${esc(P)}. Click a block to go in, a net to select it.</div>`}
function waves(){const cv=$('cv'),RH=22,NW=200,dpr=window.devicePixelRatio||1,w=$('wc').clientWidth,h=Math.max(st.rows.length*RH+20,40);
  cv.width=w*dpr;cv.height=h*dpr;cv.style.height=h+'px';const g=cv.getContext('2d');g.scale(dpr,dpr);
  const css=n=>getComputedStyle(document.body).getPropertyValue(n).trim();g.font='11px ui-monospace,monospace';g.textBaseline='middle';
  const X=t=>NW+(t-st.t0)/(st.t1-st.t0)*(w-NW-6);
  if(!W){g.fillStyle=css('--dim');g.fillText('No waveform: run mdebug with -vcd <file> to link waves to the source.',10,18);return}
  st.rows.forEach((i,r)=>{const y=r*RH+2,c=W[i]||[];g.fillStyle=i==st.sig?css('--acc'):css('--fg');
    const nm=S[i].path;g.fillText(nm.length>30?'…'+nm.slice(-29):nm,6,y+RH/2);
    for(let k=Math.max(0,chAt(i,st.t0));k<c.length&&c[k][0]<=st.t1;k++){const a=Math.max(X(c[k][0]),NW),b=Math.min(k+1<c.length?X(c[k+1][0]):X(st.t1),w-6),v=c[k][1];
      if(b<=a)continue;g.strokeStyle=/x/.test(v)?css('--wx'):/z/.test(v)?css('--wz'):css('--w1');g.beginPath();
      if(v.length==1){const yy=v=='1'?y+4:v=='0'?y+RH-4:y+RH/2;g.moveTo(a,yy);g.lineTo(b,yy);if(k+1<c.length&&b<w-6){const n=c[k+1][1];g.lineTo(b,n=='1'?y+4:n=='0'?y+RH-4:y+RH/2)}}
      else{g.rect(a,y+4,b-a,RH-8);if(b-a>14){g.fillStyle=css('--fg');const t=hex(v);g.fillText(t.length*6.6>b-a-6?'…':t,a+3,y+RH/2)}}
      g.stroke()}});
  const cx=X(st.t);if(cx>=NW){g.strokeStyle=css('--acc');g.beginPath();g.moveTo(cx,0);g.lineTo(cx,h);g.stroke()}
  g.fillStyle=css('--dim');g.fillText(st.t0+'',NW,h-8);const e=st.t1+' '+(DB.timescale||'');g.fillText(e,w-8-g.measureText(e).width,h-8);
  $('winfo').textContent=st.rows.length?'click to move the cursor, wheel to zoom':'select a signal to add it'}
function setT(t){st.t=Math.max(0,Math.min(END,Math.round(t)));$('tcur').textContent=W?'t = '+st.t+' '+(DB.timescale||''):'';sigList();source();panel();waves()}
function pick(i,jump){st.sig=i;st.scope=scopeOf(S[i].path);if(W&&W[i]&&!st.rows.includes(i))st.rows.push(i);
  const sc=DB.scopes.find(s=>s.path==st.scope);st.file=sc?sc.file:S[i].file;tree();sigList();
  const d=drvOf[i].find(k=>D[k].file==st.file);source(jump||(d!==undefined?D[d].line:S[i].file==st.file?S[i].line:0));panel();waves()}
function goDriver(i){const d=D[i],own=scopeOf(S[d.sig].path);
  // A port connection is written in the parent's file.
  const sc=DB.scopes.find(s=>s.path==own&&s.file==d.file)||DB.scopes.find(s=>s.path==scopeOf(own)&&s.file==d.file);
  if(sc)st.scope=sc.path;st.file=d.file;tree();sigList();source(d.line)}
document.body.addEventListener('click',e=>{const t=e.target.closest('[data-s],[data-d],[data-p],[data-f]');if(!t)return;
  if(t.dataset.f!==undefined){st.file=+t.dataset.f;source(+t.dataset.l);return}
  if(t.dataset.s!==undefined){if(t.dataset.t!==undefined&&W){st.t=Math.max(0,Math.min(END,+t.dataset.t));$('tcur').textContent='t = '+st.t+' '+(DB.timescale||'')}pick(+t.dataset.s)}else if(t.dataset.d!==undefined)goDriver(+t.dataset.d);
  else{st.scope=t.dataset.p;st.file=-1;tree();sigList();const sc=DB.scopes.find(s=>s.path==st.scope);source(sc?sc.line:0);blocks()}});
$('q').addEventListener('change',e=>{const v=e.target.value.trim();let i=byPath.get(v);
  if(i===undefined)i=S.findIndex(s=>s.path.endsWith('.'+v));if(i===undefined||i<0)i=S.findIndex(s=>s.path.includes(v));e.target.value='';if(i>=0)pick(i)});
$('q').addEventListener('input',e=>{const v=e.target.value;if(v.length<2)return;let n=0,h='';for(const s of S){if(s.path.includes(v)){h+=`<option value="${esc(s.path)}">`;if(++n>40)break}}$('ql').innerHTML=h});
$('cv').addEventListener('click',e=>{const r=$('cv').getBoundingClientRect(),x=e.clientX-r.left;if(!W||x<200)return;setT(st.t0+(x-200)/(r.width-206)*(st.t1-st.t0))});
$('cv').addEventListener('wheel',e=>{if(!W)return;e.preventDefault();const r=$('cv').getBoundingClientRect(),f=e.deltaY>0?1.4:1/1.4,
  c=st.t0+Math.max(0,e.clientX-r.left-200)/(r.width-206)*(st.t1-st.t0);let a=c-(c-st.t0)*f,b=c+(st.t1-c)*f;a=Math.max(0,a);b=Math.min(END,b);if(b-a>=2){st.t0=Math.floor(a);st.t1=Math.ceil(b);waves()}},{passive:false});
function step(dir){if(!W||st.sig<0||!W[st.sig])return;const c=W[st.sig],k=chAt(st.sig,st.t);
  if(dir<0){const j=k>=0&&c[k][0]<st.t?k:k-1;if(j>=0)setT(c[j][0])}else if(k+1<c.length)setT(c[k+1][0])}
$('prev').onclick=()=>step(-1);$('next').onclick=()=>step(1);$('clr').onclick=()=>{st.rows=[];waves()};$('fit').onclick=()=>{st.t0=0;st.t1=Math.max(END,1);waves()};
$('cone').onclick=()=>{if(st.sig<0)return;drvOf[st.sig].forEach(i=>[...D[i].reads,...D[i].ctl].forEach(s=>W&&W[s]&&!st.rows.includes(s)&&st.rows.push(s)));waves()};
window.addEventListener('resize',waves);
$('stat').textContent=DB.scopes.length+' instances · '+S.length+' signals · '+D.length+' drivers';
tree();sigList();source();setT(DB.at===undefined?END:DB.at);blocks();
</script></body></html>
)MDV";
