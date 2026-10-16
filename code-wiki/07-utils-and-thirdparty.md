# 07 · 通用工具库与第三方依赖

> 本文汇总主程序、注入模块、驱动共用的**支撑类**，以及随仓库自带的**第三方库**，便于快速定位与复用。

---

## 一、通用工具类

### 1. Logger（日志）

`JiYuTrainer/Logger.h` / `.cpp`

接口 `Logger` + 实现 `LoggerInternal`。支持三级输出目标与四级日志级别。

```cpp
enum LogLevel { LogLevelText, LogLevelInfo, LogLevelWarn, LogLevelError, LogLevelDisabled };
enum LogOutPut { LogOutPutConsolne, LogOutPutFile, LogOutPutCallback };

class Logger {
    virtual void Log(const wchar_t* fmt, ...);       // 普通文本
    virtual void LogInfo(...);  LogWarn(...);  LogError(...);
    virtual void SetLogLevel(LogLevel);
    virtual void SetLogOutPut(LogOutPut);
    virtual void SetLogOutPutFile(const wchar_t* path);
    virtual void SetLogOutPutCallback(LogCallBack cb, LPARAM lparam);
};
```

- **回调模式**把日志实时喂给 `MainWindow::WriteLogItem` 显示在界面上；
- `WritePendingLog` / `ResentNotCaputureLog`：界面未就绪时先缓存，就绪后补发；
- 宏 `LogError2/LogWarn2/...` 自动带上 `__FILE__/__LINE__/__FUNCTION__`，`PRINT_LAST_ERROR_STR`（`SysHlp.h`）输出 `错误描述 + GetLastError()`。

### 2. SettingHlp（配置）

`SettingHlp.h/.cpp` — INI 读写封装（`GetPrivateProfileString` / `WritePrivateProfileString`）。

```cpp
bool        GetSettingBool(key, default=true, appKey="JTSettings");
int         GetSettingInt (key, default=0,    appKey);
std::wstring GetSettingStr(key, default=L"", bufferSize=32, appKey);
bool        SetSettingBool/Int/Str(...);
```

默认节 `[JTSettings]`；协议同意状态存在 `[JTArgeement]`。配置文件路径为 `<exe同名>.ini`。

### 3. Path（路径工具）

`PathHelper.h/.cpp` — 静态方法集，覆盖 `LPCWSTR / std::wstring / std::string` 三套：

```cpp
GetFileName / GetFileNameWithoutExtension / GetExtension / GetDirectoryName
IsPathRooted / HasExtension / CheckInvalidPathChars / RemoveQuotes
IsValidateFolderFileName / Exists
```

内部多用 `shlwapi`（`PathRemoveFileSpec` 等）。

### 4. SysHlp（系统助手）

`SysHlp.h/.cpp`

| 方法 | 说明 |
|---|---|
| `GetSystemVersion()` | 返回 `SystemVersion` 枚举（XP/Vista/Win7+/NotSupport） |
| `GetWindowsBulidVersion()` | 取 Windows Build 号（给驱动判定偏移用） |
| `Is64BitOS()` / `IsRunasAdmin()` | 位数 / 管理员判定 |
| `EnableDebugPriv(name)` | 启用指定特权（`SE_DEBUG_NAME` 等） |
| `RunApplication(path, cmd)` | 普通启动 |
| `RunApplicationPriviledge(path, cmd)` | `ShellExecuteEx` + `runas` 提权启动 |
| `CheckIsPortabilityDevice(path)` / `CheckIsDesktop(path)` | 是否可移动设备 / 桌面 |
| `ChooseFileSingal(...)` | 文件选择对话框 |
| `HotKeyCtlToKeyCode(code, &mod, &vk)` | 配置里的热键码解析 |
| `ConvertErrorCodeToString` / `ConvertLasyErrorToString` | 错误码转文字 |
| `CopyToClipBoard` / `OpenUrl` / `GetTempPath()` | 剪贴板 / 打开 URL / 临时目录 |

### 5. NtHlp（NT 原生 API）

`NtHlp.h/.cpp` — 动态加载 `ntdll.dll` 的 `Nt*` 函数并封装进程/线程操作（避开 Win32 层的部分限制）。

`MLoadNt()` 载入：`NtOpenProcess`、`NtOpenThread`、`NtTerminateProcess/Thread`、`NtSuspend/ResumeProcess/Thread`、`NtQuerySystemInformation/InformationProcess/InformationThread`、`NtClose`。

封装函数：

| 函数 | 说明 |
|---|---|
| `MOpenProcessNt(dwId, &handle)` | `NtOpenProcess(PROCESS_ALL_ACCESS)` |
| `MTerminateProcessNt(dwId, handle)` | 结束进程 |
| `MOpenThreadNt` / `MTerminateThreadNt` | 线程打开/结束 |
| `MSuspendProcessNt` / `MResumeProcessNt` | 挂起/恢复进程 |
| `MResumeThreadNt` / `MSuspendThreadNt` | 挂起/恢复线程 |
| `MGetProcessFullPathEx(handle, path)` | 取进程完整路径（基于 `ProcessImageFileNameWin32`） |
| `MQueryProcessVariableSize(...)` | 查询可变长度进程信息 |

头文件内置 `NTSTATUS` 宏、`UNICODE_STRING` / `OBJECT_ATTRIBUTES` / `CLIENT_ID` 结构、`SYSTEM_PROCESSES` 结构、`SYSTEM_INFORMATION_CLASS` 与 `PROCESSINFOCLASS` 全量枚举。

### 6. RegHlp（注册表）

`RegHlp.h/.cpp` — 含 `MRegReadKeyString64And32`（同时按 64/32 位视图读）、`MRegCheckUninstallItemExists`、`MRegForceDeleteServiceRegkey`（强删服务键，用于驱动卸载兜底）。

### 7. StringHlp / StringSplit（字符串）

- `StringHlp`（主程序版 `StringHlp.cpp` 460 行 / DLL 版 `JiYuTrainerHooks/StringHlp.cpp` 450 行）：`StrEqualW/A`、`StrContainsW/A`、`StrLoW/A`（转小写）、`AnsiToUnicode` / `UnicodeToUtf8`、`FreeStringPtr`、`StrEmepty`。
- `StringSplit`：`SplitString(str, outVec, delim)` —— IPC 文本协议解析的基础。

### 8. MD5Utils / md5（校验）

`MD5Utils.h/.cpp` + `md5.h/.cpp` — 计算文件 MD5（用于 `-force-md5-check` 与完整性校验）。

### 9. TxtUtils / NetUtils / MsgCenter / CAutoLock

| 类 | 文件 | 说明 |
|---|---|---|
| `TxtUtils` | `TxtUtils.h/.cpp` | 文本文件读写（写 `.bat` 等） |
| `NetUtils` | `NetUtils.h/.cpp` | `GetIP` / `GetMacByIP`（`SendARP`）/ `GetHostNameByIP` / `IPToStringW` |
| `MsgCenter` | `MsgCenter.h/.cpp` | 主程序侧 `WM_COPYDATA` 发送 + 窗口句柄缓存列表 |
| `CAutoLock` | `CAutoLock.h/.cpp` | `CRITICAL_SECTION` 封装，构造加锁析构解锁 |

### 10. MemoryModule（内存加载 DLL）

`MemoryModule.h/.cpp`（fancycode 的 MemoryModule，约 1200 行）— 从内存缓冲区加载 DLL 而不落盘：`MemoryLoadLibrary` / `MemoryGetProcAddress` / `MemoryFreeLibrary`。当前主要用于 sciter 的备用加载路径（`App.cpp` 中已被注释，改走磁盘加载）。

### 11. XZip / XUnzip（zip 解压）

`XZip.h/.cpp`（约 3200 行）+ `XUnzip.h/.cpp`（约 4300 行）— 支持内存 zip 与文件 zip 的打开、查找、解压。用于把 exe 内嵌的 `sciter.zip` 资源中的 `sciter.dll` 解出来。

```cpp
hZip = OpenZip(pData, dwSize, ZIP_MEMORY);
FindZipItem(hZip, L"sciter.dll", false, &index, &entry);
UnzipItem(hZip, index, hFile, 0, ZIP_HANDLE);
```

### 12. VersionHelpers（系统版本辅助）

`VersionHelpers.h` — Windows 版本判定的内联辅助（微软官方同名头文件的精简版）。

---

## 二、第三方库（随仓库自带，无需单独安装）

| 库 | 版本/来源 | 位置 | 作用 |
|---|---|---|---|
| **mhook** | martona/mhook | `JiYuTrainerHooks/mhook-lib` | 用户层 API Hook 引擎，改写函数头为跳转 |
| **disasm-lib** | LDE 长度反汇编器 | `JiYuTrainerHooks/disasm-lib` | mhook 的依赖，识别被覆盖指令长度 |
| **Sciter SDK** | Terra Informatica | `JiYuTrainerUI/sciter-sdk` | HTML/CSS 界面渲染引擎 |
| **curl** | curl/curl | `JiYuTrainerUpdater/curl` | HTTP 同步/异步下载 |
| **MemoryModule** | fancycode | `JiYuTrainer/MemoryModule.*` | 内存加载 DLL |
| **XZip/XUnzip** | yuanjia1011 | `JiYuTrainer/XZip.*` `XUnzip.*` | zip 解压 |
| **Jiyu_udp_attack 原理** | ht0Ruial | 参考实现（`JyUdpAttack.*` 为自研） | 极域 UDP 报文格式参考 |

> 说明：`mhook`、`disasm-lib`、`curl`、`sciter-sdk` 是**以源码/静态库形式内嵌**的第三方目录，不是 NuGet 包。

---

## 三、工具类的复用价值（学习视角）

| 想学的技术 | 对应文件 |
|---|---|
| 通用日志系统（分级 + 多输出 + 回调） | `Logger.h/.cpp` |
| INI 配置读写 | `SettingHlp.h/.cpp` |
| 绕过 Win32 限制的 NT 层进程/线程操作 | `NtHlp.h/.cpp` |
| 提权启动（`runas`） | `SysHlp.cpp::RunApplicationPriviledge` |
| 内存解压 zip 并加载 DLL | `XZip.cpp` + `MemoryModule.cpp` |
| 路径字符串跨编码处理 | `PathHelper.cpp` / `StringHlp.cpp` |
| 简单可靠的 IPC（`WM_COPYDATA`） | `MsgCenter.cpp` + `JiYuTrainerHooks.cpp::VHandleMsg` |

---

**上一篇** → [06 · 更新器](./06-updater.md)　|　**下一篇** → [08 · 进程间通信与数据流](./08-ipc-and-dataflow.md)
