# wiliwili 按键与触屏规格（Switch 移植参考）

来源：`hongguo-switch/upstream/wiliwili/`（只读）。所有断言附 `文件:行`。
**重要前提**：wiliwili **没有**用 XML 声明按键。`brls:action` 属性在全部 252 个 XML 中**不存在**；
唯一命中的 2 处 `action=` 是 `share_dialog.xml:60,86` 的 `action="clipboard"`（剪贴板，非按键）。
所有 action 均由 C++ `registerAction()` 注册。

## 一、注册 API 与全局/局部语义

| API | 语义 | 位置 |
|---|---|---|
| `Activity::registerAction(label, button, fn, hidden, repeating)` | 注册在 Activity 上；因 hint 沿父链收集，等于**全局**可见 | `borealis/.../views/applet_frame.cpp:196-219` |
| `View::registerAction(...)` | 注册在具体控件上，仅在焦点位于该控件或其子树时生效（如 `recyclingGrid`） | 同上 |
| 第二参数为 `ShortcutHelper::getXxx()` | 该重载**不带按钮图标**：键盘快捷键，非手柄键 | `wiliwili/source/activity/search_activity.cpp:39` |

**Hint/footer 机制（回答"底部按键提示从哪来"）**：不是手写 Label，而是自动生成。
`Hints::updateHints()` 从焦点控件沿 `getParent()` 向上遍历，收集每个 View 的 gamepad action，
用按钮去重、排序（A 优先）后为每个 action 生成一个 `Hint`：
`borealis/.../views/hint.cpp:196-219`（收集）、`:221-238`（生成）、
`applet_frame.cpp:229-237`（`hintBox` 挂载）。图标字形来自 `Hint::getKeyIcon()` `hint.cpp:82-124`
（A=`\uE0E0`、B=`\uE0E1`、X=`\uE0E2`、Y=`\uE0E3`、LB=`\uE0E4`、RB=`\uE0E5`、LT=`\uE0E6`、RT=`\uE0E7`、
START=`\uE0EF`、BACK=`\uE0F0`），文字来自 action 的 label。
**结论**：底部提示条是**自动的**，你注册了 action 就自动出现，不需要写 UI。
`hintsLiteMode` 打开时只显示 A/B（`hint.cpp:209`）。
**硬件按键图标集随 `SettingItem::KEYMAP` 变化**（`AGENTS.md`）；A/B 物理位置交换由
`Application::isSwapInputKeys()` 处理（`video_view.cpp:100`）。

## 二、每屏按键表

| 屏幕 | 按键 | label（UI 原文） | 行为 | 注册位置 file:line |
|---|---|---|---|---|
| **MainActivity**（主页） | BACK | `"Settings"` | 打开设置 | `main_activity.cpp:29-35`（Activity，hidden=true） |
| | START | `"Settings"` | 打开设置 | `main_activity.cpp:37-43`（Activity，hidden=true） |
| | A | `hints/ok`（自动补） | 侧栏/卡片激活 | `hint.cpp:226-229` 自动注入 |
| **HomeTab / 各 TabFragment** | LB | `上一项` | `focus2LastTab()` | `fragment/home_tab.cpp:35` |
| | RB | `下一项` | `focus2NextTab()` | `fragment/home_tab.cpp:43` |
| | Y | `wiliwili/search/tab` | 进搜索页 | `fragment/home_tab.cpp:21` |
| | X | `wiliwili/home/common/refresh` 等 | 刷新/换一批 | `home_recommends.cpp:107`、`home_hots_all.cpp:75`、`home_bangumi.cpp:28`、`home_cinema.cpp:33`、`home_live.cpp:362`、`mine_history.cpp:126`、`mine_later.cpp:61` |
| | LT/RT | `上一项`/`下一项` | 子分组切换 | `home_hots.cpp:20,29`、`home_hots_rank.cpp:93`、`home_bangumi.cpp:36,45`、`home_cinema.cpp:41,50`、`search_order.cpp:40,49` |
| **SearchActivity** | Y | `wiliwili/search/tab` | 打开输入法改关键词 | `search_activity.cpp:34-37`（Activity） |
| | — | — | 点搜索框开输入法 | `search_activity.cpp:44`（`searchBox` 上加 TapGesture） |
| **SearchTab** | LB/RB | `上一项`/`下一项` | 切 Tab | `fragment/search_tab.cpp:19-45` |
| **PlayerActivity / PlayerSeasonActivity** | LB | `\uE08F`（快退字形） | 快退，`repeating=true` | `view/video_view.cpp:91` |
| | RB | `\uE08E`（快进字形） | 快进；若同时按 Y 则反向 | `view/video_view.cpp:94-109` |
| | Y | `toggleOSD` | 显隐 OSD（hidden=true） | `view/video_view.cpp:118` |
| | X | `toggleDanmaku` | 弹幕开关（hidden=true）；OSD 隐藏时切上一 P | `view/video_view.cpp:121-130` |
| | RT | `volume` | 配合方向上/下加/减音量 | `view/video_view.cpp:132-149` |
| | START | `wiliwili/player/quality` | 清晰度菜单 | `view/video_view.cpp:308`、`player_base_activity.cpp:270` |
| | BACK | `profile` | 视频信息（hidden=true） | `view/video_view.cpp:316` |
| | A | `wiliwili/player/fs` | 全屏 | `view/video_view.cpp:466` |
| | B | `cancel` | 退出全屏/关闭 | `view/video_view.cpp:74-80, 446-447` |
| | LEFT/RIGHT | `""` | 拖进度 | `view/video_view.cpp:495-496` |
| | A（标题区） | `wiliwili/player/intro` | 展开简介 | `video_activity.cpp`→`player_activity.cpp:150`、`player_season_activity.cpp:98` |
| | LB/RB/LT/RT | `上一项`/`下一项` | 右侧评论 Tab 切换 | `player_base_activity.cpp:225-253`（Activity） |
| | X（评论网格） | `wiliwili/home/common/switch` | 评论排序切换 | `player_base_activity.cpp:212-216`（在 `recyclingGrid` 上） |
| | LB/LT/RB/RT（弹幕设置） | `hints/cancel` 等 | `player_danmaku_setting.cpp:22`、`player_setting.cpp:35` | |
| | B（评论区） | `hints/cancel` | 返回 | `fragment/player_single_comment.cpp:281` |
| **LivePlayerActivity** | START | `wiliwili/player/quality` | 清晰度 | `live_player_activity.cpp:256` |
| | START（弹幕设置下拉） | `""` | 清晰度 | `live_player_activity.cpp:143-144` |
| **PGC Index（筛选页）** | B | `hints/back` | 关闭筛选 | `pgc_index_activity.cpp:184`（`tabFrame` 上） |
| | LB / RB | `上一项`/`下一项` | 切筛选 Tab | `pgc_index_activity.cpp:199-219`（Activity） |
| | X | `wiliwili/home/common/filter` | 筛选 | `pgc_index_activity.cpp:385` |
| | A（`tabFrame`） | `hints/ok` | 应用筛选并关闭 | `pgc_index_activity.cpp:188-197` |
| **MineTab** | X | `wiliwili/mine/login/refresh` | 刷新 | `fragment/mine_tab.cpp:90` |
| | LB/RB | `上一项`/`下一项` | 切 Tab | `fragment/mine_tab.cpp:99,108` |
| | A（两个 box） | `hints/ok` | 跳用户空间 | `fragment/mine_tab.cpp:120,144`（View 级） |
| **DynamicTab / Inbox** | LB/RB (+LT/RT) | `上一项`/`下一项` | 切 Tab | `dynamic_tab.cpp:285-308`、`inbox_view.cpp:106-142` |
| | X | `wiliwili/activity/refresh`、`wiliwili/home/common/refresh` | 刷新 | `dynamic_tab.cpp:350,357,367`、`inbox_view.cpp:154-158` |
| | B | `hints/cancel` | 返回 | `inbox_chat.cpp:127`、`inbox_view.cpp:90`、`player_dlna_search.cpp:96` |
| | Y（聊天） | `""`（hidden） | 切换发送 | `inbox_chat.cpp:164` |
| **SettingActivity** | — | — | 无自定义键；`AppletFrame` 自动提供 B 返回等 | `setting_activity.cpp`（无 `registerAction`） |
| | | | | `fragment/settings_hot_keys.xml` 仅展示快捷键 |
| **GalleryActivity** | LB/LT/NAV_LEFT | `"Prev"` | 上一张 | `view/gallery_view.cpp:15,20,27`（View 级） |
| | RB/RT/NAV_RIGHT | `"Next"` | 下一张 | `view/gallery_view.cpp:33,38,45` |
| **DLNAActivity** | B | `cancel` | OSD 锁定时解锁，否则退出 | `dlna_activity.cpp:182-192` |

`ShortcutHelper::getXxx()` 重载（键盘快捷键，**无手柄图标**）另见
`video_view.cpp:1673-1743`（暂停/音量/快进快退/倍速/设置/播放列表/视频信息），
这些在 Switch 上通常不显示在 hint 条。

## 三、触屏与手势

### 3.1 手势类型（全部 file:line）

| 手势 | 实现类 | 位置 |
|---|---|---|
| 单击 tap | `TapGestureRecognizer` | `borealis/.../core/touch/tap_gesture.cpp:22-145` |
| 垂直拖拽滚动 | `ScrollGestureRecognizer`（`PanAxis::VERTICAL`） | `borealis/.../core/touch/scroll_gesture.cpp`、`views/scrolling_frame.cpp:42-78` |
| 水平拖拽 | `PanGestureRecognizer` | `borealis/.../core/touch/pan_gesture.cpp` |
| 长按 / 双击 / 左右分区垂直滑 | `OsdGestureRecognizer`（**wiliwili 自研**） | `wiliwili/include/utils/gesture_helper.hpp:41`、`wiliwili/source/utils/gesture_helper.cpp:19-199` |
| pinch 捏合 | **not found** | — |

### 3.2 各视图实现的触摸

| 视图 | 手势 | 位置 |
|---|---|---|
| `Button` | `TapGestureRecognizer(this)` | `views/button.cpp:88` |
| `RecyclerCell` | `TapGestureRecognizer(this)` | `views/recycler.cpp:43` |
| `ScrollingFrame` | 垂直 Scroll + tap 停止惯性 | `views/scrolling_frame.cpp:42-84` |
| `HScrollingFrame` | tap | `views/h_scrolling_frame.cpp:81` |
| `Sidebar` | `TapGestureRecognizer` | `views/sidebar.cpp:76` |
| `Hint`（提示条本身可点） | `TapGestureRecognizer`，**但 A 键 hint 默认被排除** | `views/hint.cpp:66-72`（`allowAButtonTouch` 才允许） |
| `GalleryView` | 上一张/下一张按钮 | `wiliwili/source/view/gallery_view.cpp:15-46` |
| `VideoView` OSD | `OsdGestureRecognizer` | `wiliwili/source/view/video_view.cpp:192-280` |
| 搜索框 | Tap | `search_activity.cpp:44` |
| 播放页 QR/赞同/投币/收藏/用户信息 | 每个父 Box 手动加 Tap | `player_base_activity.cpp:276-285` |
| PGC 筛选 `tabFrame` | Tap 应用筛选 | `pgc_index_activity.cpp:197` |

**播放器 OSD 手势明细**（`video_view.cpp:192-280` + `gesture_helper.cpp:69-170`）：
单击=显隐 OSD（`:194`）；双击=播放/暂停（`:197`）；长按=2 倍速，松开还原（`:204-220`，阈值 `LONG_TIME_US`）；
水平拖=快进快退，最大 120s（`:227-243`）；左半屏垂直拖=亮度（`:244-262`，需
`canSetBacklightBrightness()`）；右半屏垂直拖=音量（`:252-266`）。滑动识别阈值 `MAX_DELTA_MOVEMENT`
（`gesture_helper.cpp:95,98`）。

### 3.3 网格/列表的触摸滚动

**是复用的通用组件，wiliwili 侧无需额外代码。**
`RecyclerFrame` 继承 `ScrollingFrame`（`views/recycler.cpp:174-204`），并重写 `onLayout()`/`draw()`
把滚动传给 `ScrollingFrame::onLayout/draw`（`recycler.cpp:530-545`）。
滚动逻辑本体在 `scrolling_frame.cpp:42-78`（垂直 pan + 惯性 `animateScrolling`）。
惯性：End 阶段取 `state.acceleration`，`time < 100ms` 则不动（`scrolling_frame.cpp:67-75`）。
tap 时 `contentOffsetY.stop()` 立即刹停（`scrolling_frame.cpp:81-84`）。
滚动条由 `setupScrollingIndicator()` 自动绘制（`scrolling_frame.cpp:101-109`）。
结论：**只要用 `RecyclerFrame`/`ScrollingFrame`，触摸滚动/惯性/滚动条全部免费**。

### 3.4 ★ 点击是"选中并激活"还是"仅移动焦点"？

**答：一次 tap 同时做两件事——先 `giveFocus`，再执行该 View 的 `BUTTON_A` action。**
即"选中并激活"，不是仅聚焦。证据：

1. `TapGestureRecognizer` 的构造（`tap_gesture.cpp:22-56`）：
   任何非 FAILED 状态先 `Application::giveFocus(view)`（`:26-27`）；
   随后只在 `GestureState::END` 时查找该 View 的 **`BUTTON_A`** action 并调用其 listener（`:28-50`）。
2. 判定起点是**按下**而非抬起：`TouchPhase::START` 立即 `giveFocus` 并 fire
   （`tap_gesture.cpp:120-124`）；`END` fire `GestureState::END`（`:134-137`）。
   中途滑出 View 边界则置 `FAILED`，不触发（`:125-132`）。
3. `RecyclerCell` 的 A action 直接调 `didSelectRowAt()`（`recycler.cpp:31-36`）——这是激活/进入，
   不是聚焦。
4. 对话框类 `Hint` 是例外：A 键 hint 默认**不可点**，需 `allowAButtonTouch="true"`
   （`hint.cpp:66-72`、`:144-147`），避免一个 tap 既确认又穿透。

**对红果短剧的直接影响**：在 wiliwili 模型下，网格上轻点一集 = **立即开始播放**。
若你想要"点一下只聚焦、再点一下才播放"，wiliwili 里没有这种模式，必须自己改
`TapGestureRecognizer` 或改成两次判定——不要指望沿用即可。反之，wiliwili 的 `contentOffsetY.stop()`
（`scrolling_frame.cpp:81-84`）说明滚动中的 tap 只用来刹车，但 A action 仍会执行。

### 3.5 Switch 特有触屏注意事项

- 触屏来源是裸 HID，**没有** handheld/docked 判断、没有 `isHandheld()` 门控：
  `switch_input.cpp:273-290`（`hidGetTouchScreenStates`，坐标除以 `Application::windowScale`）。
  全仓 grep `isHandheld` / `handheld` → **not found**。
- 因此在**底座模式**触屏事件自然为空（硬件无触摸屏），代码不做特殊处理——
  不要依赖"检测到 docked 自动关触摸"，应自己处理"没有触摸事件"。
- 触屏与手柄共用一套 action：触摸只伪造 `BUTTON_A`（`tap_gesture.cpp:30`），
  所以**触摸无法触发 B/X/Y/LB/RB**，除非像 `search_activity.cpp:44` 那样单独加手势。
- 输入类型切换用 `Application::setInputType(InputType::TOUCH)`（`application.cpp:324,352`），
  多处 UI 据此改变绘饰（`recycler.cpp:39,69`、`view.cpp:188,258,535`、`scrolling_frame.cpp:153`）。
- 命中测试从 activity 栈顶 `hitTest(position)` 开始（`application.cpp:327-332`）。
- Switch 摇杆无 ZR 轴：`switch_input.cpp:265`。

## 四、wiliwili 没有处理的（缺口）

- **无 pinch/缩放手势**：not found。
- **无 XML 声明按键**：`brls:action` not found；按键只能在 C++ 注册。
- **无触屏专用的上下文菜单/长按菜单**（长按仅播放器加速用）：not found。
- **无手势冲突仲裁层**：`OsdGestureRecognizer` 是播放器专用的整体识别器
  （`gesture_helper.cpp:42-190`），非通用可复用组件；通用层只有 tap/pan/scroll 三种。
- **无"单击仅聚焦、双击激活"模式**：网格一律单击即激活（见 3.4）。
- `SettingActivity` 自身**未注册**任何按键（`setting_activity.cpp` 无 `registerAction`），
  仅靠 `AppletFrame` 默认行为；真实按键列表在 `resources/xml/fragment/settings_hot_keys.xml`（纯展示）。
- B（返回）的默认行为未在本报告逐个确认到定义处（返回类 action 多次被显式覆盖、
  显式 `dismiss()`），如需精确默认值需另查 `Activity::dismiss` 路径。
