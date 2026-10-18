// Fire the reference implementation's exact search request from this machine.
//
// The console gets HTTP 200 with Content-Length: 0 and Content-Type:
// application/json -- the origin accepted the request and returned nothing. The
// query string is already proven byte-identical to this file's output, so the
// remaining question is not "what did I do wrong" but "does the reference still
// work at all". Those have opposite fixes, and only the network can answer it.
//
// Usage: node probe_ref_search.mjs <hongguo.cjs>

import { readFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import vm from 'node:vm'

const require = createRequire(import.meta.url)
const src = process.argv[2]
if (!src) { console.error('usage: node probe_ref_search.mjs <hongguo.cjs>'); process.exit(2) }

const source = readFileSync(src, 'utf8')
const ctx = vm.createContext({ require, Buffer, URL, URLSearchParams, console, fetch, setTimeout, AbortController })
vm.runInContext(source + `
  ;globalThis.ref = { APP_DEVICE, DEVICE, APP_UA, pythonUrlEncode, xGorgon, helios,
                      buildMedusa, PAGE_SIZE, branchOf };
`, ctx)
const R = ctx.ref

const API = 'https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v'

async function main() {
  const khronos = Math.floor(Date.now() / 1000)
  const ticket = Date.now()
  const values = {
    ...R.APP_DEVICE,
    query: '短剧',
    count: String(R.PAGE_SIZE),
    offset: '0',
    tab_type: '11',
    bookshelf_search_plan: '4',
    use_correct: 'true',
    user_is_login: '0',
    ts: String(khronos),
    _rticket: String(ticket),
  }
  const query = R.pythonUrlEncode(values)
  const url = `${API}?${query}`
  const random = Math.floor(Math.random() * 65536)
  const headers = {
    'User-Agent': R.APP_UA,
    Accept: 'application/json; charset=utf-8,application/x-protobuf',
    'x-ss-req-ticket': String(ticket),
    'x-tt-request-tag': 't=0;n=0',
    'sdk-version': '2',
    'passport-sdk-version': '50561',
    'x-xs-from-web': '0',
    'x-khronos': String(khronos),
    'x-ladon': be32(khronos).toString('base64'),
    'x-argus': le32(khronos).toString('base64'),
    'x-gorgon': R.xGorgon(query, null, khronos, random),
    'x-helios': R.helios(khronos),
    'x-medusa': R.buildMedusa(url, null, khronos),
    'x-tt-dt': '',
  }

  function be32(v) { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0, 0); return b }
  function le32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0, 0); return b }

  console.log('query length :', query.length)
  console.log('headers      :', Object.keys(headers).length)

  try {
    const res = await fetch(url, { headers, redirect: 'follow' })
    const text = await res.text()
    console.log('--- reference implementation, live ---')
    console.log('status       :', res.status)
    console.log('content-type :', res.headers.get('content-type'))
    console.log('content-len  :', res.headers.get('content-length'))
    console.log('body length  :', text.length)
    console.log('body head    :', text.slice(0, 600))
  } catch (e) {
    console.log('fetch failed :', e && (e.cause ? e.cause.message : e.message))
  }
}

main()
