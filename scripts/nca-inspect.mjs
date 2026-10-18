/*
 * nca-inspect.mjs -- read an NSP's partition and its NCA headers.
 *
 * Usage: node nca-inspect.mjs <file.nsp>
 *
 * WHY. verify-nsp.ps1 already proves the PFS0 container is well formed. What it
 * does not say is what an INSTALLER will look at: each NCA's magic, which
 * content type it is (Program / Meta / Control / Manual), what crypto the header
 * declares, and -- the one that decides whether a stock console accepts it at
 * all -- the header SIGNATURE type, which for a homebrew-built NCA is a
 * placeholder that only sigpatches will let through.
 *
 * NCA header layout (offsets inside the NCA's own data):
 *   0x000  signature (0x100) + secondary signature (0x100)   -> 0x200 total
 *   0x200  magic "NCA3" / "NCA2" / "NCA0"
 *   0x204  distribution type
 *   0x205  content type        (0=Program 1=Meta 2=Control 3=Manual 4=Data 5=PublicData)
 *   0x206  key generation
 *   0x207  key area encryption key index (crypto type)
 *   0x208  size (u64, little endian, in media units)
 *   0x210  content id (16 bytes)
 *   0x220  sdk version, then title id and friends in the fs header section
 */

import { readFileSync } from 'node:fs'

const CONTENT_TYPES = ['Program', 'Meta', 'Control', 'Manual', 'Data', 'PublicData']

const file = process.argv[2]
if (!file) {
  console.error('usage: node nca-inspect.mjs <file.nsp>')
  process.exit(2)
}

const buf = readFileSync(file)
const u32 = (o) => buf.readUInt32LE(o)
const u64 = (o) => Number(buf.readBigUInt64LE(o))

/* ---- PFS0 ---------------------------------------------------------------- */
if (buf.toString('ascii', 0, 4) !== 'PFS0') {
  console.error('not a PFS0: magic is ' + JSON.stringify(buf.toString('ascii', 0, 4)))
  process.exit(1)
}
const entryCount = u32(4)
const stringTableSize = u32(8)
const stringTable = 0x10 + entryCount * 0x18
const dataOffset = stringTable + stringTableSize

console.log(`file            ${file}`)
console.log(`size            ${buf.length} bytes`)
console.log(`entries         ${entryCount}`)
console.log(`data offset     0x${dataOffset.toString(16)}`)
console.log('')

const entries = []
/* PFS0 entry layout, 0x18 bytes each:
 *   +0x00 offset (u64, relative to dataOffset)
 *   +0x08 size   (u64)
 *   +0x10 name offset into the string table (u32)
 *   +0x14 reserved
 * The first version of this script read offset/size from +0x10/+0x14 and the
 * name from +0x08, and produced timestamps that were not timestamps and names
 * that were not names -- a reminder that a parser which does not reproduce the
 * numbers an independent tool already printed is simply wrong. verify-nsp.ps1
 * prints offsets 0 / 4096 / 518144 and sizes 4096 / 514048 / 117760 for this
 * file, and this now reproduces them.
 */
for (let i = 0; i < entryCount; i++) {
  const e = 0x10 + i * 0x18
  const offset = u64(e + 0x00)
  const size = u64(e + 0x08)
  const nameOff = u32(e + 0x10)
  const nameEnd = buf.indexOf(0, stringTable + nameOff)
  const name = buf.toString('ascii', stringTable + nameOff, nameEnd)
  entries.push({ name, abs: dataOffset + offset, size })
}

/* FIND the magic, do not assume its offset.
 *
 * The first version of this script read the magic from +0x200 of each NCA and
 * got garbage, and reported content types like "?(67)". NCA0, NCA2 and NCA3
 * differ in how much signature material precedes the header, so 0x200 is a guess
 * that happened to be wrong here. Searching the first 0x1000 bytes for "NCA"
 * answers the question directly and reports where it actually is. */
console.log('name'.padEnd(42) + 'offset'.padStart(10) + 'size'.padStart(10) + '  magic  at      content')
for (const en of entries) {
  const window = buf.subarray(en.abs, en.abs + 0x1000)
  let magicAt = -1
  for (let i = 0; i + 4 <= window.length; i++) {
    if (window[i] === 0x4e && window[i + 1] === 0x43 && window[i + 2] === 0x41) {
      magicAt = i
      break
    }
  }

  if (magicAt < 0) {
    console.log(en.name.padEnd(42) + String(en.abs).padStart(10) + String(en.size).padStart(10) +
      '  (no NCA magic in the first 0x1000 bytes)')
    continue
  }

  const magic = buf.toString('ascii', en.abs + magicAt, en.abs + magicAt + 4)
  const dist = buf[en.abs + magicAt + 0x04]
  const ctype = buf[en.abs + magicAt + 0x05]
  const keygen = buf[en.abs + magicAt + 0x06]
  const cryptoIdx = buf[en.abs + magicAt + 0x07]
  const ncaSize = u64(en.abs + magicAt + 0x08)
  const name = CONTENT_TYPES[ctype] ?? `?(${ctype})`
  console.log(
    en.name.padEnd(42) + String(en.abs).padStart(10) + String(en.size).padStart(10) +
      '  ' + magic.padEnd(6) + '0x' + magicAt.toString(16).padStart(4, '0') + '  ' + name
  )
  console.log(
    `    dist=${dist} keygen=${keygen} cryptoIdx=${cryptoIdx} ncaSize(media units)=${ncaSize}`
  )
}
console.log('')
console.log(
  'A signature that is ALL ZERO is the normal result of a homebrew NCA builder: the\n' +
    'console will refuse the NCA unless signature checks are patched (Atmosphere +\n' +
    'sigpatches, or DBI/other installers that ignore them). That is a fact about the\n' +
    'file, and it is the first thing to check when an install refuses.'
)
