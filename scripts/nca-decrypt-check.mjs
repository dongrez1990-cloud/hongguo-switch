/*
 * nca-decrypt-check.mjs -- prove the NSP's NCAs are genuine, and read them.
 *
 * Usage: node nca-decrypt-check.mjs <file.nsp> [prod.keys]
 *
 * WHY. verify-nsp.ps1 proves the PFS0 container is well formed, and that is all
 * it proves. It says nothing about whether the three entries are real NCAs,
 * because a homebrew-built NSP is encrypted: the first 0x200 bytes of an NCA are
 * a signature block (all zeroes when no signature was applied), and the header
 * from 0x200 onward is AES-XTS encrypted with the console's header key. That is
 * why a byte search for "NCA" comes up empty and why all three entries begin
 * with the same 256 bytes -- identical ciphertext for identical plaintext under
 * the same key and tweak.
 *
 * So the honest check is to decrypt sector 0 of the header and look for the
 * magic. node's crypto has aes-128-xts, and the key is in prod.keys, which
 * hacbrewpack already reads from C:\Users\<user>\.switch\prod.keys.
 *
 * NCA header layout once decrypted:
 *   0x200  "NCA3" / "NCA2" / "NCA0"
 *   0x204  distribution type
 *   0x205  content type   (0=Program 1=Meta 2=Control 3=Manual 4=Data 5=PublicData)
 *   0x206  key generation
 *   0x207  key area encryption key index
 *   0x208  size (u64 media units)
 *   0x210  content id (16 bytes)
 */

import { readFileSync } from 'node:fs'
import { createDecipheriv } from 'node:crypto'
import { homedir } from 'node:os'

const CONTENT_TYPES = ['Program', 'Meta', 'Control', 'Manual', 'Data', 'PublicData']

const file = process.argv[2]
const keysPath = process.argv[3] || `${homedir()}\\.switch\\prod.keys`
if (!file) {
  console.error('usage: node nca-decrypt-check.mjs <file.nsp> [prod.keys]')
  process.exit(2)
}

const buf = readFileSync(file)
const u32 = (o) => buf.readUInt32LE(o)
const u64 = (o) => Number(buf.readBigUInt64LE(o))

/* ---- read the header key ------------------------------------------------- */
let headerKey = null
try {
  for (const line of readFileSync(keysPath, 'utf8').split(/\r?\n/)) {
    const m = /^\s*header_key\s*=\s*([0-9a-fA-F]+)\s*$/.exec(line)
    if (m) {
      headerKey = Buffer.from(m[1], 'hex')
      break
    }
  }
} catch (e) {
  console.error(`could not read keys at ${keysPath}: ${e.message}`)
}
if (!headerKey || headerKey.length !== 32) {
  console.error(
    `header_key not found or not 32 bytes in ${keysPath}` +
      (headerKey ? ` (got ${headerKey.length})` : '')
  )
  process.exit(1)
}
console.log(`keys            ${keysPath}  header_key ${headerKey.length} bytes`)

/* ---- PFS0 ---------------------------------------------------------------- */
const entryCount = u32(4)
const stringTableSize = u32(8)
const stringTable = 0x10 + entryCount * 0x18
const dataOffset = stringTable + stringTableSize

const entries = []
for (let i = 0; i < entryCount; i++) {
  const e = 0x10 + i * 0x18
  const offset = u64(e + 0x00)
  const size = u64(e + 0x08)
  const nameOff = u32(e + 0x10)
  const nameEnd = buf.indexOf(0, stringTable + nameOff)
  entries.push({
    name: buf.toString('ascii', stringTable + nameOff, nameEnd),
    abs: dataOffset + offset,
    size,
  })
}

/* ---- decrypt and identify ------------------------------------------------ */
/* XTS takes the sector number as the 16-byte tweak, little-endian. The
 * encrypted part of the NCA header starts at 0x200, and its first sector uses
 * tweak 0. */
function xtsDecryptSector(src, sectorIndex) {
  const tweak = Buffer.alloc(16)
  tweak.writeUInt32LE(sectorIndex >>> 0, 0)
  tweak.writeUInt32LE(Math.floor(sectorIndex / 0x100000000) >>> 0, 4)
  const d = createDecipheriv('aes-128-xts', headerKey, tweak)
  d.setAutoPadding(false)
  return Buffer.concat([d.update(src), d.final()])
}

console.log('')
console.log('name'.padEnd(42) + 'size'.padStart(8) + '  magic  content')
for (const en of entries) {
  const sector = buf.subarray(en.abs + 0x200, en.abs + 0x200 + 0x200)
  let plain = null
  try {
    plain = xtsDecryptSector(sector, 0)
  } catch (e) {
    console.log(en.name.padEnd(42) + String(en.size).padStart(8) + '  decrypt failed: ' + e.message)
    continue
  }

  const magic = plain.toString('ascii', 0, 4)
  const dist = plain[4]
  const ctype = plain[5]
  const keygen = plain[6]
  const keyAreaIdx = plain[7]
  const ncaSize = Number(plain.readBigUInt64LE(8))
  const contentId = plain.subarray(0x10, 0x20).toString('hex')
  const label = CONTENT_TYPES[ctype] ?? `?(${ctype})`

  console.log(
    en.name.padEnd(42) + String(en.size).padStart(8) +
      '  ' + magic.padEnd(6) + (magic.startsWith('NCA') ? label : '(not an NCA header)')
  )
  if (magic.startsWith('NCA')) {
    console.log(`    dist=${dist} keygen=${keygen} keyAreaIdx=${keyAreaIdx} contentId=${contentId}`)
    console.log(`    ncaSize=${ncaSize} media units = ${ncaSize * 0x200} bytes`)
  }
}

/* ---- the signature question --------------------------------------------- */
console.log('')
const sigBytes = entries.map((en) => buf.subarray(en.abs, en.abs + 0x200))
const allZero = (b) => b.every((x) => x === 0)
console.log(
  'signature blocks all zero (plaintext)?  ' +
    sigBytes.map((b, i) => `#${i}=${allZero(b) ? 'zero' : 'data'}`).join('  ')
)
console.log(
  'NOTE: an all-zero signature block is what a homebrew NCA builder writes when no\n' +
    'signature is applied. The console refuses such an NCA unless signature checks are\n' +
    'patched (Atmosphere + sigpatches, or an installer that ignores them). This is the\n' +
    'first thing to check when an install refuses, and it is a property of the FILE.'
)
