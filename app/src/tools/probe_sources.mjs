// Probe which upstream entry points still serve anything.
//
// The signed search API answers 200 with Content-Length: 0 both from the console
// AND from this machine running the reference implementation verbatim, so that
// endpoint is not usable right now and no amount of client fixing changes it.
// The project has a second family of entry points -- the server-rendered pages
// whose payload this port already extracts with hg_router_extract -- and those
// are worth measuring before concluding anything about the project as a whole.
//
// Usage: node probe_sources.mjs

const CANDIDATES = [
  ['home',            'https://hongguoduanju.com/'],
  ['category',        'https://hongguoduanju.com/category'],
  ['category-ai',     'https://hongguoduanju.com/category/ai'],
  ['search',          'https://hongguoduanju.com/search/%E7%9F%AD%E5%89%A7'],
  ['rank-hot',        'https://hongguoduanju.com/rank/hot-comic-drama'],
  ['rank-hot2',       'https://hongguoduanju.com/rank/hot'],
  ['api-search',      'https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?query=x&count=1'],
  ['api-videomodel',  'https://api5-normal-sinfonlineb.fqnovel.com/novel/player/multi_video_model/v1/?ts=1'],
]

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'

for (const [name, url] of CANDIDATES) {
  const started = Date.now()
  try {
    const res = await fetch(url, {
      headers: { 'User-Agent': UA, 'Accept-Language': 'zh-CN,zh;q=0.9' },
      redirect: 'follow',
      signal: AbortSignal.timeout(15000),
    })
    const text = await res.text()
    const hasRouter = text.includes('_ROUTER_DATA')
    const ms = Date.now() - started
    console.log(
      `${name.padEnd(15)} ${String(res.status).padEnd(4)} ${String(text.length).padStart(8)} b  ` +
      `${String(res.headers.get('content-type') || '').split(';')[0].padEnd(24)} ` +
      `router=${hasRouter ? 'YES' : 'no '}  ${ms}ms`
    )
    if (hasRouter) {
      const i = text.indexOf('_ROUTER_DATA')
      console.log(`                 ...context: ${text.slice(Math.max(0, i - 30), i + 90).replace(/\s+/g, ' ')}`)
    }
  } catch (e) {
    console.log(`${name.padEnd(15)} FAIL  ${e && (e.cause ? e.cause.message : e.message)}`)
  }
}
