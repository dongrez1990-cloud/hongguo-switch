# build175：把"服务端拒绝"与"网络抖动"分开（含 ttwid 的**实测**结论）

> 本文记录 build175 的实测依据。**两条与我收到的任务描述不完全一致的发现**写在最前面，
> 因为它们决定了这个构建**没有**做什么。

---

## 1. ⚠️ `ttwid` 不能修好"打不开的集"（我复现不出证据 1）

任务描述说：第 50 集不带 cookie = 403、带上 cookie = 200。**我没能复现。**

### 我确认的部分

| 项 | 结果 |
|---|---|
| 站点确实发 `ttwid` | ✅ `GET /` 与播放页都回 `set-cookie: ttwid=1%7C…%7C1792115422%7C<64hex>; Path=/; HttpOnly; Secure` |
| cookie 本身是好的 | ✅ 133 字符、格式正确；站点**接受它**（回显同一值而不是重发新的） |
| 用它请求仍被拒 | ❌ 见下表 |

### 实测（2026-10-16，本机）

**原样复现（每轮都新取 cookie，同一时刻只差这一个头）：**

```
ep50 round 1: noCookie=403 {"error":"protected_playback_unavailable"}   withCookie=403 同样
ep50 round 2: noCookie=403                                              withCookie=403
ep50 round 3: noCookie=403                                              withCookie=403
ep50 round 4: noCookie=403                                              withCookie=403
```

**交错对照（去掉时间漂移，奇数轮与偶数轮交换顺序）：**

```
ep[45] interleaved: noCookie 2/12   withCookie 0/12
ep[48] interleaved: noCookie 3/12   withCookie 0/12
ep[49] interleaved: noCookie 1/12   withCookie 0/12
ep[50] interleaved: noCookie 0/12   withCookie 0/12
```

**更大样本（每集每档各 10 次）：**

```
ep[ 0] noCookie 10/10  withCookie 10/10
ep[45] noCookie  1/10  withCookie  0/10
ep[48] noCookie  2/10  withCookie  0/10
ep[49] noCookie  1/10  withCookie  0/10
ep[50] noCookie  0/10  withCookie  0/10
```

⇒ **带 cookie 不但没有"解锁"，在交错对照里反而更低** —— 这是抖动，不是 cookie 的效果。
（两次独立跑还出现过互相矛盾的结果：一次 ep49 带 cookie 6/6、另一次 0/10。**这正是抖动的特征**，
不能在 n 小的时候当成因果。）

### 另外两处对不上

- 描述说"该剧 222 集"⇒ 我读到 **76 集**（`episode_cnt=76`、`episode_right_text="全76集"`）；
- 描述提到 `accessible_episode_cnt=10` ⇒ 我拿到的响应里**没有这个字段**。

### ⇒ 所以 build175 **仍然实现了 ttwid**，但**不声称它修了什么**

带上它是**对的行为**：站点自己的客户端就是这么做的，缺它意味着我们这个客户端是唯一的例外，
将来可能因为别的原因被拒。代码注释里明确写了"**这不是那些 403 的修复**"。

---

## 2. ✅ 证据 2 我完全复现了：边界因剧而异，且不是付费标记

| 剧 | 集数 | `pay_info.pay_type` | `episode_right_text` | 首个被拒的下标 |
|---|---|---|---|---|
| 7503745441910017049 | 76 | 0 | 全76集 | **50** |
| 7691938290202577944 | 132 | 0 | 全132集 | 12 |
| 7695704804978330649 | 222 | 0 | 全222集 | 11 |
| 7692043430955863102 | 126 | 0 | 全126集 | 13 |
| 7690494161551625241 | 100 | 0 | 全100集 | 6 |

- 每集条目**都没有** locked / pay / vip / right 类字段（`keys=14`，marker 字段为空，`disable_play=false`）；
- 边界**稳定**：ep0 连测 5 次全 200，ep50 连测 5 次全 403（同一分钟内）。
- 边界**因剧而异**（6/11/12/13/50）⇒ **任何写进代码的集数都是错的**。

**⇒ 代码与文案里不得出现任何集数、也不得称"免费范围/付费"。** 已由测试钉死
（`test_build175_no_invented_reason`，扫描**去掉注释后**的代码里是否出现
`free range` / `episode 50` / `paywall` 等）。

---

## 3. 真正的用户问题：那句话既慢又难懂

卡上（build174）报的是：

```
Playback failed, unavailable from this source after 20 attempts over 3 quality tiers.
The server said HTTP 403-42 bytes-{"error":"protected_playback_unavailable"}
```

两个毛病，都在**我们这边**：

1. **对一个"不"字花光了预算。** 实测这类集在**每一档每一轮**都是 403（0/12），
   而重试预算（720p×6 → 480p×3 → 360p×3）是为**间歇性** 403 设计的
   （同一请求连发 8 次会得到 `403 200 200 403 200 403 403 403`）。对确定性拒绝重试 6 次纯属等待。
2. **把机器的话念给了人听。** `20 attempts over 3 quality tiers` + `HTTP 403-42 bytes-{...}` 是诊断，
   不是解释。

### build175 的改法

| 问题 | 改法 |
|---|---|
| 慢 | **确定性拒绝**（body 含 `protected_playback_unavailable`）在**该档第一次**就停，不再耗完预算。最坏从 ~12 次降到 **3 次**（每档一次）。**间歇性的失败仍然走完整预算** —— 那正是预算的用途 |
| 难懂 | 用户看到**一句话**（走 `hg_lang.h`）：`该集需要服务端授权后才能观看` / `This episode needs the service's permission to play`；**服务端原话仍完整进日志** |
| 两类失败混在一起 | `Result::serverRefused` 标志把它变成**机器可读**的：服务端拒绝 → 上面的句子；**传输/网络失败 → 保持原来的网络表述**（反过来把"没网"说成"该集不可用"同样是错的） |
| 自动连播 | 到达边界时仍需**体面停下**：`startEpisode` → 计划失败 → `playbackFailed` 回调把集号**放回原位**并通知，不崩溃、不继续往前试 |

---

## 4. 复现脚本

| 脚本 | 作用 |
|---|---|
| `_probe/_b175_cookie1.mjs` | 确认 `Set-Cookie: ttwid=…` 确实下发 |
| `_probe/_b175_cookie6.mjs` | **原样复现**描述里的 ep50 测试（4 轮，每轮新 cookie） |
| `_probe/_b175_cookie4.mjs` | **交错对照**（去掉时间漂移） |
| `_probe/_b175_cookie3.mjs` | 每集每档 10 次的大样本 |
| `_probe/_b175_boundary.mjs` | 逐集边界扫描 + 权益字段 |
| `_probe/_b175_exhaust.mjs` | 边界集在**三档各 4 轮**下的穷举（0/12） |
| `_probe/_b175_series2.mjs` | **5 部剧**的边界对比（证明边界因剧而异） |
| `_probe/_b175_time.mjs` | 边界是否随时间漂移（不漂） |

```powershell
$env:HONGGUO_CJS='<upstream-checkout>\hongguoTV-main\server\vendor\hongguo.cjs'
node <probe-dir>\_b175_cookie6.mjs     # cookie 原样复现
node <probe-dir>\_b175_series2.mjs     # 边界因剧而异
```
