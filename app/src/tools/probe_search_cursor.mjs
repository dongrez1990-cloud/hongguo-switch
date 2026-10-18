// Does the signed search API need the search_id/passback cursors from the HTML page?
//
// Background: the signed search API answers 200 with Content-Length: 0 both from
// the console and from the reference implementation run verbatim. That looked
// like a dead endpoint. But ContentRepository.kt:68-95 shows the real flow is
// TWO steps:
//
//   1. fetch the search HTML page, read search_tabs -> search_id + passback
//   2. call the signed API WITH those cursors
//
//     if (it.searchId.isNotBlank()) values["search_id"] = it.searchId
//
// Every probe so far called step 2 with no cursors. If the cursors are what the
// server needs, the endpoint was never dead and ~400 lines of verified signing
// code has a consumer after all.
//
// Usage: node probe_search_cursor.mjs <hongguo.cjs>

import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const src = process.argv[2]
if (!src) { console.error('usage: node probe_search_cursor.mjs <hongguo.cjs>'); process.exit(2) }

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'
const KEYWORD = '短剧'

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console, fetch, setTimeout, AbortController })
vm.runInContext(source + `
  ;globalThis.ref = { APP_DEVICE, APP_UA, pythonUrlEncode, xGorgon, helios, buildMedusa, md5 };
`, ctx)
const R = ctx.ref

function extractRouter(html) {
  const m = html.indexOf('_ROUTER_DATA')
  if (m < 0) return null
  const first = html.indexOf('{', m)
  let depth = 0, q = false, esc = false
  for (let i = first; i < html.length; i++) {
    const c = html[i]
    if (q) { if (esc) esc = false; else if (c === '\\') esc = true; else if (c === '"') q = false; continue }
    if (c === '"') q = true
    else if (c === '{' || c === '[') depth++
    else if (c === '}' || c === ']') { depth--; if (!depth) return html.slice(first, i + 1) }
  }
  return null
}

/* find every object carrying search_id, wherever it lives */
function findCursors(node, path = '', out = []) {
  if (!node || typeof node !== 'object') return out
  if (!Array.isArray(node) && 'search_id' in node) {
    out.push({ path, search_id: node.search_id, passback: node.passback, keys: Object.keys(node) })
  }
  for (const [k, v] of Object.entries(node)) findCursors(v, path ? `${path}.${k}` : k, out)
  return out
}

// ---- step 1: the HTML page gives the cursors ------------------------------
const pageUrl = `https://hongguoduanju.com/search/${encodeURIComponent(KEYWORD)}`
const html = await (await fetch(pageUrl, { headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' } })).text()
const blob = extractRouter(html)
console.log(`search page  : ${html.length} bytes, router=${blob ? blob.length : 'NONE'}`)
const data = JSON.parse(blob)
console.log(`loaderData   : ${Object.keys(data.loaderData).map((k) => JSON.stringify(k)).join(', ')}`)

const cursors = findCursors(data.loaderData)
console.log(`cursors found: ${cursors.length}`)
for (const c of cursors.slice(0, 6)) {
  console.log(`  @ ${c.path}`)
  console.log(`      search_id="${String(c.search_id).slice(0, 40)}" passback="${String(c.passback || '').slice(0, 40)}"`)
  console.log(`      keys: ${c.keys.join(',')}`)
}

// also look for search_tabs by name
function findKey(node, key, path = '', out = []) {
  if (!node || typeof node !== 'object') return out
  for (const [k, v] of Object.entries(node)) {
    if (k === key) out.push({ path: `${path}.${k}`, value: v })
    findKey(v, key, path ? `${path}.${k}` : k, out)
  }
  return out
}
const tabs = findKey(data.loaderData, 'search_tabs')
console.log(`\nsearch_tabs  : ${tabs.length} occurrence(s)`)
for (const t of tabs.slice(0, 3)) {
  const v = t.value
  console.log(`  @ loaderData${t.path}  type=${Array.isArray(v) ? 'array[' + v.length + ']' : typeof v}`)
  if (Array.isArray(v) && v.length) console.log(`      entry keys: ${Object.keys(v[0]).join(',')}`)
}

// ---- step 2: call the API WITH the cursors --------------------------------
const cursor = cursors[0] || {}
const API = 'https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v'
const khronos = Math.floor(Date.now() / 1000)

async function attempt(label, extra) {
  const ticket = Date.now()
  const values = {
    ...R.APP_DEVICE,
    query: KEYWORD, count: '20', offset: '0', tab_type: '11',
    bookshelf_search_plan: '4', use_correct: 'true', user_is_login: '0',
    ts: String(khronos), _rticket: String(ticket),
    ...extra,
  }
  const query = R.pythonUrlEncode(values)
  const url = `${API}?${query}`
  const random = Math.floor(Math.random() * 65536)
  const be32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0, 0); return b }
  const le32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0, 0); return b }
  const headers = {
    'User-Agent': R.APP_UA,
    Accept: 'application/json; charset=utf-8,application/x-protobuf',
    'x-ss-req-ticket': String(ticket), 'x-tt-request-tag': 't=0;n=0',
    'sdk-version': '2', 'passport-sdk-version': '50561', 'x-xs-from-web': '0',
    'x-khronos': String(khronos),
    'x-ladon': be32(khronos).toString('base64'),
    'x-argus': le32(khronos).toString('base64'),
    'x-gorgon': R.xGorgon(query, null, khronos, random),
    'x-helios': R.helios(khronos),
    'x-medusa': R.buildMedusa(url, null, khronos),
    'x-tt-dt': '',
  }
  const res = await fetch(url, { headers })
  const text = await res.text()
  console.log(`\n${label}`)
  console.log(`  query len ${query.length}  ->  status ${res.status}, body ${text.length} bytes`)
  if (text.length) console.log(`  head: ${text.slice(0, 500)}`)
  return text.length
}

await attempt('A. WITHOUT cursors (what every probe did so far)', {})
if (cursor.search_id) {
  await attempt('B. WITH search_id from the HTML page', { search_id: String(cursor.search_id) })
  if (cursor.passback) {
    await attempt('C. WITH search_id AND passback', {
      search_id: String(cursor.search_id), passback: String(cursor.passback),
    })
  }
} else {
  console.log('\nno search_id found in the page -- cannot test the cursor hypothesis')
}
