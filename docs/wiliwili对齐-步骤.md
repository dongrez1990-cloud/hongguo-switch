# 对齐 wiliwili：步骤与依据

用户要求："排版 UI 做成 wiliwili 那样，主页上有图片加文字，以及快进、倒退、播放、暂停等功能"。
这份文件把这件事拆成有序步骤，每一步都有**参考实现的出处**和**可验证的观察点**。

---

## 进度（第 9 轮结束）

| 步骤 | 状态 |
|---|---|
| 1 首页卡片 图 + 文 | **✅ 真机验证：首页有封面图** |
| 2 播放器控件（快进/倒退/暂停） | **✅ 真机验证：左右能快进倒退、按 Y 能隐藏信息** |
| 3 播放器 UI | **✅ 真机验证：Y 能隐藏，进度条在位**（截图见下） |
| 4 首页改成 wiliwili 那样的网格排版 | 未开始，方案已定（见本节末） |

### 真机验证后的两个发现

**发现 1（真 Bug，已修）：列表封面缓冲上限太小，导致一张卡没有封面。**

真机截图里卡片 4 没有封面，日志给出了原因：

```
IMG: .../a2cb1301...~tplv-shrink:640:0.image is larger than the 327680-byte cap for this call site
IMG: gave up on ... (transport)
```

我当初按**一个样本**（76,430 字节）就把列表上限定成 320 KiB。实测 14 张：

| 分布 | 值 |
|---|---|
| 13 张 | 71 KB – 168 KB |
| **1 张** | **1,117,229 字节（1091 KiB）** |

那张特别大的仍然带着 `~tplv-shrink:640:0.image` 后缀——**CDN 对这张没缩**。所以"用样本当规律"在这里错了。

**修法不是把上限调高**（缓冲是按上限一次性分配的，13 个可见格子 × 1.5 MiB ≈ 19 MiB 空缓冲），
而是**先按小上限取，只在 `HG_HTTP_ERR_TOO_BIG` 时按实测最坏值重试一次**：
常见情况只分配 256 KiB，那张 1.09 MB 的也能拿到。

**发现 2（机制正常）：请求令牌确实在干活。** 日志里有 6 条 `IMG: DROPPED`——
首次布局时 recycler 把 6 个格子重新绑定到了彼此的内容上，于是 6 个已经到达的响应被正确丢弃。
**这正是没有它就会"静默贴错图"的那种情况**，而现在它在日志里是可见的。

---

## 步骤 4 的方案（依据：用户提供的 wiliwili 真机截图）

抄什么、不抄什么，都写清楚：

### 要抄的结构

| wiliwili 的做法 | 出处 | 我们怎么对应 |
|---|---|---|
| **4 列网格**，每格 = 缩略图 + 标题（2 行）+ 作者/日期 | wiliwili 首页截图 | 网格 + 海报 + 标题。**缩略图比例要改**，见下 |
| **顶部标签页**（直播/推荐/热门/追番/影视） | 同上 | 我们的 **4 个分区**（`tab_name`：热播短剧/热播真人剧/热播漫剧/热播AI剧）就是天然的分页 |
| **底部按键提示条** | 同上（`Y 搜索  B 返回  A 确定`） | borealis 自带 `BottomBar`（`views/bottom_bar.hpp`） |
| **缩略图上的时长角标 / 播放量角标** | 同上 | 红果的卡片有 `episode_cnt`（集数）与 `hot_score_data`，可以放角标 |
| 左侧图标侧栏 | 同上 | **不抄**：那是在多个功能页之间导航，我们只有一页，纯装饰 |

### 一个必须偏离的地方：缩略图比例

wiliwili 的缩略图是**16:9 横版**（B 站视频帧）；而红果的 `series_cover` 是**竖版海报**。
把竖版硬塞进 16:9 只会拉伸或留黑边，所以**网格保持，比例改成海报**——
这是照抄结构、不照抄比例，理由是可验证的（图片本身的比例）。

### 关键技术前提（已查明）

**borealis 的 `RecyclerFrame` 没有网格能力**——我查过 `recycler.hpp`，只有 `prepareForReuse`，
没有列数之类的接口。**网格来自 wiliwili 自己的 `RecyclingGrid`**：

```
upstream/wiliwili/wiliwili/include/view/recycling_grid.hpp    7,989 字节
upstream/wiliwili/wiliwili/source/view/recycling_grid.cpp    29,112 字节
#include 只有  <map> + borealis 的 application.hpp / bind.hpp / scrolling_frame.hpp
```

**没有任何 wiliwili 专属依赖**，所以这份组件可以整份搬进来（改 include 路径即可），
再为我们的数据写一个 `RecyclingGridDataSource`。

> **风险提示（写给自己）**：第 1 步的教训是"索引算错是静默的"。
> 网格的数据源有 `getItemCount` / `cellForRow` / `onItemSelected` / `prepareForReuse` 四个入口，
> 比列表多，所以**照抄组件**的同时必须把行/列换算仍留在可测的地方，并保留 `home.tiers=no` 这条退路。

### 步骤 3 实际怎么做的

**进度条是自己画的（nanovg 两个矩形），故意没用 `brls::Slider`。** 理由是个具体的坑：

```cpp
void Slider::draw(...) { buttonsProcessing(); ... }          // slider.cpp:124-126
void Slider::buttonsProcessing() { if (pointer->isFocused()) // slider.cpp:130
View* Slider::getDefaultFocus() { return pointer; }          // slider.cpp:119  <- 无条件
```

`pointer` 在构造时就是可聚焦的，而 `getDefaultFocus()` **无条件**返回它——所以把一个 `Slider` 放进这一屏，
borealis 的焦点搜索就会落到它身上，`buttonsProcessing()` 随即吃掉 **LEFT/RIGHT**，
而那正是我用来 seek 的两个键。**seek 按键会看起来像坏的**，而这个项目已经为"按键是坏的还是没收到"
耗过一整轮。两个矩形没有任何输入路径。

位置在覆盖层文字上方 82 px，**跟着 Y 一起隐藏**（否则"Y 隐藏信息"只说了一半）。

**两行并成一行**：左侧剧名/集号，右侧 `PLAYING  00:12 / 03:45`。
原来的诊断字段（`frame-type`/`hwdec`/`ao`/声道/采样率）从屏幕上**移除了**——画面和声音都已经不是问题，
而音频那条证据本来就在日志里（mpv 自己的 `AO: [hos] 44100Hz stereo 2ch s16` 加上一次性的 `POLL:` 行）。
按键提示也从屏幕上撤掉了：播放器不需要一直广播自己的按键，而它们已经在交接文档和启动日志里。

**`duration` 拿不到时进度条停在 0，不猜测。** mpv 在 load 之后有一段时间会报 0 或负的 duration；
一个谎报位置的进度条比一个停在起点的进度条更糟。

### 步骤 1 实际怎么做的（四个决定都值得留下）

1. **请求令牌 `hg::ImageSlot`**（第 8 轮）：`loadImageAsync` 的第三个参数。响应到达时若 URL 已变
   则丢弃并记一行 `IMG: DROPPED ...`。**没有它，复用的格子会静默贴错图。**
2. **没有封面时用 `setVisibility(GONE)`，而不是"清空纹理"。** 因为 borealis **没有**清空纹理的路径
   （`innerSetImage(0)` 只打一条 `Cannot set texture: 0` 就返回），残留的上一张封面会被继续画出来。
3. **取图前先贴一个 1×1 占位纹理**（`nvgCreateImageRGBA` + `innerSetImage`），这样"新图在路上的那几百毫秒"
   显示的是深灰而不是上一部剧的封面。**不会泄漏**：`Image::freeTexture` 默认 true，
   而 `innerSetImage()` 会释放旧纹理（`image.cpp:379`）。
4. **列表用更小的缓冲上限**（`HG_IMAGE_LIST_MAX_BYTES` = 320 KiB）：可见格子有十几个，
   若每个都按详情页的 2 MiB 上限分配就是约 26 MiB 瞬时内存；而列表封面实测只有 76 KB
   （CDN 自带的 `~tplv-shrink:640:0.image` 在缩图）。

卡片形态：高 104，左侧 60×84 封面（`FIT`），右侧"序号 + 标题"一行、下面一行简介
（`desc` 来自 `series_intro`，此前一直在取但从没显示过）。

### 步骤 2 实际怎么做的

`PlayerView::seekRelative(int seconds)` 用 **`mpv_command_async`**（而不是 `mpv_command`）——
按键处理跑在渲染线程上，而 `mpv_command` 是阻塞的，seek 时会把出帧的线程卡住；
wiliwili 的 `command_async` 包的就是 `mpv_command_async`。

按键：**←/→ = ∓10 秒**，**LB/RB = ∓60 秒**，**A = 播放/暂停**（已有），**Y = 开合覆盖层**，B = 返回，+ = 退出。
每一次按键都打日志 **并把 `time-pos` 回读出来**——因为"命令被接受了"和"真的动了"是两件事，
而按钮"没反应"与"根本没收到"在这个项目里已经耗过一整轮（B 键那次）。

> 注意 LB/RB 在 wiliwili 里是**上/下一集**（`player_base_activity.cpp:226-253`），
> 那是另一个功能——本 App 还没有可以驱动的剧集列表，所以这里先让它们承担大跨度 seek，
> 而不是留成死键。等有了"下一集"再看是否改回去。

---

## 已经做到的（对照起点）

| 环节 | 现状 |
|---|---|
| 首页 | 36 卡分 4 个区（`homeSections[]`），**纯文字列表，无封面** |
| 详情 | 76 集列表 + **封面缩略图**（已验证显示） |
| 播放 | **有画面有声音**（软件渲染路径）、底部两行文字、A 播放/暂停、B 返回、+ 退出 |

所以缺的是：**首页的图片**、**播放器的快进/倒退/进度/OSD**、以及**摆脱那两行常驻文字**。

---

## 参考实现里这些到底在哪

### 播放器按键映射（`wiliwili/source/view/video_view.cpp`）

| 按键 | wiliwili 的行为 | 出处 |
|---|---|---|
| A | 全屏 + TV 控制模式下唤起 OSD 并聚焦进度条；否则 **`togglePlay()`**（播放/暂停） | `video_view.cpp:466-482` |
| 左 / 右 | 唤起 OSD 进度条并把焦点交给它（`osdSlider`） | `video_view.cpp:484-496` |
| LT / LB | **上一项** | `player_base_activity.cpp:226-239` |
| RT / RB | **下一项** | `player_base_activity.cpp:240-253` |
| START | 画质菜单 | `player_base_activity.cpp:270` |

### seek 的实现

```cpp
void MPVCore::seekRelative(int64_t p) { command_async("seek", p, "relative"); }   // mpv_core.cpp:1209
```
外加拖动进度条时的 `requestSeeking(...)`（`video_view.cpp:234,242`）。

### 首页卡片（wiliwili 用 `RecyclingGrid`）

- 数据源契约：`cellForRow` / `getItemCount` / `onItemSelected` / `clearData`。
- 图片用 `ImageHelper::with(imageView)->load(url + ImageHelper::h_ext)`。
- 但**这个 CDN 的尺寸后缀会 400**（已实测），所以用它自带的 `~tplv-shrink:640:0.image`。

---

## 步骤（每步结束都要有可验证的东西）

### 步骤 1 —— 首页卡片：图 + 文（先解决"复用的格子会贴错图"）

**为什么这是第一步**：它最显眼，而且是唯一带**真实危险**的一步。`RecyclerCell` 会被复用，
一个正在取图的格子如果被复用到别的剧集，**后到的响应会把旧图贴到新标题上**——界面照常，
只是内容错了。所以先把机制做对，再谈好看。

1. **给封面加载器加请求令牌**：`loadImageAsync` 返回一个句柄；调用方（每个 cell）保存
   "当前 URL"；响应到达时若 URL 已变则**丢弃**。参考 wiliwili 的 per-request 令牌 + `clearData` 纪律。
2. **`HomeCell` 改成卡片**：左侧 `brls::Image`（缩略图），右侧标题；高度从 64 提到 ~96。
3. **限制并发**：可见格子约 13 个，若每个都开一个 2 MiB 缓冲就是 26 MiB。改成**最多 N 个在飞**，
   装完立刻释放缓冲。
4. 验证：构建 + 日志（`IMG:` 行 + 新的"丢弃过期响应"行）；再看真机。

### 步骤 2 —— 播放器控件（快进 / 倒退 / 播放 / 暂停）

1. 新增 `PlayerView::seekRelative(int seconds)`：`mpv_command({"seek", "+N"/"-N", "relative"})`。
2. 按键（按 wiliwili 的用法，但用我们已有的按键，因为我们要保留 B 返回、+ 退出）：
   - **A** = 播放/暂停（已有）
   - **←/→** = ∓10 秒
   - **LB/RB** = ∓60 秒（对应 wiliwili 的快进量级）
   - **Y** = 显示/隐藏 OSD（wiliwili 的 OSD 就是这个作用）
3. 每个按键都打一行日志 + 回读 `time-pos`，**所以"按了有没有生效"在日志里能看见**，不必靠问。
4. 验证：真机上按一下，日志里出现按键与新的 `time-pos`。

### 步骤 3 —— 播放器 UI 做成"播放器样"

1. 用 `brls::Slider` 或自绘进度条显示 `time-pos / duration`，从 poller 更新（`duration`、`percent-pos` 已在
   wiliwili 的观察列表里，`mpv_core.cpp:401-404`）。
2. 把那两行常驻文字换掉：**标题 + 进度条**，加上一行短提示；**Y 键隐藏/显示**（对应 wiliwili 的 OSD）。
3. 验证：真机上看到进度条在动、Y 能切换显示。

### 步骤 4 —— 更接近 wiliwili 的首页（可选）

分区标题已就位；再往前是**网格卡片**（多列）、横幅区（`bannerList`/`mBannerList`，它们不在那 36 里）、
以及画质菜单。这一步没有新机制，只是布局。

---

## 每一步的"回去的路"（这个项目的惯例）

| 步骤 | 逃生开关 |
|---|---|
| 1 | `image.enable=no` 关掉全部封面加载；`home.tiers=no` 回到扁平列表 |
| 2 | 控件只加不改：A/B/+ 的行为保持不变 |
| 3 | `overlay.labels=no` 隐藏覆盖层 |

## 一个必须记住的约束

**首页是当前唯一"已经能用"的屏幕。** 步骤 1 会改它，所以：① 令牌机制先做对；
② 保留 `image.enable=no`；③ 如果真机上首页出问题，一个 cfg 文件就能回到现在的样子，**不用重编译**。
