// The viewer's value helpers (src/mdebug_view.inc), run under Node:
// radix display (r) and the find-value parser.  node mdebug_view_test.js page.html
const fs = require('fs');
const page = fs.readFileSync(process.argv[2], 'utf8');
const grab = name => {
  const i = page.indexOf('function ' + name + '(');
  if (i < 0) throw new Error('no function ' + name + ' in the page');
  let d = 0, j = page.indexOf('{', i);
  for (let k = j; k < page.length; k++) { if (page[k] == '{') d++; else if (page[k] == '}' && --d == 0) return page.slice(i, k + 1); }
};
const hex = b => b.length == 1 ? b : /[xz]/.test(b) ? b.length + "'b" + b : BigInt('0b' + b).toString(16);
const inRad = eval('(' + grab('inRad') + ')'), wantBits = eval('(' + grab('wantBits') + ')');
const eq = (a, b, what) => { if (a !== b) { console.error(what + ': got ' + a + ', want ' + b); process.exit(1); } };
eq(inRad('11111110', 'dec'), '254', 'dec');
eq(inRad('11111110', 'sdec'), '-2', 'sdec');
eq(inRad('01111110', 'sdec'), '126', 'sdec positive');
eq(inRad('1010', 'bin'), '1010', 'bin');
eq(inRad('1x10', 'dec'), "4'b1x10", 'x stays bits');
eq(inRad('11111110', undefined), 'fe', 'hex default');
eq(wantBits('fe', 8), '11111110', 'hex');
eq(wantBits("'hFE", 8), '11111110', "'h");
eq(wantBits("'d-2", 8), '11111110', "'d negative");
eq(wantBits("'b1x", 4), '001x', "'b pads");
eq(wantBits('x', 4), null, 'x alone is not a value');
eq(wantBits('1_0000_0001', 8), '00000001', 'too wide keeps the low bits');
console.log('ok');
