# 复刻 wiliwili 的界面结构：侧边栏、播放控制、设置项（读取自它的源码）

**这份文档是"照它的做"的规格书**，每条都注明了**读自 wiliwili 的哪个文件**，不是推测。
用户的要求原话：「做的是侧边栏，然后类似于浏览器的状态，可以触屏滑动，加载了很多排，
也可以用手柄。你去看一下它的按键设置，然后按照它的按键和触屏设置，把那些对应的功能实现。」
以及：「左侧主页栏，第二个是收藏栏，第三个是自己的历史记录等等，然后下边是信息栏和设置栏，
播放界面有播放键、音量设置等界面元素，还有推荐其他视频；设置界面也都有考虑到。」

---

## 一、侧边栏：wiliwili 主界面的**确切结构**

读自 `upstream/wiliwili/cmake-build-switch/resources/xml/activity/main.xml`（74 行，整份读完）：

```
AppletFrame  headerHidden=true  footerHidden=false
└── Box axis=column 100%x100%
    ├── AutoTabFrame  id=main/tabFrame  grow=1  sidebarPosition=left
    │                 tabBackgroundColor=@theme/color/grey_1
    │                 tabItemActiveTextColor=@theme/color/bilibili
    │                 disableNavigationDown=true  defaultTab=0
    │   ├── Tab label=""  icon=ico-home.svg    图标态 ico-home-activate.svg     → <HomeTab/>
    │   ├── Tab label=""  icon=ico-dynamic.svg 图标态 ico-dynamic-activate.svg  → <DynamicTab/>
    │   └── Tab label=""  focusDown=main/inbox 图标态 ico-mine-activate.svg     → <MineTab/>
    └── Box width=100 axis=column justifyContent=center alignItems=center
            位置 positionType=absolute positionBottom=20      ← 侧边栏**底部**的两个圆形按钮
        ├── CustomButton id=main/inbox   cornerRadius=50 → SVGImage ico-inbox.svg   48x48
        └── CustomButton id=main/setting cornerRadius=50 → SVGImage ico-setting.svg 48x48
```

**四条可以直接照搬的关键设计**：

1. **侧边栏是"图标 + 空 label"**（`label=""`，`fontSize=18`）—— 文字标签留空，靠 SVG 图标表达。
   这正是截图里看到的窄图标栏。
2. **`sidebarPosition="left"`**，背景 `@theme/color/grey_1`，选中项文字色 `@theme/color/bilibili`
   （粉红），且有 `icon` / `iconActivate` **两套图标**（未选中/选中）。
3. **信息栏与设置栏在侧边栏最下方**，`positionBottom=20` 绝对定位 ——
   不是 Tab，而是两个**独立可聚焦的圆按钮**，且 `MineTab` 用 `focusDown="main/inbox"`
   声明"往下走就到信息栏"，把两个区域连成一条焦点链。
4. **`footerHidden=false`** → 底部提示条由框架**自动生成**。

### 底部提示条是**自动的**，不要手写

`Hints::updateHints()`（`library/borealis/library/lib/views/hint.cpp:196-219`）**沿焦点链
`getParent()` 走**，为拿到的每个 action 生成一个 Hint；`AppletFrame` 在
`applet_frame.cpp:229-237` 装载它。**所以注册了 action 就自动有底部提示，永远不用自己画**
（这正是截图里 `X 切换 / Y 搜索 / B 返回 / A 确定` 的来源）。

> **对我们目前的实现意味着**：我在播放器画面菜单底部手写的那行提示（"B or A closes…"）
> 是在**重新发明 Hint**。接上 `registerAction` 之后应该删掉它，交给框架生成。

### 侧边栏控件：**试过搬 wiliwili 那个类，失败了，原因是实测的**

| 事实 | 依据 |
|---|---|
| wiliwili 的 `AutoTabFrame` **不在**我们 vendored 的 borealis 里 | `library/borealis/.../views/` 下只有 `tab_frame.hpp`，**没有** `auto_tab_frame.hpp` |
| 它在 wiliwili 自己的源码树里 | `upstream/wiliwili/wiliwili/include/view/auto_tab_frame.hpp` + `source/view/auto_tab_frame.cpp`（1062 行） |
| 它的公开 API 正是侧边栏需要的 | `AutoTabBarPosition{TOP,LEFT,RIGHT}`、`AutoTabBarStyle{ACCENT,PLAIN,INLINE,NONE}`、`AutoSidebarItem`（`setLabel`/`setSubtitle`/`setActive`/`setFontSize`/`setHorizontalMode`/`setAttachedViewCreator`）、`AutoSidebarItemGroup` |

**所以先试了把它两个文件搬进构建**（`app/vendor/wiliwili/`，并加进 `build-hongguo.ps1`）。
**失败了，而且原因不是"要改代码"：**

```
auto_tab_frame.cpp:38: fatal error: view/button_refresh.hpp: No such file or directory
  → button_refresh.hpp 又 include "view/svg_image.hpp"
  → svg_image.hpp  include <lunasvg.h>
  → C:\devkitPro\portlibs\switch 里没有 lunasvg.h
```

**`AutoTabFrame` 的图标模型是 SVG，而 `lunasvg` 不在这套 devkitPro 里。**
搬它需要先给工具链加一个第三方库 —— 那是另一件工程，而且和"让界面更像 wiliwili"不是同一件事。

**已回滚**（`app/vendor/` 删除，构建脚本里的那段换成了一条说明为什么不做）。
**侧边栏改为用我们已有的 borealis 原语自己写**：一个 `Box`，里面是可聚焦的 图标+文字 行。
用户要的是**结构和行为**（左侧一列、每段一项、底部信息/设置一组、触屏与手柄都能用），
**这些不需要 SVG 光栅器就能复刻**。

### 我们现在的设置界面用的是 `brls::TabFrame`

`app/hongguo/settings_activity.cpp:771` —— **顶部标签**、不是左侧栏。用户要的是左侧栏，
所以它也要一起换。

---

## 二、用户点名的五个侧边栏项 → 我们的实现状态

用户说：主页 / 收藏 / 历史记录 / 信息 / 设置。

| 侧边栏项 | wiliwili 对应 | 我们的状态 |
|---|---|---|
| **主页** | `HomeTab` | ✅ 已有 `HomeActivity`（内容齐全：4 个分区、36 卡、网格 + 小窗/大窗） |
| **收藏** | `MineTab` → `MineCollection` | ❌ **没有**。而且**上游数据里没有"收藏"概念** —— 这是客户端本地的用户列表 |
| **历史记录** | `MineTab` → `MineHistory` | ❌ **没有**。**但这个是能做的**：本地记录"看过哪部、第几集、看到哪儿"，不需要上游支持 |
| **信息** | `main/inbox` 按钮 → 消息页 | ❌ **没有**，且**上游不提供**任何消息/通知接口（已实测：`reply`/`barrage` 出现 0 次） |
| **设置** | `main/setting` 按钮 → `Intent::openSetting()` | ✅ 已有 `SettingsActivity`（要改成左侧栏） |

**所以五个项里能落地的只有三个半**：主页（有）、历史记录（**可以做，且很有用**）、设置（有）。
**收藏与信息需要先确认数据从哪来** —— 要么是本地列表（收藏可以做：本地点"追剧"），
要么上游没有（信息做不了）。**这两个我不打算猜，等确认。**

---

## 三、播放界面：wiliwili 有什么（截图 3、4 对着看）

### 底部控制条（截图 4 的实际元素，从左到右）

```
00:06/04:38   音量图标   弹幕开关   弹幕设置   高清 720P   倍速   全屏
```

读自 `resources/xml/activity/player_activity.xml` 的对应 id 命名（`player/…`），
以及 `player_setting.xml` 的分组。

### 播放设置面板（截图 2，**完整分组**）

读自 `resources/xml/fragment/player_setting.xml`，逐项 id：

| 分组 | 项 |
|---|---|
| **通用** | 播放方式（`setting/play/strategy`）、定时关闭（`setting/sleep`）、全屏（`setting/fullscreen`）、窗口置顶（`setting/onTopMode`）、**镜像画面**（`setting/video/mirror`）、**视频比例**（`setting/video/aspect`） |
| | 自动跳过片头片尾（`setting/auto/skip`）、播放结束自动退出全屏（`setting/auto/exit`）、下方固定显示进度条（`setting/video/progress`）、下方固定显示高能进度条（`setting/video/highlight`）、上传历史播放记录（`setting/history/log`） |
| **自定义** | 自定义比例（`setting/video/custom/aspect`）、裁剪（`setting/video/custom/clip`）+ 起止时间 |
| **着色器** | Anime4K 等（`setting/shaders/*`） |
| **均衡器** | **亮度 / 对比度 / 饱和度 / 伽马 / 色相 / 重置**（`setting/equalizer/*`） |

**注意最后一行**：wiliwili 把**亮度/对比度/饱和度/伽马/色相**放在**设置面板里的"均衡器"分组**，
而**我们的第 1 步把它做成了播放器内的 X 面板**（`hg_pmenu`）—— 功能是同一批，
**位置不同**。这一点值得跟用户确认要不要挪。

### 推荐其他视频

截图 3 右侧是"评论 / 推荐 / 投稿"三标签 + 视频列表。**我们的上游没有相关推荐接口**
（详情页只有 `vid_list`），所以"推荐其他视频"**只能来自我们已经拉到的数据**：
同一个首页分区里的其他剧、或同一分类。**这是能做的，但要说清数据来源不是"推荐算法"。**

---

## 四、按键与触屏：规格在另一份文档

`wiliwili按键与触屏规格.md` 是逐屏 action 表 + 触屏手势分析，要点：

- **触屏单击 = 聚焦 + 激活一次完成**（`tap_gesture.cpp:26-50`：`TouchPhase::START` 时
  `giveFocus`，`GestureState::END` 时找该视图的 `BUTTON_A` action 并执行）。
  所以在网格上**点一下选集就直接播放**。
- **触屏只能合成 `BUTTON_A`**（`tap_gesture.cpp:30`）—— 触屏点不出 B/X/Y/LB/RB。
  这是**最大的缺口**：用户要的"触屏也能操作"需要在需要的视图上自己加手势。
- **网格触屏滚动是现成的**：`RecyclerFrame` 继承 `ScrollingFrame`，
  拖动/惯性/指示条都在 `scrolling_frame.cpp:42-109`。**我们用的是自己的
  `ScrollingFrame` + 绝对定位，所以拖动滚动应该已经可用** —— **需要真机确认**。
- **没有手持/底座判断**（全树 `isHandheld` 0 命中）；底座下触屏事件自然为空，代码不据此禁用。

逐屏 action 表（读自 wiliwili 源码）：

| 屏幕 | 按键 |
|---|---|
| MainActivity | BACK+START = 设置（都 `hidden=true`）；A = 激活（自动注入） |
| Home / Dynamic / Mine | **LB/RB = 上一个/下一个侧边栏项**，**Y = 搜索**，**X = 刷新**，LT/RT = 子分组 |
| 播放器 | **LB 快退 / RB 快进**，Y 开关 OSD，X 弹幕，**RT 音量**，**START 画质**，BACK 用户页，A 全屏，B 退出，左右方向键拖动进度 |
| SettingActivity | **一个都没注册**（靠 AppletFrame 默认） |
| Gallery | LB/LT/左 = 上一张，RB/RT/右 = 下一张 |

---

## 五、进度：**本地存储层 + 侧边栏模型已完成并测试**

用户已定：**历史记录和收藏都做本地**（收藏即"追剧列表"）；**"信息"不做**
（上游确实没有任何消息接口，已实测 —— 与其放一个永远是空的页面，不如不放）。

| 文件 | 内容 | 测试 |
|---|---|---|
| `app/src/hg_history.h` / `.c` | `HgHistoryEntry`（剧集/标题/集/进度/时间/封面）、`HgFavEntry`；加载/保存/插入/查找/删除；`hg_fmt_clock` / `hg_fmt_age` | **92 项** |
| `app/src/hg_sidebar.h` / `.c` | 侧边栏**模型**：四项（主页/收藏/历史/设置）、**两个分组**、图标枚举、可变副标题、移动/落地规则 | **61 项** |

**基线：16 套 / 1049 项 / 0 失败。**

### 历史与收藏：为什么是"一行一条的文本"

二进制结构体一加字段就崩；为六个字段引 JSON 库不值得；**而且这个文件写在 SD 卡上、
用户只有 FTP 一个别的接口** —— 文本可以手工修。位置参数、尾部字段（`cover`）可选，
所以**旧版本写的文件仍能读**。

**最关键的一条不变式：写入时净化**。字符串里的 TAB/CR/LF 全换成空格，否则
**标题里的换行会把一条记录劈成两条**，后半截被解析成另一部剧 —— 症状是"历史记录显示错的剧"，
而**任何地方都不会报错**。测试专门用含换行的标题验证。

另外两条刻意的：**保存是原子的**（播放时每几秒重写一次，断在半路会留下半条记录、用户无提示丢历史）；
**收藏满了就停手不淘汰旧的**（悄悄忘掉用户明确保存过的东西，比说一句"收藏已满"更糟）。

### 侧边栏模型：为什么先写"没有绘制"的那一半

wiliwili 的侧边栏是 `AutoTabFrame`，**搬不进来**（它 include `svg_image.hpp` → `<lunasvg.h>`，
而这套 devkitPro 没有 lunasvg）。所以自己写，但**结构照它的来**：

- **上面一组**是内容分区（wiliwili 的 HomeTab/DynamicTab/MineTab）；
- **下面一组**是固定功能（wiliwili 的 inbox + settings 两个圆按钮）——
  设置是**功能不是分区**，这正是它被放在底部而不是当成又一个 Tab 的原因；
- 每项带**图标**和**可实时更新的副标题**（wiliwili 的 `setSubtitle`）；
  副标题是让侧边栏"有信息"而不只是装饰的那一半。

**模型先于绘制，是因为会安静出错的是移动逻辑**：焦点**绝不能落到禁用项**上、
两端**必须能环绕**、**必须留在自己的分组内**。这三条都是纯逻辑、都能在主机上测，
而且错了以后在屏幕上的表现是"按键没反应"或"焦点跑到最底下"—— 靠眼睛很难定位。
61 项测试就是查这三条。

**禁用而不是隐藏**：没有历史文件时，历史那一项是**暗的**，不是消失 ——
用户能看到"这个功能存在、现在是空的"，而不是走进一个空页面才知道。

---

## 六、下一步：界面骨架（还没做）

**已经具备的全部条件**：侧边栏模型（已测）、本地存储（已测）、语言表（中/英）、
wiliwili 的逐屏按键表与触屏语义、wiliwili 播放设置面板的完整分组、
以及 wiliwili 侧边栏的确切结构（本文 §一）。

**下一步顺序**（每条单独可验证）：

1. **`hg_sidebar_view`**：用 nanovg 画图标（`HgSidebarIcon` 那几个形状），
   `Box` + 可聚焦行。**先只换设置界面**（它已是数据驱动的分组，换外壳不影响内容），
   **不碰主页**，风险可控。
2. **主页接成侧边栏第一项**，底部固定"设置"圆按钮。
3. **历史记录页**（读 §五 的存储）：每行 封面 / 标题 / 看到第几集 / 进度 / 多久之前。
4. **收藏页** + 详情页的收藏按钮。
5. **播放设置界面按 wiliwili 重做**（用户明确要求，见 §三 的分组表）：
   通用 → 自动 → 着色器 → **均衡器（亮度/对比度/饱和度/伽马/色调/重置，用滑块）**。
   **我们已有的 `hg_pmenu` 就是均衡器那一组**，位置要从"播放器 X 面板"挪进这个设置面板。
6. **播放器底部控制条**：时间 / 音量 / 画质 / 倍速 / 全屏；`registerAction` 接
   LB/RB/Y/X/START，**并删掉我手写的提示行**（框架自动生成 Hint）。
7. 触屏：确认网格拖动滚动可用；需要的视图上自己加 TapGesture。

**为什么没有一次做完**：侧边栏是**结构性重写**（主页从"整个 Activity"变成"侧边栏的一项"、
设置从顶部 Tab 换成左侧栏、均衡器要搬家）。上一轮"更新出崩溃"正是因为在没验证的情况下改结构，
所以这一轮先把**能独立验证的层**做完做透（崩溃修复 + 存储 + 侧边栏模型 + 153 项新测试），
再动界面骨架。



---

## 六、这一轮我实际做了什么（诚实记录）

用户上一轮报的是**崩溃**（`Atmosphere panic / std::abort (0xFFE)`），那是我最该先处理的，
而且**根因从日志里定死了**（`hongguo.nro` 13:54 构建的那次运行）：

```
IMG: installed ...                       9 条
IMG: 10 fetches already in flight ...    402 条
HOMEGRID: cover retry pass 141 ...
```

**九张装成功、四百零二次被拒、并且计数永不回落** —— 我自己加的那个 `image.threads` 上限
（以及为了"不会漏减"而设计的引用计数令牌）**彻底失败**：它把"优雅降级"做成了"功能死掉"，
再用 400 行日志把真问题埋掉。

**已修并已部署**：

1. **拆掉整个预算机制**（计数器 + `ImageSlotHolder` + `ImageSlotToken`），
   换成一个具名函数 `releaseFetch()`，在**六条退出路径**上各调一次 —— 一条能读、一处能查。
2. **`settled` 改成三态**（`PENDING` / `INSTALLED` / `FAILED`）：坏 URL 记 `FAILED` 后**不再重试**，
   否则一个永远 404 的地址会被每 400 ms 重问一次。
3. `image.threads` 仍是可识别的 cfg 键（不报错），但**不再是闸门**。

**基线**：14 套 / 896 项 / 0 失败；`hongguo.nro` 零警告。已部署 26,978,328 字节。
