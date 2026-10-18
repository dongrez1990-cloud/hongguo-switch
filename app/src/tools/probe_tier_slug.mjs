// Which /category/<slug> matches which home tier? Measured by id overlap, not by guessing from names.
const UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'
function extractRouter(html){const m=html.indexOf('_ROUTER_DATA');if(m<0)return null;const f=html.indexOf('{',m);let d=0,q=false,e=false
 for(let i=f;i<html.length;i++){const c=html[i];if(q){if(e)e=false;else if(c==='\\')e=true;else if(c==='"')q=false;continue}
  if(c==='"')q=true;else if(c==='{'||c==='[')d++;else if(c==='}'||c===']'){d--;if(!d)return html.slice(f,i+1)}}return null}
async function router(u){const r=await fetch(u,{headers:{'User-Agent':UA,'Accept-Language':'zh-CN,zh;q=0.9'},signal:AbortSignal.timeout(20000)})
 const h=await r.text();const b=extractRouter(h);return {status:r.status,json:b?JSON.parse(b):null}}
function idsOf(json){const out=new Set();(function w(n){if(!n||typeof n!=='object')return;if(Array.isArray(n))return n.forEach(w)
 if(n.series_id&&n.series_name)out.add(String(n.series_id));for(const v of Object.values(n))w(v)})(json);return out}
/* home tiers */
const home=await router('https://hongguoduanju.com/')
const tiers=[]
const ld=home.json.loaderData
for(const v of Object.values(ld)){ if(v&&typeof v==='object'&&Array.isArray(v.homeSections)){
  for(const s of v.homeSections){ const ids=new Set(); (s.video_list||[]).forEach(c=>{if(c.series_id)ids.add(String(c.series_id))})
    tiers.push({tab_type:s.tab_type,name:s.tab_name,ids}) } } }
console.log('home tiers:')
for(const t of tiers)console.log(`  ${String(t.tab_type).padEnd(7)} ${String(t.name).padEnd(10)} ${t.ids.size} ids  e.g. ${[...t.ids].slice(0,2).join(',')}`)
const SLUGS=['','real-drama','comic-drama','ai-drama','comic']
console.log('\n/category/<slug> vs each tier (id overlap):')
for(const slug of SLUGS){
  const r=await router('https://hongguoduanju.com/category'+(slug?'/'+slug:''))
  if(!r.json){console.log(`  ${slug||'(none)'}: status=${r.status} no router`);continue}
  const ids=idsOf(r.json)
  const line=tiers.map(t=>{let n=0;for(const id of ids)if(t.ids.has(id))n++;return `${t.tab_type}:${n}`}).join('  ')
  console.log(`  ${(slug||'(none)').padEnd(11)} status=${r.status} ids=${ids.size}  overlap -> ${line}`)
}
