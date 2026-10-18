// Emit the branch-2 hash tables AND branch-2 test vectors, from the reference.
//
// Why this exists: the 12 committed vectors in signing.json are ALL branch 0,
// because the oracle searches tickets until branchOf() == 0. But the reference
// itself only avoids branch 1 --
//     if (branchOf(url, body, khronos) !== 1) break;   // hongguo.cjs:428
// -- so in the field, requests land on branch 0 OR branch 2. A signer that only
// implements branch 0 fails roughly five times in sixteen for the search
// endpoint, which has no branch retry loop at all.
//
// The tables are EXTRACTED, not transcribed: BRANCH2_ORDERS is 384 bytes of
// permutation data and BRANCH2_SV is 64 more constants. Hand-copying either
// would be a silent-corruption risk with no upside.
//
// Usage: node gen_branch2.mjs <hongguo.cjs> <tables-header> <vectors-header>

import { readFileSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, tablesOut, vectorsOut] = process.argv.slice(2)
if (!src || !tablesOut || !vectorsOut) {
  console.error('usage: node gen_branch2.mjs <hongguo.cjs> <tables.h> <vectors.h>')
  process.exit(2)
}

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console })
vm.runInContext(source + `
  ;globalThis.oracle = { branchOf, hashF13, sm3, BRANCH2_SV, BRANCH2_ORDERS, getIv, le32 };
`, ctx)

const cbytes = (b) => [...b].map((x) => '0x' + x.toString(16).padStart(2, '0')).join(', ')
const cints = (a) => a.map((n) => '0x' + (n >>> 0).toString(16).padStart(8, '0') + 'u')

// ---- tables ---------------------------------------------------------------
const SV = ctx.oracle.BRANCH2_SV
const ORDERS = Buffer.from(ctx.oracle.BRANCH2_ORDERS)
if (SV.length !== 64) throw new Error(`BRANCH2_SV has ${SV.length} entries, expected 64`)
// branch2F13 indexes BRANCH2_ORDERS.subarray(ivV0 << 6, (ivV0 + 1) << 6).
// ivV0 = ((v >>> 15) & 255) + ((v >>> 8) & 255) with v = (iv & 13) * 86, and
// v maxes out at 13 * 86 = 1118 < 2^15, so the first term is always 0 and the
// second is at most 4. That is FIVE slices of 64, i.e. 320 bytes -- an earlier
// assertion here demanded 384 (six) and was simply wrong about the range.
if (ORDERS.length % 64 !== 0) throw new Error(`BRANCH2_ORDERS is ${ORDERS.length} bytes, not a multiple of 64`)
if (ORDERS.length < 5 * 64) throw new Error(`BRANCH2_ORDERS is ${ORDERS.length} bytes, need at least 320 for ivV0 in 0..4`)

{
  const out = []
  out.push('/*')
  out.push(' * GENERATED FILE -- do not edit by hand.')
  out.push(' *')
  out.push(' * Branch-2 hash tables, extracted from N3urda/hongguoTV')
  out.push(' * server/vendor/hongguo.cjs (GPL-3.0), derived from woshishiq1/drpys')
  out.push(' * @22261ad (GPL-3.0). The combined work stays GPL-3.0.')
  out.push(' *')
  out.push(' * Regenerate with: node gen_branch2.mjs <hongguo.cjs> <tables.h> <vectors.h>')
  out.push(' */')
  out.push('')
  out.push('#ifndef HG_BRANCH2_TABLES_H')
  out.push('#define HG_BRANCH2_TABLES_H')
  out.push('')
  out.push('/* 64 round constants for md5SumV3. */')
  out.push('static const unsigned int HG_B2_SV[64] = {')
  for (let i = 0; i < 64; i += 4) out.push('  ' + cints(SV.slice(i, i + 4)).join(', ') + ',')
  out.push('};')
  out.push('')
  out.push('/* 6 x 64 message-word orderings, indexed as [ivV0 * 64 + i]. */')
  out.push('static const unsigned char HG_B2_ORDERS[384] = {')
  for (let i = 0; i < ORDERS.length; i += 16) out.push('  ' + cbytes(ORDERS.subarray(i, i + 16)) + ',')
  out.push('};')
  out.push('')
  out.push('#endif /* HG_BRANCH2_TABLES_H */')
  out.push('')
  writeFileSync(tablesOut, out.join('\n'), 'ascii')
  console.log(`OK ${tablesOut}  SV=${SV.length} ORDERS=${ORDERS.length}`)
}

// ---- vectors --------------------------------------------------------------
// Search for tickets that land on branch 2, then pin the reference's own output.
const rows = []
for (let variant = 0; variant < 12; variant++) {
  const ts = 1800600000 + variant
  const body = variant % 2 === 0 ? Buffer.from('{"video":"9876543210"}') : null
  let url = null
  for (let ticket = 0; ticket < 4096; ticket++) {
    const candidate = `https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?q=${encodeURIComponent('短剧')}&ticket=${ticket}`
    if (ctx.oracle.branchOf(candidate, body, ts) === 2) { url = candidate; break }
  }
  if (url === null) throw new Error(`no branch-2 ticket found for ts=${ts}`)
  const query = url.split('?')[1]
  const bodyMd5 = body ? require('node:crypto').createHash('md5').update(body).digest() : Buffer.alloc(16)
  const hash = ctx.oracle.hashF13(ctx.oracle.sm3(query), bodyMd5, (() => {
    const b = Buffer.alloc(4); b.writeUInt32LE(ts >>> 0); return b
  })(), ts).toString('hex')
  rows.push({ ts, url, body: body ? body.toString() : null, hash })
  if (rows.length >= 6) break
}

{
  const cstr = (s) => '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"'
  const out = []
  out.push('/*')
  out.push(' * GENERATED FILE -- do not edit by hand.')
  out.push(' *')
  out.push(' * Branch-2 signing vectors, produced from N3urda/hongguoTV')
  out.push(' * server/vendor/hongguo.cjs (GPL-3.0). Ground truth is the reference')
  out.push(' * implementation, not an independent source -- see the note in')
  out.push(' * hg_sign_test.c about what that does and does not prove.')
  out.push(' */')
  out.push('')
  out.push('#ifndef BRANCH2_VECTORS_GEN_H')
  out.push('#define BRANCH2_VECTORS_GEN_H')
  out.push('')
  out.push(`#define B2_COUNT ${rows.length}`)
  out.push('')
  out.push('typedef struct {')
  out.push('  unsigned int ts;')
  out.push('  const char *url;')
  out.push('  const char *body;   /* NULL for a GET */')
  out.push('  unsigned int body_len;')
  out.push('  const char *hash;')
  out.push('} B2Vector;')
  out.push('')
  out.push('static const B2Vector B2_VECTORS[B2_COUNT] = {')
  for (const r of rows) {
    out.push('  {')
    out.push(`    ${r.ts}u,`)
    out.push(`    ${cstr(r.url)},`)
    out.push(`    ${r.body === null ? '0' : cstr(r.body)}, ${r.body === null ? 0 : Buffer.byteLength(r.body, 'utf8')}u,`)
    out.push(`    ${cstr(r.hash)},`)
    out.push('  },')
  }
  out.push('};')
  out.push('')
  out.push('#endif /* BRANCH2_VECTORS_GEN_H */')
  out.push('')
  writeFileSync(vectorsOut, out.join('\n'), 'ascii')
  console.log(`OK ${vectorsOut}  rows=${rows.length}`)
  for (const r of rows) console.log(`  ts=${r.ts} body=${r.body === null ? 'GET' : 'POST'} hash=${r.hash.slice(0, 16)}…`)
}
