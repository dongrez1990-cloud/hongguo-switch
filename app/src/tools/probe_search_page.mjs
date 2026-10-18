// Dump the SEARCH page's real shape, for the search screen.
//
// WHY A NEW PROBE. app/src/tools/dump_all_shapes.mjs looks for arrays whose elements carry
// `series_id`, which is what makes something a content list on the home, category and detail pages.
// On the SEARCH page that test finds nothing -- and it was reported as "the search page has no
// content" while the page actually carried 108 results. The ID there is in `keyword`, and the title
// in `name`. Guessing the shape from another page is exactly the mistake this project keeps paying
// for, so this dumps what is really there.
//
// It also answers the two questions a search screen has to have answers to before it is written:
//   * is there a COVER field per hit, and what is it called?
//   * is there any way to ask for the NEXT page from this HTML (a page/cursor field)?
//
// Usage: node probe_search_page.mjs [keyword] [--page2]

const KEYWORD = process.argv[2] && !process.argv[2].startsWith('--') ? process.argv[2] : '\u77ed\u5267'
const TRY_PAGE2 = process.argv.includes('--page2')

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

function trim(v, max = 60) {
  if (typeof v === 'string') return v.length > max ? v.slice(0, max) + '...' : v
  return v
}

async function get(url) {
  const res = await fetch(url, {
    headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' },
    signal: AbortSignal.timeout(20000),
  })
  const html = await res.text()
  return { status: res.status, html }
}

const url = `https://hongguoduanju.com/search/${encodeURIComponent(KEYWORD)}`
console.log(`GET ${url}`)
const { status, html } = await get(url)
console.log(`status=${status}  html=${html.length}b`)

const blob = extractRouter(html)
console.log(`router blob: ${blob ? blob.length + 'b' : 'NONE'}`)
if (!blob) process.exit(1)

const data = JSON.parse(blob)
const ld = data.loaderData || {}
console.log(`\nloaderData keys:`)
for (const k of Object.keys(ld)) {
  const v = ld[k]
  const kind = v === null ? 'null' : Array.isArray(v) ? `array(${v.length})` : typeof v
  console.log(`  ${JSON.stringify(k)}  -> ${kind}${v && typeof v === 'object' && !Array.isArray(v) ? `  keys: ${Object.keys(v).slice(0, 12).join(', ')}` : ''}`)
}

/* THE SEARCH PAGE'S OWN KEY, which contains the keyword and therefore cannot be hardcoded by name
 * with certainty -- so find it by the array inside it instead. */
let hits = null
let hitsPath = null
for (const [k, v] of Object.entries(ld)) {
  if (!v || typeof v !== 'object') continue
  for (const [k2, v2] of Object.entries(v)) {
    if (Array.isArray(v2) && v2.length && v2[0] && typeof v2[0] === 'object' && ('keyword' in v2[0] || 'name' in v2[0])) {
      hits = v2
      hitsPath = `loaderData[${JSON.stringify(k)}].${k2}`
    }
  }
}

if (!hits) {
  console.log('\nNO search hits found by the keyword/name test. Dumping each loaderData value:')
  for (const [k, v] of Object.entries(ld)) console.log(`  ${k}: ${JSON.stringify(v).slice(0, 400)}`)
  process.exit(1)
}

console.log(`\n=== hits at ${hitsPath} ===`)
console.log(`count: ${hits.length}`)
console.log(`first item keys: ${Object.keys(hits[0]).join(', ')}`)

const it = hits[0]
console.log(`\nfirst item (trimmed):`)
for (const [k, v] of Object.entries(it)) {
  if (v && typeof v === 'object') console.log(`  ${k}: <${Array.isArray(v) ? `array(${v.length})` : 'object'}> ${JSON.stringify(v).slice(0, 160)}`)
  else console.log(`  ${k}: ${JSON.stringify(trim(v))}`)
}

/* Every hit's title/id/cover, so the parser's field names are measured rather than assumed. */
console.log(`\nall hits (title / id / cover-if-any):`)
for (const h of hits) {
  const cover =
    h.series_cover ||
    h.cover ||
    (h.video_data && (h.video_data.series_cover || h.video_data.cover || h.video_data.video_cover)) ||
    h.thumbnail ||
    '(none found)'
  console.log(`  ${JSON.stringify(trim(h.name, 28))}  id=${JSON.stringify(h.keyword)}  cover=${JSON.stringify(trim(cover))}`)
}

/* IS THERE A NEXT PAGE? The signed search API is dead (200 / 0 bytes), so a second page can only
 * come from this HTML -- which means a page/cursor field has to be visible here. */
console.log(`\npage-cursor fields anywhere in loaderData:`)
const found = []
;(function walk(node, path) {
  if (!node || typeof node !== 'object') return
  if (Array.isArray(node)) return node.forEach((v, i) => walk(v, `${path}[${i}]`))
  for (const [k, v] of Object.entries(node)) {
    if (/page|cursor|passback|offset|has_?more|total/i.test(k) && (typeof v !== 'object' || v === null)) {
      found.push(`${path}.${k} = ${JSON.stringify(trim(v, 40))}`)
    }
    walk(v, `${path}.${k}`)
  }
})(ld, 'loaderData')
console.log(found.length ? found.slice(0, 20).join('\n') : '  (none)')

if (TRY_PAGE2) {
  const url2 = `https://hongguoduanju.com/search/${encodeURIComponent(KEYWORD)}?page=2`
  console.log(`\n--- trying ${url2}`)
  const r2 = await get(url2)
  const b2 = extractRouter(r2.html)
  if (b2) {
    const d2 = JSON.parse(b2)
    const ids2 = []
    ;(function walk(node) {
      if (!node || typeof node !== 'object') return
      if (Array.isArray(node)) return node.forEach(walk)
      if (node.keyword && node.name) ids2.push(node.keyword)
      for (const v of Object.values(node)) walk(v)
    })(d2)
    console.log(`status=${r2.status}  hits=${ids2.length}  first=${ids2[0]}  same-as-page-1=${ids2[0] === it.keyword}`)
  } else {
    console.log(`status=${r2.status}  no router blob`)
  }
}
