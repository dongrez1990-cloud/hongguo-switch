/*
 * router-shape.mjs -- what structure does the home page's _ROUTER_DATA actually
 * have?
 *
 * Usage: node router-shape.mjs <fixture.json>
 *
 * WHY. "Reorganise the home page the way wiliwili does it" needs tiers to put
 * things in. wiliwili's home is tabs of grids fed by several endpoints; this app
 * has exactly one page, whose cards come out of hg_api_parse_home() as a flat
 * array of 36. Whether the JSON underneath carries sections (and a title per
 * section) decides whether a tiered layout is a grouping of data that exists, or
 * an invention -- and this project does not invent structure.
 *
 * Output is deliberately descriptive: key paths, array lengths, and the field
 * names of the first card of each array, with no interpretation.
 */

import { readFileSync } from 'node:fs'

const file = process.argv[2]
if (!file) {
  console.error('usage: node router-shape.mjs <fixture.json>')
  process.exit(2)
}

const root = JSON.parse(readFileSync(file, 'utf8'))

/* Walk the object, reporting every array of objects found and the keys of its
 * first element. Depth-limited so a deeply nested payload stays readable. */
const arrays = []

function walk(node, path, depth) {
  if (depth > 6 || node === null || typeof node !== 'object') return

  if (Array.isArray(node)) {
    if (node.length > 0 && node[0] !== null && typeof node[0] === 'object') {
      arrays.push({ path, len: node.length, keys: Object.keys(node[0]) })
      for (let i = 0; i < Math.min(node.length, 3); i++) walk(node[i], `${path}[${i}]`, depth + 1)
    }
    return
  }

  for (const k of Object.keys(node)) walk(node[k], path ? `${path}.${k}` : k, depth + 1)
}

console.log(`file       ${file}`)
console.log(`top-level  ${Object.keys(root).join(', ')}`)
console.log('')

walk(root, '', 0)

arrays.sort((a, b) => b.len - a.len)
console.log('arrays of objects found (largest first):')
for (const a of arrays.slice(0, 25)) {
  console.log(`  ${String(a.len).padStart(5)} items  ${a.path}`)
  console.log(`             keys: ${a.keys.slice(0, 14).join(', ')}`)
}

/* Section-title candidates: any key whose name suggests a section heading, and
 * which appears on a card rather than on a wrapper. */
const SECTION_HINTS = /section|title|categor|type|group|tab|name|label/i
const uniq = new Set()
for (const a of arrays) {
  const hits = a.keys.filter((k) => SECTION_HINTS.test(k))
  for (const h of hits) uniq.add(h)
}
console.log('')
console.log(`field names on cards that could carry a section label: ${[...uniq].join(', ')}`)
