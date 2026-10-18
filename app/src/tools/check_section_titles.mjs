// Check what title field the LIVE homeSections[].video_list[] cards carry, and
// whether the untrimmed entries differ from the trimmed fixture.
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

function extractRouter(html) {
  const m = html.indexOf('_ROUTER_DATA')
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

const html = await (await fetch('https://hongguoduanju.com/', { headers: { 'User-Agent': UA } })).text()
const p = JSON.parse(extractRouter(html)).loaderData.page

for (const sec of p.homeSections) {
  console.log(`--- tab_name=${JSON.stringify(sec.tab_name)} tab_type=${JSON.stringify(sec.tab_type)} n=${sec.video_list.length}`)
  const v = sec.video_list[0]
  console.log('    keys: ' + Object.keys(v).join(','))
  for (const k of ['series_name', 'series_title', 'series_cover', 'series_intro', 'episode_right_text', 'episode_cnt']) {
    console.log('    ' + k.padEnd(20) + ' = ' + JSON.stringify(v[k] ?? null).slice(0, 70))
  }
}

console.log('\n=== bannerList entry keys ===')
console.log(Object.keys(p.bannerList[0]).join(','))
console.log('=== mBannerList entry keys ===')
console.log(Object.keys(p.mBannerList[0]).join(','))

// How many section cards have a usable title? (this is what the parser needs)
let total = 0, named = 0, titled = 0
for (const sec of p.homeSections) for (const v of sec.video_list) {
  total++
  if (v.series_name) named++
  if (v.series_title) titled++
}
console.log(`\nsection cards: total=${total} with series_name=${named} with series_title=${titled}`)
