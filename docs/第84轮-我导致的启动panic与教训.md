# 第 84 轮：我上一版让主机直接 panic 了 —— 原因与教训（build84）

**是我的错，而且根因很明确**：`build83` 在启动第一步就 panic 了 ✓。这一页把它记清楚，
因为它是一个**"猜 API"**的典型代价。

---

## 一、证据：日志停在第一行

```
=== NEW SESSION app, built Oct 11 2026 00:23:10 ===
[hg 00:28:01] === hongguo app log, built Oct 11 2026 00:23:10 [build72] ===
[hg 00:28:01] breadcrumb log: sdmc:/switch/hongguo.log …
[hg 00:28:01] borealis logger -> sdmc:/switch/hongguo-brls.log at DEBUG …
[hg 00:28:01] ENTER main (shipping)
<没有下一行>
```

`ENTER main` 是 main 里的**第一步** ✓ —— 之后一行都没有 ✓ → 崩在**紧接着的那句**上 ✓✓，
而那句正是我这一轮加的"启动时报告内存" ✗：

```cpp
logPrintf("boot: argc=%d, memory in use %ld KiB at startup [build83]", argc, hgMemoryUsedKB());
```

`hgMemoryUsedKB()` 的参数在 `logPrintf` 之前求值 ✓ → 它 panic 了，所以**那行日志根本没机会写出来** ✓✓
（这解释了为什么日志只有 `ENTER main` ✓）。

## 二、根因：**用错了 syscall**

```cpp
svcGetSystemInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);   // 我的写法 ✗
```

libnx 头文件里查实：

| 行 | 内容 |
|---|---|
| `svc.h:192` | `InfoType_UsedMemorySize = 7` —— 它属于 **`svcGetInfo`** 的枚举 |
| `svc.h:847` | `Result svcGetInfo(u64* out, u32 id0, Handle handle, u64 id1)` |
| `svc.h:1555` | `Result svcGetSystemInfo(u64* out, u64 id0, Handle handle, u64 id1)` ← **另一套属性 ID** |

我把 `svcGetInfo` 的枚举值喂给了 `svcGetSystemInfo` ✓ → 内核收到**非法 id0** ✓ ——
而**非法 id0 是 panic，不是 Result** ✓✓（所以 `R_SUCCEEDED` 那层判断根本没机会起作用 ✗）。

## 三、修法：**不再用系统调用**

```cpp
long hgMemoryUsedKB(void) {
    struct mallinfo mi = mallinfo();          /* libc，不可能让进程 panic */
    return (long)((unsigned long)mi.uordblks / 1024ul);
}
```

并且**把日志措辞改准**：它报告的是**堆（heap）**，不是整个进程 ✓ ——
GPU 侧分配不会出现在这里，但**"这个 App 自己的 C++ 分配有没有一直不释放"**会 ✓（那正是你问的问题 ✓）。
日志因此写成 `heap in use …` / `heap=… KiB` ✓，而不是含糊的"memory" ✓。

## 四、这一轮同时保留的东西（`build83` 的真正价值）

`build83` 里那条**性能修复**是对的、也是这一轮最值钱的东西 ✓✓，它没有被这次 panic 抵销：

> 日志实测 `5.3 fps / mpv_sw=179 ms/frame / surface=1920x1080` —— **播 360p 的源却按 1080p 做 CPU 转换**
> → 软件表面改为**按视频自身高度分配**（画质零损失）✓

这条现在位于 `build84` ✓。

## 五、教训（写给下一个我）

1. **系统调用用错参数是 panic 而不是返回值** —— 所以"试一下能不能用"这种思路在 syscall 上不成立 ✗；
2. 我这次是在**日志验证**里发现的（构建通过 ✓、测试通过 ✓，两者都抓不到 panic ✗）——
   也就是说**只有真机日志能证明它** ✓，这正是为什么每一步都要部署后读回 ✓；
3. 以后加 syscall 时：**先在头文件里确认它属于哪个 API**（这次错在把 `svcGetInfo` 的枚举给了 `svcGetSystemInfo`）✓。

## 六、验证

| 项 | 结果 |
|---|---|
| 主机测试 | **19 套 + 1 项源码不变量 / 1230 项 / 0 失败** |
| 构建 | 零警告（`out/build84.log`） |
| 读回 | `/switch/hongguo.nro` **SHA-256 与本地一致**（`71C32D33286DF0E5…`），27,240,472 字节 |
| 二进制核对 | `heap in use %ld KiB`、`heap=%ld KiB` 在；**`svcGetSystemInfo` 为 0 处** ✓✓（真的删干净了） |
| 主机产物 | `hongguo.nro` / `hongguo-build84.nro` / `hongguo-build84.nsp`（带图标；`build83` 已删除） |

## 七、请试

1. **启动**（应当能正常进主页 ✓ —— 这是这一版首先要确认的）；
2. **播放**：应明显比之前流畅（`mpv_sw=` 从 179 ms 掉下来 ✓），日志多出 `the stream is 360p …` 与每秒 `heap=… KiB`；
3. 控制条（`build82` 起）：音量格 A → **拖动条**；倍速/缩放格 A → **上拉菜单**；
4. `PLAYING` 那行应彻底消失（它的真正来源 `drawScrubTick` 已在 `build82` 修）。
