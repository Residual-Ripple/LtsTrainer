# 01 · 架构总览

## 1. 设计目标与整体思路

JiYuTrainer 的核心思路可以概括为三条主线：

1. **不改极域程序本身，而是"寄生"其中**——用 `CreateRemoteThread` + `LoadLibrary` 把自研 DLL（`JiYuTrainerHooks.dll`）注入到极域学生端 `StudentMain.exe`，在**目标进程内部**用 `mhook` 挂钩一批 API。
2. **用窗口 API 层面的"温和改写"替代"断网/杀进程"**——被 hook 的 `SetWindowPos / MoveWindow / ShowWindow` 等会让极域的**全屏广播窗口降级为普通可缩放窗口**，学生既能自由操作，也不会被教师端判定为掉线。
3. **用户层不够用时下沉到内核**——极域有自我保护驱动，普通 `TerminateProcess` 打不开它的进程；于是提供 `JiYuTrainerDriver.sys` 做进程句柄保护（`ObRegisterCallbacks`）与强制结束（APC 终止线程）。

整体是一个**多进程、多模块协同**的软件系统：主程序负责引导、调度与 UI；注入模块运行在极域进程内；驱动运行在 Ring0；UI 通过 HTML 渲染。

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                        用户交互层 (UI)                                │
│  JiYuTrainerUI (Sciter HTML/CSS UI)  ← 静态库链接进主程序             │
└──────────────────────────────────────────────────────────────────────┘
                                  ▲
                                  │ 进程内直接调用 (JTApp 接口)
┌──────────────────────────────────────────────────────────────────────┐
│                    主程序 / 编排层 (Host)                             │
│  JiYuTrainer.exe = JTAppInternal (引导/安装/参数/策略)                 │
│                  + TrainerWorkerInternal (轮询定位/注入/修窗口)        │
│                  + JyUdpAttack (UDP 报文扫描与发送)                    │
│                  + DriverLoader / KernelUtils (驱动装载与调用)         │
└──────────────────────────────────────────────────────────────────────┘
        │ 注入(CreateRemoteThread)        │ DeviceIoControl      │ WM_COPYDATA
        ▼                                 ▼                      ▼
┌───────────────────────────┐   ┌──────────────────────┐  ┌──────────────────┐
│  注入层 (Ring3, in-proc)   │   │  内核层 (Ring0)       │  │  通信             │
│  JiYuTrainerHooks.dll     │   │  JiYuTrainerDriver.sys│  │  MsgCenter        │
│  mhook API Hook × 30+     │   │  ObRegisterCallbacks  │  │  (隐藏窗口 +      │
│  窗口子类化(WNDPROC)       │   │  SSDT Hook (XP)       │  │   WM_COPYDATA)    │
└───────────────────────────┘   └──────────────────────┘  └──────────────────┘
```

---

## 3. 五个子工程与编译产物

解决方案 `JiYuTrainer.sln` 共 5 个工程。**注意**：`JiYuTrainerUI` 与 `JiYuTrainerUpdater` 在 **Win32(x86) 配置下编译为静态库**（x64 配置才是 DLL/EXE），因此推荐构建（Release|x86）只会产出单个 `JiYuTrainer.exe`，UI 与更新器代码被静态链接进主程序。

| 工程 | 工程类型（Win32 配置） | 产物 | 职责 |
|---|---|---|---|
| **JiYuTrainer** | Application | `JiYuTrainer.exe` | 主引导器 + 核心逻辑；加载 UI；定位并注入 Hook；驱动装载；UDP 功能 |
| **JiYuTrainerUI** | StaticLibrary（x64 下 DynamicLibrary） | 链接进 exe | Sciter HTML UI；导出 `JiYuTrainerUICommonEntry(int)` |
| **JiYuTrainerHooks** | DynamicLibrary | `JiYuTrainerHooks.dll` | 注入到极域进程的 Hook 模块（"病毒"） |
| **JiYuTrainerDriver** | Application（需 WDK，编译为 .sys） | `JiYuTrainerDriver.sys` | 内核驱动：进程保护、强杀、关机/重启、进程监控 |
| **JiYuTrainerUpdater** | StaticLibrary（x64 下 Application） | 链接进 exe（也是独立安装器） | 检查更新、下载新版本、触发安装 |

主程序通过 `#pragma comment` / 工程属性显式链接 `JiYuTrainerUI.lib`、`JiYuTrainerUpdater.lib`（见 `JiYuTrainer.vcxproj` 的 `AdditionalDependencies`），运行时 `JiYuTrainerUICommonEntry` 由主程序直接调用，无需额外 DLL。

---

## 4. 运行时组件与资源

主程序运行时会在自身目录（或临时目录，见下）中落地如下文件：

| 文件 | 来源 | 说明 |
|---|---|---|
| `JiYuTrainer.exe` | 本体 | 主程序 |
| `JiYuTrainerHooks.dll` | 从 exe 资源 `IDR_DLL_HOOKS` 释放 | 注入模块 |
| `JiYuTrainerDriver.sys` | 从 exe 资源 `IDR_DLL_DRIVER` 释放 | 内核驱动（需管理员 + 32 位系统） |
| `sciter.dll` | 从 exe 资源 `IDR_DLL_SCITER`（zip）解压 | Sciter 渲染引擎 |
| `JiYuTrainer.ini` | 首次运行生成 | 配置（`SettingHlp` 读写） |
| `JiYuTrainer.log` | 运行时生成 | 日志 |
| `JiYuTrainerUpdater.exe` | 更新时下载 | 新版本安装器 |

释放的资源 ID 定义在 `JiYuTrainer/AppPublic.h` 的 `PART_*` 常量中：

```cpp
#define PART_INI     -1
#define PART_MAIN     0
#define PART_LOG      1
#define PART_HOOKER   3   // JiYuTrainerHooks.dll
#define PART_DRIVER   4   // JiYuTrainerDriver.sys
#define PART_SCITER   5   // sciter.dll
```

**便携（U盘）运行**：`CheckAndInstall()` 检测到程序位于可移动设备（`SysHlp::CheckIsPortabilityDevice`）时，会把自己复制到 `%TEMP%\JiYuTrainer\`，生成一个 `.bat` 提权启动副本，避免占用 U 盘或权限受限目录。

---

## 5. 进程与模块协作模型

### 5.1 进程清单（运行时）

| 进程 | 说明 |
|---|---|
| `JiYuTrainer.exe` | 主程序（Host），常驻，轮询极域状态 |
| `StudentMain.exe` | 极域学生端进程，被**注入** `JiYuTrainerHooks.dll` |
| `JiYuTrainerDriver.sys` | 驱动（由系统加载，拥有 `\\.\JKRK` 设备） |

### 5.2 三个方向的协作通道

| 通道 | 方向 | 机制 | 实现文件 |
|---|---|---|---|
| 注入 | Host → 极域进程 | `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryW)` | `TrainerWorker.cpp::InjectDll` |
| 消息 | Host ⇄ 注入 DLL | 隐藏窗口 + `WM_COPYDATA` 文本协议 | `MsgCenter.cpp` / `JiYuTrainerHooks.cpp` |
| IOCTL | Host → 驱动 | `CreateFile("\\.\JKRK")` + `DeviceIoControl` | `KernelUtils.cpp` / `DriverLoader.cpp` |

### 5.3 关键全局对象

主程序在 `wWinMain` 中创建两个全局单例（`JiYuTrainer.cpp`）：

```cpp
JTApp * currentApp = nullptr;                 // 应用对象（策略接口）
LoggerInternal * currentLogger = nullptr;     // 全局日志
```

二者通过 `extern` 在各模块间共享，是理解主程序内部耦合的钥匙。

---

## 6. 依赖关系图

```
                         ┌─────────────────────┐
                         │   JiYuTrainer.exe   │  (主程序 / Host)
                         └──────────┬──────────┘
             静态链接               │                  运行时动态
     ┌──────────────┬───────────────┼──────────────┬────────────────┐
     ▼              ▼               ▼              ▼                ▼
 JiYuTrainerUI  JiYuTrainerUpdater  Kernel32/Nt   mhook(注入)    \\.\JKRK
 (Sciter UI)    (curl 更新)         user32/shell32  ↓              ↓
     │               │                            JiYuTrainer     JiYuTrainer
     ▼               ▼                           Hooks.dll        Driver.sys
 sciter-sdk      curl                            (+disasm-lib)    (WDK/ntifs)
```

第三方依赖（全部随仓库自带，无需额外安装）：

| 库 | 用途 | 位置 |
|---|---|---|
| **mhook** | 用户层 API Hook 引擎（改写函数前 5 字节跳转） | `JiYuTrainerHooks/mhook-lib` |
| **disasm-lib** | mhook 依赖的长度反汇编器（LDE） | `JiYuTrainerHooks/disasm-lib` |
| **Sciter SDK** | HTML/CSS 界面渲染引擎 | `JiYuTrainerUI/sciter-sdk` |
| **curl** | HTTP 下载（更新模块） | `JiYuTrainerUpdater/curl` |
| **MemoryModule** | 从内存加载 DLL（预留能力） | `JiYuTrainer/MemoryModule.*` |
| **XZip / XUnzip** | 内存解压 zip（解出 sciter.dll） | `JiYuTrainer/XZip.*` `XUnzip.*` |
| Jiyu_udp_attack 原理 | UDP 广播报文格式参考（由 ht0Ruial 提供） | `JiYuTrainer/JyUdpAttack.*` |

> 详细依赖说明见 [07-通用工具库与第三方依赖](./07-utils-and-thirdparty.md)。

---

## 7. 编码与工程约定

- **源码注释为 GBK 编码**，在 UTF-8 环境下阅读会显示乱码；函数/变量命名基本为英文，可据标识符理解逻辑。
- 采用 **策略模式 + 接口隔离**：`JTApp`/`JTAppInternal`、`TrainerWorker`/`TrainerWorkerInternal`、`Logger`/`LoggerInternal`、`SettingHlp`/`SettingHlpInternal` 均为"抽象接口 + `*Internal` 实现"成对出现，便于裁剪与替换。
- 大量使用 **`EXPORT_CFUNC` / `UIEXPORT_CFUNC` / `UPEXPORT_CFUNC`** 宏导出 C 风格函数，用于跨模块（静态链接 / DLL）调用。
- 崩溃处理：全局 `SetUnhandledExceptionFilter(AppUnhandledExceptionFilter)`，崩溃时生成 `*.dmp` + `*.txt` 并可拉起自身 `-bugreport` 模式。

---

## 8. 关键设计取舍（学习要点）

| 取舍 | 说明 |
|---|---|
| 为什么不直接杀进程？ | 极域掉线会被教师端发现；改窗口能"看起来正常"。 |
| 为什么注入 DLL 而不做外挂式操作？ | 只有进入目标进程，才能 hook 它调用的 `user32`、`LibTDMaster.dll` 等。 |
| 为什么还要内核驱动？ | 极域进程有驱动保护，Ring3 `TerminateProcess` 会 `STATUS_ACCESS_DENIED`，需要 Ring0 手段。 |
| 为什么用隐藏窗口做 IPC？ | `WM_COPYDATA` 是跨进程传文本最轻量的方式，且不易被杀软误判为网络行为。 |

---

**下一篇** → [02 · 主程序 JiYuTrainer](./02-host-main-program.md)
