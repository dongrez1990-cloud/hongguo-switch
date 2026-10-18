// Walk the whole playback chain from this machine.
//
// The signed search API is dead (proven by running the reference verbatim), but
// the video model API answers, and the server-rendered pages carry the content.
// So the chain that matters is:
//
//   detail page (_ROUTER_DATA) -> a vid -> signed video model request -> main_url
//
// If this works, the playback path is real and step 3 is unblocked. If it does
// not, better to know now than after building a UI on top of it.
//
// Usage: node probe_play_chain.mjs <hongguo.cjs> [series_id]

import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const [src, seriesArg] = process.argv.slice(2)
if (!src) { console.error('usage: node probe_play_chain.mjs <hongguo.cjs> [series_id]'); process.exit(2) }

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

function findVidList(node, depth = 0) {
  if (!node || typeof node !== 'object' || depth > 8) return null
  if (Array.isArray(node.vid_list) && node.vid_list.length) return node.vid_list
  for (const v of Object.values(node)) {
    const r = findVidList(v, depth + 1)
    if (r) return r
  }
  return null
}

// ---- 1. find a series id from the home page if none was given --------------
let seriesId = seriesArg
if (!seriesId) {
  const home = await (await fetch('https://hongguoduanju.com/', { headers: { 'User-Agent': UA } })).text()
  const d = JSON.parse(extractRouter(home))
  const sections = d.loaderData.page.homeSections || []
  outer:
  for (const s of sections) {
    for (const v of (s.video_list || [])) {
      if (v && v.series_id) { seriesId = String(v.series_id); break outer }
    }
  }
  console.log(`series id from home: ${seriesId}  (${sections.length} sections)`)
}

// ---- 2. detail page -> vid_list -------------------------------------------
const detailUrl = `https://hongguoduanju.com/detail?series_id=${seriesId}`
const detailHtml = await (await fetch(detailUrl, { headers: { 'User-Agent': UA } })).text()
const detailBlob = extractRouter(detailHtml)
console.log(`detail page  : ${detailHtml.length} bytes, router=${detailBlob ? detailBlob.length : 'NONE'}`)

let vids = null
if (detailBlob) vids = findVidList(JSON.parse(detailBlob))
console.log(`vid_list     : ${vids ? vids.length + ' entries, first=' + JSON.stringify(vids[0]) : 'NOT FOUND'}`)
if (!vids || !vids.length) { console.log('cannot continue without a vid'); process.exit(1) }
const vid = Array.isArray(vids[0]) ? vids[0][0] : vids[0]
console.log(`vid          : ${vid}\n`)

// ---- 3. signed video model request ----------------------------------------
/* ROUTE CHANGED (2026-10-13, see docs/新播放接口-第21轮.md). The batched route this probe used to
 * call now answers a blanket {"Code":110001,"Message":"未知异常"} to EVERY request -- signed,
 * unsigned, and signature-deliberately-corrupted all give the same 125 bytes -- so it cannot be
 * fixed by changing the body. The live route takes one video and a `video_id` STRING. */
const VIDEO_API = 'https://api5-normal-sinfonlineb.fqnovel.com/novel/player/video_model/v1/'
const payload = JSON.stringify({
  video_id: String(vid),
  biz_param: { detail_page_version: 0, device_level: 3, disable_digg_stat: false,
    disable_video_relate_book: false, need_all_video_definition: true, need_mp4_align: false,
    use_os_player: false, use_server_dns: false, video_platform: 1024 },
})
const body = Buffer.from(payload)
const khronos = Math.floor(Date.now() / 1000)
const ticketBase = Date.now()

function be32(v) { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0, 0); return b }
function le32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0, 0); return b }

let requestTicket = ticketBase, query = '', url = ''
for (let offset = 0; offset < 32; offset++) {
  requestTicket = ticketBase + offset
  const values = { ...R.DEVICE, ts: String(khronos), _rticket: String(requestTicket) }
  query = R.pythonUrlEncode(values)
  url = `${VIDEO_API}?${query}`
  if (R.branchOf(url, body, khronos) !== 1) break
}
const random = Math.floor(Math.random() * 65536)
const headers = {
  'User-Agent': R.VIDEO_UA,
  Accept: 'application/json; charset=utf-8,application/x-protobuf',
  'Content-Type': 'application/json; charset=UTF-8',
  'x-xs-from-web': '0',
  'x-ss-req-ticket': String(requestTicket),
  'x-tt-request-tag': 't=0;n=0',
  'sdk-version': '2',
  'passport-sdk-version': '50561',
  'x-vc-bdturing-sdk-version': '3.7.2.cn',
  'x-ss-stub': R.md5(body),
  'x-gorgon': R.xGorgon(query, body, khronos, random),
  'x-khronos': String(khronos),
  'x-ladon': be32(khronos).toString('base64'),
  'x-argus': le32(khronos).toString('base64'),
  'x-helios': R.helios(khronos),
  'x-medusa': R.buildMedusa(url, body, khronos),
  'x-tt-dt': '',
}

const res = await fetch(url, { method: 'POST', headers, body })
const text = await res.text()
console.log(`video model  : status=${res.status} len=${text.length} type=${res.headers.get('content-type')}`)
console.log(`body head    : ${text.slice(0, 900)}`)

// ---- 4. if it answered, resolve the play plan -----------------------------
try {
  const outer = JSON.parse(text)
  console.log(`\nouter keys   : ${Object.keys(outer).join(',')}`)
  console.log(`data[vid] keys: ${Object.keys(outer.data?.[vid] || {}).join(',')}`)

  /* video_model is a JSON STRING nested inside the JSON response -- it has to be
   * parsed a second time. Looking for video_list at the top level (which this
   * port did) never finds anything, however successful the request was. */
  const innerRaw = outer.data?.[vid]?.video_model
  if (typeof innerRaw !== 'string') {
    console.log('video_model is not a string -- shape differs')
    process.exit(0)
  }
  const inner = JSON.parse(innerRaw)
  console.log(`inner keys   : ${Object.keys(inner).join(',')}`)

  const list = inner.video_list
  console.log(`video_list   : ${Array.isArray(list) ? list.length + ' entries' : 'absent'}`)
  if (Array.isArray(list) && list.length) {
    console.log(`  entry 0 keys: ${Object.keys(list[0]).join(',')}`)
  }

  for (const item of (list || [])) {
    const codec = String(item.codec_type || '')
    if (codec === 'bytevc2' || codec === 'bvc2') continue

    let playUrl = R.cleanUrl(String(item.main_url || ''))
    const seed = Buffer.from(R.str(item.key_seed || ''), 'base64')
    if (seed.length && playUrl) playUrl = R.decryptSpadeUrl(playUrl, seed)
    const key = item.spade_a ? R.deriveContentKey(item.spade_a) : null

    console.log(`\n--- resolved source ---`)
    console.log(`codec_type   : ${codec || '(field absent)'}`)
    console.log(`quality_type : ${item.quality_type ?? '?'}`)
    console.log(`main_url len : ${String(item.main_url || '').length}`)
    console.log(`spade_a      : ${item.spade_a ? 'present' : 'ABSENT'}`)
    console.log(`key_seed     : ${item.key_seed ? 'present' : 'ABSENT'}`)
    console.log(`encrypt_info : ${JSON.stringify(item.encrypt_info)?.slice(0, 200)}`)
    console.log(`gear_des_key : ${item.gear_des_key ? String(item.gear_des_key).slice(0, 80) : 'absent'}`)
    console.log(`video_meta   : ${JSON.stringify(item.video_meta)?.slice(0, 300)}`)
    console.log(`\nfull url (${String(playUrl).length} chars):\n${playUrl}`)

    /* Does that URL actually serve an MP4, and is it encrypted?
     * A plain `Range: bytes=0-63` answers both: an unencrypted MP4 starts with
     * a size field followed by "ftyp"; a CENC stream usually still does, but the
     * moov will carry encv/sinf, and the body will be opaque afterwards. */
    console.log('\n--- fetching the first 128 bytes ---')
    const head = await fetch(playUrl, {
      headers: { Range: 'bytes=0-127', 'User-Agent': R.VIDEO_UA, Referer: 'https://novel.snssdk.com/' },
    })
    const buf = Buffer.from(await head.arrayBuffer())
    console.log(`status       : ${head.status}`)
    console.log(`content-type : ${head.headers.get('content-type')}`)
    console.log(`content-range: ${head.headers.get('content-range')}`)
    console.log(`content-len  : ${head.headers.get('content-length')}`)
    console.log(`bytes        : ${buf.length}`)
    console.log(`hex          : ${buf.subarray(0, 32).toString('hex')}`)
    console.log(`ascii        : ${buf.subarray(0, 64).toString('latin1').replace(/[^\x20-\x7e]/g, '.')}`)
    console.log(`has ftyp     : ${buf.includes(Buffer.from('ftyp'))}`)
    console.log(`has encv     : ${buf.includes(Buffer.from('encv'))}`)
    console.log(`has moov     : ${buf.includes(Buffer.from('moov'))}`)
    break
  }
  if (!list || !list.length) {
    console.log('\nvideo_list absent; showing what inner does have:')
    console.log(innerRaw.slice(0, 1200))
  }
} catch (e) {
  console.log('\nresponse was not JSON:', e.message)
}
