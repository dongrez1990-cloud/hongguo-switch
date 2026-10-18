const UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'
async function get(u){const r=await fetch(u,{headers:{'User-Agent':UA,'Accept-Language':'zh-CN,zh;q=0.9'},signal:AbortSignal.timeout(20000)});return{status:r.status,html:await r.text()}}
/* 1) ????????? /category/... ?? */
const c=await get('https://hongguoduanju.com/category')
const hrefs=[...new Set([...c.html.matchAll(/href="([^"]*\/category[^"]*)"/g)].map(m=>m[1]))]
console.log(`/category ??=${c.status}  html=${c.html.length}b  ?? /category ?? ${hrefs.length} ?:`)
for(const h of hrefs.slice(0,40))console.log('   ',h)
/* 2) ????????????tags ????*/
const ld=(c.html.match(/_ROUTER_DATA[^{]*(\{.*?\})\s*<\/script>/s)||[])[1]
const tags=new Set()
if(ld){try{(function w(n){if(!n||typeof n!=='object')return;if(Array.isArray(n))return n.forEach(w)
  if(Array.isArray(n.tags))n.tags.forEach(t=>{const s=typeof t==='string'?t:(t&&t.name);if(s)tags.add(s)})
  for(const v of Object.values(n))w(v)})(JSON.parse(ld))}catch(e){console.log('parse:',e.message)}}
console.log(`\n??? router ???? tag ? ${tags.size} ?: ${[...tags].slice(0,30).join(' / ')}`)
