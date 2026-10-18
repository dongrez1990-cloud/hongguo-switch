# 第三方组件、许可与参考来源

本仓库是 **GPL-3.0** 项目，并且**是 [wiliwili](https://github.com/xfangfang/wiliwili) 的衍生工作**。
这份文件回答三件事：链接了什么、许可证是什么、哪些知识来自别人的研究。

> 界面里原本有一个"开源许可"栏目（`makeLicenseGroup()`，`app/hongguo/settings_activity.cpp`）。
> 它被按用户要求从界面上移除，但**分发时的许可通知义务不因此消失**，所以清单留在这里。
> 恢复界面只需一行，见 `docs/第三方组件与许可.md`。

---

## 1. 本项目

| 项目 | 许可 | 说明 |
|---|---|---|
| hongguo-switch **v1.0.0**（build176） | **GPL-3.0-only** | 红果短剧在 Nintendo Switch 上的非官方客户端 |

---

## 2. 构建自（构建时链接或包含的第三方代码）

| 组件 | 许可 | 用途 | 出处 |
|---|---|---|---|
| **wiliwili** | **GPL-3.0** | Switch 端 TV 客户端。本项目的**工程结构、borealis 集成、mpv/deko3d 渲染接入、按键与界面布局规格**均来自它。**衍生关系成立。** | https://github.com/xfangfang/wiliwili |
| **borealis** | **GPL-3.0** | UI 框架（wiliwili 的 fork）。所有界面、焦点、动画、主题、nanovg 绘制都走它 | https://github.com/xfangfang/borealis |
| **libnx** | **ISC** | Switch homebrew 运行时：服务初始化、线程、软键盘、applet 通知 | https://github.com/switchbrew/libnx |
| **mpv / libmpv** | **LGPL-2.1-or-later**（本构建）/ **GPL-2.0-or-later**（若启用了 GPL-only 模块） | 解码、渲染、seek、截图探针。以动态库方式链接，**未修改** | https://github.com/mpv-player/mpv |
| **FFmpeg**（libavformat / libavcodec / libavutil / libswscale / libswresample / libavfilter / libdav1d） | **LGPL-2.1-or-later** | mpv 的解复用/解码后端 | https://ffmpeg.org/ |
| **libcurl** | **curl（MIT/X 风格）** | HTTP：主页/详情抓取、搜索、封面图、播放地址 | https://curl.se/ |
| **deko3d** | **MIT / zlib 风格** | GPU API，帧呈现与 nanovg 后端 | https://github.com/devkitPro/deko3d |
| **nanovg** | **zlib** | 2D 矢量绘制（borealis 的渲染后端，含 deko3d 移植） | https://github.com/memononen/nanovg |
| **libplacebo** | **LGPL-2.1-or-later** | mpv 的 GPU 渲染管线 | https://github.com/haasn/libplacebo |
| **mbedTLS** | **Apache-2.0** | mpv 的 TLS/加密后端 | https://github.com/Mbed-TLS/mbedtls |
| **lua 5.1** | **MIT** | mpv 内嵌脚本运行时 | https://www.lua.org/ |
| **harfbuzz / fribidi / freetype** | harfbuzz **MIT** / fribidi **LGPL-2.1+** / freetype **FTL 或 GPL-2.0+** | 文本整形与字体（经由 libass） | harfbuzz.github.io / github.com/fribidi/fribidi / freetype.org |
| **libass** | **ISC** | 字幕渲染（mpv 依赖） | https://github.com/libass/libass |
| **glm** | **MIT（或 Happy Bunny License）** | deko3d 相关数学 | https://github.com/g-truc/glm |
| **Vulkan-Headers** | **Apache-2.0 / MIT** | 仅头文件，deko3d 构建期依赖 | https://github.com/KhronosGroup/Vulkan-Headers |
| **nlohmann/json** | **MIT** | **仅主机侧测试夹具**使用，不进 NRO | https://github.com/nlohmann/json |

**构建方式**：以上库来自 devkitPro 的 `switch-dev` / `switch-curl` / `switch-mpv` / `switch-ffmpeg`
portlibs 包，或由本仓库 `scripts/` 下的交叉编译脚本构建。**本仓库不重新分发它们的源码或二进制。**

---

## 3. 研究参考（接口知识来源）

这一节是**必须写**的，理由和上面一样：接口层的每一条都是别人先趟出来的。

| 项目 | 许可 | 借用了什么 | 出处 |
|---|---|---|---|
| **N3urda/hongguoTV** | **GPL-3.0** | 接口形状与部分常量来源（`kotlin-tv/core` 的请求构造与响应解析）。`app/src/hg_tables.h`、`hg_branch2_tables.h` 由它的 `VendorConstants.kt` 生成 | https://github.com/N3urda/hongguoTV |
| **zhenyong97/hongguo-downloader** | —（以其仓库声明为准） | 接口行为的对照参考：端点是否还存在、返回什么形状 | https://github.com/zhenyong97/hongguo-downloader |
| **zhangbaio/hongguo** | —（以其仓库声明为准） | 错误码含义的对照参考（如播放失败的 `110001` 的含义） | https://github.com/zhangbaio/hongguo |
| **563617356/hongguo-tv** | —（以其仓库声明为准） | 播放链路整体形状的对照参考（Apple TV 端实现） | https://github.com/563617356/hongguo-tv |
| **woshishiq1/drpys**（`spider/js/红果果[短].js`） | **GPL-3.0** | 红果签名与 CENC 逻辑的**上游来源**；`N3urda/hongguoTV` 由它派生。本项目的 `hg_sign.c`、`hg_cenc.c`、`hg_aesv3.c`、`hg_medusa.c` 的注释逐处标了行号来源 | 见 `N3urda/hongguoTV` 的 vendor 目录 |

### 借鉴了什么、没借鉴什么

**借鉴的是"接口的形状"**（这个端点要哪些参数、返回体的字段叫什么、错误码是什么意思）
和**必须逐字节一致的常量**（S-box、UA 串、设备指纹串 —— 这些是协议数据，换个字符就不通）。

**没有做的事**：

* **没有把任何第三方实现拷进本仓库。** 具体地说，`hongguoTV` 的
  `server/vendor/hongguo.cjs` **没有**出现在这个仓库里，也没有被 `#include`。
* 标了来源的文件采用的都是**按形状重写**：C 语言实现，带主机测试，
  不复制原实现的字符串拼接与错误处理。
* `app/src/tools/gen_*.mjs` 是生成器，需要你自己把上游仓库检出到某个位置再跑 ——
  它们**接受路径参数**，不硬编码第三方源码位置。

因为 `N3urda/hongguoTV` 与 `woshishiq1/drpys` 都是 **GPL-3.0**，而本项目的衍生关系
本来就成立（wiliwili），许可证上**没有冲突**：合并后的整体保持 GPL-3.0。

---

## 4. 未包含

| 项 | 说明 |
|---|---|
| **品牌素材** | 无。不重新分发红果、wiliwili 或任何上游的 logo、图标、美术资源。`assets/icon-256.jpg` 是自绘的 |
| **字体** | 未捆绑。使用主机提供的系统字体 |
| **视频内容** | 无。不缓存、不转存 |
| **NSP 密钥** | 无。`scripts/build-nsp.ps1` 读你本机的密钥库，**不随仓库分发** |
| **个人数据** | 无。历史/收藏是运行时写进 SD 卡的本地文件，仓库里没有任何用户数据 |
| **第三方源码** | 无。`upstream/` 与 `third_party/` 都不在发布树里，见 README 的构建步骤 |

---

## 5. 如果这份清单和代码不一致

**以代码为准**，并回来改这份文件。
`app/hongguo/settings_activity.cpp` 里的 `makeLicenseGroup()` 是这份清单的另一份副本
（从同一个来源抄的），两边都应保持准确。
