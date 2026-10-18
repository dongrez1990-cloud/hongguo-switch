// 解压 devkitPro 的 .pkg.tar.zst / .pkg.tar.xz。
//
// 为什么不用系统 tar：Windows 自带的 bsdtar 3.5.2 没有 zstd 滤镜
// （"Can't initialize filter; unable to run program zstd -d"），而 msys2 的
// bsdtar 又要提权才能跑（沙箱禁止命名管道）。Node 22+ 自带 zlib.zstdDecompressSync，
// 所以这里纯 JS 解第一层，再用已装好的 Python 解第二层 tar。
//
// 用法: node pkg-extract.mjs <pkgfile> <outdir> [memberSubstring]

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { zstdDecompressSync } from 'node:zlib'
import { join, basename } from 'node:path'
import { execFileSync } from 'node:child_process'

const [pkg, outdir, filter] = process.argv.slice(2)
if (!pkg || !outdir) {
  console.error('usage: node pkg-extract.mjs <pkgfile> <outdir> [memberSubstring]')
  process.exit(2)
}

mkdirSync(outdir, { recursive: true })

const raw = readFileSync(pkg)
let tarBytes
if (pkg.endsWith('.zst') || pkg.endsWith('.zst')) {
  tarBytes = zstdDecompressSync(raw)
} else if (pkg.endsWith('.xz')) {
  // xz 没有内建支持，交给 tar 处理（-J）
  tarBytes = null
} else {
  tarBytes = raw
}

const tarPath = join(outdir, basename(pkg).replace(/\.zst$/, ''))
if (tarBytes) {
  writeFileSync(tarPath, tarBytes)
  console.log(`zstd -> tar: ${tarBytes.length} bytes -> ${tarPath}`)
} else {
  console.log('xz: 交给 tar -xJ 处理')
}

// 第二层用 Windows bsdtar 解普通 tar（它支持未压缩 tar）
const args = tarBytes
  ? ['-xf', tarPath, '-C', outdir]
  : ['-xJf', pkg, '-C', outdir]
if (filter) args.push(filter)

try {
  execFileSync('tar', args, { stdio: 'inherit' })
  console.log('tar OK')
} catch (e) {
  console.error('tar failed:', e.message)
  process.exit(1)
}
