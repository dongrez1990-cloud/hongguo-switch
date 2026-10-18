/*
 * cover-probe.mjs -- does the CDN hand back something stb_image can decode?
 *
 * Usage: node cover-probe.mjs <fixture.json> [--accept <header>] [--limit N]
 *
 * WHY. The cover loader (app/hongguo/hg_image.cpp) fetches the bytes and hands
 * them to Image::setImageFromMem -> nvgCreateImageMem, which decodes with
 * stb_image. stb_image covers JPEG, PNG, BMP, TGA, GIF, PSD, HDR, PIC and PNM --
 * and NOT WebP. This build links no libwebp either (checked: WebPDecodeRGBA is
 * absent from every archive in C:/devkitPro/portlibs/switch/lib).
 *
 * The cover URLs carry a ByteDance image-service suffix (~tplv-...), and those
 * services negotiate the output format from the Accept header. If the default
 * Accept makes the CDN answer with WebP, the loader fetches real bytes, hands
 * them to a decoder that cannot read them, and the only symptom is a thumbnail
 * that never appears -- which is indistinguishable from a network failure unless
 * the format is actually inspected.
 *
 * So this asks the CDN directly and reports the magic bytes of the answer.
 * It performs real HTTPS requests; it is a probe, not a unit test.
 */

import { readFileSync } from 'node:fs'
import https from 'node:https'

const args = process.argv.slice(2)
const file = args[0]
if (!file) {
  console.error('usage: node cover-probe.mjs <fixture.json> [--accept <h>] [--limit N]')
  process.exit(2)
}

const acceptArg = args.indexOf('--accept')
const ACCEPT = acceptArg >= 0 ? args[acceptArg + 1] : null
const limitArg = args.indexOf('--limit')
const LIMIT = limitArg >= 0 ? Number(args[limitArg + 1]) : 3

/* Collect cover URLs out of the fixture wherever they live. */
const root = JSON.parse(readFileSync(file, 'utf8'))
const urls = []
function walk(node, depth) {
  if (depth > 8 || node === null || typeof node !== 'object') return
  if (Array.isArray(node)) {
    for (const v of node) walk(v, depth + 1)
    return
  }
  for (const [k, v] of Object.entries(node)) {
    if (typeof v === 'string' && /^https?:\/\//.test(v) && /cover/.test(k)) urls.push({ k, v })
    else walk(v, depth + 1)
  }
}
walk(root, 0)

console.log(`fixture      ${file}`)
console.log(`cover fields ${urls.length}`)
console.log(`accept       ${ACCEPT ?? '(not set -- node default)'}`)
console.log('')

/* Identify a format from the first bytes. Reporting the magic is the point: a
 * Content-Type header is a claim, the bytes are the fact, and they disagree more
 * often than one would like. */
function identify(b) {
  if (b.length < 12) return 'too short'
  if (b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return 'JPEG (stb_image: YES)'
  if (b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return 'PNG (stb_image: YES)'
  if (b.toString('ascii', 0, 4) === 'RIFF' && b.toString('ascii', 8, 12) === 'WEBP')
    return 'WEBP (stb_image: NO -- this would never appear)'
  if (b.toString('ascii', 0, 3) === 'GIF') return 'GIF (stb_image: YES)'
  if (b[0] === 0x42 && b[1] === 0x4d) return 'BMP (stb_image: YES)'
  if (b.toString('ascii', 4, 8) === 'ftyp') return `ISO-BMFF (${b.toString('ascii', 8, 12)}) -- AVIF/HEIC: stb_image: NO`
  return 'unrecognised: ' + [...b.subarray(0, 8)].map((x) => x.toString(16).padStart(2, '0')).join(' ')
}

function head(url) {
  return new Promise((resolve) => {
    const req = https.request(
      url,
      { method: 'GET', headers: ACCEPT ? { Accept: ACCEPT } : {} },
      (res) => {
        /* The redirect chain matters: the CDN may send us elsewhere. */
        const chunks = []
        res.on('data', (d) => {
          chunks.push(d)
          if (Buffer.concat(chunks).length > 4096) res.destroy()
        })
        res.on('end', () => done(res, Buffer.concat(chunks)))
        res.on('close', () => done(res, Buffer.concat(chunks)))
        let settled = false
        function done(r, b) {
          if (settled) return
          settled = true
          resolve({
            status: r.statusCode,
            type: r.headers['content-type'],
            len: r.headers['content-length'],
            bytes: b,
          })
        }
      }
    )
    req.on('error', (e) => resolve({ error: e.message }))
    req.setTimeout(20000, () => {
      req.destroy()
      resolve({ error: 'timeout' })
    })
    req.end()
  })
}

for (const { k, v } of urls.slice(0, LIMIT)) {
  console.log(`--- ${k}`)
  console.log(`    ${v.slice(0, 120)}`)
  const r = await head(v)
  if (r.error) {
    console.log(`    ERROR ${r.error}`)
    continue
  }
  console.log(`    HTTP ${r.status}  content-type=${r.type}  content-length=${r.len ?? '(none)'}`)
  console.log(`    first bytes: ${identify(r.bytes)}`)
}
