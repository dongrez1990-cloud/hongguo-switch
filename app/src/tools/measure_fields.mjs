// Measure every field series_from_card reads, per card, in the DEVICE's blob.
//
// parse_home returns HG_API_ERR_JSON on this data on the host as well as on the
// console, so this is a shape problem and series_from_card is where it turns into
// a hard error: it returns HG_API_ERR_JSON when any of its opt_field calls fails,
// which happens when a value exists but does not fit the destination buffer.
//
// The destinations (HgSeries) are id 31, title 192, cover 512, badge 64, desc 512.
//
// Usage: node measure_fields.mjs <router.bin>

import { readFileSync } from 'node:fs'

const path = process.argv[2]
const raw = readFileSync(path, 'utf8')
const d = JSON.parse(raw)
const ld = d.loaderData
const page = Object.entries(ld).find(([k]) => !k.endsWith('_layout') && k !== 'layout')[1]

const LIMITS = { series_id: 30, series_name: 191, series_title: 191, series_cover: 511,
                 background_cover: 511, series_intro: 511, episode_right_text: 63 }
const FIELDS = Object.keys(LIMITS)

function byteLen(v) {
  if (v === null) return -1            // null: a distinct case
  if (typeof v !== 'string') return -2 // not a string
  return Buffer.byteLength(v, 'utf8')
}

function report(name, cards) {
  console.log(`\n${name}: ${cards.length} cards`)
  cards.forEach((c, i) => {
    const bad = []
    const parts = []
    for (const f of FIELDS) {
      if (!(f in c)) continue
      const n = byteLen(c[f])
      const mark = (n === -1) ? 'null' : (n === -2) ? 'NOT-STRING' :
                   (n > LIMITS[f]) ? `**${n}>${LIMITS[f]}**` : `${n}`
      parts.push(`${f}=${mark}`)
      if (n === -1 || n === -2 || n > LIMITS[f]) bad.push(`${f} ${mark}`)
    }
    console.log(`  [${i}] ${parts.join(' ')}`)
    if (bad.length) console.log(`      OVERFLOW: ${bad.join(', ')}`)
  })
}

for (const k of ['bannerList', 'mBannerList']) {
  if (Array.isArray(page[k])) report(k, page[k])
}
if (Array.isArray(page.homeSections)) {
  page.homeSections.forEach((s, i) => {
    if (Array.isArray(s.video_list)) report(`homeSections[${i}].video_list`, s.video_list)
  })
}
