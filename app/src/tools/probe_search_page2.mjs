const UA='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36'
function extractRouter(html){const m=html.indexOf('_ROUTER_DATA');if(m<0)return null;const f=html.indexOf('{',m);let d=0,q=false,e=false
 for(let i=f;i<html.length;i++){const c=html[i];if(q){if(e)e=false;else if(c==='\\')e=true;else if(c==='"')q=false;continue}
  if(c==='"')q=true;else if(c==='{'||c==='[')d++;else if(c==='}'||c===']'){d--;if(!d)return html.slice(f,i+1)}}return null}
async function page(u){const r=await fetch(u,{headers:{'User-Agent':UA,'Accept-Language':'zh-CN,zh;q=0.9'},signal:AbortSignal.timeout(20000)})
 const h=await r.text();const b=extractRouter(h);if(!b)return{status:r.status,json:null}
 return {status:r.status,json:JSON.parse(b)}}
const kw='ai'
const a=await page(`https://hongguoduanju.com/search/${encodeURIComponent(kw)}`)
const sp=a.json.loaderData['search_(keyword)/page']
console.log('--- searchList[0].video_data keys ---')
console.log(Object.keys(sp.searchList[0].video_data).join(', '))
const vd=sp.searchList[0].video_data
for(const k of Object.keys(vd)){const v=vd[k];if(typeof v==='string'&&v.length>0&&/cover|pic|image|url/i.test(k))console.log(`   cover-ish: ${k} = ${v.slice(0,70)}`)}
console.log(`totalCount=${sp.totalCount} accessibleEpisodeCnt=${sp.accessibleEpisodeCnt}`)
console.log(`item count=${sp.searchList.length}`)
/* page 2: path form with ?page=2 */
for(const u of [`https://hongguoduanju.com/search/${encodeURIComponent(kw)}?page=2`,
                `https://hongguoduanju.com/search?keyword=${encodeURIComponent(kw)}&page=2`]){
 try{
  const r=await page(u)
  let ids=[]
  if(r.json){(function w(n){if(!n||typeof n!=='object')return;if(Array.isArray(n))return n.forEach(w)
    if(n.keyword&&n.name)ids.push(n.keyword);for(const v of Object.values(n))w(v)})(r.json.loaderData)}
  console.log(`\n${u}\n  status=${r.status} hits=${ids.length} first=${ids[0]||'-'} sameAsP1=${ids[0]===sp.searchList[0].keyword}`)
 }catch(e){console.log(`\n${u}\n  FAILED ${e.cause?e.cause.message:e.message}`)}
}
