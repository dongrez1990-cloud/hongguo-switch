// Save the FULL _ROUTER_DATA blob as a fixture.
//
// The existing home_router.json is a 1,976-byte trim of a 45,609-byte blob, and
// hg_api_parse_home returns HG_API_ERR_JSON on the real one while passing on the
// trim. A fixture 23x smaller than reality cannot reproduce that: whatever card
// breaks the parser was almost certainly cut away.
//
// Usage: node capture_full_router.mjs <url> <out-file>

import { writeFileSync, readFileSync } from 'node:fs'

const url = process.argv[2] || 'https://hongguoduanju.com/'
const out = process.argv[3]
if (!out) { console.error('usage: node capture_full_router.mjs <url> <out-file>'); process.exit(2) }

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

const res = await fetch(url, { headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' } })
const html = await res.text()
const blob = extractRouter(html)
if (!blob) { console.error('no _ROUTER_DATA'); process.exit(1) }

// Re-serialise through JSON.parse so the fixture is valid JSON in its own right
// and does not depend on the page's exact escaping.
//
// WRITE AS UTF-8, NOT 'ascii'.
//
// Node's 'ascii' encoding masks every code unit to 7 bits, one byte per
// character. Chinese characters whose low 7 bits happen to be 0x22 or 0x5C are
// therefore written as a literal '"' or '\' -- which injects a quote or an escape
// into the JSON stream and destroys the structure. This fixture came out
// UNBALANCED (loaderData's value never closed) and made the C parser look broken
// when the parser was fine.
//
// That is the third appearance of this exact mistake in this project: it first
// mangled the URL-encoder test inputs, then the signing vectors. Any generated
// file containing non-ASCII must be written as utf8, or have its non-ASCII
// emitted as escapes.
const parsed = JSON.parse(blob)
writeFileSync(out, JSON.stringify(parsed), 'utf8')

/* Prove the fixture is structurally sound before anyone builds on it. */
{
  const s = readFileSync(out, 'latin1')
  const start = s.indexOf('{')
  let d = 0, q = false, esc = false, end = -1
  for (let i = start; i < s.length; i++) {
    const c = s[i]
    if (q) { if (esc) esc = false; else if (c === '\\') esc = true; else if (c === '"') q = false; continue }
    if (c === '"') q = true
    else if (c === '{' || c === '[') d++
    else if (c === '}' || c === ']') { d--; if (d === 0) { end = i + 1; break } }
  }
  if (end !== s.length) {
    console.error(`FIXTURE IS NOT BALANCED: closes at ${end}, file is ${s.length} bytes`)
    process.exit(1)
  }
  console.error(`balance check: ok (closes at ${end})`)
}

/* Report the shape of every card so the parser's expectations can be checked
 * against the same data the console sees. */
const ld = parsed.loaderData || {}
/* The content key is neither "layout" nor a "*_layout". Testing only for the
 * suffix picked "layout" itself, which is an empty shell. */
const page = Object.entries(ld).find(([k]) => !k.endsWith('_layout') && k !== 'layout')?.[1] || {}
console.error(`loaderData keys: ${Object.keys(ld).join(', ')}`)
console.error(`content key    : ${Object.entries(ld).find(([k]) => !k.endsWith('_layout') && k !== 'layout')?.[0]}`)
const report = []
for (const [listName, arr] of [['bannerList', page.bannerList], ['mBannerList', page.mBannerList]]) {
  if (Array.isArray(arr)) report.push([listName, arr])
}
if (Array.isArray(page.homeSections)) {
  page.homeSections.forEach((s, i) => {
    if (Array.isArray(s.video_list)) report.push([`homeSections[${i}].video_list`, s.video_list])
  })
}

console.error(`wrote ${out}  (${Buffer.byteLength(JSON.stringify(parsed))} bytes)`)
for (const [name, arr] of report) {
  console.error(`\n${name}: ${arr.length} cards`)
  /* every card, and whether it carries the titles the parser looks for */
  arr.forEach((c, i) => {
    if (!c || typeof c !== 'object') { console.error(`  [${i}] NOT AN OBJECT: ${JSON.stringify(c).slice(0, 60)}`); return }
    const keys = Object.keys(c)
    const has = (k) => (k in c ? String(c[k]).slice(0, 12) : '-')
    console.error(`  [${i}] id=${has('series_id')} name=${has('series_name')} title=${has('series_title')} ` +
                  `keys=${keys.length} [${keys.slice(0, 10).join(',')}${keys.length > 10 ? ',...' : ''}]`)
  })
}
