// What is on the CATEGORY page, and what selects a tier of it?
//
// The home page's "more" card is one per TIER (its heading is the tier's name), so wiring it up means
// answering two questions with measurements rather than guesses:
//
//   1. what URL is the category page, and does it take a parameter that selects a tier?
//   2. what is the item shape, and does the EXISTING parser (hg_api_parse_home, whose second pass
//      looks for an array of things shaped like a series) already read it -- or is a new parser
//      needed, as it was for the search page?
//
// The home page's own sections carry `tab_type` (measured: hg_api.h HgHomeSection), so those numbers
// are the candidates. Every attempt prints the item count and the first few titles, because "the
// parameter was accepted" and "the parameter changed the list" are different facts.
//
// Usage: node probe_category_page.mjs [--save <file>]

const UA =
  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

function extractRouter(html) {
  const m = html.indexOf('_ROUTER_DATA')
  if (m < 0) return null
  const first = html.indexOf('{', m)
  let depth = 0
  let q = false
  let esc = false
  for (let i = first; i < html.length; i++) {
    const c = html[i]
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
      if (!depth) return html.slice(first, i + 1)
    }
  }
  return null
}

async function get(url) {
  const res = await fetch(url, {
    headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' },
    signal: AbortSignal.timeout(20000),
  })
  const html = await res.text()
  return { status: res.status, html, blob: extractRouter(html) }
}

/* Every array whose elements carry a series id/name, with the path it was found at. */
function lists(node, path = '', out = []) {
  if (!node || typeof node !== 'object') return out
  if (Array.isArray(node)) {
    const first = node.find((e) => e && typeof e === 'object')
    if (first && ('series_id' in first || 'name' in first)) {
      out.push({ path, count: node.length, keys: Object.keys(first), first })
    }
    node.forEach((v, i) => lists(v, `${path}[${i}]`, out))
    return out
  }
  for (const [k, v] of Object.entries(node)) lists(v, path ? `${path}.${k}` : k, out)
  return out
}

/* The home page's own tiers, so the category parameter can be tried with REAL values. */
const home = await get('https://hongguoduanju.com/')
let tabs = []
if (home.blob) {
  const ld = JSON.parse(home.blob).loaderData || {}
  for (const v of Object.values(ld)) {
    if (!v || typeof v !== 'object' || !Array.isArray(v.homeSections)) continue
    tabs = v.homeSections.map((s) => ({ tab_type: s.tab_type, tab_name: s.tab_name }))
  }
}
console.log(`home sections (tab_type / tab_name): ${tabs.map((t) => `${t.tab_type}=${JSON.stringify(t.tab_name)}`).join('  ')}`)

const CANDIDATES = [
  ['/category', 'https://hongguoduanju.com/category'],
]
for (const t of tabs.slice(0, 4)) {
  CANDIDATES.push([`/category?tab_type=${t.tab_type}`, `https://hongguoduanju.com/category?tab_type=${t.tab_type}`])
  CANDIDATES.push([`/category/${t.tab_type}`, `https://hongguoduanju.com/category/${t.tab_type}`])
}

const saveIndex = process.argv.indexOf('--save')
const saveTo = saveIndex >= 0 ? process.argv[saveIndex + 1] : null

for (const [label, url] of CANDIDATES) {
  try {
    const r = await get(url)
    if (!r.blob) {
      console.log(`\n${label}\n  status=${r.status} no router blob (${r.html.length}b)`)
      continue
    }
    const data = JSON.parse(r.blob)
    const ld = data.loaderData || {}
    const found = lists(ld).filter((l) => l.count > 2)
    console.log(`\n${label}`)
    console.log(`  status=${r.status} html=${r.html.length}b router=${r.blob.length}b loaderData=[${Object.keys(ld).map((k) => JSON.stringify(k)).join(', ')}]`)
    for (const l of found.slice(0, 3)) {
      const first = l.first
      const title = first.series_name || first.series_title || first.name || '?'
      const id = first.series_id || first.keyword || '?'
      console.log(`  list @ loaderData.${l.path}  count=${l.count}  first: ${JSON.stringify(String(title).slice(0, 22))} id=${id}`)
      console.log(`    keys: ${l.keys.slice(0, 14).join(', ')}`)
    }
    if (saveTo && label === '/category') {
      const fs = await import('node:fs')
      fs.writeFileSync(saveTo, r.blob)
      console.log(`  saved router blob -> ${saveTo}`)
    }
  } catch (e) {
    console.log(`\n${label}\n  FAILED ${e && (e.cause ? e.cause.message : e.message)}`)
  }
}
