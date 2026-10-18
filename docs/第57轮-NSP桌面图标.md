# 第 57 轮：NSP 桌面图标（H1 / H2）（build57 = 纯打包变更）

**代码一行没改**，所以 SD 上的 NRO 仍是 `build56`；这一轮的产物是**带桌面图标的 NSP**。

---

## 一、图标放在哪：**权威依据在仓库里**，不用猜

`third_party/hacbrewpack/README.md:40-48`（我们打包用的就是这份 hacbrewpack）：

> `You should place your icon with "icon_{Language}.dat" file name in control folder, "icon_AmericanEnglish.dat"`
> is the default one… **dat files are just renamed jpg files**
> Your icon file format must be **JPEG with 256x256 dimensions**
> It's highly recommended to **delete unnecessary exif data**… If you see **placeholder** instead of icon after
> installing nsp, It's likely due to exif data

**没有命令行开关**（`hacbrewpack --help` 里没有任何 icon 参数）—— 它把 `control/` 整个当作控制区打包，
**文件名本身就是那面旗**。

## 二、图标是我们自己画的（H2：版权）

`scripts/make-icon.ps1`：256×256，深色圆角底 + 强调色描边 + 播放三角 + 两字"短剧"。
**脚本就是美术的来源**：可复现、可审查，没有任何一部分来自上游服务或别的应用 ——
交接文档 §3.5 明确要求"自己画优先，用上游品牌资产必须先问用户"，所以这一轮**没有碰**任何品牌素材。

脚本结尾**自己验证**产物（这是关键，因为"装完是占位图"正是这类文件出错的表现）：

```
icon: assets\icon-256.jpg
  size      : 11101 bytes, 256 x 256
  JPEG SOI  : True   EOI: True
  Exif block: False   (must be False -- see the note at the top of this script)
```

**过程中踩到一个编码坑**：第一版把中文直接写在 .ps1 里，Windows PowerShell 5.1 无 BOM 时按 **ANSI** 读脚本，
画出来是**乱码**（我核对了图）。改成用**码点** `[char]0x77ED + [char]0x5267` 后正确 —— 这样也不依赖脚本自身的编码。

## 三、打包脚本：`control/icon_AmericanEnglish.dat`

`build-nsp.ps1` 新增一步（含 `-NoIcon` 退路 + 体积小于 2 KB 就报错）：

```
icon: control\icon_AmericanEnglish.dat  (11101 bytes)
```

## 四、怎么验证"图标真的进了 NSP"（以及这个证据有多强）

| 证据 | 强度 |
|---|---|
| hacbrewpack 自己的**逐步日志**：`Writing control\control.nacp …` → `Writing control\icon_AmericanEnglish.dat to …control_sec0_ivfc_lvl6` → `lvl6 → hacbrewpack_nca\control.nca` | **强**：工具的每一步都被它自己打印出来，图标出现在写进控制 NCA 的那条链上 |
| 带图标 / 不带图标两次打包的 **SHA-256 不同**（大小相同，`18085080`，因为 NCA 有填充） | **弱**：我随后发现**同一个输入两次打包的 NSP 哈希也不同**（hacbrewpack 输出不是字节确定的），所以这一条**不能单独作证**——如实记下来 |
| 直接在 NSP 里搜 `JFIF` | **不可行**：NSP 里的 NCA 是加密的（这也是为什么上面的 A/B 只能比哈希） |

**下一步的真机验证最直接**：装 `hongguo-build57.nsp` 后在主菜单看图标是不是那张红三角方块。
如果显示的是通用图标或占位图，就是 JPEG/EXIF 那一环出了问题（脚本里的检查就是为它写的）。

## 五、产物与验证

| 项 | 结果 |
|---|---|
| 构建 | 零代码改动；打包脚本改动（`out/build-nsp-build57.log`） |
| NSP | `out/hongguo-build57.nsp`，18,085,080 字节，带图标 |
| 部署读回 | `/switch/hongguo-build57.nsp` **SHA-256 与本地一致**（`E6AD880FE21A4E74…`） |
| 主机测试 | 17 套 / 1191 项 / 0 失败（本轮未改代码） |

> **H2 的剩余部分**：如果你想要"上游那套视觉"（红果自己的图标/品牌），
> 那需要你**明确决定**（交接文档要求先问），而且**不要**打进分发的 NSP。目前的图标是纯自绘的。
