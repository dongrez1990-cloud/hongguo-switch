// How does the CATEGORY page paginate, and how do you know when to stop?
//
// The category page's own HTML links to /category?page=2 .. ?page=34, so paging exists -- but three
// things have to be measured before the screen can implement "keep scrolling":
//
//   1. does `?page=N` work on a SLUG page too (/category/comic-drama?page=2), or only on the bare one?
//   2. do the pages actually differ (compare ids, not counts)?
//   3. is there a field that says how many pages there are (`totalCount`, `hasMore`, a page count), or
//      does the screen have to stop when a page comes back short?
//
// Usage: node probe_category_paging.mjs [slug]

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
  const r = await fetch(url, {
    headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' },
    signal: AbortSignal.timeout(25000),
  })
  const html = await r.text()
  const blob = extractRouter(html)
  return { status: r.status, html, json: blob ? JSON.parse(blob) : null }
}

/* The result list on a category page: the array of series-shaped cards, wherever it hangs. */
function listOf(json) {
  let best = null
  ;(function walk(n, path) {
    if (!n || typeof n !== 'object') return
    if (Array.isArray(n)) {
      const first = n.find((e) => e && typeof e === 'object')
      if (first && (first.series_id || first.name)) {
        if (!best || n.length > best.arr.length) best = { path, arr: n }
      }
      return n.forEach((v, i) => walk(v, `${path}[${i}]`))
    }
    for (const [k, v] of Object.entries(n)) walk(v, path ? `${path}.${k}` : k)
  })(json.loaderData || {}, 'loaderData')
  return best
}

/* Every scalar field of the page object that could be a count/cursor, so "how do I stop" is answered by
 * the page rather than by a guess. */
function pageFields(json) {
  const out = []
  ;(function walk(n, path, depth) {
    if (!n || typeof n !== 'object' || depth > 3) return
    if (Array.isArray(n)) return
    for (const [k, v] of Object.entries(n)) {
      if (typeof v !== 'object' || v === null) {
        if (/page|total|count|has_?more|size|num|end|cursor/i.test(k)) out.push(`${path}.${k} = ${JSON.stringify(v)}`)
      } else {
        walk(v, `${path}.${k}`, depth + 1)
      }
    }
  })(json.loaderData || {}, 'loaderData', 0)
  return out
}

const slug = process.argv[2] || 'comic-drama'
const base = slug ? `https://hongguoduanju.com/category/${slug}` : 'https://hongguoduanju.com/category'

console.log(`base: ${base}\n`)
const seen = []
for (const page of [1, 2, 3, 34, 35]) {
  const url = page === 1 ? base : `${base}?page=${page}`
  const r = await get(url)
  if (!r.json) {
    console.log(`page ${String(page).padEnd(3)} status=${r.status}  no router blob (${r.html.length}b)`)
    continue
  }
  const l = listOf(r.json)
  const ids = l ? l.arr.map((e) => String(e.series_id || e.keyword)) : []
  const first = ids[0] || '-'
  const dup = seen.includes(first)
  if (first !== '-') seen.push(first)
  console.log(
    `page ${String(page).padEnd(3)} status=${r.status} html=${String(r.html.length).padEnd(7)} list@${l ? l.path : 'NONE'} count=${ids.length} first=${first}${dup ? '  <-- SAME AS AN EARLIER PAGE' : ''}`
  )
  if (page === 1) {
    console.log(`   first 3 titles: ${l ? l.arr.slice(0, 3).map((e) => e.series_name || e.name).join(' | ') : '-'}`)
    console.log(`   count-ish fields on the page:`)
    for (const f of pageFields(r.json).slice(0, 12)) console.log(`     ${f}`)
  }
}

/* And the bare category page, for comparison: does it page too? */
if (slug) {
  const r2 = await get('https://hongguoduanju.com/category?page=2')
  const l2 = listOf(r2.json || {})
  const ids2 = l2 ? l2.arr.map((e) => String(e.series_id)) : []
  console.log(`\nbare /category?page=2  status=${r2.status} count=${ids2.length} first=${ids2[0] || '-'}`)
}
