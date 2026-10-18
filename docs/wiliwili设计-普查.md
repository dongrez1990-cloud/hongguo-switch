# wiliwili 设置与设计：代码普查

用户要求："包括还有其他的设置和设计，你在代码里面都看一下"。
这份文件是把 wiliwili 代码里**所有**设置面与可复用设计**逐个列全**，并标注对红果是否适用。
判定"不适用"的每一条都给**实测理由**（这条规矩来自用户自己："有不符合实际情况的地方，自适应"）。

---

## 一、设置面：wiliwili 有四处，不是一个

| # | 设置面 | 出处 | 我们的状态 |
|---|---|---|---|
| 1 | **应用设置界面**（标签页 + 分组行） | `activity/setting_activity.cpp`（36 KB）+ `resources/xml/activity/setting_activity.xml` | **✅ 已复刻骨架**（4 标签 + SelectorCell + About 页），项待补 |
| 2 | **播放器内设置菜单** | `fragment/player_setting.cpp`（25 KB） | **✅ 画面部分已完成**（第 13 轮，未上机）：亮度/对比度/饱和度/伽马/色相 + 画面比例 + 镜像 + 重置，X 开合。见 `播放器画面菜单-步骤1.md`。**未做**：字幕组、shader 组（本服务无独立字幕轨，本构建不带 shader 文件） |
| 3 | **网络诊断页** | `fragment/setting_network.cpp` | **✅ 已复刻并加强**（第 13 轮）：WiFi/IP/DNS + 端到端探针 + 时钟偏差。**不用**它的 `get_unix_time`（那是 B 站接口），改用服务器自己的 `Date` 头。见 `刷新与网络诊断-步骤4.md` |
| 4 | **快捷键映射** | `utils/shortcut_helper.hpp` | **不适用**：桌面键盘专属（Switch 上按键是固定的） |

### 1.2 播放器内设置菜单的内容（这是第 2 处设置面）

| 分组 | 项 | 红果是否适用 | 对应实现 |
|---|---|---|---|
| **画面比例** | 自动 / 裁剪 / 拉伸 | ✅ | mpv `video-aspect` / `panscan` |
| **画面调节** | **亮度 / 对比度 / 伽马 / 色相 / 饱和度** | ✅ | mpv 同名属性（各 −100..100） |
| 画面 | 镜像 | ✅ | mpv `vf=mirror` |
| 画面 | 高亮 | 部分 | 需要 shader |
| 番剧 | 跳过片头/片尾裁剪点、时间单位 | ❌ | B 站番剧数据，红果没有 |
| 其他 | 百科（wiki） | ❌ | B 站词条 |

**这五个画面调节项值得一提**：它们是 mpv 的原生属性，接起来几乎是白送的，
而且是"看短剧"最常用的调整（暗场景亮度、色彩）。**列进下一步第一优先。**
→ **✅ 第 13 轮已完成**（连同画面比例与镜像），见 `播放器画面菜单-步骤1.md`。

---

## 二、自定义视图：22 个设计资产

出处 `include/view/`，按大小排序，逐个判定：

| 文件 | 用途 | 判定 |
|---|---|---|
| `mpv_core.hpp` (17 KB) | 播放核心 | ✅ 已逐行对照（软件渲染、seek、hold-seek 曲线都是从这里抄的） |
| `video_view.hpp` (13.8 KB) | 播放器视图：OSD / 手势 / 进度条 / 快捷键 | ✅ **部分已抄**：hold-seek 曲线、OSD 开合。手势与缩略图预览未做 |
| `video_card.hpp` (12.7 KB) | **视频卡片**：缩略图 + 时长角标 + 播放量 + 两行标题 + 作者/日期 | **✅ 已抄其设计**（第 13 轮）：海报 + 集数角标 + 两行标题；**小窗密度**下再补一行 `tags`。见 `首页网格-步骤2.md` 与 `首页小窗大窗切换.md` |
| `danmaku_core.hpp` (8.2 KB) | 弹幕渲染 | ❌ 无数据（见第四节） |
| `recycling_grid.hpp` (8 KB) | 网格 | **✅ 布局模型已抄，滚动/焦点/回收未抄**（第 13 轮）：borealis 的 `RecyclerFrame` 确实没有网格能力，但 `ScrollingFrame` + `Box::getNextFocus` 已经覆盖了滚动与导航，回收对 36 张卡的页面是负收益。逐项理由见 `首页网格-步骤2.md` §二 |
| `auto_tab_frame.hpp` (7.5 KB) | 自动隐藏标签栏的 TabFrame | ⭕ 可选：观感更接近 wiliwili，但 borealis 的 `TabFrame` 够用 |
| `grid_dropdown.hpp` (4.3 KB) | 网格内的下拉筛选 | ⭕ 取决于红果有没有筛选接口 |
| `text_box.hpp` (3.8 KB) | 长文本（简介） | **✅ 部分已抄**（第 13 轮）：详情页简介用一个可换行的 `Label` 就够——本服务的简介只有一小段，而且**经常根本没有**。逐条回退规则见 `详情页-步骤3.md` |
| `video_progress_slider.hpp` (2.6 KB) | 可拖动进度条 + 缩略图预览 | **✅ 拖动模型已抄，缩略图未做**（第 13 轮）：`progressEvent`/`progressSetEvent` 的形状搬过来了（拖动设目标、松手提交），**画面缩略图没搬**——它的画面来自服务端精灵图，本服务不提供。见 `可拖动进度条-步骤5.md` |
| `video_snapshot_core.hpp` (2.3 KB) | 截图预览（拖进度条时显示画面） | ❌ **不适用**：它消费的是服务端的 `VideoSnapshotData` 精灵图描述，本服务的视频模型只有 URL 与 CENC 密钥 |
| `button_refresh.hpp` (2.4 KB) | 刷新按钮 | **✅ 已抄其意图**（第 13 轮）：用 `brls::Button` 而不是搬它的旋转箭头美术——需要的是**可聚焦**且说明用途。见 `刷新与网络诊断-步骤4.md` |
| `video_snapshot_core.hpp` (2.3 KB) | 截图预览（拖进度条时显示画面） | ⭕ 好看但成本高 |
| `video_comment.hpp` / `player_single_comment.hpp` | 评论 | ❌ 无数据 |
| `subtitle_core.hpp` (2 KB) | 外挂字幕 | ❌ **不需要**：红果的视频**自带烧入字幕**（真机截图里能看到"你从大角仙域而来"），没有独立字幕轨 |
| `video_profile.hpp` (1.7 KB) | 清晰度/编码选择菜单 | **✅ 已复刻为面板一行**（第 13 轮）：X 面板的 Quality 行。它的 `update()` 展示的是**运行时解码状态**（fps、丢帧、音视频同步），本 App 的对应物是 mpv 日志与 Network 页。档位顺序取自真实响应而非它的枚举。见 `清晰度菜单-步骤6.md` |
| `gallery_view.hpp` (1.7 KB) | 图片浏览 | ⭕ 可选 |
| `user_info.hpp` / `animation_image.hpp` / `qr_image.hpp` | 用户信息 / 头像 / 扫码登录 | ❌ 没有账号体系 |
| `live_core.hpp` / `live_danmaku_item.hpp` | 直播 | ❌ 没有直播 |
| `inbox_msg_card.hpp` | 私信 | ❌ 没有 |

## 三、片段（子页面）：50 个，按类归

出处 `include/fragment/`：

| 类别 | 代表 | 判定 |
|---|---|---|
| 首页各标签 | `home_recommends` / `home_hots_*` / `home_bangumi` / `home_cinema` / `home_live` | ✅ **结构参考**（我们的 4 个分区就是它的标签页）；具体页不适用 |
| 搜索 | `search_tab` (6.5 KB) / `search_video` / `search_history` / `search_hots` / `search_order` | ❌ **接口已死**（见第五节），界面无从接起 |
| 我的 | `mine_history` / `mine_collection` / `mine_later` / `mine_bangumi` / `mine_qr_login` | ❌ 没有账号；**"历史记录"要看红果有没有对应接口**（未查） |
| 播放器 | `player_setting` / `player_danmaku_setting` / `player_fragments` / `player_coin` / `player_evaluate` / `player_collection` / `player_dlna_search` | ✅ `player_setting`（见 1.2）；其余是 B 站投币/评分/收藏/投屏 |
| 动态/私信 | `dynamic_tab` / `inbox_*` | ❌ 没有 |
| 其他 | `setting_network`（网络诊断）、`test_rumble`（震动测试）、`share_dialog`（分享） | ✅ 网络诊断该做；震动测试可做；分享不适用 |

## 四、明确不做的，以及证据

| 不做 | 证据 |
|---|---|
| **弹幕**（设置 15 项 + `danmaku_core` + `player_danmaku_setting`） | 首页与详情载荷里 `danmaku`/`comment`/`reply`/`barrage`/`弹幕`/`评论` 出现次数**全为 0** |
| **评论**（`video_comment`、`player_single_comment`） | 同上 |
| **直播**（`live_core`、`live_danmaku_item`、`home_live`） | 服务没有直播 |
| **字幕轨**（`subtitle_core`） | 视频自带烧入字幕，没有独立字幕轨 |
| **账号/收藏/历史/投币/评分**（`mine_*`、`player_coin`、`player_evaluate`） | 没有账号体系 |
| **桌面专属**（窗口置顶、`KEYMAP` 图标、20 项 `SHORTCUT_*`、自更新、主题仓库、DLNA、自定义资源） | Switch 上没有对应概念 |

## 五、搜索：曾经想接，但接口是死的（两次实测）

- 第 1 轮归档：**"签名搜索接口已死（200 + `Content-Length: 0`，用参考实现本机实跑对照确认）"**。
- 第 12 轮复测：用 `app/src/tests/dump_query.c` 构建**真实签名请求**（关键词"短剧"），直连：

```
HTTP 200   content-type: application/json   content-length: 0   body length: 0
```

失败特征与第 1 轮完全一致。（诚实标注：这次用的 `ts` 是 dump 工具里的固定值，
更严谨的复测需要新鲜时间戳；但两条独立证据指向同一结论。）

**所以搜索界面不做**，尽管 `hg_api_parse_search` / `hg_request_search` 的解析与签名都已经写好并测试过
——缺的是能回答的服务器。

---

## 六、下一步的优先顺序（按"对用户的价值 ÷ 成本"排）

1. ~~**播放器内设置菜单**（亮度/对比度/伽马/色相/饱和度/画面比例/镜像）~~ **✅ 第 13 轮完成，未上机**
   —— 全是 mpv 原生属性，**成本最低、用户价值高**。
2. ~~**网格排版**~~ **✅ 第 13 轮完成，未上机**（`首页网格-步骤2.md`）。
   剩下的**小窗/大窗切换（X）**未做，理由见 `交接.md` §二 第 2 步末尾。
3. ~~**详情页补全**：`text_box` 式简介、集数选择网格~~ **✅ 第 13 轮完成，未上机**
   （`详情页-步骤3.md`）：简介有回退规则（`desc` 缺失用 `tags`，两者都空则隐藏），
   选集改成编号按钮网格（720p 下 8 列 × 10 行）。
4. ~~**刷新按钮** + 网络诊断页~~ **✅ 第 13 轮完成，未上机**（`刷新与网络诊断-步骤4.md`）：
   首页 Refresh 按钮 + 设置新增 Network 标签页（逐项测量 + 时钟偏差）。
5. ~~**可拖动进度条**~~ **✅ 第 13 轮完成，未上机**（`可拖动进度条-步骤5.md`）：
   拖动模型搬自 wiliwili（拖动设目标、松手提交一次绝对 seek）；
   **缩略图画面未做**，因为它的画面来自服务端精灵图。
6. ~~**清晰度菜单**~~ **✅ 第 13 轮完成，未上机**（`清晰度菜单-步骤6.md`）：
   X 面板的 Quality 行，五档实测（`quality_type` 40/30/20/10/1 = 360p…1080p，
   **上游最差在前**，故按像素高度排序）。

> **§六 的六项至此全部完成（均未上机）。** 仍待做的大项见 `交接.md` §二 中
> "§二 之后还剩这些" 一节：首页小窗/大窗切换、缩略图画面预览、设置界面补齐剩余项、
> `hwdec=auto` 验证、deko3d 直接渲染。
3. **详情页补全**：`text_box` 式简介、集数选择网格（现在是纯列表）。
4. **刷新按钮**（`button_refresh`）+ 网络诊断页（`setting_network`）。
5. **可拖动进度条 + 缩略图预览**（`video_progress_slider` + `video_snapshot_core`）——好看，成本高。
6. **清晰度菜单**（`video_profile`）。
