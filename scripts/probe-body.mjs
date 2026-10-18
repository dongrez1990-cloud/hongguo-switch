// 打印响应体片段，判断 403 是 Cloudflare 挑战、IP 封锁还是路径错误。
// 用法: node probe-body.mjs <url>
const url = process.argv[2]
const res = await fetch(url, { redirect: 'follow' })
const text = await res.text()
console.log(`status=${res.status} ${res.statusText}`)
console.log('--- headers ---')
for (const [k, v] of res.headers) console.log(`  ${k}: ${v}`)
console.log('--- body (first 600 chars) ---')
console.log(text.slice(0, 600))
