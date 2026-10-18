// Generate test vectors for the spade URL decryptor and the CENC key deriver.
//
// MediaCryptoTest.kt does NOT cover either of these, so unlike the CTR/CENC work
// there is no committed ground truth to check against. The next best thing is to
// pin them to the reference implementation's own output -- which is what this
// does -- and to be explicit that this proves agreement with the reference, not
// that the reference is right.
//
// BUILDING A SPADE URL VECTOR requires care: the parameters must actually satisfy
// deriveContentKey's length algebra AND the derived bytes must spell valid hex, so
// the candidate is searched rather than assumed. For the URL the generator has to
// CONSTRUCT a ciphertext (encrypt a known URL with the derived key, prepend the
// 0xa8 header) because no real sample is available offline.
//
// Usage: node gen_media_vectors.mjs <hongguo.cjs> <out-header>

import { readFileSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import { createHash, createCipheriv } from 'node:crypto'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, outPath] = process.argv.slice(2)
if (!src || !outPath) {
  console.error('usage: node gen_media_vectors.mjs <hongguo.cjs> <out-header>')
  process.exit(2)
}

const out = []
const emit = (s) => out.push(s)

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console,
                               crypto: require('node:crypto') })
vm.runInContext(source + ';globalThis.deriveContentKey = deriveContentKey; globalThis.decryptSpadeUrl = decryptSpadeUrl;', ctx)

const cbytes = (b) => [...b].map((x) => '0x' + x.toString(16).padStart(2, '0')).join(', ')
const SPADE_CONSTANTS = Buffer.from(
  '4dd4c2e6b83162090e52b3c7a6733ba41cb2462b829ab58a196b39db57177524f49baf7f08e8d68d26a72e37c1a95a2f1f05a51892aef2949732b62a38aadd58', 'hex')

// ---- construct a spade_a value the reference accepts ---------------------
// Searching for one is hopeless: deriveContentKey returns
// Buffer.from(work[1..33].toString('ascii'), 'hex'), so all 32 derived bytes must
// happen to be hex digits -- about (22/256)^32 if the input were random.
//
// The transform is invertible, though, so do that instead. Forward:
//     work[i] = (-21 - popcount(i) + (previous ^ old)) & 255
// with `previous` taken from a/b and a/b then set to `old` (the ORIGINAL byte).
// Given a desired work[i] and the current a/b, the required input byte is
//     old = previous ^ ((work[i] + 21 + popcount(i)) & 255)
// so we choose the 32 hex characters we want and walk forward deriving the input.
const popcount = (v) => { let n = 0; while (v) { n += v & 1; v >>= 1 } return n }
const spadeInput = (() => {
  const target = Buffer.from('0123456789abcdef0123456789abcdef', 'ascii')  // 32 hex chars
  const WORKLEN = 63                       // raw length 64, so v8 clamps to 63
  const desired = Buffer.alloc(WORKLEN)
  for (let i = 1; i <= 32; i++) desired[i] = target[i - 1]

  const orig = Buffer.alloc(WORKLEN)
  let a = 85, b = 246
  for (let i = 0; i < WORKLEN; i++) {
    const previous = (i & 1) ? a : b
    const old = previous ^ ((desired[i] + 21 + popcount(i)) & 255)
    if (i & 1) a = old; else b = old
    orig[i] = old
  }

  const raw = Buffer.alloc(64)
  for (let i = 0; i < WORKLEN; i++) raw[1 + i] = orig[i]
  // raw[1] and raw[2] are also part of the length XOR; solve raw[0] so the XOR is
  // zero, which makes v8 = min(64 + 47, 63) = 63.
  raw[0] = (orig[0] ^ orig[1]) & 0xff
  return raw.toString('base64')
})()

let derivedKey = null
try { derivedKey = ctx.deriveContentKey(spadeInput) } catch (e) { derivedKey = null }
if (derivedKey === null || derivedKey.length !== 16) {
  throw new Error(`constructed spade_a was rejected (${derivedKey && derivedKey.length})`)
}

// ---- construct a spade URL ciphertext -----------------------------------
const seed = Buffer.from([...Array(32).keys()].map((i) => (i * 7 + 3) & 0xff))
const url = 'https://example.com/video/abc123.mp4?token=xyz'
{
  const h1 = createHash('sha512').update(seed).digest()
  const h2 = createHash('sha512').update(Buffer.concat([h1, SPADE_CONSTANTS])).digest()
  const pad = 16 - (url.length % 16)
  const plain = Buffer.concat([Buffer.from(url, 'utf8'), Buffer.alloc(pad, pad)])
  const cipher = createCipheriv('aes-128-cbc', h2.subarray(0, 16), h2.subarray(16, 32))
  cipher.setAutoPadding(false)
  const body = Buffer.concat([cipher.update(plain), cipher.final()])
  const enc = Buffer.concat([Buffer.from([0xa8, 0x00, 0x01, 0x00]), body]).toString('base64')
  const back = ctx.decryptSpadeUrl(enc, seed)
  if (back !== url) throw new Error(`reference did not round-trip: ${JSON.stringify(back)}`)
  emit('/* ---- derived from the reference implementation ---- */')
  emit(`static const unsigned char SPADE_SEED[${seed.length}] = { ${cbytes(seed)} };`)
  emit(`static const char SPADE_INPUT[] = "${enc}";`)
  emit(`static const char SPADE_EXPECTED[] = "${url}";`)
}

emit('')
emit('/* ---- spade_a -> 16-byte CENC key, from the reference ---- */')
emit(`static const char SPADE_A_INPUT[] = "${spadeInput}";`)
emit(`static const unsigned char SPADE_A_KEY[16] = { ${cbytes(derivedKey)} };`)

writeFileSync(outPath, out.join('\n') + '\n', 'ascii')
console.log(`OK ${outPath}`)
console.log(`  spade_a input : ${spadeInput.length} base64 chars`)
console.log(`  derived key   : ${derivedKey.toString('hex')}`)
console.log(`  spade url     : ${url.length} chars plaintext`)
