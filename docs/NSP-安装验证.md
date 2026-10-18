# NSP 安装：可验证的部分、未确定的部分、以及确切配方

这一份只写**在这台机器上能证到什么程度**。凡是没有真机确认的，一律标注。

最后更新：本轮结束。

---

## 一、结论先行

| 问题 | 状态 |
|---|---|
| 真正的 App NSP 存在吗？ | **有**：`out\hongguo.nsp`，**17,764,568 字节**（第 8 轮用当时的 ELF 重打；title id `0101000000000000`） |
| 容器结构对吗？ | **已验证**：PFS0、3 个 NCA、偏移/大小/边界全部自洽（两个独立工具给出相同数字） |
| 三个 NCA 是不是合法 NCA？ | **未确定** —— 见第三节，扩充搜索空间后仍然证不出来，所以不说"应该没问题" |
| 网络安装链路能跑吗？ | **主机侧已验证**：HTTP 列表、Range 下载、返回的字节就是 NSP。**但 PC 防火墙 Private 配置为 `BlockInbound` 且 8083 没有放行规则**（见 4.3） |
| 真机装得上吗？ | **未验证**——两条路线都已备好，只差在控制台上按一次安装 |

> **第 8 轮的重打是必要的**：原来那份 NSP 是第 1 轮打的，之后 App 改了太多（软件渲染让画面能显示、
> 封面加载器、分级首页），装旧的那份等于装一个没有这些修复的版本。

---

## 二、已验证的部分（有实际输出）

### 2.1 产物的构建

```powershell
powershell -File scripts\build-nsp.ps1 `
  -Nro out\hongguo.nro -OutNsp out\hongguo.nsp -Elf out\hongguo.elf `
  -TitleId 0101000000000000 -TitleName hongguo -TitleAuthor DSH -TitleVersion 1.0.0
```

**`-Elf` 不是可选的**：`build-nsp.ps1` 需要 `exefs/main` 是一个 NSO，而 `elf2nro` 产出的是
**裸 NRO**（`NRO0` 之后直接是段数据，没有 PFS0 容器、也没有任何 NSO0 块）。
`build-hongguo.ps1` 会把 ELF 留在 `out\` 旁边，直接用它即可。

打包器自己的输出（节选）：

```
SDK Version: 0.12.17.0
Section Crypto Type: Regular Crypto
Keygeneration: 1
Program NCA RomFS Section: No        <- 与 --noromfs 一致
Program NCA Logo Section: No         <- 与 --nologo 一致
Created NSP: hacbrewpack_nsp\0101000000000000.nsp
```

### 2.2 容器结构（`scripts\verify-nsp.ps1` 的真实输出）

```
magic           : 'PFS0'
entry count     : 3
string table sz : 128
data offset     : 216
  43775d870c2aa3ec982c4dd5e6433a8c.nca            0   117760   True
  a6ca87f2f5edec540c1e24c41e6daf3a.nca       117760 17626112   True
  c756eeae02d23cf20cf839c6a9477ea3.cnmt.nca 17743872     4096   True
sum of entry sizes : 17747968   payload region : 17747968
OK  valid PFS0/NSP: 3 entries, 3 .nca (cnmt present: True), magic PFS0
```

三个 NCA 的大小符合预期：**control 117,760 / program 17,626,112 / cnmt 4,096**。

### 2.3 网络安装链路（主机侧实测）

```powershell
# 1) 把 out\ 用 HTTP 列出来（注意端口：8082 在本机已被别的服务占用）
node <host-tools-dir>\tools\nsp-server.mjs --root <项目>\out --port 8083
```

实测结果：

```
GET /            -> 200，列表里含  hongguo.nsp   16.9M
GET /hongguo.nsp  (Range: bytes=0-63)
                 -> 206  Content-Range: bytes 0-63/17748184
                    取回的头 4 字节 = "PFS0"
```

**Range 支持是必要的**：DBI 装大文件时会分片续传。已确认可用。

> **端口冲突这条要记住**：`8082` 在本机**已经被另一个服务占着**（PID 1312），
> 它对任何请求都回 `400 unkown the request`——注意这个字符串**不是** `nsp-server.mjs` 的
> （它写的是 `bad request`），所以那是另一个项目的服务。
> 一开始我用 `Invoke-WebRequest` 测，拿到 400 就以为是 nsp-server 的问题；
> 换成 Node 直接发原始 HTTP 请求才看清是**别人的服务在应答**。
> 换到 8083 后一切正常。

---

## 三、未确定：三个 NCA 的身份（**我证不出来，所以不说"没问题"**）

`verify-nsp.ps1` 只证明**容器**合法。它不能证明那三个条目是合法 NCA，而这里有一个具体现象：

* 在**整份 636 KB / 17.7 MB 文件里搜不到一个明文 `NCA` 字符串**；
* 三个条目的**开头 256 字节完全相同**（`44 99 2f c0 05 09 0c 46 …`）。

完全相同的前缀有两种解释，而且都会给同一现象：
(a) 签名区是全零占位、而 NCA 头用 `header_key` 做了 AES-XTS 加密——同样密钥、同样明文、
同样扇区号，密文自然相同；
(b) 打包器写了固定的占位块。

`hacBrewPack` 自己的说明支持 (a)：它**必须**有 `header_key`，而且**默认会给 NCA 头签名**
（`--nosignncasig2` 才是跳过）。

**但我要诚实说明**：我写了 `scripts\nca-identify.mjs` 去解密验证。**第 8 轮把搜索空间扩到了**：四个候选密钥（`header_key` + `key_area_key_application_00/01/02`，全部存在）× 四个加密起点（`0x000`/`0x100`/`0x200`/`0x400`）× 16 个 XTS tweak —— **没有一种组合能还原出 `NCA` 魔数**。所以：

* 我**没有**证出三个 NCA 的内容类型（Program / Control / Meta）；
* 我**没有**证出签名区的真实性质；
* 因此我**不能**说"这个 NSP 结构完全正确、装上去就行"。

**能确定的**：容器合法，且这三个条目**不是明文 NCA 头**（整份文件搜不到 `NCA`，且三条开头的 256 字节完全相同——同密钥同明文同 tweak 出同样密文，这正是全零签名块被加密的样子）。

**还没做的那半条路**：找一个**已知能安装的 NSP 做对照**。SD 卡上没有任何 `.nsp`（已逐个目录查过），所以对照样本目前没有来源。若拿到一份，用同一个脚本跑一遍即可判定"没有明文魔数"是常态还是异常——**这是控制实验，不是猜测**。

---

## 三、第一次装上去之后发现的真问题：**NSP 里没有 romfs**

**第 8 轮实测**：NSP 装上、能启动（这一条顺带回答了容器/NCA/sigpatch 三个问题——**都能过**），
但**灰屏一下就直接退出**。而同一个可执行文件从 NRO 启动一切正常。

**根因**：`build-nsp.ps1` 原来无条件传 **`--noromfs`**，于是装出来的标题**一个着色器都没有**。
borealis 是**按文件系统路径**加载 deko3d 着色器的：

```
dk_renderer.cpp  m_vertex_shader.load(m_code_mem_pool, "romfs:/shaders/fill_vsh.dksh");
```

**没有 romfs 就没有着色器，就一笔都画不出来——而且不报错。** 这个坑 `build-hongguo.ps1:200-220`
早就为 NRO 记过一遍（连注释都写了），NSP 那条路只是从来没做同样的事。

**修法**：`build-nsp.ps1` 增加 `-RomfsDir`；**没传就自己从
`upstream\wiliwili\resources\shaders` 取那三个 `.dksh` 自己搭**——把"造出没有 romfs 的 NSP"
变成不可能，而不是靠调用方记得传。

**打包器自己的读数（可作为验证）**：

```
romfs: staged 3 shader(s) from ...\upstream\wiliwili\resources\shaders
===> Building RomFS
Writing romfs\shaders\fill_aa_fsh.dksh ...
Writing romfs\shaders\fill_fsh.dksh ...
Writing romfs\shaders\fill_vsh.dksh ...
Program NCA RomFS Section: Yes        <-- 修改前是 No
```

产物从 17,764,568 增到 **17,862,872 字节**（+98 KB 即 romfs 与三个着色器）。

> **这一条比第三节那个"未确定"重要得多**：NCA 身份读不出来只是我的工具不够，而**装不上/装上了画不出**
> 是真实发生过的故障，现在有了根因和可验证的读数。

---

## 四、真机安装配方（第 8 轮已把两条路都备好，只差在控制台上按一次）

### 4.1 路线 B（推荐）：从 SD 卡装 —— 不需要任何防火墙改动

`out\hongguo.nsp` 已经拷到 **`/switch/hongguo.nsp`**（17,764,568 字节）。

控制台侧：**DBI → 从 SD 卡安装（Browse SD / install from file）→ `/switch/hongguo.nsp` → 安装**。

### 4.2 路线 A：网络安装 —— 但**先看 4.3 的防火墙问题**

```powershell
# 1) 起服务（第 8 轮已在 8083 上跑起来过，验证过列表与 Range）
node <host-tools-dir>\tools\nsp-server.mjs --root <项目>\out --port 8083

# 2) 把服务地址写进控制台的 DBI 配置（会先备份 dbi.config）
powershell -File <host-tools-dir>\tools\switch-fs.ps1 dbisource http://<PC的局域网IP>:8083/ "hongguo"
```

第 8 轮已经执行过第 2 步，结果：

```
backup : <host-tools-dir>\.switch\backup\dbi.config.20261009-070900.bak
wrote  : hongguo=ApacheHTTP|http://192.168.1.x:8083/
into   : /switch/DBI/dbi.config   (existing [Network sources] updated)
```

（PC 的局域网地址是 **192.168.1.x**，控制台是 192.168.1.x，同网段。按 `nsp\README.txt`，
控制台侧：**DBI → Home server → 选 `hongguo.nsp` → 安装**。要撤销这次改动，
把上面那个 `.bak` 覆盖回 `/switch/DBI/dbi.config` 即可。）

### 4.3 防火墙：网络路线可能连不上，而且这一步需要管理员

实测本机状态：

```
Private Profile: State ON / Firewall Policy BlockInbound,AllowOutbound
                 LocalFirewallRules N/A (GPO-store only)
8083 的放行规则：不存在（8082、dsh-switch 命名的规则也不存在）
本进程是否提权：False
```

**方向很重要**：以前所有操作都是 PC→控制台（连它的 FTP、上传 NRO），出站本来就是允许的；
网络安装第一次要求**控制台→PC**，那需要一条入站放行。`allow-firewall.ps1` 就是为此写的，
但它要管理员权限，而第 8 轮没有提权。所以要网络安装的话得先以管理员运行：

```powershell
powershell -ExecutionPolicy Bypass -File <host-tools-dir>\tools\allow-firewall.ps1 -Port 8083
```

**这就是为什么推荐路线 B——它绕开了整件事。**

### 安装失败时的排查顺序

1. **签名**：这个 NSP 由 homebrew 密钥对签名（`hacBrewPack` 默认行为），**不是任天堂的密钥**。
   没打 **sigpatch**（Atmosphère + sigpatches）的机器会直接拒绝。这是第一位原因。
2. **必需固件版本**：`SDK Version 0.12.17.0`、`Keygeneration 1` —— 都很低，通常不是障碍。
3. **title id 冲突**：`0101000000000000` 是 homebrew 段内的合法 id；若已装过同 id 的旧版，
   DBI 会要求先卸载。
4. **密钥库**：`hacbrewpack` 读 `%USERPROFILE%\.switch\prod.keys`（18,394 字节，存在）。
   若它回落到 `keys.dat` 缺失并失败，打包阶段就会报错，不会产出 NSP——本次没有报错。

---

## 五、这一节留下的工具

| 文件 | 作用 |
|---|---|
| `scripts\verify-nsp.ps1` | PFS0 结构校验（已有） |
| `scripts\nca-inspect.mjs` | 自己解析 PFS0，打印每个条目的名字/偏移/大小——**用来独立复现 verify 的数字**。第一版字段偏移写错了（把 offset/size 读成 +0x10/+0x14），产出的是假的"时间戳"和乱码名字；改成正确偏移后与 verify 完全一致 |
| `scripts\nca-identify.mjs` | 试图用 `header_key` + AES-XTS 还原 NCA 头并报出内容类型。**目前穷举失败**，脚本如实报告"no NCA magic recovered"，而不是假装成功 |
