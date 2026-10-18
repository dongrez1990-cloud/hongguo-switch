# build133 之后的待办与「历史卡片被挡」的根因（下一轮起点）

**当前保留版本**：`build133`（卡上 `/switch/hongguo.nro` + `/switch/hongguo-build133.nsp`，SHA-256 `B551DC7CB4DDABDC…` ✓）
**用户确认**：进出历史/收藏页**正常了** ✓；主页"收藏"那格已按你的要求**取消** ✓（收藏从历史页进去 ✓）。

---

## 一、历史卡片：「第 N 集」被下一排封面挡住 —— 根因已定位 ✓✓

`home_activity.cpp:481` 的 `HomeGridCard`（**历史页与主页共用同一个卡片** ✓）：

```cpp
art->setGrow(1.0f);                     // 封面区撑满剩余空间 ✓
cover->setHeightPercentage(100.0f);     // ← 封面占满整个艺术区 ✗
```

**卡片高度** = 封面(撑满) + 标题 + `第 N 集` ✗ → **超出网格分配的格高**
（历史页日志实测：`HOMEGRID: layout #3 … cell 140.0x266.0` ✓）
→ 卡片**溢出到下一排** ✓ → **下一排的封面盖住上一排的「第 N 集」** ✓✓ = 用户原话 ✓。

**修法（要点）**：
1. 封面从"占满"改为**按宽度定高的海报比例** ✓（140 宽 → 150± 高 ✓），不要用 100% ✓；
2. 标题与 `第 N 集` 各留**固定空间** ✓（`setHeight` 或固定行高 ✓），使卡片总高**不超过格高** ✓；
3. 改完**两种密度都要看** ✓（`COMPACT` 与 `COMFORT` 的格高不同 ✓，只用一种数值会漏 ✓）。

## 二、其余待办（建议顺序）

| # | 内容 | 落点（已核实） |
|---|---|---|
| 1 | **切分辨率保留播放位置**（现在从头播 ✗） | `switchQuality`（`player_view.cpp:1612` 附近 ✓）：切换前记住 `time-pos` ✓ → 重载后等 `file-loaded` ✓ → `seekAbsolute(记住的位置)` ✓ |
| 2 | **默认 1080P 且只 loadfile 一次** | `fetchQualityLadder`（`hg_client.hpp:293`）→ 选定档 → `fetchPlayPlanForSource(episodeId, tier.sourceIndex, …)`（`:303`）→ **一次** loadfile ✓ |
| 3 | **触屏拖动进度条 / 音量条** | `tickControls` 的 libnx 触摸轮询 ✓：现在只处理**按下沿** ✗ → 加 **STAY**（按住并移动）阶段 ✓ |
| 4 | **续播**（详情页 + 历史页回到上次位置） | 读历史已在用 ✓（`Store::history()` ✓，build125 ✓），差"打开该集并 seek" ✓ |
| 5 | **提示中文还剩 7 处** | `Controls` / `Prev episode` / `Next episode` / `Toggle info` / `Size` / `Clear history` ✓：需在 `hg_lang.h` **加新键** ✓ 并在多语言表**填字** ✓（必须成组做 ✓） |
| 6 | 历史行显示"看到多少秒"（时长未知时） | `detail_activity.cpp` 去掉 `duration > 0` 判断 ✓ |

## 三、这几轮换来的硬教训（务必遵守）

1. **绝不在 `draw()` 里改视图树** ✓✓ —— `build131`（内联）与 `build132`（`brls::sync` 推迟）**都崩** ✓；
   `brls::sync` **仍在当前帧内** ✗ → 重建/增删子视图只能在**帧之外**的安全点做 ✓。
   （本项目此前在"活动 pop"上已踩过一次 ✓，`detail_activity.cpp` 的注释就写着这条 ✓。）
2. **撤补丁前先 `read`，不要凭记忆写接口** ✓ —— 我曾写成 `hg_sidebar_is_watchlist_row()`（**不存在** ✗），
   正确写法是读出来的字段：`items[i].icon == HG_SB_ICON_STAR` ✓。
3. **批量替换必须先确认工作目录** ✓（用绝对路径 ✓）—— 有两次因目录不对而"替换 0 处" ✓。
4. **主机测试会拦住契约破坏** ✓ —— 把历史归并放进 C 加载器时，`hg_history_test` 立刻三条失败 ✓（已改到 C++ 侧 ✓）。
