/*
 * nca-identify.mjs -- identify the NCAs inside an NSP, and say so honestly when
 * it cannot.
 *
 * Usage: node nca-identify.mjs <file.nsp> [prod.keys]
 *
 * WHY. scripts/verify-nsp.ps1 proves the PFS0 container is well formed and stops
 * there. It cannot say whether the three entries are real NCAs, and for THIS
 * project's NSP that question is not academic:
 *
 *   - a byte search for "NCA" over the whole file finds NOTHING
 *   - the first 256 bytes of all three entries are IDENTICAL
 *     (44 99 2f c0 05 09 0c 46 ...)
 *
 * Identical ciphertext for identical plaintext under the same key and tweak is
 * the signature of a ZEROED prefix -- an unsigned NCA header -- which also means
 * the encrypted region starts at offset 0, not at the usual 0x200. That is a
 * hypothesis about bytes, so it is tested by trying it: decrypt and look for the
 * magic. A previous version of this script assumed the region began at 0x200 (or
 * 0x100) with header_key alone and found nothing, and it reported that failure
 * rather than dressing it up as success.
 *
 * This version widens the search: several candidate keys out of prod.keys, three
 * region starts, and a sweep of XTS tweak values. Every combination is tried and
 * the FIRST that yields "NCA" is reported WITH the parameters that found it, so
 * the answer can be reproduced by hand.
 */

import { readFileSync } from 'node:fs'
import { createDecipheriv } from 'node:crypto'
import { homedir } from 'node:os'

const CONTENT_TYPES = ['Program', 'Meta', 'Control', 'Manual', 'Data', 'PublicData']

const file = process.argv[2]
const keysPath = process.argv[3] || `${homedir()}\\.switch\\prod.keys`
if (!file) {
  console.error('usage: node nca-identify.mjs <file.nsp> [prod.keys]')
  process.exit(2)
}

const buf = readFileSync(file)
const u32 = (o) => buf.readUInt32LE(o)
const u64 = (o) => Number(buf.readBigUInt64LE(o))

/* ---- keys ----------------------------------------------------------------- */
/* Candidate keys, in the order they are worth trying. header_key is the right one
 * by the format's definition; the key-area keys are here because a builder that
 * encrypted from offset 0 might have used one of them, and trying is cheaper than
 * arguing. */
const KEY_NAMES = [
  'header_key',
  'key_area_key_application_00',
  'key_area_key_application_01',
  'key_area_key_application_02',
]

const keys = {}
try {
  for (const line of readFileSync(keysPath, 'utf8').split(/\r?\n/)) {
    const m = /^\s*([a-z0-9_]+)\s*=\s*([0-9a-fA-F]+)\s*$/.exec(line)
    if (!m) continue
    if (KEY_NAMES.includes(m[1])) keys[m[1]] = Buffer.from(m[2], 'hex')
  }
} catch (e) {
  console.error(`cannot read ${keysPath}: ${e.message}`)
  process.exit(1)
}
for (const n of KEY_NAMES) {
  console.log(`key ${n.padEnd(30)} ${keys[n] ? keys[n].length + ' bytes' : 'ABSENT'}`)
}
console.log('')

/* ---- PFS0 ----------------------------------------------------------------- */
const entryCount = u32(4)
const stringTableSize = u32(8)
const stringTable = 0x10 + entryCount * 0x18
const dataOffset = stringTable + stringTableSize

const entries = []
for (let i = 0; i < entryCount; i++) {
  const e = 0x10 + i * 0x18
  const nameOff = u32(e + 0x10)
  const nameEnd = buf.indexOf(0, stringTable + nameOff)
  entries.push({
    name: buf.toString('ascii', stringTable + nameOff, nameEnd),
    abs: dataOffset + u64(e + 0x00),
    size: u64(e + 0x08),
  })
}
console.log(`entries ${entryCount}, data offset 0x${dataOffset.toString(16)}`)
console.log('')

function xts(key, src, tweakNo) {
  const tw = Buffer.alloc(16)
  tw.writeUInt32LE(tweakNo >>> 0, 0)
  const d = createDecipheriv('aes-128-xts', key, tw)
  d.setAutoPadding(false)
  return Buffer.concat([d.update(src), d.final()])
}

const STARTS = [0x000, 0x100, 0x200, 0x400]
const TWEAKS = Array.from({ length: 16 }, (_, i) => i)

let everFound = false
for (const en of entries) {
  let solved = null
  for (const keyName of KEY_NAMES) {
    const key = keys[keyName]
    if (!key || key.length !== 32) continue
    if (solved) break
    for (const start of STARTS) {
      if (solved) break
      for (const tweak of TWEAKS) {
        const src = buf.subarray(en.abs + start, en.abs + start + 0x200)
        if (src.length < 0x200) continue
        let plain
        try {
          plain = xts(key, src, tweak)
        } catch {
          continue
        }
        const at = plain.indexOf(Buffer.from('NCA'))
        if (at >= 0) {
          solved = { keyName, start, tweak, plain, at }
          break
        }
      }
    }
  }

  console.log(`${en.name}  size=${en.size}`)
  if (!solved) {
    console.log('    NO NCA magic recovered from any key x region-start x tweak tried')
    continue
  }
  everFound = true

  const { keyName, start, tweak, plain, at } = solved
  const magic = plain.toString('ascii', at, at + 4)
  const ctype = plain[at + 5]
  const keygen = plain[at + 6]
  const ncaSize = Number(plain.readBigUInt64LE(at + 8))
  console.log(`    RECOVERED with key=${keyName} regionStart=0x${start.toString(16)} firstTweak=${tweak}`)
  console.log(`    magic=${magic} at plaintext offset ${at}`)
  console.log(`    content type ${CONTENT_TYPES[ctype] ?? '?'} (${ctype}), keygeneration ${keygen}`)
  console.log(`    declared size ${ncaSize} media units = ${ncaSize * 0x200} bytes`)
}

console.log('')
if (everFound) {
  console.log(
    'The NCAs are real and the parameters above are how to read them. The signatures\n' +
      'are hacBrewPack\'s own key pair, not Nintendo\'s, so a console still needs\n' +
      'signature checks patched (Atmosphere + sigpatches) or an installer that ignores\n' +
      'them -- that part is a property of the file and does not depend on this script.'
  )
} else {
  console.log(
    'UNDETERMINED, and that is the honest result rather than a failure to dress up.\n' +
      'What IS established: the PFS0 container is well formed (verify-nsp.ps1 and\n' +
      'nca-inspect.mjs agree on every offset and size, and the sizes sum exactly to the\n' +
      'file length), and the entries are NOT plaintext NCA headers (no "NCA" anywhere in\n' +
      'the file, and the three entries share an identical 256-byte prefix).\n' +
      'What is NOT established: which content each entry is. To settle it, either widen\n' +
      'the search further (the tweak may not be a plain sector counter, and hacBrewPack\n' +
      'can patch the header with its acid public key), or build a known-good NSP with\n' +
      'the same hacBrewPack and compare shapes -- a control, not a guess.'
  )
}
