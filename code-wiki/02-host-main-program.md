# 02 · 主程序 JiYuTrainer（Host）

> 工程目录：`JiYuTrainer/`　产物：`JiYuTrainer.exe`
> 这是整个软件的**大脑**：负责引导安装、解析参数、加载 UI、定位并注入 Hook、装载驱动、以及与极域进程通信。

---

## 1. 程序入口与全局对象

`JiYuTrainer/JiYuTrainer.cpp`

```cpp
JTApp * currentApp = nullptr;
LoggerInternal * currentLogger = nullptr;

int APIENTRY wWinMain(HINSTANCE hInstance, ...)
{
    SetUnhandledExceptionFilter(AppUnhandledExceptionFilter); // 崩溃转储
    currentLogger = new LoggerInternal();
    currentApp    = new JTAppInternal(hInstance);
    currentApp->Run(nCmdShow);
    int rs = currentApp->GetResult();
    ShowAppStartupFail(rs);      // 失败时弹提示
    delete currentApp; delete currentLogger;
    return rs;
}
```

该文件同时定义了**崩溃处理链**：`AppUnhandledExceptionFilter` → `GenerateMiniDump` → 写出 `JiYuTrainerCrashDump*.dmp` 与 `JiYuTrainerCrashInfo*.txt`，再用 `-bugreport` 拉起自身进入报告模式。

**导出的 C 函数**（供 UI/Updater 等模块调用）：

| 导出函数 | 作用 |
|---|---|
| `GetApp()` | 返回 `JTApp*` 单例 |
| `GetSciterAPI()` | 返回 Sciter API 指针 |
| `ForceExit()` | 强制退出 |
| `AppUnhandledExceptionFilter()` | 全局异常过滤器 |
| `ShowAppStartupFail(int)` | 启动失败提示 |

---

## 2. 应用对象：JTApp 抽象接口

`JiYuTrainer/AppPublic.h`

`JTApp` 是一个纯虚接口（约 100 行），把"应用能力"抽象成统一入口，避免各模块直接依赖实现类。核心虚函数：

| 分类 | 函数 | 说明 |
|---|---|---|
| 生命周期 | `Run(int)` / `GetResult()` / `Exit(int)` | 运行、取结果、退出 |
| 路径 | `GetFullPath()` / `GetCurrentDir()` / `GetPartFullPath(int)` | 取 exe、目录、各部件路径 |
| 资源安装 | `InstallResFile(...)` / `LoadDriver()` / `UnInstall()` | 释放内置资源、装驱动、卸载 |
| 命令行 | `IsCommandExists()` / `GetCommandLineArray()` / `FindArgInCommandLine()` | 参数解析 |
| 子对象 | `GetLogger()` / `GetSettings()` / `GetTrainerWorker()` / `GetJyUdpAttack()` | 取各子系统句柄 |
| 其他 | `GetSelfProtect()` / `GetAppIsHiddenMode()` / `RunOperation(AppOperation)` | 自我保护、隐藏模式、操作分发 |

`JTAppInternal`（`App.h` / `App.cpp`）是其唯一实现。

---

## 3. 启动流程：JTAppInternal::RunInternal

`App.cpp` 的 `RunInternal()` 是主程序的"主时间线"，逻辑顺序如下：

```
setlocale("chs"); MLoadNt();                              // ① 加载 ntdll 函数表
if (系统不支持) return APP_FAIL_SYSTEM_NOT_SUPPORT;
InitPrivileges();  // SE_DEBUG / SE_SHUTDOWN / SE_LOAD_DRIVER
InitLogger(); InitPath();                                 // ② 日志 + 路径
appLogger->SetLogOutPut(File); SetLogOutPutFile(log);
InitCommandLine(); InitArgs(); InitSettings();            // ③ 参数 + 配置
EnableVisualStyles();
if (!CheckAppCorrectness()) return APP_FAIL_PIRACY_VERSION;

// ④ 特殊模式判定
if (长按 Alt) appIsConfigMode = true;                      // 隐藏入口
if (appCmdHelpMode) 打印帮助并返回
if (appKillStMode)  仅执行"杀死极域进程"后返回

// ⑤ 单实例检查 + 安装资源
int old = RunCheckRunningApp();
if (old == 1) return APP_FAIL_ALEDAY_RUN;
if (CheckAndInstall()) return APP_FAIL_INSTALL;
if (appIsInstaller) return 0;

// ⑥ 创建核心对象
appWorker       = new TrainerWorkerInternal();
appJyUdpAttack  = new JyUdpAttack();

// ⑦ 进入 UI
return JiYuTrainerUICommonEntry(mode);  // 0 主界面 1 更新 2 配置 3 Bug报告 4 崩溃测试
```

### 3.1 关键私有初始化

| 方法 | 职责 |
|---|---|
| `InitPrivileges()` | 启用 `SE_DEBUG_NAME`、`SE_SHUTDOWN_NAME`、`SE_LOAD_DRIVER_NAME` 特权 |
| `InitPath()` | 计算 `fullPath/fullDir/fullIniPath/fullLogPath` 等；`MergePathString()` 拼出 hook/driver/sciter 路径 |
| `InitCommandLine()` | `CommandLineToArgvW` 解析参数数组 |
| `InitArgs()` | 按参数名设置各 `appXxx` 标志（见 §5） |
| `InitSettings()` | 用 `SettingHlpInternal` 读 INI：FirstUse / Argeed / DisableDriver / SelfProtect 等 |

### 3.2 资源安装：CheckAndInstall()

```cpp
if (从可移动设备运行 && 非安装模式) → 复制自身到 %TEMP%\JiYuTrainer，生成 bat 提权启动，return 0
if (!存在 JiYuTrainerHooks.dll) InstallResFile(IDR_DLL_HOOKS → fullHookerPath)
if (!存在 sciter.dll)           InstallSciter()          // 解压 IDR_DLL_SCITER(zip) 中的 sciter.dll
LoadLibrary(fullHookerPath); LoadLibrary(fullSciterPath) // 预加载
if (XTestDriverCanUse() && !存在 .sys) InstallResFile(IDR_DLL_DRIVER → fullDriverPath)
if (appIsInstaller) { 替换旧 exe、复制自身、以 -rc 拉起副本后退出 }   // 自我更新落盘
```

- `InstallResFile`：`FindResource → LoadResource → LockResource → WriteFile`，写完设为 `FILE_ATTRIBUTE_HIDDEN`。
- `InstallSciter`：用 `OpenZip/FindZipItem/UnzipItem`（XZip）在**内存**中定位并解出 `sciter.dll`。

---

## 4. 核心工作器：TrainerWorkerInternal

`JiYuTrainer/TrainerWorker.h`（接口 + 实现类声明）与 `TrainerWorker.cpp`（约 1200 行实现）。
它继承自抽象接口 `TrainerWorker`，并实现回调接口 `TrainerWorkerCallback`（由 UI 侧的 `MainWindow` 实现）。

### 4.1 定时器驱动的两大循环

`Start()` 创建两个 `SetTimer`：

| 定时器 ID | 周期 | 回调 | 职责 |
|---|---|---|---|
| `TIMER_RESET_PID` (40115) | 2000 ms | `RunResetPid()` | 刷新进程表 → 定位 `StudentMain.exe` → 若 PID 变化则注入 Hook DLL |
| `TIMER_CK` (40116) | `setCkInterval`（默认 ~3100 ms） | `RunCk()` | 枚举桌面窗口 → 找到极域广播/黑屏窗口 → "修正"为窗口模式 |

```
RunResetPid()
├─ FlushProcess()                 // NtQuerySystemInformation(SystemProcessInformation) 取全进程快照
├─ LocateStudentMain(&pid)        // 在快照中找 StudentMain.exe，排除带 LibTDMaster.dll 的疑似假进程
├─ 若 pid 变化 → InstallVirus()   // InjectDll(pid, "JiYuTrainerHooks.dll")
└─ UpdateState() / UpdateStudentMainInfo()

RunCk()
├─ EnumDesktopWindows(hDesktop, EnumWindowsProc, this)
│     └─ EnumWindowsProc: 对每个可见窗口
│           ├─ ChecIsJIYuWindow()  // 窗口属于极域进程？
│           ├─ CheckIsTargetWindow()// 标题含"广播/演示/共享"或 "BlackScreen Window"
│           ├─ MsgCenteAppendHWND() // 记入待通知列表
│           └─ FixWindow()          // 去置顶 / 加边框 / 变小 / 隐藏黑屏窗
└─ MsgCenterSendHWNDS()           // 把窗口句柄列表发给注入 DLL
```

### 4.2 窗口"修正"策略：FixWindow / FakeFull

| 函数 | 行为 |
|---|---|
| `FixWindow(hWnd, text)` | 对广播窗：清 `WS_EX_TOPMOST`、加 `WS_BORDER|WS_OVERLAPPEDWINDOW`（可缩放）；对黑屏窗 `BlackScreen Window`：`SW_HIDE` 隐藏 |
| `FakeFull(true/false)` | "伪装全屏"：给广播/黑屏窗设回全屏 + `TOPMOST`，或恢复成 3/4 宽 × 4/5 高的窗口 |
| `SwitchFakeFull()` | 切换伪装全屏（供热键调用），并通知注入 DLL `hk:fkfull:true/false` |
| `ManualFull` / `ManualTop` | 响应右键菜单的"全屏/置顶"手动请求 |

广播窗口判定：`CheckWindowTextIsGb()` 检查标题是否含「广播 / 演示 / 共享」或等于「屏幕演播室窗口」；黑屏窗口标题固定为 `BlackScreen Window`。

### 4.3 进程操作（无驱动 → 有驱动兜底）

| 函数 | 说明 |
|---|---|
| `Kill(bool autoWork)` | 先用 `MOpenProcessNt`+`MTerminateProcessNt`；`STATUS_ACCESS_DENIED` 时询问用户是否用驱动 `KForceKill` 强杀 |
| `KillProcess(pid, force)` | 按 PID 杀；失败且 force 时走驱动 |
| `Rerun(autoWork)` | 用 `SysHlp::RunApplication(_StudentMainPath)` 重新拉起极域 |
| `KillStAuto()` | 供 `-killst` 命令行模式：一次扫描 + 一次强杀 |

### 4.4 定位极域路径：LocateStudentMainLocation()

多路兜底：
1. 注册表 `...\Uninstall\e-Learning Class V6.0` 的 `DisplayIcon`；
2. 用户配置 `StudentMainPath`；
3. 硬编码常见安装路径数组（`C:\Program Files\Mythware\...\StudentMain.exe` 等）。

`LocateStudentMain()` 还会主动排除"假"进程——若 exe 同目录**不存在** `LibTDMaster.dll`，则认为不是正主并加入过滤列表 `incorrectStudentMainPids`。

### 4.5 读取极域卸载/管理密码

| 函数 | 途径 |
|---|---|
| `ReadTopDomanPassword(false)` | 读注册表 `...e-Learning Class Standard\1.00\UninstallPasswd`（老版本），格式 `Passwd[xxx]`，剥掉前 6 字符 |
| `ReadTopDomanPassword(true)` | 读 `...e-Learning Class\Student\Knock1`（二进制），调用 `UnDecryptJiyuKnock()` 解密（对 4 字节按 `0x50434C45` 异或两轮 + 逐字复制） |

### 4.6 与注入 DLL 的消息处理

`HandleMessageFromVirus(LPCWSTR)` 解析 `MSG:...` 文本协议（详见 [08-IPC](./08-ipc-and-dataflow.md)）：

| 收到的消息 | 含义 / 处理 |
|---|---|
| `hkb:succ` | 注入 DLL 已接管目标进程 → 置 `_StudentMainControlled=true`，回发主窗口句柄 `hs:<hwnd>` |
| `hkb:immck` | 立刻执行一次窗口检查 `RunCk()` |
| `hkb:jyk:1/0` | 极域锁屏状态变化，更新 UI |
| `hkb:gbmfull` / `gbmnofull` | 广播窗口进入/退出全屏（DLL 侧右键菜单触发） |
| `hkb:algbtop` | 请求允许广播置顶 → 回调 `OnAllowGbTop()` |
| `hkb:wtf:<pid>` | DLL 发现宿主不是极域 → 过滤该 PID、重置状态 |
| `wcd:<n>` | 看门狗心跳计数（每 20 次记一次日志） |
| `vback:...` | DLL 的调试/回显信息 |

`RunOperation(TrainerWorkerOp)` 是**反向控制**（Host → DLL）的操作入口：

| Op | 发送给 DLL 的命令 | 含义 |
|---|---|---|
| `TrainerWorkerOpVirusBoom` | `ss:0` | 令 DLL 自杀（`ExitProcess`） |
| `TrainerWorkerOpVirusQuit` | `hk:ckend` | 让 DLL 优雅退出 |
| `TrainerWorkerOp1` | `hk:path:<exe>` + `hk:inipath:<ini>` | 告知 DLL 主程序路径与 INI 路径 |
| `TrainerWorkerOpForceUnLoadVirus` | — | `UnLoadAllVirus()` 远程卸载 DLL |
| `TrainerWorkerOp2/3` | — | 读极域密码（普通/破解模式） |
| `TrainerWorkerOp4/5` | — | 卸载极域保护驱动 / 网络过滤驱动 |

### 4.7 注入实现：InjectDll

经典三步式远程 DLL 注入：

```cpp
MOpenProcessNt(pid, &hRemote);
psz = VirtualAllocEx(hRemote, ..., sizeof(wchar_t)*(len+1), MEM_COMMIT, PAGE_READWRITE);
WriteProcessMemory(hRemote, psz, dllPath, ...);                  // 写入 DLL 路径
pfn = GetProcAddress(GetModuleHandle("Kernel32"), "LoadLibraryW");
CreateRemoteThread(hRemote, ..., pfn, psz, 0, NULL);             // 远程 LoadLibraryW
```

`UnInjectDll` 则相反：远程调用 `GetModuleHandleW` 取模块基址 → 远程调用 `FreeLibrary(基址)`。

---

## 5. 命令行参数

`App.cpp` 顶部 `CMD_HELP` 与 `InitArgs()` 定义了全部开关（`-x` 或 `/x` 均可）：

| 参数 | 作用 |
|---|---|
| `-install-full` | 进入安装模式（不弹协议、强制安装） |
| `-config` | 打开"更多设置"界面 |
| `-hidden` / `-h` | 隐藏模式（启动后自动最小化到托盘） |
| `-killst` | 只杀极域进程后退出 |
| `-force-md5-check` | 强制校验文件 MD5 |
| `-bugreport -bugfile <path>` | 进入 Bug 报告模式 |
| `-crash-test` | 崩溃测试（触发 `RunOperation(AppOperation3)`） |
| `-break` / `-b` | 触发 `DebugBreak`（调试用） |
| `-r1` / `-r2` / `-r3` | 重启/恢复模式标志 |
| `-f <sourcePath>` | 指定安装源路径（更新用） |
| `-rc <updaterPath>` | 启动后删除指定更新器文件 |
| `-?` | 打印命令行帮助 |

> **隐藏入口**：启动时按住 <kbd>Alt</kbd> 会进入配置模式（`appIsConfigMode = true`）。

---

## 6. 驱动装载与调用

### 6.1 DriverLoader.cpp（装载层）

| 函数 | 说明 |
|---|---|
| `MLoadKernelDriver(name, path, display)` | `CreateService(SERVICE_KERNEL_DRIVER)` + `StartService`；处理 `ERROR_SERVICE_MARKED_FOR_DELETE` 时清理注册表重来 |
| `MUnLoadKernelDriver(name)` | `ControlService(STOP)` + `DeleteService` |
| `MUnLoadDriverServiceWithMessage(name)` | 同上但带错误弹窗（用于卸载极域 `TDNetFilter` 驱动） |
| `XOpenDriver()` | `CreateFile("\\.\JKRK", ...)` 打开设备，得句柄 `hKDrv` |
| `XDriverLoaded()` | `hKDrv != NULL` |
| `XTestDriverCanUse()` | 仅 **32 位系统 + 管理员**（非 XP）可用 |
| `XLoadDriver()` | `MLoadKernelDriver` → `XOpenDriver` → `KFSendDriverinitParam(版本信息)` |
| `XInitSelfProtect()` | 调 `KFInstallSelfProtect()`（让驱动保护本进程） |
| `XCloseDriverHandle()` / `XUnLoadDriver()` | 关句柄 / 卸载驱动 |

### 6.2 KernelUtils.cpp（调用层，Ring3 → Ring0）

围绕 `DeviceIoControl(hKDrv, CTL_xxx, ...)` 封装：

| 函数 | 对应 IOCTL | 作用 |
|---|---|---|
| `KFSendDriverinitParam` | `CTL_INITPARAM` | 告知驱动 Windows 版本（XP/Win7/Build） |
| `KFInstallSelfProtect` | `CTL_INITSELFPROTECT` | 保护当前进程不被结束 |
| `KFUnInstallSelfProtect` | `CTL_CLIENT_QUIT` | 解除对本进程的保护 |
| `KForceKill(pid)` | `CTL_KILL_PROCESS` | 内核强制结束进程 |
| `KFShutdown` / `KFReboot` | `CTL_SHUTDOWN` / `CTL_REBOOT` | 内核级关机 / 重启 |
| `KFBeforeUnInitDriver` | `CTL_UNINIT` | 卸载前反初始化（摘 SSDT Hook） |

---

## 7. UDP 报文功能：JyUdpAttack

`JiYuTrainer/JyUdpAttack.h` / `.cpp`

模拟极域教师端的 UDP 报文（用于向局域网其他终端发消息 / 执行命令 / 关机 / 重启——学习报文结构用）。核心数据是四段**预置二进制模板包**：

```cpp
UCHAR basePack[4][128] = { ... };   // 均以 "DMOC" 魔数开头
#define BASE_PACK_MSG       0  // 发消息
#define BASE_PACK_CMD       1  // 执行命令（模板中可见 C:\Windows\system32\cmd.exe）
#define BASE_PACK_REBOOT    2
#define BASE_PACK_SHUTDOWN  3
```

| 公开方法 | 作用 |
|---|---|
| `ScanNetworkIP(hWnd, start, end, host, current)` | 多线程扫描网段内主机（每线程 10 个 IP，`GetMacByIP` 判存活） |
| `CheckStudentMainTCPPort(hWnd)` | 用 `GetExtendedTcpTable` 找出极域进程监听的 TCP 端口 |
| `SendCommand/SendText/SendShutdown/SendReboot` | 以模板 + 附加负载构造 UDP 包并 `sendto` |
| 私有线程 | `SendThread` / `ScanNetworkIPThread` / `ScanNetworkIPSubTaskThread` / `CheckStudentMainTCPPortThread` |

跨线程结果通过自定义消息（`WM_MY_SCAN_IP_ADD` 等）回投 UI 窗口。

---

## 8. 主程序内其他支撑类

| 类 | 文件 | 说明 |
|---|---|---|
| `Logger` / `LoggerInternal` | `Logger.h/.cpp` | 分级日志，支持控制台/文件/回调三种输出；回调模式把日志实时喂给 UI |
| `SettingHlp` / `SettingHlpInternal` | `SettingHlp.h/.cpp` | INI 读写（`GetPrivateProfileString` 系列） |
| `Path` | `PathHelper.h/.cpp` | 路径处理静态工具（跨 `LPCWSTR/std::wstring/std::string`） |
| `SysHlp` | `SysHlp.h/.cpp` | 系统版本、提权、管理员判定、64 位判定、运行程序、热键转换 |
| `NtHlp` | `NtHlp.h/.cpp` | 动态加载 `ntdll` 的 `Nt*` 函数，封装进程/线程打开与结束 |
| `RegHlp` | `RegHlp.h/.cpp` | 注册表读写（含 WOW64 双视图） |
| `StringHlp` / `StringSplit` | 同名文件 | 字符串比较/包含/编码转换；字符串切分 |
| `MD5Utils` / `md5` | 同名文件 | 文件 MD5 校验 |
| `TxtUtils` | `TxtUtils.h/.cpp` | 文本文件读写（写 bat 等） |
| `MsgCenter` | `MsgCenter.h/.cpp` | 主程序侧的 `WM_COPYDATA` 发送与窗口句柄缓存 |
| `MemoryModule` | `MemoryModule.h/.cpp` | 从内存加载 DLL（预留，当前 sciter 走磁盘加载） |
| `XZip` / `XUnzip` | 同名文件 | zip 解压（内存 zip 解出 sciter.dll） |

---

## 9. 小结（学习要点）

- 主程序用**策略接口 `JTApp`** 把"应用环境"与"业务逻辑"解耦，`TrainerWorker` 只依赖接口取路径/配置/日志。
- **两个定时器**构成心跳：一个管"进程与注入"，一个管"窗口修正"，简单但可靠。
- **兜底设计**随处可见：定位路径多路回退、杀进程 Ring3 失败降级 Ring0、可移动设备自动搬运到临时目录。
- 注入与通信是最值得学习的部分：`CreateRemoteThread(LoadLibraryW)` 注入 + 隐藏窗口 `WM_COPYDATA` 通信，是经典的 Ring3 组合拳。

---

**上一篇** → [01 · 架构总览](./01-architecture-overview.md)　|　**下一篇** → [03 · Hooks 注入模块](./03-hooks-injection-module.md)
