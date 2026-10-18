# 设置界面补齐剩余项：HTTP 客户端与图片线程数

`设置界面-复刻清单.md` §一 D 的剩余项（**这是 §二 六步之后按价值排序的第 2 项**）。
**代码已完成、已构建已部署、未上机。**

---

## 一、做了哪些项，以及为什么是这几项

清单里 D 组有六项，判定如下：

| wiliwili 设置项 | 本次 | 说明 |
|---|---|---|
| `HTTP_PROXY` / `HTTP_PROXY_STATUS` | **✅ 做** | libcurl 的 `CURLOPT_PROXY` |
| `TLS_VERIFY` | **✅ 做** | `CURLOPT_SSL_VERIFYPEER/HOST` |
| `HTTP_TIMEOUT` | **✅ 做** | 原本**硬编码 20 s** |
| `HTTP_CONNECTION_TIMEOUT` | **✅ 做** | 原本**硬编码 10 s** |
| `HTTP_DNS_CACHE_TIMEOUT` | **✅ 做** | |
| `IMAGE_REQUEST_THREADS` | **✅ 做** | 我们这边是 `hg_thread` 起的工作线程 |
| `TEXTURE_CACHE_NUM` | **❌ 不做，理由实测** | 我们的封面是**一格一图、加载一次就留着**（网格不做 cell 回收，见 `首页网格-步骤2.md` §二），所以**没有可缓存的东西**：纹理缓存要缓存的正是"同一张图被多个格子/多次滚动复用"，而这个 App 不存在这种复用。**做出来会是纯假 UI**——一个改了没有任何效果的滑块 |

---

## 二、之前是什么样，问题在哪

`hg_http_request()` 里四个字面量：

```c
CURLOPT_TIMEOUT,        20L
CURLOPT_CONNECTTIMEOUT, 10L
CURLOPT_FOLLOWLOCATION, 1L
CURLOPT_ACCEPT_ENCODING, "identity"
```

**没有代理，也没有关掉证书校验的开关。**

作为**默认值**这样写没问题；作为**策略**是错的：这个服务**只从某些网络可达**，
而"等多久"和"信不信这个证书"是使用者**必须能回答**的问题。

---

## 三、三个"宁可拒绝也不接受"的值 —— 这是这一步的重点

一个**解析不可测**的旋钮，迟早会被设成某个不可能的值。所以解析与校验在
`hg_httpcfg.c`，93 项主机测试；而下面三条是**真正被测的东西**：

### 1. `http.timeout=0` 被**拒绝**

libcurl 把 `CURLOPT_TIMEOUT 0` 读成 **"永不超时"**。所以一个想表达"快点放弃"的 0
会得到**相反**的结果：请求挂到主机被关掉为止，**没有错误、没有日志**。

### 2. 连接超时**大于**总超时被**拒绝**

连接阶段发生在总超时**内部**，所以比总超时更大的连接超时**永远不可能被触发**——
一个看起来生效、实际什么都不做的数字。**比拒绝更糟**，因为它看起来是对的。

### 3. 代理**缺 scheme** 被**拒绝**

libcurl 要 `http://host:port`。`192.168.1.x:7890` 是**另一个字符串**，
libcurl 会按自己的方式误解它——而症状是**每个请求都失败**，看起来像网络坏了，
不像 cfg 打错了一个字。

**每一次拒绝都写一行到 `hongguo.log`，带上原因，并保留默认值。**
被静默忽略的 cfg 行比没写过的更糟，因为使用者相信它生效了。

---

## 四、图片线程数：为什么需要一个上限

网格一页绑十几个格子，每个格子立刻要图。**每次取图 = 一个 HTTP 请求 + 一个工作线程 +
最多 1.5 MiB 缓冲**（超限重试的那一档）。所以无上限的页面会把"快速滚动"变成
"瞬间申请一大片内存"，而主机内存是固定的。

`image.threads` 默认 6，范围 1..16。

**用计数器不用队列**：正在要图的格子**不关心**图是现在到还是 300 毫秒后到——
两种情况它都显示占位图，而**请求令牌（`ImageSlot`）让晚到的响应是安全的**。
所以碰到上限的请求**干脆不启动**，日志说明原因。队列会让视图被 `ptrLock` 锁更久，
还给这个项目已经付过一次代价的生命周期问题再加一层。

### 这里有一个值得记下来的实现细节

一次取图有**六种**结束方式（传输失败、状态码错、空 body、响应被丢弃、安装成功、
没有可用工作线程）。**在每一条路径上手动减计数**就是六次漏掉一次的机会，
而**漏掉一次会让预算永久变小**——症状是"跑了很久之后封面不再出现"，
而原因在几百行之外的代码里。

所以用了一个**自定义引用计数的 token**，每个退出回调各持一份，**只会释放一次**。

**而且它不是 `std::shared_ptr`**：带自定义 deleter 的 `shared_ptr` 在这个工具链上会报

```
warning: array subscript 'std::mutex[1]' is partly outside array bounds of 'unsigned char [24]'
```

——libstdc++ 控制块析构的 `-Warray-bounds` 误报。**这个项目的构建结果是要用眼睛读的**，
所以"大概没事的警告"不能接受：它训练读者跳过下一个警告。二十行显式引用计数就能避开，
而且把意图写得更清楚。

---

## 五、退路

| 退路 | 效果 |
|---|---|
| 不写这些键 | 与改动前**逐字节相同**的默认值（20 / 10 / 60 / 跟随即开 / 校验开 / 6 线程） |
| `http.proxy=` 留空 | 被拒绝并保留默认（写空值不是"清空"的写法——见下） |

**写空值为什么也是拒绝**：cfg 文件的约定是**键不存在 = 用默认**，
所以留空更像打错而不是"清空"，而一个"清空代理"的键如果被理解为"用空代理"，
结果就是所有请求都走一个空代理。

---

## 六、观察点

**屏幕**：设置界面本身**没有加新标签页**——这些键走的是既有的 cfg 文件，
可以在 FTP 上改一行验证，**不需要点界面**（这不是省略，而是这个项目一贯的用法：
`hongguo.cfg` 是唯一的存储，见 `hg_cfg.hpp`）。

**日志**（每次启动都会打）：

```
HTTPCFG: 4 applied, 0 refused -- timeout=45s connect=15s dns_cache=60s redirect=yes tls_verify=no proxy='socks5://127.0.0.1:1080' image_threads=4
HTTPCFG: TLS verification is OFF (http.tls.verify=no) -- the connection is encrypted but the certificate is not checked
HTTPCFG: using proxy socks5://127.0.0.1:1080
```

被拒绝时：

```
HTTPCFG: http.timeout=0 REFUSED -- 0 would mean NO timeout in libcurl, which is the opposite of what it reads as; the default is kept
HTTPCFG: http.connect.timeout=60 REFUSED -- the connect timeout is larger than the total timeout, so it can never be reached
HTTPCFG: http.proxy=192.168.1.x:7890 REFUSED -- needs a scheme, e.g. http://host:port or socks5://host:port
```

判读：

| 现象 | 结论 |
|---|---|
| `N refused` 大于 0 且请求仍然失败 | 看那几行 `REFUSED` 的原因——**cfg 的那一行没生效** |
| `tls_verify=no` 但连不上 | 证书问题被绕过了，那么是**网络层**的问题（DNS/代理/路由） |
| 设了代理但 `proxy=''` | 那一行被拒绝了；原因就在上面几行 |
| `image.threads` 相关的 `IMG: N fetches already in flight` | 上限生效了；封面会**成批**到达而不是一次全到，**不是故障** |

---

## 七、文件清单

| 文件 | 作用 |
|---|---|
| `app/src/hg_httpcfg.h` / `.c` | HTTP 客户端参数的解析、范围与**拒绝**（**新增**，93 项测试） |
| `app/src/tests/hg_httpcfg_test.c` | 其中重点是被拒绝的那三类值（**新增**） |
| `app/src/hg_http.h` / `.c` | `hg_http_set_options()` / `hg_http_get_options()`；四个字面量改为读配置 |
| `app/hongguo/hg_client.cpp` | `globalInit()` 读 cfg、逐键应用、**记录每一次拒绝的原因** |
| `app/hongguo/hg_image.cpp` | 并发取图上限（`image.threads`）+ 引用计数 token |
| `out/hongguo.cfg` | 全部新键的说明与三条拒绝规则 |
| `app/src/tests/run.ps1` | 第 12 个套件；基线 823 |

**基线（实测）**：**12 套 / 823 项 / 0 失败**。
逐套：crypto 21、signer 107、cenc 15、mp4 13、router 45、api 97、fixture 102、
pmenu 98、grid 77、scrub 81、quality 43、**httpcfg 93**。`hongguo.nro` 零警告。
