# 第 67 轮：把新开关写进 cfg 说明书（build67 = 只改配置）

`hongguo.cfg` **就是这个项目的说明书**（每一项都写成注释 + 一行解释），而我这批新增的开关
**一个都没写进去** —— 其中 `player.sw.height` 正是做 1080p 卡顿对照实验要用的那个键。
用户不看我的 27 份文档也知道有哪些选项，靠的就是这个文件。

---

## 一、做法：**从源码里把键抽出来**再对比（不靠记忆）

```powershell
# 抓所有 cfgValue/cfgIs/cfgIntOr/cfgDoubleOr("key") 的第一个参数
```

结果：源码里出现 **41 个 cfg 键**，模板里**完全没提到 14 个**：

```
app.fps.limit   app.fps.show    app.hints     app.rumble    app.swap.ab
home.sidebar    player.cache.mb player.decode.fast  player.eq.mode
player.speed    player.sw.height player.volume  player.zoom  theme
```

于是新增一块 **"switches added in builds 40-66"**，按用途分组（界面/输入、主页、播放器、画面菜单），
每一项一行解释 + 默认值 + 是否需要重启；并对**最容易被误解的两项**写了专门说明：

- `player.sw.height`：主机模式下表面是 1920×1080 而画面只占 1280×720，**720 能省掉约 2.25 倍的每帧 CPU 工作**；
- `player.eq.mode`：**为什么画面五值需要滤镜**（mpv 的这五个属性由 GPU 视频链当 shader 参数应用，
  而本 App 走 `MPV_RENDER_API_TYPE_SW` 软件路径，没有那一级）—— 这段注释以后会被下一个人读到，
  所以它写在 cfg 里而不是只写在文档里。

## 二、**没有覆盖用户的设置**（这一步最容易出事，所以做了核对）

SD 上原本生效 6 项：`ao=hos`、`hwdec=no`、`mpv.msglevel=…`、`home.tiers=yes`、`ui.scale=720p`、`overlay.labels=no`。

| 步骤 | 做法 |
|---|---|
| 备份 | 旧文件存到 `/switch/hongguo.cfg.before-build67`（**没有**覆盖已有的 `.bak`） |
| 合并 | 以模板为底，只把**与模板默认不同的**两项（`hwdec=no`、`overlay.labels=no`）追加为生效行 |
| 去重 | 第一版我整段贴了 SD 的 6 行 → **产生重复键**（`ao=hos` 两次、`hwdec` 两个值）✗ → 改成只贴差异，并把模板里的 `hwdec=auto` 那行注释掉 |
| 核对 | 新文件的**生效键集合与 SD 完全一致** ✓、**无重复键** ✓、读回哈希一致 ✓ |

（顺带一个交叉验证：SD 的 `hwdec=no` 与主机日志里的 `CFG: option hwdec = 'no'` **互相印证** ✓。）

## 三、验证

| 项 | 结果 |
|---|---|
| 主机测试 | 17 套 / 1203 项 / 0 失败（**本轮没有改代码**，只改配置） |
| 配置 | 40/41 → **41/41 个键都在说明书里**（剩下的那个是 `image.suffix`，模板里本来就有 ✓） |
| 部署读回 | `/switch/hongguo.cfg` **SHA-256 与本地一致**（`9C0E859A0E1CA233…`），16,002 字节 |
| 备份 | `/switch/hongguo.cfg.before-build67`（12,303 字节，原样） |
| 生效行 | 与改动前**完全相同**（6 项，逐行核对） |

## 四、你可以怎么用它

现在打开 `sdcard:/switch/hongguo.cfg`，顶部会看到 **"switches added in builds 40-66"** 那一块 ——
包括这一批要做卡顿对照实验的三行：

```
player.sw.height=720     # 1080p 卡顿的第一杠杆（默认 0 = 最清晰）
player.cache.mb=128      # 解码缓存
player.decode.fast=yes   # 低画质解码（默认关，会糊）
```

**改完存盘、重启应用即可**（cfg 在启动时读；其中 `app.hints` 与 `theme` 是**即时生效**的例外）。
