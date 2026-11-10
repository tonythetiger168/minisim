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
.ln.drv{background:var(--hi)}.ln.load{background:var(--sel)}.ln.cur{outline:1px solid var(--acc)}
.id{cursor:pointer;border-bottom:1px dotted var(--dim)}.id:hover,.id.on{background:var(--sel);color:var(--acc)}
.id em{font-style:normal;color:var(--w1);font-size:10px;margin-left:2px}
.kw{color:var(--kw)}.cm{color:var(--cm)}.nu{color:var(--num)}
.fname{position:sticky;top:0;background:var(--panel);border-bottom:1px solid var(--line);padding:3px 10px;font-family:system-ui;color:var(--dim);z-index:1}
.d{padding:4px 6px;border:1px solid var(--line);border-radius:4px;margin-bottom:5px;cursor:pointer;background:var(--bg)}
.d:hover{border-color:var(--acc)}.d code{display:block;font:11.5px ui-monospace,monospace;white-space:pre-wrap;word-break:break-all}
.d small{color:var(--dim)}.d .w{color:var(--kw)}
#name{font:600 13px ui-monospace,monospace;word-break:break-all}#meta{color:var(--dim)}
#schp{grid-area:sch;border-top:1px solid var(--line);border-left:1px solid var(--line);overflow:auto;min-height:0;min-width:0}#sch{padding:6px 10px}
svg{display:block}svg text{font:11px ui-monospace,monospace;fill:var(--fg)}
svg .n rect{fill:var(--bg);stroke:var(--dim)}svg .n.me rect{stroke:var(--acc);stroke-width:2}svg .n{cursor:pointer}
svg .b rect{fill:var(--panel);stroke:var(--fg)}svg .b text{fill:var(--dim)}svg .b{cursor:pointer}
svg path{fill:none;stroke:var(--dim)}svg path.c{stroke-dasharray:3 3}svg .tri{fill:none;stroke:var(--fg)}
#wbar{display:flex;gap:8px;align-items:center;padding:3px 10px;border-bottom:1px solid var(--line);color:var(--dim)}
#wc{flex:1;min-height:0;position:relative;overflow-y:auto}canvas{display:block;width:100%}
.none{color:var(--dim);padding:4px 0}
</style></head><body>
<header><b>mdebug</b><input id="q" placeholder="find a signal (tb.u.q)" list="ql"><datalist id="ql"></datalist>
<span id="stat"></span><span style="margin-left:auto" id="tcur"></span></header>
<div id="left"><h3>Hierarchy</h3><div id="tree"></div><h3>Signals</h3><div id="sigs"></div></div>
<div id="src"></div>
<div id="right"><h3>Signal</h3><div id="name" class="none">Click a signal in the source or the list.</div><div id="meta"></div>
<h3>Drivers</h3><div id="drv"></div><h3>Loads</h3><div id="lod"></div></div>
<div id="schp"><h3>Schematic</h3><div id="sch"><div class="none">Select a signal to draw its drivers and loads.</div></div></div>
<div id="wave"><div id="wbar"><span>Waves</span><button id="prev" title="previous change of the selected signal">&#9664; change</button>
<button id="next" title="next change of the selected signal">change &#9654;</button><button id="cone" title="add the inputs of the selected signal">+ inputs</button>
<button id="clr">clear</button><button id="fit">fit</button><span id="winfo"></span></div><div id="wc"><canvas id="cv"></canvas></div></div>
<script>
const DB=/*MDEBUG_DATA*/null;
const $=id=>document.getElementById(id),esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const S=DB.signals,D=DB.drivers,W=DB.waves||null,END=DB.end||0;
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
function tree(){const kids=new Map();DB.scopes.forEach(s=>{const p=s.path.includes('.')?scopeOf(s.path):'';(kids.get(p)||kids.set(p,[]).get(p)).push(s)});
  let h='';const walk=(p,dep)=>(kids.get(p)||[]).forEach(s=>{h+=`<div class="row${s.path==st.scope?' on':''}" data-p="${esc(s.path)}" style="padding-left:${10+dep*14}px">${esc(leaf(s.path))} <small>${esc(s.def)}</small></div>`;walk(s.path,dep+1)});
  walk('',0);$('tree').innerHTML=h}
function sigList(){const l=sigsIn.get(st.scope)||[];$('sigs').innerHTML=l.map(i=>{const v=W?val(i,st.t):null;
  return `<div class="row${i==st.sig?' on':''}" data-s="${i}">${esc(leaf(S[i].path))}<i>${v===null?esc(S[i].type):v}</i></div>`}).join('')||'<div class="row none">no signals here</div>'}
function source(scroll){const sc=DB.scopes.find(s=>s.path==st.scope);const f=st.file>=0?st.file:(sc?sc.file:-1);
  if(f<0||!DB.files[f]){$('src').innerHTML='<div class="fname">no source</div>';return}
  st.file=f;const drv=new Set,lod=new Set;
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
    h+=`<div class="ln${drv.has(n+1)?' drv':lod.has(n+1)?' load':''}${n+1==scroll?' cur':''}" id="L${n+1}"><u>${n+1}</u><span>${o}</span></div>`});
  $('src').innerHTML=h;if(scroll){const e=$('L'+scroll);e&&e.scrollIntoView({block:'center'})}}
function card(i,showT){const d=D[i],f=DB.files[d.file];return `<div class="d" data-d="${i}"><code>${esc(d.text)}</code><small>${esc(f?f.name:'?')}:${d.line} &middot; ${esc(d.kind)}${showT?' &rarr; '+esc(S[d.sig].path):''}</small>${d.when?`<small><br>when <span class="w">${esc(d.when)}</span></small>`:''}</div>`}
function panel(){if(st.sig<0)return;const s=S[st.sig],v=W?val(st.sig,st.t):null;$('name').className='';$('name').textContent=s.path;
  $('meta').textContent=s.type+(v===null?'':'  =  '+v+'  @'+st.t)+'  ·  '+(DB.files[s.file]?DB.files[s.file].name:'?')+':'+s.line;
  $('drv').innerHTML=drvOf[st.sig].map(i=>card(i)).join('')||'<div class="none">No driver: an input or a testbench value.</div>';
  $('lod').innerHTML=loadOf[st.sig].map(i=>card(i,1)).join('')||'<div class="none">Nothing reads it.</div>';schematic()}
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
document.body.addEventListener('click',e=>{const t=e.target.closest('[data-s],[data-d],[data-p]');if(!t)return;
  if(t.dataset.s!==undefined)pick(+t.dataset.s);else if(t.dataset.d!==undefined)goDriver(+t.dataset.d);
  else{st.scope=t.dataset.p;st.file=-1;tree();sigList();const sc=DB.scopes.find(s=>s.path==st.scope);source(sc?sc.line:0)}});
$('q').addEventListener('change',e=>{const v=e.target.value.trim();let i=byPath.get(v);
  if(i===undefined)i=S.findIndex(s=>s.path.endsWith('.'+v));if(i===undefined||i<0)i=S.findIndex(s=>s.path.includes(v));if(i>=0)pick(i)});
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
tree();sigList();source();setT(DB.at===undefined?END:DB.at);
</script></body></html>
)MDV";
