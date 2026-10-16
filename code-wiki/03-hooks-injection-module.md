# 03 · 注入模块 JiYuTrainerHooks

> 工程目录：`JiYuTrainerHooks/`　产物：`JiYuTrainerHooks.dll`
> 这是被注入到极域进程（`StudentMain.exe`）里的 DLL，代码中自称 "Virus"（病毒）。
> 它是**改窗口、反监视、防控制、拦关机**等全部核心能力的实现地。约 2100 行，是项目最核心的模块。

---

## 1. 模块入口与"分身"机制

### 1.1 DllMain → VLoad

`JiYuTrainerHooks/dllmain.cpp`

```cpp
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { hInst = hModule; VLoad(); }
    if (reason == DLL_PROCESS_DETACH) { VUnloadAll(); }
    return TRUE;
}
```

`VLoad()`（`JiYuTrainerHooks.cpp`）先 `VParamInit()`（取屏幕尺寸、创建右键菜单），再**按宿主 exe 名字分发**：

| 宿主进程名 | 走的分支 | 目的 |
|---|---|---|
| `StudentMain.exe` | 检查是否真的极域（须存在 `LibTDMaster.dll`）→ `VRunMain()` | 主要目标 |
| `JiYuTrainer.exe` | `VLoadMainProtect()`（当前为空实现） | 保护主程序 |
| `MasterHelper.exe` | `VInstallHooks(VirusModeMaster)` | 只做少量 Hook |
| `ProcHelper64.exe` | `VInstallHooks(VirusModeMaster)` | 同上 |

> 若宿主是 `StudentMain.exe` 但目录里没有 `LibTDMaster.dll`，则判定"混进了奇怪的东西"，回发 `hkb:wtf:<pid>` 并自我卸载（`VUnloadAll` + `FreeLibrary`）。

`VRunMain()` 三步走：

```cpp
desktopWindow = GetDesktopWindow();
VCreateMsgCenter();          // 起隐藏窗口 + 消息循环线程
VOpenFuckDrivers();          // 提前打开极域三个设备句柄
VInstallHooks(VirusModeHook);// 装 30+ 个 API Hook
VSendMessageBack(L"hkb:succ", hWndMsgCenter); // 通知主程序"接管成功"
```

### 1.2 病毒模式枚举

```cpp
enum VirusMode { VirusModeProtet, VirusModeHook, VirusModeMaster, VirusModeReplaceStub };
```

---

## 2. 消息中心（进程内隐藏窗口）

`VCreateMsgCenter()` 起一条线程 `VMsgCenterRunThread`，在桌面窗口上创建两个隐藏对话框：

| 窗口 | 资源 | 用途 |
|---|---|---|
| `IDD_MSGCT`（`MainWndProc`） | 标题设为 `JiYu Trainer Virus Window` | 接收主程序 `WM_COPYDATA`，并显示一个可拖动的状态小窗 |
| `IDD_FAKEDESKTOP`（`FakeDesktopWndProc`） | 假的"桌面窗口" | 反监视用的替身，见 §5 |

主程序通过 `FindWindow(NULL, L"JiYu Trainer Virus Window")` 找到它并投递消息（见 [08-IPC](./08-ipc-and-dataflow.md)）。

`MainWndProc` 还负责：
- `WM_COPYDATA` → `VHandleMsg(recvData)`：解析主程序指令；
- `WM_TIMER TIMER_WATCH_DOG_SRV`(6666ms)：回发 `wcd:<n>` 看门狗心跳；
- `WM_TIMER TIMER_AUTO_HIDE`(2600ms)：自动把自己藏到屏幕外 / 隐藏；
- 状态图标红/绿/灰切换（`IDC_SW_STATUS_*`）。

---

## 3. API Hook 的安装

### 3.1 依赖 mhook

Hook 引擎是第三方 **mhook**：`Mhook_SetHook((PVOID*)&原函数指针, 挂钩函数)` 会把目标函数入口改写为跳转；`Mhook_Unhook` 还原。为了安全改写机器码，mhook 依赖 **disasm-lib**（长度反汇编器）来算出被覆盖指令的完整长度。

`VInstallHooks(VirusMode mode)` 的核心就是 `GetProcAddress` 拿到原始函数地址后逐个 `Mhook_SetHook`。

### 3.2 Hook 清单（VirusModeHook）

以 `hk1..hk40` 编号记录安装结果，主要钩子如下（节选 + 归类）：

| 编号 | 被 Hook 的 API | 所属 DLL | 挂钩函数 | 目的（归类） |
|:--:|---|---|---|---|
| hk1 | `SetWindowPos` | user32 | `hkSetWindowPos` | **改窗口**（核心） |
| hk2 | `MoveWindow` | user32 | `hkMoveWindow` | 改窗口 |
| hk9 | `DeferWindowPos` | user32 | `hkDeferWindowPos` | 改窗口 |
| hk18 | `ShowWindow` | user32 | `hkShowWindow` | 改窗口 |
| hk16/17 | `SetWindowLongA/W` | user32 | `hkSetWindowLongA/W` | 改窗口 |
| hk35/36 | `SetClassLongA/W` | user32 | `hkSetClassLongA/W` | 去 `CS_NOCLOSE` |
| hk34 | `EnableMenuItem` | user32 | `hkEnableMenuItem` | 禁掉 `SC_CLOSE`（保窗口） |
| hk3 | `SetForegroundWindow` | user32 | `hkSetForegroundWindow` | 阻止抢前台 |
| hk4 | `BringWindowToTop` | user32 | `hkBringWindowToTop` | 阻止抢前台 |
| hk31 | `GetForegroundWindow` | user32 | `hkGetForegroundWindow` | 反监视/防控制 |
| hk10 | `SendInput` | user32 | `hkSendInput` | **防控制**（拦模拟输入） |
| hk11 | `mouse_event` | user32 | `hkmouse_event` | 防控制 |
| hk8 | `SetWindowsHookExA` | user32 | `hkSetWindowsHookExA` | **防极域 Hook 键盘鼠标** |
| hk25 | `CallNextHookEx` | user32 | `hkCallNextHookEx` | 配合上者 |
| hk29 | `UnhookWindowsHookEx` | user32 | `hkUnhookWindowsHookEx` | 配合上者 |
| hk19 | `ExitWindowsEx` | user32 | `hkExitWindowsEx` | **拦关机/重启** |
| hk22 | `CreateProcessW` | kernel32 | `hkCreateProcessW` | 拦命令行执行 |
| hk30 | `CreateProcessA` | kernel32 | `hkCreateProcessA` | 拦命令行执行 |
| hk24 | `WinExec` | kernel32 | `hkWinExec` | 拦命令行执行 |
| hk6/7 | `CreateFileA/W` | kernel32 | `hkCreateFileA/W` | **断极域设备访问** |
| hk5 | `DeviceIoControl` | kernel32 | `hkDeviceIoControl` | 断极域设备访问 |
| hk40 | `FilterConnectCommunicationPort` | FltLib | `hkFilterConnectCommunicationPort` | 断文件过滤 |
| hk39 | `TerminateProcess` | kernel32 | `hkTerminateProcess` | 防被极域杀 |
| hk37 | `PostMessageW` | user32 | `hkPostMessageW` | **保广播窗口**（拦关闭消息） |
| hk38 | `SendMessageW` | user32 | `hkSendMessageW` | 保广播窗口 |
| hk27 | `GetDesktopWindow` | user32 | `hkGetDesktopWindow` | **反监视**（返回假桌面） |
| hk26 | `GetWindowDC` | user32 | `hkGetWindowDC` | 反监视 |
| hk33 | `CreateDCW` | gdi32 | `hkCreateDCW` | 反监视（拦截屏） |
| hk28 | `EncodeToJPEGBuffer` | LibJPEG20 | `hkEncodeToJPEGBuffer` | 反监视（拦截图编码） |
| hk23 | `DwmEnableComposition` | dwmapi | `hkDwmEnableComposition` | 反监视 |

`VUnInstallHooks()` 按同样编号逐一 `Mhook_Unhook`。

> `VirusModeMaster` 只装 3 个（`hk8` `hk19` `hk22`），并额外 `SetWindowsHookExA(WH_CBT, VCBTProc, ...)` 占位。

---

## 4. 关键 Hook 语义详解

### 4.1 窗口降级（改窗口）

`hkSetWindowPos` / `hkDeferWindowPos` / `hkMoveWindow` 的共同逻辑：判断调用是否想把窗口设成 **全屏**（`x==0 && y==0 && cx==screenWidth && cy==screenHeight`）：

```cpp
if (整屏尺寸) {
    VSwitchLockState(hWnd, TRUE);                 // 判定为"被锁定/被控制"
    if (fakeFull && VIsWindowGbOrHp(hWnd)) return 原函数(...); // 伪装全屏模式下放行
    if (VIsInIllegalCanSizeWindows(hWnd)) { ...; return TRUE; } // 放行但去掉置顶
    return TRUE;                                    // 否则直接吞掉这次全屏请求
} else {
    VSwitchLockState(hWnd, FALSE);
    if (fakeFull) return 原函数(hWnd, HWND_NOTOPMOST, ...);
}
if (VIsInIllegalWindows(hWnd)) return TRUE;         // 极域窗口的操作统一吞掉
```

效果：极域想把广播窗口拉成全屏 → 被吞掉；窗口保持窗口化，学生可自由操作。

### 4.2 反监视（假桌面）

极域靠抓取"桌面窗口"画面来监视学生。Hook 思路是让极域拿到**另一个桌面窗口**：

```cpp
HWND hkGetDesktopWindow(void) {
    if (loaded) return allowMonitor ? desktopWindow : fakeDesktopWindow; // 假桌面
    return desktopWindow;
}
HDC hkGetWindowDC(HWND hWnd) {
    if (loaded && hWnd == desktopWindow && !allowMonitor)
        return faGetWindowDC(fakeDesktopWindow);   // 换成假桌面的 DC
    return faGetWindowDC(hWnd);
}
BOOL hkEncodeToJPEGBuffer(...) { if (!allowMonitor) return FALSE; ... } // 截屏编码直接失败
HDC hkCreateDCW(..., pwszDriver, ...) {
    if (isGbFounded) return 原函数(...);           // 正在广播时不拦，便于看老师画面
    if (!allowMonitor && StrEqual(pwszDriver, L"DISPLAY")) return NULL;
    ...
}
```

`allowMonitor` 由配置控制（`AllowMonitor`，默认 TRUE 即允许监视）。

### 4.3 防控制（拦模拟输入）

```cpp
VOID hkmouse_event(...) { if (allowControl) 原函数(...); }         // 默认丢弃
UINT hkSendInput(...)   { if (!allowControl && loaded) return cInputs; ... }
HWND hkGetForegroundWindow() { return allowAllRunOp ? 原函数() : NULL; }
```

`allowControl` 默认 FALSE，教师端通过 `SendInput`/`mouse_event` 控制鼠标键盘会被丢弃。

### 4.4 防极域安装键盘/鼠标钩子

`hkSetWindowsHookExA` 专门拦截**来自 `LibTDMaster.dll`** 的低级鼠标/键盘钩子：

```cpp
if (idHook == WH_CBT) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
if (WH_MOUSE_LL/WH_MOUSE/WH_KEYBOARD_LL && hmod == hLibTDMaster) {
    // 记录 lpfn，改用我们自己的 LowLevelMouseProc/LowLevelKeyboardProc 装上，并返回伪句柄
    lpMouseHookfn = lpfn; hMouseHook = faSetWindowsHookExA(idHook, LowLevelMouseProc, hmod, dwThreadId);
    return (HHOOK)INVALID_HHOOK_MOUSE;  // 返回 0x0001/0x0002 假句柄
}
```

配合 `hkCallNextHookEx`（收到假句柄返回 0）与 `hkUnhookWindowsHookEx`（把假句柄映射回真句柄），实现"钩子看似装上、实则空转"。

### 4.5 拦关机/重启与命令行

```cpp
BOOL hkExitWindowsEx(UINT uFlags, ...) {
    // 弹窗询问"极域试图关闭/重启您的计算机，是否允许？" 选否则返回 FALSE
}
BOOL hkCreateProcessW(...) {  // 识别 shutdown.exe / tdchalk.exe 等
    // 白名单(jiyutrainer/explorer/输入法…)放行；否则弹窗询问或按 BandAllRunOp 拦截
}
UINT hkWinExec(LPCSTR lpCmdLine, ...) { /* 同上逻辑，返回值 32 */ }
```

### 4.6 保广播窗口（不允许被关）

```cpp
BOOL hkPostMessageW(HWND hWnd, UINT Msg, ...) {
    if (loaded && hWnd == mainWindow) return 0;         // 不打扰主程序窗口
    if (loaded && (Msg==WM_CLOSE||Msg==WM_DESTROY) && ProhibitCloseWindow) {
        DWORD pid; GetWindowThreadProcessId(hWnd, &pid);
        if (pid != currentPid) return 0;                // 别的进程想关极域窗口 → 拦掉
    }
    return 原函数(...);
}
```

另有 `hkEnableMenuItem`（干掉 `SC_CLOSE`）与 `hkSetClassLongA/W`（去掉 `CS_NOCLOSE`，让窗口可正常关闭——用于我们自己加的菜单）。

### 4.7 断极域设备与文件过滤

```cpp
void VOpenFuckDrivers() {   // 提前打开三个极域设备句柄
    hDeviceTDNetFilter = CreateFileW(L"\\\\.\\TDNetFilter", ...);
    hDeviceTDKeybd     = CreateFileW(L"\\\\.\\TDKeybd", ...);
    hDeviceTDProcHook  = CreateFileW(L"\\\\.\\TDProcHook", ...);
}
HANDLE hkCreateFileW(LPCWSTR lpFileName, ...) {  // 极域再想打开这三个设备 → ACCESS_DENIED
    if (loaded && (等于 TDKeybd/TDProcHook/TDNetFilter)) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
}
BOOL hkDeviceIoControl(HANDLE hDevice, ...) {   // 对已打开的句柄 → 直接拒绝
    if (hDevice == hDeviceTDKeybd) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } ...
}
HRESULT hkFilterConnectCommunicationPort(LPCWSTR lpPortName, ...) {
    if (loaded && StrEqual(lpPortName, L"\\TDFileFilterPort")) return S_FALSE;   // 断文件过滤通信
}
```

原理：**先自己占用**这三个设备句柄，再让极域后续的 `CreateFile`/`DeviceIoControl` 全部失败，从而屏蔽键盘拦截驱动、进程 Hook 驱动、网络过滤驱动。

---

## 5. 窗口子类化（WNDPROC Hook）

除了 API Hook，模块还用**子类化**直接接管极域广播窗口的窗口过程。

### 5.1 接管广播窗口：JiYuWndProc

`VFixGuangBoWindow(hWnd)`：
1. 用 `SetWindowLong(hWnd, GWL_WNDPROC, JiYuWndProc)` 把极域广播窗的过程替换成 `JiYuWndProc`（旧过程存 `jiYuWndProc`）；
2. 加 `WS_OVERLAPPEDWINDOW | WS_SYSMENU`（可缩放、有系统菜单）；
3. 往系统菜单追加两项：`idm_full 广播窗口全屏`、`idm_topmost 广播窗口置顶`；
4. 去掉类样式 `CS_NOCLOSE`（允许关闭）。

`JiYuWndProc` 处理的消息：

| 消息 | 处理 |
|---|---|
| `WM_GETMINMAXINFO` | 允许窗口缩到 0 尺寸 |
| `WM_SIZE` | 找到子窗口 `TDDesk Render Window`（极域真正的画面渲染窗）并 `MoveWindow` 贴合客户区；新版本还会找工具栏 `jiYuGBToolWnd` 计算高度 |
| `WM_SHOWWINDOW` | 显示时把自己居中、设为 3/4 宽 × 4/5 高 |
| `WM_DESTROY` | 从列表移除，恢复锁定状态 |
| `WM_COMMAND` / `WM_SYSCOMMAND` | 处理"全屏/置顶/关闭广播/帮助"菜单，并把结果回发主程序（`hkb:gbmfull` 等） |
| `WM_KEYUP` | 按 ESC 退出伪装全屏 |
| `WM_RBUTTONUP` | 弹出我们自定义的右键菜单 `hMenuGb`（含全屏/置顶/关闭/帮助） |

### 5.2 接管渲染窗：JiYuTDDeskWndProc

`TDDesk Render Window` 若尺寸与父窗口客户区不一致就 `MoveWindow` 贴合；右键/按键转发给父窗口。

### 5.3 右键菜单 VParamInit

```cpp
hMenuGb = CreatePopupMenu();
AppendMenu(..., L"JiYuTrainer 添加的菜单");
AppendMenu(..., L"JiYuTrainer 帮助");
AppendMenu(..., L"广播窗口全屏");      // IDM_FULL
AppendMenu(..., L"广播窗口置顶");      // IDM_TOPMOST
AppendMenu(..., L"关闭广播窗口");      // IDM_CLOSE_GB
```

这是"用户能手动把广播窗切回全屏/置顶/关掉"的入口，与主程序的 `ManualFull/ManualTop` 联动。

---

## 6. 版本适配

```cpp
void VGetStudentainVersion() {
    // 用 GetFileVersionInfo/VerQueryValue 读宿主 exe 的 ProductVersion
    if (版本 == "5.01 Baseline")  jiYuVersion = jiYuVersions40;        // 极域 V4 2010
    if (版本 == "2.07 CMPC")      jiYuVersion = jiYuVersions402016HH;  // 极域 V6.0 2016 豪华版
}
```

不同版本工具栏/子窗口结构不同，`WM_COMMAND` 里对 `jiYuVersions402016HH` 有额外处理（按钮 id `0x07E5` 置顶、`0x03EC` 全屏），`WM_RBUTTONUP` 对 V4 版本限制菜单弹出区域。

---

## 7. 键盘解锁：VUnHookKeyBoard

极域用 `LibTDMaster.dll` 的钩子锁键盘。本模块直接调用其导出：

```cpp
HMODULE h = GetModuleHandle(L"LibTDMaster.dll");
fnUnLockLocalInput UnLockLocalInput = GetProcAddress(h, "UnLockLocalInput");
if (UnLockLocalInput) UnLockLocalInput();
else { fnDoneHook DoneHook = GetProcAddress(h, "DoneHook"); if (DoneHook) DoneHook(); }
```

---

## 8. LibTDAjust.dll 替换（VLoadRealVirus）

极域组件 `LibTDAjust.dll` 被本模块"顶替"：真身被改名备份为 `LibTDAjust.dll.bak.dll`，本 DLL 伪装成 `LibTDAjust.dll` 并导出转发函数：

```cpp
EXTERN_C HRESULT __declspec(dllexport) __cdecl TDAjustCreateInstance(CLSID*, ...) {
    return faTDAjustCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv); // 转发给真身
}
```

即"狸猫换太子"：极域调用 `TDAjustCreateInstance` 时先经过我们，便于拦截/旁路。

---

## 9. 消息协议处理：VHandleMsg

DLL 侧解析来自主程序的 `MSG:...` 文本（冒号分隔）：

| 消息 | 处理 |
|---|---|
| `hw:<hwnd>` / `hwf:<hwnd>` | 主程序发现一个极域窗口 → `VHookWindow`/`VHookFWindow` 接管其过程并记入 `jiYuWnds` |
| `sh:<hwnd>` | 记录主程序窗口句柄 `mainWindow` |
| `ss:0` | `VBoom()` → `ExitProcess(0)` 自杀（被主程序"杀毒"） |
| `ss2:0` | 同上（退出） |
| `hk:ckstat` | 查询状态：读版本、解键盘锁、回发 `hkb:succ` |
| `hk:ckend` | `VManualQuit()` 优雅退出 |
| `hk:path:...` / `hk:inipath:...` | 收到主程序 exe / ini 路径，`VLaterInit()` 加载配置与白名单 |
| `hk:reset` | 重读配置 |
| `hk:fkfull:true/false` | 切换伪装全屏 |
| `ukt` | 尝试解锁键盘（`UnLockLocalInput`） |
| `test` / `test2` | 调试命令 |

`VOutPutStatus(fmt, ...)` 把带时间戳的日志打到隐藏窗口的列表框中。

---

## 10. 配置项（INI [JTSettings]）

`VInitSettings()` 读取（由主程序 `-r3` 重启模式传入，见 `SaveSettings`）：

| 键 | 默认 | 作用 |
|---|---|---|
| `AutoForceKill` | FALSE | 与主程序失联时是否自动强杀 |
| `AllowAllRunOp` | FALSE | 是否放行极域所有命令行执行 |
| `BandAllRunOp` | TRUE | 是否默认禁止极域执行程序 |
| `ProhibitKillProcess` | TRUE | 是否阻止极域杀本进程 |
| `ProhibitCloseWindow` | TRUE | 是否阻止关广播窗口 |
| `DoNotShowVirusWindow` | TRUE | 是否隐藏状态小窗 |
| `ForceDisableWatchDog` | FALSE | 是否关闭看门狗心跳 |
| `AllowGbTop` | FALSE | 是否允许广播窗置顶 |
| `AllowMonitor` | TRUE | 是否允许被监视 |
| `AllowControl` | FALSE | 是否允许被控制 |

---

## 11. 小结（学习要点）

- **API Hook + 子类化**双管齐下：前者拦系统调用，后者直接接管目标窗口消息。
- **"用句柄占位"**思路：先自己打开极域设备，再让极域打不开——比写驱动更轻量。
- **返回假句柄**思路：`SetWindowsHookExA` 返回自有编号，配合 `CallNextHookEx`/`UnhookWindowsHookEx` 做完整闭环。
- **状态机 + 回调**：`VSwitchLockState` 把"是否被锁定/全屏"的变化双向同步给主程序，形成闭环控制。
- mhook + disasm-lib 说明 Hook 需要**指令长度识别**才能安全改写函数头，这是用户层 Hook 的通用难点。

---

**上一篇** → [02 · 主程序](./02-host-main-program.md)　|　**下一篇** → [04 · 内核驱动](./04-kernel-driver.md)
