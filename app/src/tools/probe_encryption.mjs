// Is the media actually CENC-encrypted, or is it clear?
//
// The console's A/B decoded the SAME url identically with and without
// decryption_key. That looks like "the key was not needed", but it is not proof:
// width/height/duration come from the container header, which is readable even
// when the samples are opaque, and vo=null discards the frames. A decoder can
// report "DECODE OK" on garbage.
//
// The container itself settles it. A CENC stream's moov carries an `encv` (or
// `enca`) sample entry with `sinf`/`schm`/`tenc`; a clear stream carries `avc1`
// or `hev1` and no sinf. So: fetch the header, look for the boxes.
//
// Usage: node probe_encryption.mjs <play-url> [bytes]

const url = process.argv[2]
if (!url) { console.error('usage: node probe_encryption.mjs <play-url> [bytes]'); process.exit(2) }
const want = Number(process.argv[3] || 262144)

const UA = 'com.phoenix.read/71332 (Linux; U; Android 16; zh_CN; 25053RT47C; Build/BP2A.250605.031.A3)'

const res = await fetch(url, {
  headers: {
    Range: `bytes=0-${want - 1}`,
    'User-Agent': UA,
    Referer: 'https://novel.snssdk.com/',
  },
})
const buf = Buffer.from(await res.arrayBuffer())

console.log(`status       : ${res.status}`)
console.log(`content-type : ${res.headers.get('content-type')}`)
console.log(`content-range: ${res.headers.get('content-range')}`)
console.log(`fetched      : ${buf.length} bytes\n`)

/* List the top-level boxes in what we fetched, so the moov's extent is known. */
function boxes(b, from, to, depth) {
  let p = from
  while (p + 8 <= to) {
    let size = b.readUInt32BE(p)
    const type = b.toString('latin1', p + 4, p + 8)
    let header = 8
    if (size === 1) { size = Number(b.readBigUInt64BE(p + 8)); header = 16 }
    else if (size === 0) size = to - p
    if (size < header || p + size > to) break
    console.log(`${'  '.repeat(depth)}${type.padEnd(8)} off=${p} size=${size}`)
    if (depth < 1 && ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd'].includes(type)) {
      boxes(b, p + header, p + size, depth + 1)
    }
    p += size
  }
}
console.log('boxes:')
boxes(buf, 0, buf.length, 0)

/* The verdict: which sample entry does stsd hold? */
const marks = ['encv', 'enca', 'sinf', 'schm', 'tenc', 'senc', 'saiz', 'saio',
               'avc1', 'hev1', 'hvc1', 'avc3', 'mp4a']
console.log('\nbox / entry names present:')
for (const m of marks) {
  const at = buf.indexOf(Buffer.from(m, 'latin1'))
  console.log(`  ${m.padEnd(5)} ${at >= 0 ? `yes  at ${at}` : 'no'}`)
}

const encryptedEntry = buf.includes(Buffer.from('encv')) || buf.includes(Buffer.from('enca'))
const sinf = buf.includes(Buffer.from('sinf'))
console.log(`\nVERDICT: ${encryptedEntry || sinf
  ? 'the container declares ENCRYPTION (encv/enca/sinf) -- samples are CENC'
  : 'no encryption boxes found in this range -- the stream looks CLEAR'}`)
