// Test vectors for the URL encoder.
//
// The encoder feeds the signature, so an encoding difference is not a cosmetic
// bug -- it changes x-gorgon and x-medusa and the request is rejected. That makes
// it worth pinning to the reference the same way the hash was.
//
// The interesting cases are the ones where JS encodeURIComponent and Python
// quote disagree: `*` survives encodeURIComponent but Python would escape it,
// and `!'()` survive encodeURIComponent and are then escaped by the explicit
// .replace() that follows it.
//
// Usage: node gen_encode_vectors.mjs <hongguo.cjs> <out-header>

import { readFileSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, outPath] = process.argv.slice(2)
if (!src || !outPath) {
  console.error('usage: node gen_encode_vectors.mjs <hongguo.cjs> <out-header>')
  process.exit(2)
}

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console })
vm.runInContext(source + ';globalThis.pythonUrlEncode = pythonUrlEncode;', ctx)

// Deliberately adversarial: each pair probes one rule.
const CASES = [
  ['plain', 'abc'],
  ['digit', '12345'],
  ['unreserved-dash-underscore-dot-tilde', 'a-b_c.d~e'],
  ['STAR is kept by encodeURIComponent (Python would escape it)', 'a*b'],
  ['bang-quote-paren are escaped explicitly', "a!b'c(d)e"],
  ['space', 'a b'],
  ['plus', 'a+b'],
  ['slash and colon', 'a/b:c'],
  ['ampersand and equals', 'a&b=c'],
  ['percent', 'a%b'],
  ['question and hash', 'a?b#c'],
  ['comma semicolon at', 'a,b;c@d'],
  ['brackets and braces', 'a[b]c{d}'],
  ['backslash and caret', 'a\\b^c'],
  ['chinese', '短剧'],
  ['chinese with space', '红果 短剧'],
  ['emoji', '\u{1F3AC}'],
  ['key and value both need encoding', 'k=1'],
  ['empty', ''],
]

const rows = CASES.map(([name, value]) => ({
  name,
  // a single key/value pair goes through the same path as a full map
  input: value,
  output: ctx.pythonUrlEncode({ q: value }),
}))

// Also pin one full multi-key map, since ordering and joining matter.
const DEVICE_SAMPLE = {
  iid: '1905892595382586', device_id: '1905892595378490', ac: 'wifi',
  aid: '8662', device_type: '25053RT47C', language: 'zh',
  resolution: '1280*2772', version_name: '7.1.3.32',
  ts: '1800600000', _rticket: '1800600000123',
}
rows.push({ name: 'device map (ordering + joining)', input: '<map>', output: ctx.pythonUrlEncode(DEVICE_SAMPLE) })

/* Emit non-ASCII as OCTAL byte escapes for the same reason gen_sign_vectors.mjs
 * does: the header is written with the 'ascii' encoding, and Node's 'ascii'
 * masks every code unit to 7 bits -- "短剧" came out as two mangled bytes and the
 * C test then exercised an encoder that had never seen real UTF-8. Octal escapes
 * are at most three digits, so they cannot bleed into a following character. */
const cstr = (s) => {
  let out = '"'
  for (const ch of s) {
    const cp = ch.codePointAt(0)
    if (ch === '\\') out += '\\\\'
    else if (ch === '"') out += '\\"'
    else if (cp >= 32 && cp <= 126) out += ch
    else {
      for (const byte of Buffer.from(ch, 'utf8')) {
        out += '\\' + byte.toString(8).padStart(3, '0')
      }
    }
  }
  return out + '"'
}
const out = []
out.push('/*')
out.push(' * GENERATED FILE -- do not edit by hand.')
out.push(' *')
out.push(' * URL-encoding vectors, produced by running the reference')
out.push(' * pythonUrlEncode() (N3urda/hongguoTV server/vendor/hongguo.cjs:79,')
out.push(' * GPL-3.0). Ground truth is the reference implementation.')
out.push(' */')
out.push('')
out.push('#ifndef ENCODE_VECTORS_GEN_H')
out.push('#define ENCODE_VECTORS_GEN_H')
out.push('')
out.push(`#define ENC_COUNT ${rows.length}`)
out.push('')
out.push('typedef struct { const char *name; const char *input; const char *expected; } EncVector;')
out.push('')
out.push('/* For the map case, `input` is a placeholder; the expected string is compared')
out.push(' * against hg_url_encode_map() driven by the same fixed key/value list that')
out.push(' * gen_encode_vectors.mjs used. */')
out.push(`#define ENC_MAP_INDEX ${rows.length - 1}`)
out.push('static const EncVector ENC_VECTORS[ENC_COUNT] = {')
for (const r of rows) {
  out.push('  {')
  out.push(`    ${cstr(r.name)},`)
  out.push(`    ${cstr(r.input)},`)
  out.push(`    ${cstr(r.output)},`)
  out.push('  },')
}
out.push('};')
out.push('')
out.push('#endif /* ENCODE_VECTORS_GEN_H */')
out.push('')

writeFileSync(outPath, out.join('\n'), 'ascii')
console.log(`OK ${outPath}  rows=${rows.length}`)
for (const r of rows) console.log(`  ${r.name}\n      -> ${r.output.slice(0, 90)}`)
