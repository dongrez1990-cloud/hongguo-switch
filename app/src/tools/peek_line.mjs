// Print a slice of one line of a file.
//
// Needed because some of the reference implementation is minified onto single
// lines far longer than the read tool will return, and the tail is the part that
// matters (the final assembly and return value of buildMedusa, for instance).
//
// Usage: node peek_line.mjs <file> <1-based line> [start] [len]

import { readFileSync } from 'node:fs'

const [file, lineNoRaw, startRaw, lenRaw] = process.argv.slice(2)
if (!file || !lineNoRaw) {
  console.error('usage: node peek_line.mjs <file> <1-based line> [start] [len]')
  process.exit(2)
}

const lineNo = Number(lineNoRaw)
const start = Number(startRaw ?? 0)
const len = lenRaw === undefined ? 4000 : Number(lenRaw)

const lines = readFileSync(file, 'utf8').split('\n')
if (lineNo < 1 || lineNo > lines.length) {
  console.error(`line ${lineNo} out of range (file has ${lines.length})`)
  process.exit(1)
}

const line = lines[lineNo - 1]
console.log(`line ${lineNo}: total length ${line.length}`)
console.log('---')
console.log(line.slice(start, start + len))
