# 09 · 构建、运行与调试指南

> 本文说明如何编译该仓库、如何运行、关键配置项，以及调试/排错的切入点。
> ⚠️ 请在**已授权的实验环境 / 虚拟机**中进行，遵守当地法律法规。

---

## 1. 环境要求

| 项 | 要求 |
|---|---|
| IDE | Visual Studio 2019（推荐，解决方案为 VS 格式） |
| 平台 | **Win32 / x86 为推荐目标**（`Release|x86`） |
| 驱动编译 | 需安装 **WDK 8 或以上**（仅在需要自行编译驱动时） |
| 运行系统 | Win7 / Win8 / Win8.1 / Win10（32 位优先；XP 需旧版） |
| 权限 | 需**管理员**运行（提权、装驱动、注入） |

> 64 位系统：驱动与部分注入能力不可用（`XTestDriverCanUse` 直接拒绝 64 位）；主程序本身可在 64 位系统上运行做窗口处理，但功能受限。

---

## 2. 解决方案与平台配置

`JiYuTrainer.sln` 含 `Debug|x64`、`Release|x64`、`Debug|x86`、`Release|x86` 四种组合（`x86` 映射到工程的 `Win32`）。

**各工程在 Win32 与 x64 下的类型不同**，这点很关键：

| 工程 | Win32(x86) | x64 |
|---|---|---|
| JiYuTrainer | Application | Application |
| JiYuTrainerUI | **StaticLibrary** | DynamicLibrary |
| JiYuTrainerUpdater | **StaticLibrary** | Application |
| JiYuTrainerHooks | DynamicLibrary | DynamicLibrary |
| JiYuTrainerDriver | Application(驱动) | Application(驱动) |

因此 **`Release|x86` 构建只产出 `JiYuTrainer.exe`**（UI 与更新器被静态链接进去），Hook DLL 与驱动则作为**资源**打进 exe，运行时释放。

---

## 3. 构建步骤（README 推荐流程）

1. 用 Visual Studio 2019 打开 `JiYuTrainer.sln`；
2. 顶部工具栏选择配置 **Release**、平台 **x86**；
3. 解决方案资源管理器右键 `JiYuTrainer` 项目 → **生成**；
4. 等待生成完成；
   - **不用理会 `JiYuTrainerDriver` 的编译错误**——无须自行生成驱动，源码/资源中已附带编译好的 `JiYuTrainerDriver.sys`（见 `Release/JiYuTrainerDriver.sys`）。
5. 生成产物在 `Release/` 目录下（`JiYuTrainer.exe` 等）。

### 3.1 依赖的库与头文件

主程序工程属性中显式链接（`JiYuTrainer.vcxproj`）：

```
Ntdll.lib; Iphlpapi.lib; Mpr.lib; Ws2_32.lib; ComCtl32.lib; Wininet.lib; wldap32.lib;
JiYuTrainerUI.lib; JiYuTrainerUpdater.lib; Dbghelp.lib; Shlwapi.lib;
kernel32.lib; user32.lib; gdi32.lib; winspool.lib; comdlg32.lib; advapi32.lib;
shell32.lib; ole32.lib; oleaut32.lib; uuid.lib; odbc32.lib; odbccp32.lib
```

附加包含目录：`$(SolutionDir)JiYuTrainerUI`（供主程序 `#include "../JiYuTrainerUI/MainWindow.h"`）。

### 3.2 git 忽略项提示

`.gitignore` 忽略 `*.dll / *.lib / *.ini / *.bat / *.log / *.pdb` 等。这意味着：

- `JiYuTrainerHooks.dll` **不在仓库里**，它是被编译后**作为资源嵌入** `JiYuTrainer.exe`（`IDR_DLL_HOOKS`）；
- 因此必须**先编译 `JiYuTrainerHooks` 并确保其被嵌入**，否则主程序运行时无 DLL 可释放。

---

## 4. 运行方式

### 4.1 常规运行

1. 把 `Release/JiYuTrainer.exe` 复制到目标机器（单文件即可，内置资源会自释放）；
2. **右键 → 以管理员身份运行**；
3. 首次运行会弹使用协议；之后自动最小化到托盘，后台开始工作。

### 4.2 便携 / U 盘运行

程序检测到自身位于可移动设备时，会自动把副本搬到 `%TEMP%\JiYuTrainer\` 并生成 `.bat` 提权启动，避免在被写保护的介质上运行。

### 4.3 命令行启动（常用参数）

```batch
JiYuTrainer.exe -hidden          :: 隐藏模式启动（直接进托盘）
JiYuTrainer.exe -config          :: 打开「更多设置」
JiYuTrainer.exe -killst          :: 仅杀极域进程后退出
JiYuTrainer.exe -install-full    :: 完整安装模式
JiYuTrainer.exe -bugreport -bugfile "C:\...\CrashInfo.txt"   :: Bug 报告模式
JiYuTrainer.exe -?               :: 打印帮助
```

> 隐藏入口：启动时按住 <kbd>Alt</kbd> 进入配置模式。

完整参数表见 [02 · §5 命令行参数](./02-host-main-program.md)。

---

## 5. 运行时生成的文件

| 文件 | 位置 | 说明 |
|---|---|---|
| `JiYuTrainerHooks.dll` | exe 同目录 | 从资源释放 |
| `sciter.dll` | exe 同目录 | 从内嵌 zip 解出 |
| `JiYuTrainerDriver.sys` | exe 同目录 | 从资源释放（32 位 + 管理员时） |
| `JiYuTrainer.ini` | exe 同目录 | 配置 |
| `JiYuTrainer.log` | exe 同目录 | 日志 |
| `JiYuTrainerCrashDump*.dmp` / `CrashInfo*.txt` | exe 同目录 | 崩溃时生成 |

释放的资源会设为**隐藏属性**。退出时可从界面"卸载"按钮触发 `UnInstall()`：卸载驱动、删除文件，并写一个 `uninstall-final.bat` 延迟删除自身。

---

## 6. 关键配置项（INI）

`JiYuTrainer.ini` 的 `[JTSettings]` 节（节选自 `MainWindow::LoadSettings` 与 `VInitSettings`）：

| 键 | 默认 | 含义 |
|---|---|---|
| `TopMost` | false | 主窗口置顶 |
| `AutoUpdate` | true | 启动自动检查更新 |
| `AutoIncludeFullWindow` | false | 把任意全屏窗口也当极域窗口处理 |
| `AllowAllRunOp` | true | 允许极域执行程序（不拦截命令行） |
| `AutoForceKill` | false | 与主程序失联时自动强杀极域 |
| `AllowMonitor` | true | 允许被监看 |
| `AllowControl` | false | 允许被控制 |
| `AllowGbTop` | false | 允许广播窗置顶 |
| `ProhibitKillProcess` | true | 阻止极域结束本进程 |
| `ProhibitCloseWindow` | true | 阻止关闭广播窗 |
| `BandAllRunOp` | false | 默认禁止极域执行程序 |
| `DoNotShowTrayIcon` | false | 不显示托盘图标 |
| `CKInterval` | 3100 | 窗口检查间隔（ms，限 1000~10000） |
| `DisableDriver` | false | 禁用内核驱动 |
| `SelfProtect` | true | 自我保护 |
| `StudentMainPath` | — | 手动指定 `StudentMain.exe` 路径 |
| `HotKeyShowHide` / `HotKeyFakeFull` | 1604 / 1606 | 热键码 |

---

## 7. 调试切入点

| 目标 | 入口 |
|---|---|
| 看主流程 | `JiYuTrainer.cpp::wWinMain` → `App.cpp::RunInternal` |
| 看注入/轮询 | `TrainerWorker.cpp::RunResetPid` / `InjectDll` |
| 看窗口处理 | `TrainerWorker.cpp::RunCk` / `FixWindow` / `EnumWindowsProc` |
| 看 Hook | `JiYuTrainerHooks.cpp::VInstallHooks` → 各 `hkXxx` |
| 看 IPC | `MsgCenter.cpp` + `JiYuTrainerHooks.cpp::VHandleMsg` |
| 看驱动 | `Driver.c::DriverEntry` / `IOControlDispatch` |
| 看更新 | `JiYuTrainerUpdater.cpp::JUpdater_*` |

### 7.1 内置调试命令

主界面命令框（`link_runcmd`）支持大量命令：`version`、`whereisi`、`inspector`（Sciter 调试器）、`unloaddrv` / `floaddrv`、`jypasswd`、`attack`、`crash` 等，详见 [05 · §4.5](./05-ui-layer.md)。

### 7.2 崩溃排查

- 崩溃会生成 `JiYuTrainerCrashDump*.dmp`（可用 VS / WinDbg 打开）+ `JiYuTrainerCrashInfo*.txt`，并自动拉起 `-bugreport` 模式；
- `-break` 参数在启动时触发 `DebugBreak`，方便附加调试器；
- Sciter 界面问题可用命令 `inspector` 打开 Sciter 自带检查器。

### 7.3 驱动调试

- 驱动编译错误可忽略（用仓库自带 `.sys`）；
- 若自行编译驱动，注意必须是 **32 位 + 测试签名/关闭驱动签名强制**的环境；
- `KdPrint` / `DbgPrint` 输出可用 DebugView 查看（需内核调试或开启相应过滤）。

---

## 8. 常见问题（基于源码可见的坑）

| 现象 | 可能原因 |
|---|---|
| 杀毒软件报毒 | 注入 + 驱动 + Hook 行为特征（README 已提示需加白名单） |
| 驱动加载失败 | 非管理员 / 64 位系统 / 服务名被标记删除（代码会自动清注册表重试） |
| 无法注入 | 极域进程受保护（需驱动强杀后才能注入）/ 权限不足 |
| 界面空白 | `sciter.dll` 释放失败（`IDR_DLL_SCITER` 解压异常），查日志 |
| 源码注释乱码 | 源文件为 GBK 编码，需以 GB2312/GBK 打开 |
| 单实例提示 | 已有实例在运行（`RunCheckRunningApp` 用互斥量 `JYTMutex` 判定） |

---

## 9. 小结

- 构建的关键认知：**Win32 配置下 UI 与更新器是静态库**，只需生成 `JiYuTrainer` 一个工程即可；
- 运行的关键认知：**必须管理员**；DLL/驱动/界面引擎都是运行时自释放；
- 调试的关键认知：主程序内置了丰富的**命令控制台**与**崩溃转储**，是排错的高效入口。

---

**上一篇** → [08 · 进程间通信与数据流](./08-ipc-and-dataflow.md)　|　**回到** → [README 索引](./README.md)
