// Build the reference's search query string for fixed inputs, so it can be
// diffed byte-for-byte against the C implementation's.
//
// Why this exists: the C build produced HTTP 200 with a ZERO-LENGTH body, which
// means the server accepted the request and then decided it had nothing to say.
// The reference expects a JSON body with a `code` field. Guessing at the cause
// has already cost two rounds; the query string is a thing that can simply be
// compared.
//
// Usage: node gen_ref_query.mjs <hongguo.cjs> <out-file> <khronos> <ticket>

import { readFileSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, outPath, khronosArg, ticketArg] = process.argv.slice(2)
if (!src || !outPath) {
  console.error('usage: node gen_ref_query.mjs <hongguo.cjs> <out-file> [khronos] [ticket]')
  process.exit(2)
}

const khronos = Number(khronosArg || 1791466175)
const ticket = ticketArg ? String(ticketArg) : String(khronos * 1000)

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console })
vm.runInContext(source + `
  ;globalThis.ref = { APP_DEVICE, DEVICE, pythonUrlEncode, PAGE_SIZE, str };
`, ctx)

const { APP_DEVICE, pythonUrlEncode, PAGE_SIZE } = ctx.ref

// appSearchParams (hongguo.cjs:249) for keyword=短剧, tabType=11, page=1
const values = {
  ...APP_DEVICE,
  query: '短剧',
  count: String(PAGE_SIZE),
  offset: String(Math.max(0, (1 - 1) * PAGE_SIZE)),
  tab_type: String(11),
  bookshelf_search_plan: '4',
  use_correct: 'true',
  user_is_login: '0',
  ts: String(khronos),
  _rticket: ticket,
}

const query = pythonUrlEncode(values)
writeFileSync(outPath, query, 'ascii')

const keys = Object.keys(values)
console.error(`wrote ${outPath}`)
console.error(`  keys   : ${keys.length}`)
console.error(`  length : ${query.length}`)
console.error(`  keys   : ${keys.join(',')}`)
