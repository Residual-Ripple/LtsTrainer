# 08 · 进程间通信与端到端数据流

> 主程序（Host）与注入 DLL（Virus）是两个不同进程，需要一个稳定的通信通道。
> JiYuTrainer 选择了最轻量的方案：**隐藏窗口 + `WM_COPYDATA` 传递文本命令**。
> 本文说明这套协议、双方约定，以及一次完整交互的数据流。

---

## 1. 通信端点（两个隐藏窗口）

| 窗口标题 | 由谁创建 | 谁发送 | 谁接收 |
|---|---|---|---|
| `JiYu Trainer Main Window` | 主程序 `MainWindow` | DLL（`VSendMessageBack`） | 主程序 |
| `JiYu Trainer Virus Window` | DLL `MainWindow`（`IDD_MSGCT`） | 主程序（`MsgCenterSendToVirus`） | DLL |

两端都通过 `FindWindow(NULL, 标题)` 定位对方，再 `SendMessageTimeout(..., WM_COPYDATA, ...)` 投递。

### 发送封装（对称的两段代码）

**主程序 → DLL**（`MsgCenter.cpp`）：

```cpp
void MsgCenterSendToVirus(LPCWSTR buff, HWND form) {
    HWND w = FindWindow(NULL, L"JiYu Trainer Virus Window");
    if (w) {
        COPYDATASTRUCT cds = { 0 };
        cds.lpData = (PVOID)buff;
        cds.cbData = sizeof(WCHAR) * (wcslen(buff) + 1);
        SendMessageTimeout(w, WM_COPYDATA, (WPARAM)form, (LPARAM)&cds,
                           SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, 0);
    }
}
```

**DLL → 主程序**（`JiYuTrainerHooks.cpp::VSendMessageBack`）：

```cpp
HWND w = FindWindow(NULL, L"JiYu Trainer Main Window");
if (w) { ... SendMessageTimeout(w, WM_COPYDATA, (WPARAM)hDlg, (LPARAM)&cds, SMTO_NORMAL, 500, 0); }
else { /* 失联：UI 变红；forceKill 则自杀并拉起主程序 -r2/-r3 */ }
```

- `cbData` 以 `sizeof(WCHAR)*(len+1)` 计算，**包含结尾 `\0`**；
- 接收端用 `wcsncpy_s(recvData, lpData, cbData)` 拷贝到 `WCHAR[256]` 缓冲再解析；
- `SendMessageTimeout` 带 500ms 超时与 `SMTO_ABORTIFHUNG`，避免对方卡死拖垮自己。

---

## 2. 命令文本协议

协议格式：**`字段:字段:...`**，用 `SplitString(str, arr, L":")` 切成 `vector<wstring>`。
`arr[0]` 是命令组，`arr[1]+` 是参数。约定：

- 路径类参数因含盘符冒号（`C:\...`），发送时被拆成 `path: C : \xxx`，接收端再把后续字段用 `:` 拼回（见 `VHandleMsg` 的 `path` / `inipath` 分支）。

### 2.1 主程序 → DLL 的命令

| 命令示例 | 触发 | DLL 侧动作 |
|---|---|---|
| `hw:<hwnd>` | 发现极域窗口 | 接管其 WNDPROC，记入 `jiYuWnds` |
| `hwf:<hwnd>` | 同上（另一种） | 同上 |
| `sh:<hwnd>` | 告知主窗口句柄 | 记 `mainWindow`，用于判断"别关它" |
| `ss:0` | 杀毒/自杀 | `VBoom()` → `ExitProcess(0)` |
| `ss2:0` | 退出 | `PostQuitMessage` + `ExitProcess` |
| `hk:ckstat` | 查询状态 | 读版本、解锁键盘、回 `hkb:succ` |
| `hk:ckend` | 优雅退出 | `VManualQuit()` |
| `hk:path:<p1>:<p2>` | 下发主程序 exe 路径 | 记录 `mainFullPath` |
| `hk:inipath:<p1>:<p2>` | 下发 ini 路径 | 记录 `mainIniPath` 并 `VLaterInit()` |
| `hk:reset` | 重读配置 | `VInitSettings()` |
| `hk:fkfull:true/false` | 切换伪装全屏 | 更新 `fakeFull` |
| `ukt` | 解锁键盘 | 调 `UnLockLocalInput`/`DoneHook` |
| `test` / `test2` | 调试 | 弹确认框 / 触发状态闪烁 |

### 2.2 DLL → 主程序的命令

| 命令示例 | 含义 | 主程序侧处理 |
|---|---|---|
| `hkb:succ` | 注入成功、已接管 | 置"已控制"，回发 `hs:<hwnd>` |
| `hkb:immck` | 要求立即检查窗口 | `RunCk()` |
| `hkb:jyk:1/0` | 锁屏状态变化 | 更新 UI 锁定指示 |
| `hkb:wtf:<pid>` | 宿主不是极域 | 过滤该 PID 并复位 |
| `hkb:algbtop` | 请求允许广播置顶 | `OnAllowGbTop()`（更新设置） |
| `hkb:gbmfull` / `hkb:gbmnofull` | 广播进/出全屏 | `ManualFull(true/false)` |
| `hkb:gbtop` / `hkb:gbuntop` | 广播置顶/取消 | `ManualTop(true/false)` |
| `hkb:showhelp` | 请求显示帮助 | `OnShowHelp()` |
| `wcd:<n>` | 看门狗心跳 | 心跳计数与日志 |
| `vback:...` | 调试回显 | 记日志 |

> 命令组前缀含义：`hk`/`hkb` = Hook（病毒主控）；`hw`/`hwf` = 窗口句柄；`ss` = 自毁；`ukt` = unlock keyboard；`wcd` = watchdog；`vback` = virus back。

---

## 3. 端到端数据流（时序）

### 3.1 启动 → 注入 → 接管

```
主程序                                极域 StudentMain.exe           内核驱动
  │                                          │                        │
  ├─ CheckAndInstall 释放 Hooks.dll/sciter.dll                        │
  ├─ LoadDriver ──────── DeviceIoControl(CTL_INITPARAM) ─────────────►│
  │                                        │                         ├─ 保护本进程
  ├─ 起 TIMER_RESET_PID(2s) / TIMER_CK(3.1s)│                        │
  │                                        │                         │
  ├─ RunResetPid: NtQuerySystemInformation → 找到 StudentMain.exe      │
  ├─ InjectDll: VirtualAllocEx/WriteProcessMemory/CreateRemoteThread ──┤
  │                                        │  LoadLibrary(Hooks.dll)  │
  │                                        ├─ DllMain → VLoad → VRunMain
  │                                        ├─ 创建隐藏窗口 "JiYu Trainer Virus Window"
  │                                        ├─ VInstallHooks(30+ API)
  │  ◄──── WM_COPYDATA "hkb:succ" ─────────┤
  ├─ 置 _StudentMainControlled=true
  ├─ RunOperation(Op1) ── WM_COPYDATA "hk:path:.../inipath:..." ──────►│
  │                                        ├─ 记路径、读配置
  │                                        │
  ├─ RunCk: EnumDesktopWindows 找极域窗口                                │
  ├─ MsgCenterSendHWNDS ── WM_COPYDATA "hw:<hwnd>" × N ───────────────►│
  │                                        ├─ VHookWindow: 子类化窗口
  │                                        │   VFixGuangBoWindow → 去全屏、加边框
  │  ◄──── "hkb:immck" / "hkb:jyk:1" ──────┤
  └─ UpdateState() → UI 刷新「已保护」
```

### 3.2 教师广播 → 窗口降级

```
教师端下发全屏广播
  │
极域进程调用 SetWindowPos(hwnd, ..., cx=屏宽, cy=屏高, ...)   ← 被 hkSetWindowPos 截获
  ├─ 判定为整屏尺寸 → VSwitchLockState(TRUE)
  ├─ fakeFull? 否 → 直接 return TRUE（吞掉这次全屏）
  └─ 窗口保持窗口化，学生可自由操作
主程序侧：收到 hkb:jyk 等状态 → UI 显示「极域尝试锁定，已还原窗口」
```

### 3.3 老师广播窗口的"手动全屏/置顶"

```
学生在广播窗右键 → 自定义菜单 hMenuGb（JiYuWndProc 里 TrackPopupMenu）
  ├─ 选「广播窗口全屏」→ VSendMessageBack("hkb:gbmfull") → 主程序 ManualFull(true)
  └─ 选「广播窗口置顶」→ 若不允许则先请求： "hkb:algbtop" → 主程序 OnAllowGbTop()（写配置）
```

### 3.4 杀进程（Ring3 → Ring0 降级）

```
点击「杀死极域」
  ├─ MOpenProcessNt + MTerminateProcessNt   ← 成功则结束
  └─ 若 STATUS_ACCESS_DENIED
        ├─ 弹窗询问是否用驱动强杀
        └─ KForceKill(pid) → DeviceIoControl(CTL_KILL_PROCESS) → 驱动 KillProcess/ZeroKill
```

### 3.5 主程序退出 / 失联处理

```
主程序关闭 → RunOperation(VirusQuit) 发 "hk:ckend" → DLL VManualQuit() 卸载 Hook
若主程序异常退出（DLL 找不到 "JiYu Trainer Main Window"）
  ├─ 状态图标变红，outlineEndJiy = true
  ├─ forceKill? → ShellExecute(mainFullPath, "-r2") 重拉主程序并 VBoom() 自杀
  └─ 否则弹窗让用户选择：结束 DLL / 重启主程序(-r3)
```

---

## 4. 其他跨进程/跨线程通道

| 通道 | 双方 | 机制 |
|---|---|---|
| UDP 报文 | 本机 → 局域网终端 | `JyUdpAttack` 原始 UDP `sendto` |
| UI 线程 ↔ 扫描线程 | 同进程 | 自定义消息 `WM_MY_SCAN_IP_ADD` / `WM_MY_SCAN_IP_FINISH` / `WM_MY_SEND_ADD_RESULT` / `WM_MY_GET_SM_TICP_PORT_FINISH` |
| UI ↔ 子窗口 | 同进程 | `WM_MY_WND_CLOSE` / `WM_MY_FORCE_HIDE` |
| Host → 驱动 | 跨特权级 | `DeviceIoControl` + `CTL_*` 控制码 |
| DLL ↔ LibTDMaster | 同进程 | 直接 `GetProcAddress` 调用其导出（`UnLockLocalInput`/`DoneHook`） |

---

## 5. 协议的稳健性设计（学习要点）

- **定长缓冲 + 限长拷贝**：接收端固定 `WCHAR[256]`，用 `wcsncpy_s` 防止溢出。
- **超时保护**：`SendMessageTimeout` 500ms，配合 `SMTO_ABORTIFHUNG` 防止对方无响应导致卡死。
- **失联自愈**：DLL 检测不到主窗口时能自行决策（自杀 / 重拉主程序）。
- **状态双向同步**：`VSwitchLockState` 与 `OnUpdateState` 形成闭环，UI、Host、DLL 三方状态一致。
- **路径参数转义**：因分隔符是冒号，路径里的 `:` 被拆字段后在接收端重新拼接。

---

**上一篇** → [07 · 工具库与第三方依赖](./07-utils-and-thirdparty.md)　|　**下一篇** → [09 · 构建与运行](./09-build-run-debug.md)
