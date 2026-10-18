// Locate the exact byte where loaderData's value ends in the fixture, so the C
// side can be tested with a length that provably covers the whole object.
//
// Context: hg_json_skip(buf, 38008, 14) returns 0 -- it cannot balance a ~38KB
// nested object -- while the identical call on a 53-byte literal succeeds. Either
// the blob contains something the C scanner mishandles, or the C scanner gives up
// early. Knowing where the object ACTUALLY ends separates those two.

import { readFileSync } from 'node:fs'

const path = process.argv[2]
const s = readFileSync(path, 'latin1')

const START = 14   // the '{' opening loaderData's value
let p = START, depth = 0, q = false, esc = false
for (; p < s.length; p++) {
  const c = s[p]
  if (q) {
    if (esc) esc = false
    else if (c === '\\') esc = true
    else if (c === '"') q = false
    continue
  }
  if (c === '"') q = true
  else if (c === '{' || c === '[') depth++
  else if (c === '}' || c === ']') {
    depth--
    if (depth === 0) { p++; break }
  }
}
console.log(`total length          : ${s.length}`)
console.log(`loaderData value ends : ${p}   (balanced: ${depth === 0})`)
console.log(`context after end     : ${JSON.stringify(s.slice(p, p + 24))}`)

/* Does any string fail to terminate under C's own skip_string rule?
 * Rule: a backslash consumes the next character, unconditionally. */
let i = 0, firstBad = -1, badAt = -1
while (i < s.length) {
  if (s[i] === '"') {
    let j = i + 1
    while (j < s.length) {
      if (s[j] === '\\') { j += 2; continue }
      if (s[j] === '"') { j++; break }
      j++
    }
    if (j > s.length || (j >= s.length && s[s.length - 1] !== '"')) { firstBad = i; badAt = j; break }
    i = j
  } else i++
}
console.log(`first runaway string  : ${firstBad}${firstBad >= 0 ? ` (scanner ran to ${badAt}, len ${s.length})` : ' (none)'}`)
if (firstBad >= 0) {
  console.log(`  context             : ${JSON.stringify(s.slice(Math.max(0, firstBad - 40), firstBad + 40))}`)
}

/* How deep does the object nest? An int depth counter cannot overflow on this,
 * but printing it rules the idea out. */
let d = 0, maxd = 0
let k = START
let inq = false, e2 = false
for (; k < p; k++) {
  const c = s[k]
  if (inq) { if (e2) e2 = false; else if (c === '\\') e2 = true; else if (c === '"') inq = false; continue }
  if (c === '"') inq = true
  else if (c === '{' || c === '[') { d++; if (d > maxd) maxd = d }
  else if (c === '}' || c === ']') d--
}
console.log(`max nesting depth     : ${maxd}`)
