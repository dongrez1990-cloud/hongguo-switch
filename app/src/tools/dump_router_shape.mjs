// Dump the real structure of the pages this port now depends on.
//
// The signed search API is dead (200 / Content-Length: 0, confirmed by running
// the reference implementation itself), but home / category / search / rank all
// serve real server-rendered HTML carrying _ROUTER_DATA. hg_router_extract()
// already pulls that blob out; what it does NOT know is the field names inside,
// and the api parser currently carries fields that were INFERRED rather than
// observed. This prints what is actually there.
//
// Usage: node dump_router_shape.mjs [url]

const URL_ = process.argv[2] || 'https://hongguoduanju.com/'
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

function extractRouter(html) {
  const marker = html.indexOf('_ROUTER_DATA')
  if (marker < 0) throw new Error('no _ROUTER_DATA')
  const first = html.indexOf('{', marker)
  let depth = 0, quoted = false, escaped = false
  for (let i = first; i < html.length; i++) {
    const ch = html[i]
    if (quoted) { if (escaped) escaped = false; else if (ch === '\\') escaped = true; else if (ch === '"') quoted = false; continue }
    if (ch === '"') quoted = true
    else if (ch === '{' || ch === '[') depth++
    else if (ch === '}' || ch === ']') { depth--; if (depth === 0) return html.slice(first, i + 1) }
  }
  throw new Error('unbalanced')
}

function shape(v, depth = 0, maxDepth = 3) {
  if (v === null) return 'null'
  if (Array.isArray(v)) {
    if (!v.length) return '[] (empty)'
    return `[${v.length} x ${depth < maxDepth ? shape(v[0], depth + 1, maxDepth) : '...'}]`
  }
  if (typeof v === 'object') {
    if (depth >= maxDepth) return '{...}'
    const keys = Object.keys(v).slice(0, 24)
    return `{ ${keys.map((k) => `${k}: ${shape(v[k], depth + 1, maxDepth)}`).join(', ')}${Object.keys(v).length > 24 ? ', ...' : ''} }`
  }
  if (typeof v === 'string') return v.length > 42 ? `"${v.slice(0, 42)}..."` : JSON.stringify(v)
  return String(v)
}

const res = await fetch(URL_, { headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' } })
const html = await res.text()
const blob = extractRouter(html)
const data = JSON.parse(blob)

console.log(`url        : ${URL_}`)
console.log(`status     : ${res.status}`)
console.log(`router size: ${blob.length} bytes\n`)

const ld = data.loaderData
console.log('loaderData keys:')
for (const [k, v] of Object.entries(ld)) {
  console.log(`  ${JSON.stringify(k).padEnd(44)} -> ${typeof v}`)
}

// Find the first object that looks like a content list and show one entry.
function findList(node, path = '') {
  if (node === null || typeof node !== 'object') return null
  if (Array.isArray(node) && node.length && node[0] && typeof node[0] === 'object') return { path, arr: node }
  for (const [k, v] of Object.entries(node)) {
    const r = findList(v, path ? `${path}.${k}` : k)
    if (r) return r
  }
  return null
}

console.log('\nfirst list found:')
const hit = findList(ld.page)
if (hit) {
  console.log(`  path  : loaderData.page.${hit.path}`)
  console.log(`  length: ${hit.arr.length}`)
  console.log('  entry 0:')
  console.log('   ', shape(hit.arr[0], 0, 2))
  console.log('\n  entry 0 raw (first 1500 chars):')
  console.log(JSON.stringify(hit.arr[0], null, 1).slice(0, 1500))
} else {
  console.log('  (none found)')
}

console.log('\n=== loaderData.page shape (depth 3) ===')
console.log(shape(ld.page, 0, 3))
