// Temporary probe: dump intermediate values from the pinned JS oracle.
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);

const root = '<upstream-checkout>/hongguoTV-main';
const source = fs.readFileSync(path.join(root, 'server/vendor/hongguo.cjs'), 'utf8');
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams });
vm.runInContext(source + ';globalThis.oracle = {branchOf,hashF13,xGorgon,helios,buildMedusa,sm3,AesV3,xmxor,keyHash,proto,deviceProto,getIv,sumMd5,le32,be32,rc4Gorgon,md5};', ctx);

const rows = JSON.parse(fs.readFileSync(path.join(root, 'kotlin-tv/core/src/test/resources/signing.json'), 'utf8'));
const out = [];
for (let i = 0; i < 12; i++) {
  const r = rows[i];
  const ts = r.ts;
  vm.runInContext(`Date.now=()=>${ts * 1000}; letSeed=17; Math.random=()=>{letSeed=(Math.imul(letSeed,1664525)+1013904223)>>>0;return letSeed/4294967296;};`, ctx);
  const url = r.url;
  const query = url.split('?')[1];
  const body = r.body === null ? null : Buffer.from(r.body);
  const bodyMd5 = body ? require('crypto').createHash('md5').update(body).digest() : Buffer.alloc(16);
  const querySm3 = ctx.oracle.sm3(query);
  const tsBytes = ctx.oracle.le32(ts);
  out.push({
    i, ts,
    querySm3: querySm3.toString('hex'),
    bodyMd5: bodyMd5.toString('hex'),
    iv: (ctx.oracle.getIv(ctx.oracle.getIv(ctx.oracle.getIv(0x20230928, querySm3), bodyMd5), tsBytes) >>> 0).toString(16),
    branch: ctx.oracle.branchOf(url, body, ts),
    hash: ctx.oracle.hashF13(querySm3, bodyMd5, tsBytes, ts).toString('hex'),
  });
}
fs.writeFileSync(path.join(root, 'probe-out.json'), JSON.stringify(out, null, 1));
console.log('ok');
