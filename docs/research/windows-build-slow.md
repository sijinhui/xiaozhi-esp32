# 为什么 Windows 上编译小智固件很慢、CPU 却很闲

更新：2026-10-16 · 板子 ESP32-S3-Touch-AMOLED-1.75C · ESP-IDF v6.1（D:\Espressif）· 代码在 %USERPROFILE%\Documents\code\xiaozhi-esp32

## 结论

**（已验证，见下方"验证结果"）元凶是 360 的内核文件过滤驱动（360FsFlt + 主动防御 ZhuDongFangYu）。** 编译时每个 gcc 进程要打开几百个头文件、写 .o/.d 文件，每一次文件打开/创建都要先经过 360 的过滤驱动检查。编译进程大部分时间在等这个检查，不是在算，所以任务管理器里看不到 CPU 被吃满。

- Windows Defender 已经是停用状态（AMRunningMode: Not running，WinDefend 服务和 WdFilter 驱动都已停止），可以排除。
- 火绒在安全中心里有注册，但没有进程和驱动在运行，可以排除。
- 360 托盘程序没开，但 360FsFlt 等 9 个 360 驱动和主动防御服务都在跑。**只退出 360 托盘不能当作"关了 360"**。
- 进程启动本身不慢（gcc --version 每次 12 ms，ccache --version 每次 7 ms），所以不是"Windows 起进程慢"的问题。

## 证据：.ninja_log 分析（build-175c 那次完整编译）

脚本：`docs/research/analyze_ninja_log.py`。

| 指标 | 数值 |
|---|---|
| 编译步骤 | 2224 |
| 墙钟时间 | 664 s（约 11 分钟） |
| 所有任务耗时之和 | 17975 s |
| 平均同时运行的任务数 | 27（ninja 默认 -j 约 30，几乎全程打满） |

| 文件类型 | 数量 | 平均每个耗时 |
|---|---|---|
| 普通 .c（IDF 组件） | 1217 | 7.2 s |
| LVGL | 537 | 4.1 s |
| 其他 .cc/.cpp | 77 | 22.1 s |
| main 下的 .cc | 86 | 55.2 s |
| .S 汇编 | 111 | 2.0 s |

对比：同样的 IDF 组件 .c 在 Linux/macOS 上通常 0.2–1 s，main 的 .cc 通常 3–8 s。
**并行度没问题，ninja 一直保持 30 个任务在跑；问题是每个任务都慢了 5–10 倍。** 30 个进程都在阻塞等待 I/O（被过滤驱动拦住），CPU 自然空闲。
连 `smartconfig_ack.c` 这种几十行的文件都要 10 s+，也说明慢的不是编译本身，而是文件访问。

另外几个旁证（2026-10-15 的一次测量）：

- -j30 编译时整机只有约 38% 忙，**内核态时间大于用户态时间**（正常编译应以用户态为主），360tray.exe 一度占 1.2–6.4 个核。
- 每次 CMake configure 要 73–135 s，其中真正 generate 只占 2.8 s，其余大多是读写大量小文件。
- 每个 main 下的 .cc 要搜 70–340 个 -I 目录（中位数 78），每个 #include 都会在这些目录里逐个探测文件，探测次数是几十万量级，每一次都过一遍过滤驱动。

## 验证结果（2026-10-16，卸载 360 并重启后）

同一配置（1.75C、sdkconfig 相同、2239 步）完整编译，关闭 ccache，新建 build 目录。编译期间 Defender 实时保护是开着的。

| 指标 | 有 360（build-175c） | 卸载 360 后（build-bench） |
|---|---|---|
| ninja 编译阶段墙钟 | 664 s | **102 s（快 6.5 倍）** |
| 所有任务耗时之和 | 18066 s | 2138 s |
| main 的 .cc 平均 | 55.2 s | 4.7 s |
| LVGL 平均 | 4.07 s | 0.54 s |
| 其他 .c 平均 | 7.18 s | 0.85 s |
| 其他 .cc/.cpp 平均 | 22.1 s | 2.95 s |
| bootloader 子工程 | 34.9 s | 15.6 s |
| ninja 阶段 CPU 占用 | 很低 | 平均 72%，多段 95–100% |

**结论已确认：慢的元凶是 360 的文件过滤驱动。** 卸载后 CPU 能正常吃满。

总耗时 233 s 里，**CMake configure 占了 129 s（126 s configure + 2.6 s generate），现在是最大的瓶颈**。这一阶段 CPU 平均只有 7%，是单线程在跑。另外 Defender 在 ninja 阶段占用约 484 CPU 秒（平均 4.7 个线程）。

### 下一步建议

1. **日常不要重新 configure**：只改代码时直接 `idf.py build`（或 `ninja -C <build>`），configure 只在第一次、改 sdkconfig 或 CMakeLists 时才会跑。切换板型时用各自独立的 build 目录。
2. **ccache 保持开启**：日常增量编译受益很大（本次是为了公平对比才关掉的）。注意 `idf.py` 会强制追加 `-DCCACHE_ENABLE=True`，要关只能用 `--no-ccache`。
3. **给 Defender 加排除项或用 Dev Drive**（安全设置，由你决定）：可以再省掉 Defender 占用的那部分 CPU，预计 ninja 阶段还能快一些。
4. **查 configure 为什么要两分钟**：可用 `cmake --profiling-format=google-trace --profiling-output=cfg.json` 看具体卡在哪里（通常是组件管理器解析 74 个依赖和大量 Python 子进程）。

## 建议（按性价比排序）（写于验证之前）

1. **先做对照实验（确认元凶，5 分钟）**：在 360 里关闭"主动防御"（或临时退出整个 360 安全卫士，包括驱动防护），然后删掉 build 目录重新完整编译一次，用 `analyze_ninja_log.py` 看 .c 平均耗时。预期从 7 s 降到 1 s 以内。
2. **把编译相关目录加入 360 信任区**（不想关 360 的长期方案）：
   - `D:\Espressif`（IDF、工具链、python venv）
   - `%USERPROFILE%\Documents\code`（源码和 build 目录）
   - `%LOCALAPPDATA%\ccache`（ccache 缓存，以 `ccache -p` 显示的 cache_dir 为准）
   - `%APPDATA%\uv\python`（venv 背后的 Python）
   - 信任进程：`xtensa-esp32s3-elf-*.exe`、`cc1.exe`、`cc1plus.exe`、`as.exe`、`ld.exe`、`ninja.exe`、`cmake.exe`、`ccache.exe`、`python.exe`
   注意：有些安全软件的"信任目录"只免除查杀，不免除主动防御的行为监控，加完后要用第 1 步的方法再测一次。
3. **Dev Drive（Windows 11）**：建一个 ReFS 的开发驱动器，把代码、`D:\Espressif`、ccache 都放上去。Dev Drive 默认只允许 Defender 以"性能模式"挂过滤驱动，第三方过滤驱动需要显式允许（`fsutil devdrv setfiltersallowed`），能从根上绕过 360 的过滤。需要管理员。
4. **WSL2 里编译**：把代码放到 WSL 的 ext4 文件系统里（不要放 /mnt/c），用 Linux 版 ESP-IDF 编译，通常比原生 Windows 快 3–5 倍；烧录用 usbipd 转发 USB，或者回 Windows 用 esptool 烧 build 产物。
5. 小优化：ccache 已开启（上次命中率约 65%），保持；不要每次都删 build 目录；切换板型时用不同 build 目录（如 build-175c）避免全量重编。

## 还缺的数据（需要管理员 PowerShell，自愿补充）

```powershell
fltmc instances            # 看 C:/D: 上挂了哪些过滤驱动及其 altitude
fsutil devdrv query C:
fsutil devdrv query D:
```

## 文件

- `analyze_ninja_log.py`：分析任意 build 目录下的 `.ninja_log`，用于前后对比。
- 环境采集原始数据只保存在本地和私有项目目录，不放进公开仓库。
