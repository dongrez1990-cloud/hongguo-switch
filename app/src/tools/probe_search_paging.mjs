// Can the search page give MORE than its first 10 results?
//
// WHY THIS EXISTS. The search screen shows the page's first page (10 items) and no way to ask for the
// next one was found: `/search/<kw>?page=2` came back item-for-item identical to page 1, and the HTML
// carries no cursor field (only `totalCount`, e.g. 201). Before telling the user "upstream only gives
// ten", every plausible parameter spelling is worth trying ONCE, because the difference is between a
// screen that shows 10 of 201 and a screen that can page through them.
//
// Each attempt is judged by the FIRST ITEM ID it returns: different from page 1 means the parameter
// did something, identical means it was ignored. Comparing ids rather than counts is deliberate --
// a paging parameter that silently returns page 1 would otherwise look like success.
//
// Usage: node probe_search_paging.mjs [keyword]

const KEYWORD = process.argv[2] || 'ai'
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

/* Every object with both a name and a keyword, wherever it hangs -- the search page's own test. */
function hitsOf(json) {
  const out = []
  ;(function walk(n) {
    if (!n || typeof n !== 'object') return
    if (Array.isArray(n)) return n.forEach(walk)
    if (n.keyword && n.name) out.push({ id: String(n.keyword), name: String(n.name) })
    for (const v of Object.values(n)) walk(v)
  })(json.loaderData || json)
  return out
}

async function tryUrl(label, url) {
  try {
    const res = await fetch(url, {
      headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' },
      signal: AbortSignal.timeout(20000),
    })
    const html = await res.text()
    const blob = extractRouter(html)
    if (!blob) return { label, status: res.status, note: 'no router blob' }
    const json = JSON.parse(blob)
    const hits = hitsOf(json)
    return { label, status: res.status, count: hits.length, first: hits[0] ? hits[0].id : '-', firstTitle: hits[0] ? hits[0].name : '' }
  } catch (e) {
    return { label, status: 0, note: e && (e.cause ? e.cause.message : e.message) }
  }
}

const kw = encodeURIComponent(KEYWORD)
const base = `https://hongguoduanju.com/search/${kw}`

const CANDIDATES = [
  ['page 1 (the control)', base],
  ['?page=2', `${base}?page=2`],
  ['?pageNo=2', `${base}?pageNo=2`],
  ['?page_num=2', `${base}?page_num=2`],
  ['?p=2', `${base}?p=2`],
  ['?pn=2', `${base}?pn=2`],
  ['?offset=10', `${base}?offset=10`],
  ['?start=10', `${base}?start=10`],
  ['?cursor=10', `${base}?cursor=10`],
  ['?search_page=2', `${base}?search_page=2`],
  ['?tab_type=11&page=2', `${base}?tab_type=11&page=2`],
  ['/2 (path segment)', `${base}/2`],
  ['/page/2', `${base}/page/2`],
  ['query form ?keyword=<kw>&page=2', `https://hongguoduanju.com/search?keyword=${kw}&page=2`],
  ['query form ?q=<kw>&page=2', `https://hongguoduanju.com/search?q=${kw}&page=2`],
]

console.log(`keyword: ${KEYWORD}\n`)
const results = []
for (const [label, url] of CANDIDATES) {
  const r = await tryUrl(label, url)
  results.push(r)
  const extra = r.note ? r.note : `hits=${r.count} first=${r.first} ${r.firstTitle ? JSON.stringify(r.firstTitle.slice(0, 18)) : ''}`
  console.log(`${label.padEnd(32)} status=${String(r.status).padEnd(4)} ${extra}`)
}

const control = results[0]
console.log(`\n=== verdict ===`)
const moved = results.filter((r) => r.count > 0 && r.first !== control.first)
if (!moved.length) {
  console.log('EVERY candidate returned the SAME first item as page 1 (or nothing at all).')
  console.log('So the first page of this HTML is all upstream will give: the site pages its results')
  console.log('through the signed API, which answers 200 with an empty body.')
} else {
  console.log('These candidates CHANGED the first item, i.e. they page:')
  for (const m of moved) console.log(`  ${m.label}  -> hits=${m.count} first=${m.first} ${JSON.stringify(m.firstTitle.slice(0, 18))}`)
}
