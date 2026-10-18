/*
 * gotscan.mjs -- read the 8 bytes a given instruction will load out of the GOT.
 *
 * Usage: node gotscan.mjs <file.elf> <vaddr-hex>
 *
 * WHY THIS EXISTS. The Switch build reports, at runtime, that pthread_create
 * returned ENOSYS (88). Its only two ENOSYS exits are taken when the GOT slot
 * holding __syscall_thread_create is NULL. That is a claim about the bytes in
 * the file, so it is checked by reading the bytes -- not by reasoning about the
 * linker.
 *
 * vaddr is translated to a file offset through the program headers, which is
 * how a PIE segment is laid out in the file.
 */
import { readFileSync } from 'node:fs'

const [, , file, vaddrArg] = process.argv
if (!file || !vaddrArg) {
  console.error('usage: node gotscan.mjs <file.elf> <vaddr-hex>')
  process.exit(2)
}
const vaddr = BigInt(vaddrArg.startsWith('0x') ? vaddrArg : '0x' + vaddrArg)

const buf = readFileSync(file)
if (buf.length < 64 || buf.readUInt32LE(0) !== 0x464c457f || buf[4] !== 2) {
  console.error('not a 64-bit ELF:', file)
  process.exit(2)
}
const ePhoff = Number(buf.readBigUInt64LE(0x20))
const ePhentsize = buf.readUInt16LE(0x36)
const ePhnum = buf.readUInt16LE(0x38)

let mapped = null
for (let i = 0; i < ePhnum; i++) {
  const o = ePhoff + i * ePhentsize
  const type = buf.readUInt32LE(o)
  if (type !== 1) continue // PT_LOAD
  const off = buf.readBigUInt64LE(o + 0x08)
  const va = buf.readBigUInt64LE(o + 0x10)
  const fsz = buf.readBigUInt64LE(o + 0x20)
  if (vaddr >= va && vaddr < va + fsz) {
    mapped = { off, va, fsz }
    break
  }
}
if (!mapped) {
  console.error('vaddr not inside any PT_LOAD file range: ' + vaddr.toString(16))
  process.exit(1)
}

const fileOff = Number(mapped.off + (vaddr - mapped.va))
const value = buf.readBigUInt64LE(fileOff)

console.log(`file          ${file}`)
console.log(`vaddr         0x${vaddr.toString(16)}`)
console.log(`file offset   0x${fileOff.toString(16)}`)
console.log(`bytes         ${[...buf.subarray(fileOff, fileOff + 8)].map(b => b.toString(16).padStart(2, '0')).join(' ')}`)
console.log(`value (LE64)  0x${value.toString(16).padStart(16, '0')}`)
console.log(value === 0n ? 'VERDICT       ZERO -- a load from here yields NULL' : 'VERDICT       non-zero')

/* A RELR-relative fixup adds the load bias to this stored value at load time,
 * so a non-zero stored value is an unrelocated address, not a final one. The
 * question this script answers is only whether it is 0 or not. */
