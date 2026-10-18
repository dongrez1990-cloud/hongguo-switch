// 下载 Switch 依赖包。本机 schannel 损坏（SEC_E_NO_CREDENTIALS），git/curl 全挂，
// 必须走 Node 自带的 OpenSSL 栈。
// 用法: node dl-pkgs.mjs <outdir> <url...>
import { existsSync, statSync, writeFileSync } from 'node:fs'
import { mkdir } from 'node:fs/promises'
import { createHash } from 'node:crypto'
import { basename, join } from 'node:path'

// code.videolan.org (libplacebo's home) answers `406 Not Acceptable` to a
// non-browser User-Agent, so always present one. Harmless for GitHub et al.
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36'

const [outdir, ...urls] = process.argv.slice(2)
if (!outdir || urls.length === 0) {
  console.error('usage: node dl-pkgs.mjs <outdir> <url...>')
  process.exit(2)
}
await mkdir(outdir, { recursive: true })

let failed = 0
for (const url of urls) {
  const name = basename(new URL(url).pathname)
  const dest = join(outdir, name)
  if (existsSync(dest) && statSync(dest).size > 0) {
    console.log(`SKIP ${name} (${statSync(dest).size} bytes already present)`)
    continue
  }
  try {
    const res = await fetch(url, { redirect: 'follow', headers: { 'User-Agent': UA } })
    if (!res.ok) throw new Error(`HTTP ${res.status} ${res.statusText}`)
    const buf = Buffer.from(await res.arrayBuffer())
    writeFileSync(dest, buf)
    console.log(`OK   ${name}  ${buf.length} bytes  sha256=${createHash('sha256').update(buf).digest('hex').slice(0, 16)}`)
  } catch (e) {
    failed++
    console.error(`FAIL ${name}: ${e.message}`)
  }
}

console.log(failed === 0 ? 'ALL OK' : `${failed} FAILED`)
process.exit(failed === 0 ? 0 : 1)
