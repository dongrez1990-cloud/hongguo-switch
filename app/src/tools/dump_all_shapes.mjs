// Find the content lists in the remaining pages, by shape rather than by guess.
//
// The home page's layout was measured and it still taught the wrong lesson once:
// bannerList uses series_name while homeSections[].video_list[] uses series_title,
// so "the homepage field is series_name" was wrong for 36 of 36 section cards.
// Category, search and rank are still known only by their loaderData KEY names --
// their content layout has never been looked at. Guessing it from the home page
// is exactly the mistake that was just corrected.
//
// Rather than a hardcoded path, this walks each document and reports every array
// whose elements carry series_id, which is what makes something a content list.
//
// Usage: node dump_all_shapes.mjs [hongguo.cjs]

const PAGES = [
  ['category', 'https://hongguoduanju.com/category'],
  ['search',   'https://hongguoduanju.com/search/%E7%9F%AD%E5%89%A7'],
  ['rank',     'https://hongguoduanju.com/rank/hot-comic-drama'],
  ['rank2',    'https://hongguoduanju.com/rank/comic-drama'],
]

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

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

function shape(v, depth = 0, maxDepth = 3) {
  if (v === null) return 'null'
  if (Array.isArray(v)) {
    if (!v.length) return '[]'
    return `[${v.length} x ${depth < maxDepth ? shape(v[0], depth + 1, maxDepth) : '...'}]`
  }
  if (typeof v === 'object') {
    if (depth >= maxDepth) return '{...}'
    const keys = Object.keys(v)
    const shown = keys.slice(0, 18)
    return `{ ${shown.map((k) => `${k}: ${shape(v[k], depth + 1, maxDepth)}`).join(', ')}${keys.length > 18 ? `, +${keys.length - 18} more` : ''} }`
  }
  if (typeof v === 'string') return v.length > 34 ? `"${v.slice(0, 34)}..."` : JSON.stringify(v)
  return String(v)
}

/* every array whose elements look like content */
function findContentLists(node, path = '', out = []) {
  if (!node || typeof node !== 'object') return out
  if (Array.isArray(node)) {
    const first = node.find((e) => e && typeof e === 'object')
    if (first && 'series_id' in first) {
      out.push({ path, count: node.length, keys: Object.keys(first) })
    }
    node.forEach((v, i) => findContentLists(v, `${path}[${i}]`, out))
    return out
  }
  for (const [k, v] of Object.entries(node)) findContentLists(v, path ? `${path}.${k}` : k, out)
  return out
}

for (const [name, url] of PAGES) {
  console.log(`\n${'='.repeat(72)}\n${name}  ${url}`)
  try {
    const res = await fetch(url, { headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' }, signal: AbortSignal.timeout(20000) })
    const html = await res.text()
    const blob = extractRouter(html)
    console.log(`status=${res.status}  html=${html.length}b  router=${blob ? blob.length + 'b' : 'NONE'}`)
    if (!blob) continue
    const data = JSON.parse(blob)
    const ld = data.loaderData || {}
    console.log(`loaderData keys: ${Object.keys(ld).map((k) => JSON.stringify(k)).join(', ')}`)

    const lists = findContentLists(ld)
    if (!lists.length) {
      console.log('  no content list found -- dumping the page object to see what IS there')
      /* The page object is the one under loaderData whose key is not a "*_layout". */
      for (const [k, v] of Object.entries(ld)) {
        if (k.endsWith('_layout')) continue
        console.log(`\n  loaderData[${JSON.stringify(k)}] shape:`)
        console.log('   ', shape(v, 0, 3))
      }
      continue
    }
    for (const l of lists) {
      console.log(`\n  list @ loaderData.${l.path}   (${l.count} items)`)
      console.log(`    keys: ${l.keys.join(', ')}`)
      /* which of the two title names does THIS list use? */
      const hasName = l.keys.includes('series_name')
      const hasTitle = l.keys.includes('series_title')
      console.log(`    title field: ${hasTitle ? 'series_title' : ''}${hasTitle && hasName ? ' AND ' : ''}${hasName ? 'series_name' : ''}${!hasTitle && !hasName ? '(neither!)' : ''}`)
    }
  } catch (e) {
    console.log(`FAILED: ${e && (e.cause ? e.cause.message : e.message)}`)
  }
}
