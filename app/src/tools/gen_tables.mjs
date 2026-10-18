// Generate hg_tables.h from the Kotlin vendor constants.
//
// Why generate instead of transcribing: VendorConstants.sbox is a 1024-byte
// S-box that arrives as one base64 literal, hashTable is 64 x uint32, and the
// two User-Agent strings plus the 27-entry device fingerprint map are long
// exact-match strings that the upstream API is picky about. Hand-copying any of
// them is a silent-corruption risk with no upside; extracting them mechanically
// makes transcription errors impossible and keeps the C in sync with the Kotlin.
//
// Usage: node gen_tables.mjs <VendorConstants.kt> <out-header>

import { readFileSync, writeFileSync } from 'node:fs'

const [srcPath, outPath] = process.argv.slice(2)
if (!srcPath || !outPath) {
  console.error('usage: node gen_tables.mjs <VendorConstants.kt> <out-header>')
  process.exit(2)
}

const src = readFileSync(srcPath, 'utf8')

// ---- sbox: decode64("....") -> must be 1024 bytes -------------------------
const sboxM = src.match(/val\s+sbox\s*=\s*decode64\("([^"]+)"\)/)
if (!sboxM) throw new Error('could not find sbox literal')
const sbox = Buffer.from(sboxM[1].replace(/-/g, '+').replace(/_/g, '/'), 'base64')
if (sbox.length !== 1024) {
  // AesV3.kt indexes sbox[word*256 .. word*256+255] for word in 0..3.
  throw new Error(`sbox decoded to ${sbox.length} bytes, expected 1024 (4 x 256)`)
}

// ---- hashTable: intArrayOf(0x...toInt(), ...) -> 64 entries ---------------
const htM = src.match(/val\s+hashTable\s*=\s*intArrayOf\(([\s\S]*?)\)\s*\n?\s*}/)
if (!htM) throw new Error('could not find hashTable literal')
const hashTable = [...htM[1].matchAll(/0x([0-9a-fA-F]+)/g)].map((m) => parseInt(m[1], 16) >>> 0)
if (hashTable.length !== 64) {
  throw new Error(`hashTable parsed ${hashTable.length} entries, expected 64`)
}

// ---- the two User-Agent strings -------------------------------------------
const ua = (name) => {
  const m = src.match(new RegExp(`const\\s+val\\s+${name}\\s*=\\s*"([^"]*)"`))
  if (!m) throw new Error(`could not find ${name}`)
  if (/["\\]/.test(m[1])) throw new Error(`${name} needs C escaping; extend the generator`)
  return m[1]
}
const videoUa = ua('VIDEO_UA')
const appUa = ua('APP_UA')

// ---- device fingerprint: "key" to "value" pairs ---------------------------
const devM = src.match(/val\s+device\s*=\s*linkedMapOf\(([\s\S]*?)\n\s*\)/)
if (!devM) throw new Error('could not find device map')
const device = [...devM[1].matchAll(/"([^"]+)"\s*to\s*"([^"]+)"/g)].map((m) => [m[1], m[2]])
if (device.length < 20) throw new Error(`device map parsed only ${device.length} entries`)
for (const [k, v] of device) {
  if (/["\\]/.test(k) || /["\\]/.test(v)) throw new Error(`device entry ${k} needs escaping`)
}

// ---- emit -----------------------------------------------------------------
const wrapBytes = (bytes, perLine = 16) => {
  const out = []
  for (let i = 0; i < bytes.length; i += perLine) {
    const chunk = [...bytes.slice(i, i + perLine)].map((b) => '0x' + b.toString(16).padStart(2, '0'))
    out.push('  ' + chunk.join(', ') + ',')
  }
  return out.join('\n')
}
const wrapInts = (ints, perLine = 4) => {
  const out = []
  for (let i = 0; i < ints.length; i += perLine) {
    const chunk = ints.slice(i, i + perLine).map((n) => '0x' + n.toString(16).padStart(8, '0') + 'u')
    out.push('  ' + chunk.join(', ') + ',')
  }
  return out.join('\n')
}

const banner = `/*
 * GENERATED FILE -- do not edit by hand.
 *
 * Produced by app/src/tools/gen_tables.mjs from
 *   kotlin-tv/core/src/main/kotlin/com/hongguotv/core/VendorConstants.kt
 * in N3urda/hongguoTV (GPL-3.0), which derives these from woshishiq1/drpys
 * spider/js/红果果[短].js @22261ad (GPL-3.0). The combined work stays GPL-3.0.
 *
 * Regenerate with:
 *   node gen_tables.mjs <VendorConstants.kt> hg_tables.h
 *
 * Regenerating is cheaper than reviewing a diff: sbox is 1024 opaque bytes and
 * the UA / device strings must match upstream byte for byte.
 */

#ifndef HG_TABLES_H
#define HG_TABLES_H

/* 4 x 256. AesV3 selects the quarter at (timestamp & 3). */
static const unsigned char HG_SBOX[1024] = {
${wrapBytes(sbox)}
};

/* 64 round constants used by hashF13. */
static const unsigned int HG_HASH_TABLE[64] = {
${wrapInts(hashTable)}
};

/* Exact-match identity strings; the upstream API rejects near misses. */
#define HG_VIDEO_UA "${videoUa}"
#define HG_APP_UA   "${appUa}"

/* Device fingerprint. The port must decide whether presenting an Android/Redmi
 * identity is acceptable; upstream may refuse requests that do not. */
typedef struct { const char *key; const char *value; } HgKv;
static const HgKv HG_DEVICE[] = {
${device.map(([k, v]) => `  { "${k}", "${v}" },`).join('\n')}
};
#define HG_DEVICE_COUNT ${device.length}

#endif /* HG_TABLES_H */
`

writeFileSync(outPath, banner)
console.log(`OK ${outPath}`)
console.log(`  sbox      : ${sbox.length} bytes`)
console.log(`  hashTable : ${hashTable.length} entries`)
console.log(`  device    : ${device.length} entries`)
console.log(`  UA        : video=${videoUa.length} chars, app=${appUa.length} chars`)
