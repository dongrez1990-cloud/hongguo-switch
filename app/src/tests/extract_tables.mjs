// Temporary: extract constant tables from the pinned JS source into C fragments.
import fs from 'node:fs';
const f = '<upstream-checkout>/hongguoTV-main/server/vendor/hongguo.cjs';
const raw = fs.readFileSync(f, 'utf8');
const lines = raw.split('\n');
const line = (n) => lines[n - 1];

function grabArray(ln, name) {
  const m = line(ln).match(new RegExp(name + '\\s*=\\s*\\[([^\\]]*)\\]'));
  if (!m) throw new Error('no ' + name);
  return m[1].split(',').map((s) => s.trim()).filter((s) => s.length);
}
function grabBuffer(ln, name) {
  const m = line(ln).match(new RegExp(name + "\\s*=\\s*Buffer\\.from\\('([0-9a-f]+)','hex'\\)"));
  if (!m) throw new Error('no ' + name);
  return m[1];
}

const out = [];
const SBOX = line(374).match(/const SBOX = Buffer\.from\('([^']+)','base64'\)/)[1];
out.push('static const char HG_SBOX_B64[] =');
out.push('    "' + SBOX + '";');
out.push('');

const tt = grabArray(353, 'tt02');
out.push('static const uint32_t HG_TT02[64] = {');
for (let i = 0; i < 64; i += 4) out.push('    ' + tt.slice(i, i + 4).map((v) => v + 'u').join(', ') + (i + 4 < 64 ? ',' : ''));
out.push('};');
out.push('');

const sv = grabArray(334, 'BRANCH2_SV');
out.push('static const uint32_t HG_BRANCH2_SV[64] = {');
for (let i = 0; i < 64; i += 4) out.push('    ' + sv.slice(i, i + 4).map((v) => v + 'u').join(', ') + (i + 4 < 64 ? ',' : ''));
out.push('};');
out.push('');

const orders = grabBuffer(335, 'BRANCH2_ORDERS');
out.push('static const uint8_t HG_BRANCH2_ORDERS[320] = {');
for (let i = 0; i < 320; i += 16) {
  const chunk = [];
  for (let j = 0; j < 16; j++) chunk.push('0x' + orders.slice((i + j) * 2, (i + j) * 2 + 2) + 'u');
  out.push('    ' + chunk.join(', ') + (i + 16 < 320 ? ',' : ''));
}
out.push('};');
out.push('');

fs.writeFileSync('app/src/tests/_tables.inc', out.join('\n') + '\n');
console.log('tt02[0]=', tt[0], 'sv[0]=', sv[0], 'orders.len=', orders.length / 2, 'sbox.b64len=', SBOX.length);
