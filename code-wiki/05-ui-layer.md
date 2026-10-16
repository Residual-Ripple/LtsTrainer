# 05 · UI 层 JiYuTrainerUI

> 工程目录：`JiYuTrainerUI/`　Win32 配置下为**静态库**，代码链接进 `JiYuTrainer.exe`
> 界面引擎：**Sciter**（用 HTML/CSS/脚本描述界面，C++ 侧通过 DOM API 操作）
> 统一入口：导出 `JiYuTrainerUICommonEntry(int)`。

---

## 1. 统一入口与模式分发

`JiYuTrainerUI/JiYuTrainerUI.cpp`

```cpp
int JiYuTrainerUICommonEntry(int i) {
    SetUnhandledExceptionFilter(AppUnhandledExceptionFilter);
    switch (i) {
        case 0: return JiYuTrainerUIRunMain();      // 主界面
        case 1: return JiYuTrainerUIRunUpdate();    // 更新界面
        case 2: return JiYuTrainerUIRunConfig();    // 更多设置
        case 3: return JiYuTrainerUIRunBugReport(); // Bug 报告
        case 4: currentApp->RunOperation(AppOperation3); return 0; // 崩溃测试
    }
}
```

`JiYuTrainerUIRunMain()` 先设置 Sciter 运行时选项（允许文件/套接字/`eval`），再创建 `MainWindow` 并进入消息循环：

```cpp
SciterSetOption(NULL, SCITER_SET_SCRIPT_RUNTIME_FEATURES, ALLOW_FILE_IO | ALLOW_SOCKET_IO | ALLOW_EVAL | ALLOW_SYSINFO);
currentMainWindow = new MainWindow();
int result = currentMainWindow->RunLoop();   // 标准 GetMessage 循环
```

主程序在 `RunInternal` 末尾根据启动模式调用对应编号（见 [02 · §3](./02-host-main-program.md#3-启动流程jiyutrainerinternalruninternal)）。

---

## 2. 窗口类层次

```
sciter::event_handler
        ▲
        │
   CommonWindow  ──────────────────────────────┐   (通用窗口基类)
        ▲                                       │
        │  sciter::host<T> 多继承               │
   ┌────┴────────┬──────────────┬───────────────┼───────────────┐
 MainWindow  HelpWindow   AttackWindow   ConfigWindow   BugReportWindow
 (主窗口)    (帮助)        (UDP 攻击)      (更多设置)      (Bug 报告)
                                     ScanIPWindow(IP 扫描) / UpdaterWindow(更新)
```

`MainWindow` 三继承：`sciter::host<MainWindow>` + `sciter::event_handler` + `TrainerWorkerCallback`。

| 类 | 文件 | 说明 |
|---|---|---|
| `CommonWindow` | `CommonWindow.h/.cpp` | 通用 Sciter 窗口基类：注册窗口类、加载 HTML 资源、`docunmentComplete` 回调 |
| `MainWindow` | `MainWindow.h/.cpp` | 主窗口（约 1050 行），承载状态显示、日志、设置、命令控制台 |
| `HelpWindow` | `HelpWindow.h/.cpp` | 帮助页 |
| `AttackWindow` | `AttackWindow.h/.cpp` | UDP 报文发送界面（目标 IP/端口、发消息/命令/关机/重启） |
| `ScanIPWindow` | `ScanIPWindow.h/.cpp` | 局域网 IP 扫描界面 |
| `ConfigWindow` | `ConfigWindow.h/.cpp` | "更多设置"对话框 |
| `BugReportWindow` | `BugReportWindow.h/.cpp` | 崩溃报告界面 |
| `UpdaterWindow` | `UpdaterWindow.h/.cpp` | 更新进度界面 |

---

## 3. CommonWindow：Sciter 窗口基类

`CommonWindow` 封装了"创建一个 Sciter 承载窗口 + 从资源加载 HTML"的通用流程：

```cpp
CommonWindow(HWND parent, int w, int h, LPCWSTR className, LPCWSTR title, int htmlResId);
int  RunLoop();
void Show(); void Hide(); void Close(); void Release(); void SetToTop();
HWND get_hwnd();  HINSTANCE get_resource_instance();
```

- `initClass()`：`RegisterClassExW` 注册窗口类；
- `onLoadHtml(pData, len)` → `load_html(...)`：把 exe 内的 HTML 资源喂给 Sciter；
- `wndProc` 里先调 `SciterProcND(hWnd, message, wParam, lParam, &handled)`，`handled` 为真则直接返回（Sciter 接管）；
- `BEGIN_FUNCTION_MAP` 暴露 `docunmentComplete` / `inspectorIsPresent` 给页面脚本。

---

## 4. MainWindow：主界面

`MainWindow.h/.cpp`

### 4.1 创建与初始化

```cpp
MainWindow::MainWindow() {
    asset_add_ref();
    swprintf_s(wndClassName, MAIN_WND_CLS_NAME);   // "JiYuTrainerWindow"
    initClass();
    hWndMain = CreateWindow(wndClassName, MAIN_WND_NAME, WS_POPUP, 0,0,430,520, ...);
    init();
    // 隐藏模式则 SW_SHOW，否则按启动 showCmd
}
```

`init()` 做四件事：

```cpp
SciterSetOption(_hWnd, SCITER_ALPHA_WINDOW, TRUE);   // 支持透明窗口
setup_callback(); attach_dom_event_handler(_hWnd, this);
LoadSettings();                                       // 读配置
currentWorker = currentApp->GetTrainerWorker();
currentWorker->SetUpdateInfoCallback(this);           // 注册为 Worker 回调 → UI 实时刷新
currentApp->LoadDriver();                             // 装载内核驱动
load_html(内存中的 IDR_HTML_MAIN);                     // 加载界面
```

### 4.2 DOM 元素绑定：docunmentComplete

页面加载完成后，回调 `docunmentComplete()` 会用 `get_element_by_id` 抓取一大批元素句柄（`status_text_main`、`btn_kill`、`check_allow_control`、`status_area`…），然后 `LoadSettingsToUi()` 把配置回填到界面。

### 4.3 事件处理：on_event

Sciter 的 DOM 事件（`HYPERLINK_CLICK` / `BUTTON_CLICK`）统一进 `on_event`：

| 元素 id | 行为 |
|---|---|
| `btn_kill` | `currentWorker->Kill()` 杀死极域进程 |
| `btn_restart` | `currentWorker->Rerun()` 重开极域 |
| `btn_top` | 切换主窗口置顶（`TIMER_AOP` 定时保置顶） |
| `link_save_setting` / `link_setto_default` | 保存 / 重置配置 |
| `link_checkupdate` | 手动检查更新 |
| `link_runcmd` | 执行命令框里的内建命令（见 §4.5） |
| `link_read_jiyu_password(2)` | 读取极域密码 |
| `link_unload_netfilter` | 卸载极域网络过滤驱动 |
| `link_locate_jiyu_position` | 手动指定 `StudentMain.exe` 位置 |
| `link_shutdown` / `link_reboot` | 关机 / 重启 |
| `link_uninstall` | 卸载本软件（`currentApp->UnInstall()`） |
| `check_ck` | 启停 Worker（`currentWorker->Start()/Stop()`） |

### 4.4 作为 TrainerWorkerCallback 的实时刷新

`MainWindow` 实现 `TrainerWorkerCallback`，Worker 状态变化时回调：

| 回调 | 更新内容 |
|---|---|
| `OnUpdateStudentMainInfo(running, path, pid, byUser)` | 显示极域 PID / 路径，切换"杀/重启"按钮 |
| `OnUpdateState(status, textMain, textMore)` | 切换状态图标样式类与文案（`state-not-found`/`state-ctl-unlock`…），联动"保护状态"指示 |
| `OnResolveBlackScreenWindow()` | 提示"已关闭黑屏窗口" |
| `OnBeforeSendStartConf()` | 延迟 1.5s 再发配置 |
| `OnAllowGbTop()` | 广播被请求置顶 → 更新设置并保存 |
| `OnShowHelp()` | 打开帮助 |

日志回显：`SetLogOutPutCallback(LogCallBack, this)` 把 `LoggerInternal` 的输出重定向到界面列表 `status_area`（`WriteLogItem` 按等级上色）。

### 4.5 内建命令控制台：OnRunCmd

`link_runcmd` 触发的命令框支持大量调试/运维命令（空格分隔）：

| 命令 | 作用 |
|---|---|
| `killst` / `rerunst` | 杀死 / 重启极域 |
| `kill <pid> [force]` / `findps <name>` | 杀指定进程 / 按名找 PID |
| `ss` | 令注入 DLL 自杀 |
| `sss` / `ssr` | 关机 / 重启（先卸 DLL 再关机） |
| `sst <秒>` / `sst -c` | 定时关机 / 取消 |
| `ssss` / `sssr` | 内核级关机 / 重启（走驱动） |
| `ckend` | 让注入 DLL 优雅退出 |
| `unloaddrv` / `floaddrv` | 卸载 / 强制加载驱动 |
| `fuljydrv` | 卸载极域保护驱动（危险，需二次确认） |
| `jypasswd` | 读取极域密码（普通/破解两种模式） |
| `attack` | 打开 UDP 报文发送窗 |
| `unload_netfilter` | 卸载极域网络过滤驱动 |
| `uj` | 卸载注入的 DLL |
| `inspector` / `whereisi` / `version` / `config` / `crash` / `hide` / `exit` | 调试工具、显示路径/版本、打开设置、崩溃测试、隐藏、退出 |

### 4.6 托盘、热键与生命周期

- 托盘：`CreateTrayIcon()` + `ShowTrayBaloonTip()`（`Shell_NotifyIcon`）；`WM_USER` 处理左键双击显隐、右键弹菜单。
- 热键：`OnFirstShow` 里 `RegisterHotKey`，默认 `Alt+... ` 显示/隐藏（`HotKeyShowHide`）、切换伪装全屏（`HotKeyFakeFull`），键码由 `SysHlp::HotKeyCtlToKeyCode` 转换。
- `WM_SYSCOMMAND` 的 `SC_CLOSE` 只隐藏窗口不退出；`OnWmDestroy` 里若仍处于"控制中"则以 `-r1` 模式重启自身（下次启动恢复）。
- `WM_DISPLAYCHANGE` → 更新屏幕尺寸并通知 Worker `UpdateScreenSize()`。

---

## 5. 其余窗口要点

### AttackWindow（UDP 报文发送）

- 构造时即 `JyUdpAttack::currentJyUdpAttack->sendResultReceivehWnd = _hWnd`，接收发送结果消息；
- `SendCommonCheck()` 用正则校验 IPv4 与端口范围；
- 按钮 → `SendCmd/SendText/SendShutdown/SendReboott`；
- `WM_MY_GET_SM_TICP_PORT_FINISH` 回填 TCP 端口，`WM_MY_SEND_ADD_RESULT` 追加结果行。

### ScanIPWindow（IP 扫描）

调用 `JyUdpAttack::ScanNetworkIP(...)`，通过 `WM_MY_SCAN_IP_ADD` / `WM_MY_SCAN_IP_FINISH` 增量刷新列表。

### UpdaterWindow（更新）

持有 `progress` / `progress_text` 元素，回调 `UpdateDownloadCallback(LPCWSTR precent, int status)` 更新进度条，`OnCancel()` 取消下载。

### ConfigWindow / BugReportWindow / HelpWindow

较简单的对话框；`ConfigWindow` 暴露 `ShowMoreSettings(HWND)`，`BugReportWindow` 在崩溃后被 `-bugreport` 模式拉起。

---

## 6. UI 与业务层的桥接方式

| 方向 | 机制 |
|---|---|
| C++ → JS | `element.set_text()` / `set_html()` / `set_attribute("class", ...)` / `call_function("showFastTip")` |
| JS → C++ | `BEGIN_FUNCTION_MAP` 暴露的函数（`docunmentComplete` 等）；`on_event` 处理 DOM 事件 |
| 数据同步 | `MainWindow` 实现 `TrainerWorkerCallback`，Worker 主动回调刷新 UI |
| 日志 | `LoggerInternal` 回调 → `WriteLogItem` 渲染 |

> 注意：源码里函数名 `docunmentComplete`（`docunment` 缺 `u`）是原作者的拼写，页面脚本与之约定一致，不要"修正"。

---

## 7. 小结（学习要点）

- **Sciter 模式**：C++ 只做逻辑，界面用 HTML/CSS；C++ 与脚本通过 DOM 元素与函数映射双向通信，比传统 Win32 控件布局高效得多。
- **回调驱动的 UI**：UI 不轮询，而是注册为 `TrainerWorkerCallback`，由后台 Worker 在状态变化时推送。
- **单例窗口管理**：`currentHelpWindow` / `currentAttackWindow` 等以指针保存，`WM_MY_WND_CLOSE` 统一释放，避免重复创建。

---

**上一篇** → [04 · 内核驱动](./04-kernel-driver.md)　|　**下一篇** → [06 · 更新器](./06-updater.md)
