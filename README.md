# 红果 TV 移植版 · hongguo-switch

**在 Nintendo Switch 上观看红果短剧（hongguoduanju.com）的自制程序。**

用 C++ 写成，UI 基于 [borealis](https://github.com/xfangfang/borealis)，
视频走 mpv（deko3d 渲染），工程结构沿用 [wiliwili](https://github.com/xfangfang/wiliwili) 的
Switch 集成方式。许可证 **GPL-3.0**（全文见 `LICENSE`）。

**版本：v1.0.0（build176）**　·　变更历史见 [版本历史](#版本历史)。

> **本项目与红果短剧官方无关。** 它通过逆向得到的公开接口访问内容，
> 不包含、也不重新分发任何平台内容。请只观看你有权观看的内容 —— 详见 [免责声明与限制](#免责声明与限制)。
>
> **状态说明**：本程序依赖的上游接口**随时可能变更**，变更后程序会无法使用，直到重新实测修好。
> 本项目历史中已发生过两次。发版说明里写日期，就是"截至那天可用"。

---

## 功能

| 功能 | 说明 |
|---|---|
| 主页 | 按上游栏目取剧集，海报网格；方向键/摇杆移动，滚动到边界自动翻页 |
| 搜索 | 系统软键盘（可拼音）输入；结果来自上游搜索 + 本地索引，两个来源分开标注 |
| 详情页 | 剧名、简介、选集网格；**L/R 切集数组**（一组 20 集），跨组自动定位 |
| 播放 | mpv + deko3d 硬解；多清晰度档位（上限由服务端决定，当前接口最高 720p） |
| 播放控制条 | A 呼出：播放/暂停、拖动进度、音量、倍速、缩放、清晰度、设置 |
| 画面菜单 | X 呼出；滤镜（亮度/对比/饱和/色相）、缩放模式、电视安全区 |
| 收藏 / 历史 | 本地存储（`hg_store`），播放中每 15 秒记录进度，可从卡片续播 |
| 自动下一集 | 仅在 EOF 时触发（STOP 不算）；ZL/ZR 手动上一集/下一集 |
| 自检状态灯 | 侧边栏末尾一盏灯，把"播放链路能不能走通"直接画在界面上 |
| 底座模式 | 检测 OperationMode 切换，按主机的 TV underscan 居中内缩 |
| 不休眠 | 启动时 `appletSetAutoSleepDisabled`，并在模式切换后重新断言 |
| 设置界面 | 五大栏目（播放 / 外观 / 网络 / 诊断 / 关于），全部落到 `sdmc:/switch/hongguo.cfg` |
| 网络诊断 | IP、DNS、API 连通性、主机时钟偏差，直接显示在屏幕上 |
| 界面语言 | 中文 / 英文 / 跟随系统 |

---

## 构建

### 0. 前置

* **devkitPro**（devkitA64 + libnx），装在 `C:\devkitPro`
* **Node.js**（只用于生成表和主机侧探针，不是构建必需）
* **TinyCC**（`tcc.exe`，仅主机测试需要）

```powershell
# devkitPro 环境里安装 Switch 开发包
pacman -S switch-dev switch-curl switch-mpv switch-ffmpeg
```

### 1. 检出 wiliwili 到 `upstream/wiliwili`

**仓库里不含 `upstream/`** —— 那是 1.6 GB 的上游克隆，它的许可证跟着它自己走。
构建需要它提供的两样东西：borealis 源码树，和预编译的 deko3d 着色器。

```powershell
git clone https://github.com/xfangfang/wiliwili.git upstream/wiliwili
cd upstream/wiliwili
git checkout v1.6.0        # 本仓库对齐的版本；其他版本未经测试
git submodule update --init --recursive
cd ..\..
```

> 如果 `git submodule` 在当前环境的 MSYS2 下起不来（"couldn't create signal pipe"），
> 用仓库自带的 `scripts\fetch-submodules.ps1 -RepoRoot upstream\wiliwili` 代替：
> 它用 `ls-tree` 读出 gitlink SHA，逐个 clone/checkout。

### 2. 编译 borealis（一次，约 2 分钟）

```powershell
powershell -File scripts\stage-borealis.ps1      # 拷到 ASCII 暂存目录并校验完整性
powershell -File scripts\build-borealis.ps1 -Jobs 8
```

> `scripts\build-hongguo.ps1` 会在发现 `libborealis.a` 缺失时自动跑这两步，
> 所以这一步可以被跳过；单独跑只是让失败点更清楚。

### 3. 编译 NRO

```powershell
powershell -File scripts\build-hongguo.ps1 -OutNro out\hongguo.nro
```

产物是 `out\hongguo.nro`（以及同名 `.elf`）。拷到 SD 卡的 `sdmc:/switch/` 下，
从 hbmenu 启动。

变体：

```powershell
# 最小启动测试：只有 borealis，没有网络也没有 mpv —— 用来把"链接/启动问题"和"应用逻辑问题"切开
powershell -File scripts\build-hongguo.ps1 -SmokeTest -OutNro out\hongguo-smoke.nro

# 分阶段诊断入口：同样的启动序列，先画 60 帧空标签，再上真正的 HomeActivity
powershell -File scripts\build-hongguo.ps1 -Diag -OutNro out\hongguo-diag.nro
```

### 4. 主机测试（不需要主机，不需要 devkitPro 的 Switch 部分）

```powershell
powershell -File app\src\tests\run.ps1 -SrcDir app\src -TccPath <tcc.exe>
```

**19 个套件、1314 项检查、0 失败**（外加一个源码 grep 检查"存活令牌不变量"）。

为什么用 TinyCC：它是 **i386（ILP32）**，而目标是 **aarch64（LP64）**。
同一份源码在两种字宽下都编一遍，是抓"多写的 `long`"或隐式整型截断最便宜的办法 ——
那种 bug 在主机上正常，在 Switch 上会签坏签名。

同样的测试也可以交叉编译成 NRO 上机跑：`scripts\build-switch-test.ps1`。

---

## 目录结构

```
LICENSE               GPL-3.0 全文
README.md             本文件
THIRD-PARTY.md        第三方组件清单、许可、研究参考来源
.gitignore

app/
  src/                核心逻辑（纯 C，可主机测试，不依赖 borealis）
    hg_crypto.c       MD5 / SHA-512 / SM3 / RC4 / AesV3（厂商变体）
    hg_aes.c          AES-128 / CTR
    hg_cenc.c         CENC（cenc-aes-ctr）解密与样本表遍历
    hg_mp4.c          ISO-BMFF 头解析
    hg_sign.c         签名器：5 字段 + medusa 变体
    hg_request.c      出站请求构造（签名参数、UA、设备指纹）
    hg_router.c       _ROUTER_DATA 网页抓取（首页 / 详情）
    hg_api.c          响应解析：搜索 / 详情 / video model → 定长结构体
    hg_play.c         播放地址链路（player_access_key → spade → CENC key）
    hg_quality.c      清晰度阶梯与排序
    hg_grid.c         海报网格几何（列数、行距、内容高度）
    hg_sidebar.c      侧边栏布局与移动规则
    hg_pmenu.c        播放器画面菜单的状态机
    hg_scrub.c        拖动 seek 的模型
    hg_index.c        本地索引合并策略
    hg_ctl.c          控制条布局
    hg_history.c      本地历史 / 收藏的记录格式
    hg_http.c         libcurl 封装
    hg_httpcfg.c      HTTP 参数校验（拒绝非法配置，而不是放过去）
    hg_lang.c         中英文界面文本表
    hg_shot.c         screenshot-raw 探针
    hg_tables.h       从上游常量生成的 AesV3 S-box / UA / 设备串
    tests/            主机测试：19 套件 + 源码不变量检查 + 真机响应夹具
    tools/            研究期的一次性生成器与探针（见下）
  hongguo/            UI 层（C++，borealis）
    main.cpp          入口：borealis 初始化、窗口、推入 Home
    hg_boot.cpp       启动序列（服务初始化、cfg、主题、日志）
    hg_client.cpp     核心 C 层与 UI 之间的桥（线程、回调、数据整形）
    home_activity.cpp 主页
    search_activity.cpp
    category_activity.cpp
    detail_activity.cpp
    player_activity.cpp / player_view.cpp   播放器：容器、mpv、deko3d、每帧绘制
    library_activity.cpp                    收藏 / 历史
    settings_activity.cpp                   设置五栏目
    hg_selfcheck.cpp                        播放链路自检（状态灯的数据来源）
    hg_image.cpp                            封面图下载与解码
    hg_store.cpp                            本地历史 / 收藏持久化
    hg_applet.cpp                           底座模式与不休眠
    hg_sidebar_view.cpp / hg_status_dot.cpp
    hg_log.cpp / hg_cfg.cpp / hg_thread.cpp / hg_haptic.cpp / ...
  switch-test/main.c  真机自检 NRO 的入口（密码学与媒体层上机验收）
  net-test/           网络层最小 NRO
  play-test/          播放链路最小 NRO
scripts/              构建、打包、部署辅助脚本（PowerShell 与 shell）
docs/                 设计规格、逐轮排查记录、接口实测笔记（中文）
assets/               应用图标源图
```

**三层是怎么分的**：`app/src` 是纯 C，不 include 任何 borealis 头文件，
所以它能在主机上用 TinyCC 跑、也能交叉编译到 aarch64 —— **逻辑的验证不依赖主机**。
`app/hongguo` 是 UI，只做"把数据变成画面、把按键变成调用"。
两边唯一的接触面是 `hg_client.cpp`。

`app/src/tools/` 与 `app/src/tests/*.mjs` 是**研究期的一次性工具**：抓夹具、
生成向量、探参数形状。它们不是构建的一部分，保留是为了让"这个结论是怎么测出来的"可查。

---

## 文档

`docs/` 里是完整的工程记录，按性质分三类：

* **规格与结论**：`移植方案与结论.md`、`wiliwili界面结构复刻规格.md`、
  `wiliwili按键与触屏规格.md`、`设置界面-复刻清单.md`
* **接口实测**：`播放接口-2026-10-16-实测补充.md`、`新播放接口-第21轮.md`、
  `HTTP与图片设置.md` —— 记录上游接口**当时**长什么样，以及怎么测出来的
* **逐轮排查**：`交接*.md`、`第 N 轮-*.md`、`真机修复记录.md` —— 每个 bug 的根因、
  证据、修法，包括**修错了的那几次**

入口是 `docs/交接.md`。它是写给"对项目一无所知的接手者"的。

> 这些文档保留原貌，包括失败和返工。它们不是宣传材料，是工程日志 ——
> 里面写的判断有一小部分后来被证明是错的，那也照样留着。

---

## 致谢

本项目站在别人的工作上：

* **[wiliwili](https://github.com/xfangfang/wiliwili)**（GPL-3.0）— Switch 端的 TV 客户端。
  本项目的工程结构、borealis 集成方式、mpv/deko3d 渲染接入、按键与界面布局规格
  **都来自它**。`docs/wiliwili*.md` 是对它逐文件的普查与对齐记录。
  **这个项目是 wiliwili 的衍生工作。**
* **[borealis](https://github.com/xfangfang/borealis)**（GPL-3.0，wiliwili 的分支）— UI 框架
* **[libnx](https://github.com/switchbrew/libnx)**（ISC）— Switch 自制运行时
* **[mpv](https://github.com/mpv-player/mpv)** / **FFmpeg**（LGPL-2.1+ / GPL-2.0+）— 解码与渲染
* **[libcurl](https://curl.se/)**（curl 许可证）— HTTP
* **[deko3d](https://github.com/devkitPro/deko3d)**（MIT/zlib 风格）、
  **[nanovg](https://github.com/memononen/nanovg)**（zlib）

接口层的知识来自四个公开研究项目，**感谢作者的公开工作**：

* **[N3urda/hongguoTV](https://github.com/N3urda/hongguoTV)**（GPL-3.0）
* **[zhenyong97/hongguo-downloader](https://github.com/zhenyong97/hongguo-downloader)**
* **[zhangbaio/hongguo](https://github.com/zhangbaio/hongguo)**
* **[563617356/hongguo-tv](https://github.com/563617356/hongguo-tv)**

完整的组件清单、许可与"哪些是借鉴接口、哪些是独立实现"的说明见 **`THIRD-PARTY.md`**。

---

## 版本历史

| 版本 | 说明 |
|---|---|
| **v1.0.0（build176）** | 首个公开发布版本。 |

### v1.0.0（build176）— 首个公开发布

**修复：build175 引入的过度提前放弃。**

服务端的播放接口在免费可看区间内**也会偶发拒绝**（HTTP 403）：对同一个请求连续重试，
实测返回序列是 `403 200 200 403 200 403 403 403` 这类**间歇**结果 —— 也就是说，
一次拒绝的含义通常是"这次请求落在了不巧的后端"，而**不是**"这个设备不能看这一集"。

build175 把"第一次拒绝"当成了定论并立即停止该档位的重试。真机结果是**第 50 集以内本来能看的
很多集反而看不了** —— 因为它们恰好要第 2 次或以后的请求才成功。build174 只是把重试预算用完，
所以它能看。按用户的要求（原话是"按 174 的就行"），本次发布**回到 build174 的完整重试行为**：

* 撤掉了 build175「服务端拒绝就立刻停止该档重试」那段逻辑；
* **拒绝仍然被识别并记录**（它决定给用户看的措辞，这部分是 build175 里值得保留的），
  但**不再终结重试预算**；
* `outRefusal` 是**黏性**的 —— 只要本轮出现过一次拒绝，最终失败就按**"服务端拒绝"**上报，
  而不是报成"网络失败"，即使最后一次尝试恰好是超时也一样。

同时保留了 build175 里两项**无副作用**的改进：

* **清晰的"需要授权"提示** —— 越过免费区间时，界面如实说明该集需要解锁，
  而不是把一串 `HTTP 403-42 bytes-{"error":"protected_playback_unavailable"}`
  当作解释丢给用户。
* **官方站点的设备 cookie** —— 请求携带官网本身下发的 `ttwid` 设备标识。

**验证状态**：构建**零警告零错误**；主机测试 **19 套件 / 0 失败**，
其中 play-info 套件 **74 → 75 项** —— 那条断言是被**反转并补回**的，不是删掉的：
"拒绝不再终结重试"与"拒绝仍被记录"两条断言都在，历史写在测试注释里。
本版已部署到真机验证。

> **未发布过 build175。** 该版本带着上面这个回归，只在开发中构建过、从未对外提供；
> 公开历史从 build176 开始，因此不存在"下载到有回归的版本"这条路径。

更早的逐轮开发记录（build31 → build175）在 `docs/构建号与产物对照.md`，
包含每一次改动的内容与真机确认状态。

---

## 免责声明与限制

**请先读完这一节再决定要不要用。**

1. **接口是逆向得到的，随时可能失效。**
   本程序不调用任何官方 API，它复现的是网页端与 App 端**当时观察到**的请求形状 ——
   包括签名参数、设备指纹、频率限制。上游一次改动就能让它彻底不能用。
   **项目历史里已经发生过两次**：一次旧接口整体退役（`Code:110001 未知异常` 回给每一个请求，
   签名的、不签名的都一样），一次播放地址迁移到新路由。两次都是重新实测才修好，
   期间程序不可用。**没有稳定接口的承诺。**

2. **内容的著作权归平台与出品方所有。**
   本项目不分发、不缓存、不转存任何视频内容；它只显示接口返回的元数据并播放接口返回的地址。
   **请只观看你有权观看的内容**，并遵守当地法律与平台条款。

3. **每条剧只开放前若干集，之后服务端拒绝。**
   这是**服务端**的行为，不是本程序的限制。实测 5 部剧的边界分别是
   **50 / 12 / 11 / 13 / 6 集**，因剧而异 —— 越过边界的集，服务端一律拒绝播放。

   **本项目不试图绕过任何访问控制**：不做会员、不做付费剧、不做任何形式的越权尝试，
   也不把"重试"当成绕过手段。重试在这里只用于对付**免费区间内**的**间歇性**拒绝
   （见[版本历史](#版本历史)），而越过边界后的拒绝是**持续性**的 ——
   每个清晰度档位、每次尝试都被拒绝，程序随即停止并如实告知。

   越过边界时，界面会明确提示**"该集需要授权/解锁"**，
   而不是把原始的 HTTP 错误码丢给用户，也不会假装是网络故障。

4. **清晰度上限由服务端决定。**
   当前接口给出的最高档是 **720p**。程序不做插值、不做"伪 1080p"；
   菜单里显示的编码与档位就是实际在播的那一档。

5. **不要期望"未做"的功能。**
   弹幕、评论、点赞、投屏、登录都不做，原因写在 `docs/` 里
   （主要是：载荷里根本没有这些字段）。**界面不做假菜单** ——
   能做就显示，不能做就不出现，而不是放一个按不动的按钮。

6. **未签名的 homebrew。**
   NRO 需要在已破解的主机上通过 hbmenu 运行；NSP 打包（`scripts/build-nsp.ps1`）
   需要你自己的密钥库，**不打进去**，也不随仓库分发。

7. **它是个人项目。** 没有测试团队，没有发布流程，没有兼容性承诺。
   已经踩过的坑与"已知未修"的项都写在 `docs/待做清单.md` 里。

---

## 许可

**GPL-3.0**（`GPL-3.0-only`）。全文见 [`LICENSE`](LICENSE)。

因为上游 wiliwili 与 borealis 是 GPL-3.0，本项目作为衍生工作**必须**同样以 GPL-3.0 分发。
如果你分发编译好的二进制，请同时提供对应的完整源码。
