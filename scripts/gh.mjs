// 通过 api.github.com 取文件 / 列目录。
//
// raw.githubusercontent.com 在本机网络被墙（fetch failed），但 api.github.com 可达，
// 所以统一走 Contents API；要原始内容就带 Accept: application/vnd.github.raw。
//
// 用法:
//   node gh.mjs ls   <repo> [path] [ref]
//   node gh.mjs cat  <repo> <path> [ref]        -> 打印到 stdout
//   node gh.mjs get  <repo> <path> <outfile> [ref]

import { writeFileSync } from 'node:fs'

const H = { 'User-Agent': 'dsh-agent', Accept: 'application/vnd.github+json' }

const [cmd, repo, p1, p2, p3] = process.argv.slice(2)

async function req(url, accept) {
  const res = await fetch(url, { headers: { ...H, ...(accept ? { Accept: accept } : {}) } })
  if (!res.ok) {
    const t = await res.text()
    throw new Error(`HTTP ${res.status} ${res.statusText} :: ${t.slice(0, 200)}`)
  }
  return res
}

function api(path) {
  return `https://api.github.com${path}`
}

if (cmd === 'ls') {
  const path = p1 ?? ''
  const ref = p2
  const q = ref ? `?ref=${ref}` : ''
  const res = await req(api(`/repos/${repo}/contents/${path}${q}`))
  const j = await res.json()
  for (const e of j) console.log(`${e.type === 'dir' ? 'd' : '-'} ${String(e.size ?? '').padStart(9)}  ${e.name}`)
} else if (cmd === 'cat' || cmd === 'get') {
  const path = p1
  const ref = cmd === 'get' ? p3 : p2
  const out = cmd === 'get' ? p2 : null
  const q = ref ? `?ref=${ref}` : ''
  const res = await req(api(`/repos/${repo}/contents/${path}${q}`), 'application/vnd.github.raw')
  const text = await res.text()
  if (out) {
    writeFileSync(out, text)
    console.log(`saved ${text.length} bytes -> ${out}`)
  } else {
    process.stdout.write(text)
  }
} else {
  console.error('usage: node gh.mjs ls <repo> [path] [ref] | cat <repo> <path> [ref] | get <repo> <path> <out> [ref]')
  process.exit(2)
}
