// 探测 URL 的可达性。用来区分"网络层面被拒"和"pacman 自己的下载器被拒"。
// 用法: node probe.mjs <url...>
const urls = process.argv.slice(2)
for (const url of urls) {
  for (const ua of ['node-fetch', 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)']) {
    try {
      const res = await fetch(url, {
        redirect: 'follow',
        headers: { 'User-Agent': ua },
      })
      const len = res.headers.get('content-length') ?? '?'
      console.log(`${res.status} ${res.statusText}  len=${len}  ua="${ua.slice(0, 20)}"  ${url}`)
      if (res.body) await res.body.cancel()
    } catch (e) {
      console.log(`ERR ${e.message}  ua="${ua.slice(0, 20)}"  ${url}`)
    }
  }
}
