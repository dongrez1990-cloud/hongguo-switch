# 第 52 轮：帧数上限 / 显示 FPS / 键位交换 / 手柄振动 / 语言跟随系统（build52）

C 组第二批：这五项**全都是 borealis 或 libnx 已有的能力**，本 App 只负责决定"哪个 cfg 键对应哪个调用"
——不发明第二套实现。

---

## 一、五个开关以及它们各自的真实依据

| 待办 | 实现 | 依据 |
|---|---|---|
| **C8 最高帧数限制** | `brls::Application::setLimitedFPS(0/30/60/120)` | `application.hpp:220`，borealis 自己的限帧 |
| **C12 显示 FPS** | `brls::Application::setFPSStatus(bool)` | `application.hpp:212`（**框架自带读数**，不用自己数帧） |
| **C13 键位交换** | `brls::Application::setSwapInputKeys(bool)` | `application.hpp:327` |
| **C15 手柄振动** | `Platform::getInputManager()->sendRumble(0, low, high)` | `input.hpp:336`（Switch 后端 `switch_input.hpp:49`） |
| **C16 语言跟随系统** | `setGetSystemLanguage(&code)` → `SetLanguage_ZHCN/ZHTW` 则中文 | libnx `set.h:929`；`SetLanguage_*` 从 `set.h:40` 起 |

**三个显示/输入开关标着"需要重启"是真的**：borealis 在**第一帧之前**取限帧、在**第一个按键之前**取键位交换，
会话中途改是框架不会兑现的承诺 —— 所以它们走"读取一次"的 cfg 路径并在启动时应用。**振动则立即生效**
（`hg_haptic` 每次调用时读标志）。

**语言"跟随系统"是一个选择，不是一种语言**：`hg_lang_parse` 只报告用户的**选择**（`HG_LANG_AUTO`），
由 `hg_boot` 去问主机并解析 —— 把解析留在能访问 `set:sys` 的那一层，主机测试才能在**没有主机服务**的机器上
覆盖这些规则。解析结果与**原始数字**都记进日志：

```
boot: language='auto' -> the console reports 9 (rc=0x0), so Chinese (this app ships zh/en only)
```

（本 App 只有两种翻译，所以非中文一律落到英文 —— **这是有意的降级，写在日志里**，
否则"为什么是英文"在日志里查不出来。）

## 二、振动：范围划得很窄，并且说清楚

`hg_haptic` 只做一件事：脉冲一次。它被接在**两个真正有意义的时刻**：

1. 设置里打开振动开关本身（让用户**立刻能感觉到**它生效 —— 一个没有任何反馈的振动设置和坏掉的分不出来）；
2. 收藏成功（不用抬头看屏幕就能确认）。

**没有**接到每一次按键上 —— 那是另一个功能、另一个开关，把它假装成这一个就是让设置项变成谎话。
另外：清除脉冲的延时**跑在工作线程上真睡**（`svcSleepThread`），不是 `brls::sync`
（后者在**下一帧**执行，会让任何 `ms` 都只有一帧长 —— 把 150 ms 写成 16 ms 正是这个项目拒绝写下的那种话）。

## 三、验证

| 项 | 结果 |
|---|---|
| 主机测试 | **17 套 / 1174 项 / 0 失败**（`HG_LANG_AUTO` 走 C 层规则，主机测试覆盖解析） |
| 构建 | 零警告（`out/build52.log`） |
| 部署读回 | `/switch/hongguo.nro` **SHA-256 与本地一致**（`7D630D6C5AC4CE1B…`），27,195,416 字节（**第一次读回 FTP 报 426，重试后一致** —— 验证这一步是必须的） |
| 主机产物 | `hongguo.nro` / `hongguo-build52.nro` / `hongguo-build52.nsp`（`build51` 已删除） |

**顺手记下的三个 API 事实**（都是编译期抓到的，写在这里省下一轮）：
`brls::Application` **没有** `getInputManager()`，要走 `getPlatform()->getInputManager()`；
libnx 的语言接口叫 **`setGetSystemLanguage`**（不是 `setsys...`）且收 `u64*`；
`hg_boot.cpp` 里 `Result` 会被解析成**本项目的 `hg::Result`**，必须写 `::Result`。

## 四、请真机确认

1. 设置 → **界面设置**：最后四行是 **Language(auto/zh/en)**、**Frame limit(0/30/60/120)**、
   **Show FPS**、**Swap A/B and X/Y**。改成 **显示 FPS = yes** → 重启后屏幕上出现框架自带 FPS 读数。
2. **其他设置** 第一行 **Vibration** → 改成 yes 时**手柄应震一下**（日志 `HAPTIC: pulse …`）；
   然后去详情页按 Y 收藏，也应震一下。
3. **Language = auto** → 重启，日志有 `language='auto' -> the console reports N`，界面语言随主机设置。
4. 回归：这几项都是"读一次"的设置，重启后生效；不重启时不会有半生效的状态。
