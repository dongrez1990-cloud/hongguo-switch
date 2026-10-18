// Capture real upstream fixtures so the C tests are not testing invented data.
//
// Everything hg_api_parse_home / hg_api_parse_episode_ids / hg_play_plan_from_model
// asserts against comes from a live response. This script writes SMALL, TRIMMED
// fixtures: the real bytes for the fields the parser reads, with the bulk (image
// CDN URLs, 222 ids, the other qualities) cut down. What is trimmed and what is
// verbatim is recorded in the fixtures themselves and in the test file.
//
// Usage: node capture_fixtures.mjs <hongguo.cjs> <outdir>

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { createRequire } from 'node:module'
import { join } from 'node:path'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, outdir] = process.argv.slice(2)
if (!src || !outdir) { console.error('usage: node capture_fixtures.mjs <hongguo.cjs> <outdir>'); process.exit(2) }

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console, fetch, setTimeout, AbortController })
vm.runInContext(source + `
  ;globalThis.ref = { DEVICE, VIDEO_UA, pythonUrlEncode, xGorgon, helios, buildMedusa,
                      branchOf, deriveContentKey, decryptSpadeUrl, cleanUrl, md5, str };
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

mkdirSync(outdir, { recursive: true })
const wrote = []
function emit(name, obj, note) {
  const body = typeof obj === 'string' ? obj : JSON.stringify(obj)
  const path = join(outdir, name)
  writeFileSync(path, body)
  wrote.push(`${name}  ${Buffer.byteLength(body)} bytes  -- ${note}`)
}

// ---- 1. home page ---------------------------------------------------------
const homeHtml = await (await fetch('https://hongguoduanju.com/', { headers: { 'User-Agent': UA } })).text()
const homeRouter = extractRouter(homeHtml)
const home = JSON.parse(homeRouter)
console.log(`home router: ${homeRouter.length} bytes, loaderData keys = ${Object.keys(home.loaderData).join(',')}`)

const pageKey = Object.keys(home.loaderData).find((k) => k !== 'layout')
console.log(`  content key: ${JSON.stringify(pageKey)}`)
const page = home.loaderData[pageKey]
console.log(`  page keys: ${Object.keys(page).join(',')}`)
console.log(`  bannerList: ${page.bannerList?.length}  mBannerList: ${page.mBannerList?.length}  homeSections: ${page.homeSections?.length}`)

console.log('\n  bannerList[0] keys : ' + Object.keys(page.bannerList[0]).join(','))
console.log('  bannerList[0]      : ' + JSON.stringify(page.bannerList[0]).slice(0, 700))
console.log('\n  mBannerList[0] keys: ' + Object.keys(page.mBannerList[0]).join(','))
console.log('  mBannerList[0]     : ' + JSON.stringify(page.mBannerList[0]).slice(0, 700))
console.log('\n  homeSections[0] keys: ' + Object.keys(page.homeSections[0]).join(','))
console.log('  homeSections[0].tab_type/tab_name: ' + JSON.stringify(page.homeSections[0].tab_type) + ' / ' + JSON.stringify(page.homeSections[0].tab_name))
const vl = page.homeSections[0].video_list
console.log(`  homeSections[0].video_list: ${vl?.length}`)
console.log('  video_list[0] keys : ' + Object.keys(vl[0]).join(','))
console.log('  video_list[0]      : ' + JSON.stringify(vl[0]).slice(0, 1200))

// Trimmed home fixture: full shape, two banner entries, two mBanner entries,
// one section with two video_list entries. Cover URLs truncated to a prefix.
//
// NOTE, measured: bannerList/mBannerList cards carry `series_name`, but ALL 36
// homeSections[].video_list[] cards carry `series_title` and NO `series_name`.
// The trim keeps whichever the source had so the fixture preserves that
// difference -- it is the reason the parser needs both names.
const trimSeries = (s, coverKey) => {
  const o = {}
  o.series_id = s.series_id
  if (s.series_name !== undefined) o.series_name = s.series_name
  if (s.series_title !== undefined) o.series_title = s.series_title
  o[coverKey] = String(s[coverKey] || '').slice(0, 60)
  o.series_intro = String(s.series_intro || '').slice(0, 40)
  if (s.episode_right_text) o.episode_right_text = s.episode_right_text
  if (s.episode_cnt !== undefined) o.episode_cnt = s.episode_cnt
  if (Array.isArray(s.tags)) o.tags = s.tags.slice(0, 2)
  return o
}
const trimmed = {
  loaderData: {
    layout: null,
    [pageKey]: {
      isSuccess: true,
      bannerList: page.bannerList.slice(0, 2).map((s) => trimSeries(s, 'series_cover')),
      mBannerList: page.mBannerList.slice(0, 2).map((s) => trimSeries(s, 'background_cover')),
      homeSections: [{ tab_type: page.homeSections[0].tab_type, tab_name: page.homeSections[0].tab_name, video_list: vl.slice(0, 2).map((s) => trimSeries(s, 'series_cover')) }],
    },
  },
}
emit('home_router.json', JSON.stringify(trimmed), 'trimmed: 2 banners, 2 mBanners, 1 section x 2 videos; covers/intros truncated')

// ---- 2. detail page -------------------------------------------------------
const seriesId = '7691228619774905368'
const detailHtml = await (await fetch(`https://hongguoduanju.com/detail?series_id=${seriesId}`, { headers: { 'User-Agent': UA } })).text()
const detailRouter = extractRouter(detailHtml)
const detail = JSON.parse(detailRouter)
console.log(`\ndetail router: ${detailRouter.length} bytes, loaderData keys = ${Object.keys(detail.loaderData).join(',')}`)
function findVidList(node, depth = 0) {
  if (!node || typeof node !== 'object' || depth > 8) return null
  if (Array.isArray(node.vid_list) && node.vid_list.length) return { list: node.vid_list, obj: node }
  for (const v of Object.values(node)) {
    const r = findVidList(v, depth + 1)
    if (r) return r
  }
  return null
}
const hit = findVidList(detail.loaderData)
console.log(`  vid_list: ${hit.list.length} entries; entry type = ${typeof hit.list[0]}; first = ${JSON.stringify(hit.list[0])}`)
console.log('  owning object keys: ' + Object.keys(hit.obj).join(','))
const dkeys = Object.keys(detail.loaderData).filter((k) => k !== 'detail_layout')
console.log('  content key: ' + JSON.stringify(dkeys))
const dp = detail.loaderData[dkeys[0]]
if (dp) console.log('  page keys: ' + Object.keys(dp).join(','))

// Trimmed detail fixture: seriesDetail with real identity fields (truncated) and
// exactly 4 episode ids -- 2 verbatim head, 2 verbatim tail -- so the test can
// assert both the strings-are-not-objects rule and the tail ordering.
const sd = hit.obj
const firstIds = hit.list.slice(0, 2)
const lastIds = hit.list.slice(-2)
const trimDetail = {
  loaderData: {
    detail_layout: null,
    [dkeys[0]]: {
      seriesDetail: {
        series_id: sd.series_id,
        series_name: sd.series_name,
        series_cover: String(sd.series_cover || '').slice(0, 60),
        series_intro: String(sd.series_intro || '').slice(0, 40),
        vid_list: [...firstIds, ...lastIds],
      },
      _capture: { real_vid_list_length: hit.list.length, kept: 'first 2 + last 2' },
    },
  },
}
emit('detail_router.json', JSON.stringify(trimDetail), `trimmed: vid_list ${hit.list.length} -> 4 (first 2 + last 2 verbatim), cover/intro truncated`)

// ---- 3. video model -------------------------------------------------------
const vid = hit.list[0]
const VIDEO_API = 'https://api5-normal-sinfonlineb.fqnovel.com/novel/player/multi_video_model/v1/'
const payload = JSON.stringify({
  biz_param: { detail_page_version: 0, device_level: 3, disable_digg_stat: false,
    need_all_video_definition: true, need_mp4_align: false, use_os_player: false,
    use_server_dns: false, video_platform: 1024 },
  mixed_video_id_map: { '1004': [String(vid)] },
})
const body = Buffer.from(payload)
const khronos = Math.floor(Date.now() / 1000)
const ticketBase = Date.now()
function be32(v) { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0, 0); return b }
function le32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0, 0); return b }

let requestTicket = ticketBase, query = '', url = ''
for (let offset = 0; offset < 32; offset++) {
  requestTicket = ticketBase + offset
  query = R.pythonUrlEncode({ ...R.DEVICE, ts: String(khronos), _rticket: String(requestTicket) })
  url = `${VIDEO_API}?${query}`
  if (R.branchOf(url, body, khronos) !== 1) break
}
const headers = {
  'User-Agent': R.VIDEO_UA,
  Accept: 'application/json; charset=utf-8,application/x-protobuf',
  'Content-Type': 'application/json; charset=UTF-8',
  'x-xs-from-web': '0', 'x-ss-req-ticket': String(requestTicket), 'x-tt-request-tag': 't=0;n=0',
  'sdk-version': '2', 'passport-sdk-version': '50561', 'x-vc-bdturing-sdk-version': '3.7.2.cn',
  'x-ss-stub': R.md5(body),
  'x-gorgon': R.xGorgon(query, body, khronos, Math.floor(Math.random() * 65536)),
  'x-khronos': String(khronos),
  'x-ladon': be32(khronos).toString('base64'),
  'x-argus': le32(khronos).toString('base64'),
  'x-helios': R.helios(khronos),
  'x-medusa': R.buildMedusa(url, body, khronos),
  'x-tt-dt': '',
}
const res = await fetch(url, { method: 'POST', headers, body })
const text = await res.text()
console.log(`\nvideo model: status=${res.status} len=${text.length}`)
const outer = JSON.parse(text)
const innerRaw = outer.data[vid].video_model
const inner = JSON.parse(innerRaw)
console.log(`  outer keys : ${Object.keys(outer).join(',')}`)
console.log(`  data[vid] keys: ${Object.keys(outer.data[vid]).join(',')}`)
console.log(`  inner keys : ${Object.keys(inner).join(',')}`)
console.log(`  video_list : ${inner.video_list.length} entries`)
for (let i = 0; i < inner.video_list.length; i++) {
  const e = inner.video_list[i]
  console.log(`   [${i}] keys=${Object.keys(e).join(',')}`)
  console.log(`       quality_type=${JSON.stringify(e.quality_type)} codec=${JSON.stringify(e.video_meta?.codec_type)} def=${JSON.stringify(e.video_meta?.definition)} vw=${e.video_meta?.vwidth} vh=${e.video_meta?.vheight} encrypt=${e.encrypt_info?.encrypt} kid=${e.encrypt_info?.kid} method=${e.encrypt_info?.encryption_method}`)
  console.log(`       TOP-LEVEL codec_type present? ${'codec_type' in e}   TOP-LEVEL spade_a present? ${'spade_a' in e}   TOP-LEVEL key_seed present? ${'key_seed' in e}`)
  console.log(`       enc keys=${Object.keys(e.encrypt_info||{}).join(',')}  meta keys=${Object.keys(e.video_meta||{}).join(',')}`)
  console.log(`       main_url[0..70]=${String(e.main_url).slice(0, 70)}`)
}

// The fixture keeps the FULL double-encoded envelope for entry 0 (that is the
// part the parser must handle), but shortens main_url -- the test asserts the
// envelope, the field paths and the codec/kid/quality decisions, not the CDN URL.
function shortenModel(m) {
  const c = JSON.parse(JSON.stringify(m))
  for (const e of c.video_list) {
    if (typeof e.main_url === 'string') e.main_url = 'https://v5-dj-reading-video.example/tos/cn/v-6fcc8e/oE18JnR0ARnTqaTr0gABDbCU2wRAWgwg5heuzf/?a=8662&ch=0&mime_type=video_mp4'
    if (typeof e.backup_url === 'string') e.backup_url = 'https://v5-dj-reading-video.example/backup.mp4'
  }
  delete c.big_thumbs
  return c
}
const innerTrim = shortenModel(inner)
const modelFixture = { BaseResp: outer.BaseResp, code: outer.code, data: { [vid]: { disable_vsr: outer.data[vid].disable_vsr, expire_time: outer.data[vid].expire_time, video_height: outer.data[vid].video_height, video_width: outer.data[vid].video_width, video_model: JSON.stringify(innerTrim) } }, message: outer.message }
emit('video_model.json', JSON.stringify(modelFixture), 'trimmed: big_thumbs dropped, main_url/backup_url shortened; field paths + encrypt_info/video_meta verbatim')

// A second fixture where a plaintext-codec entry exists, so the codec filter can
// be tested against a real non-bytevc2 shape rather than an invented one.
const innerH264 = JSON.parse(JSON.stringify(innerTrim))
innerH264.video_list[0].video_meta.codec_type = 'h264'
innerH264.video_list[0].video_meta.definition = '720p'
innerH264.video_list[0].quality_type = 30
const h264Fixture = JSON.parse(JSON.stringify(modelFixture))
h264Fixture.data[vid].video_model = JSON.stringify(innerH264)
emit('video_model_h264.json', h264Fixture, 'DERIVED from video_model.json: entry 0 codec_type edited bytevc2->h264, definition 360p->720p, quality_type 40->30 (NOT upstream bytes)')

console.log('\n--- wrote ---')
for (const w of wrote) console.log('  ' + w)
