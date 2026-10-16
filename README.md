# lxtctrainer

> Lenovo Teaching System Trainer（联想教学系统助手）
>
> 本目录是基于开源项目 [JiYuTrainer](https://github.com/imengyu/JiYuTrainer) 改造的**联想教学系统适配版**（轻量适配），已完成实测。
>
> 产物：`lxtctrainer.exe` + `LxtcTrainerHooks.dll`（单文件分发：只需 `lxtctrainer.exe`，其余模块以资源内嵌，运行时自动释放）。

---

## 重要声明（请先阅读）

- 本项目**仅用于学习、研究**，须在**已授权的实验环境/虚拟机**中使用。
- 严禁将本项目用于任何违反法律、校规或单位管理规定、侵犯他人权益的行为。
- 通过本软件进行的一切操作及其后果，由使用者自行承担。
- 分发时请保留原作者署名与开源许可（见文末「许可」）。

---

## 一、本次适配改了什么

采用**轻量适配**：**不改**任何文件名/工程名/DLL 名/服务名/驱动设备名/驱动代码，**不引入**配置层，只在原项目结构上换常量 + 补必要能力。

### 主程序 `JiYuTrainer/TrainerWorker.cpp`（GBK，无 BOM）+ `TrainerWorker.h`

- 进程名 `StudentMain.exe` → `Student.exe`（日志 / 匹配 / 路径校验）；保留原极域硬编码路径作兜底，**新增联想安装路径块**；
- 第二注入靶子 `MasterHelper.exe` → **`MultiRender.exe`**，并解开原本被注释掉的双注入代码块；
- 真伪校验：同目录 `LibTDMaster.dll` → 改判 **`StudentLogic.dll`**（联想无前者，否则真目标会被排除）；
- 窗口接管放宽为接受两个靶子 PID；广播窗判定新增**类名 `MultiRenderWindowClass`**；黑屏窗按 **`BlackSlient.exe` 的 PID** 判定（新增 `LocateBlackSlient` / `_BlackSlientPid`）；
- **`InjectDll` 重写**：逐项校验 `VirtualAllocEx` / `WriteProcessMemory`，等待远程线程并**回读 `LoadLibraryW` 返回值**（修掉"假成功"）；等待上限 1.5s，避免阻塞主程序 UI 线程；
- 注入成功后的下一个检测周期，向病毒**补发** `hk:mhwnd`（主窗句柄）+ `hk:path` + `hk:inipath`（启动时发的那次病毒还没出生，会被丢掉）。

### 注入模块 `JiYuTrainerHooks/JiYuTrainerHooks.cpp`（UTF-8 带 BOM）

- 宿主进程名接受 `Student.exe` / `MultiRender.exe`；
- **绕过 `LibTDMaster.dll` 存在性守卫**（关键：不绕的话联想环境下 DLL 会立刻自我卸载）；
- 广播判定补充关键词 `教师演示` / `窗口广播` 与类名判定；
- `VSendMessageBack` 改为**优先使用主程序经 `hk:mhwnd` 告知的窗口句柄**，再按类名 `JiYuTrainerWindow`，最后回退标题（Sciter 会用 HTML `<title>` 覆盖窗口标题，仅按标题会"失去联系"）；
- 解锁：调用 **`LockKeyboard.dll!EnableKeyboard`**，并在注入后由**独立线程**延时重试（不阻塞 `DllMain`）；同时拦截 `\\.\Kbd` 阻止再次加锁；
- **补挂 `SetWindowsHookExW`**：对 `WH_MOUSE_LL` / `WH_MOUSE` / `WH_KEYBOARD_LL` / `WH_CBT` 拒绝注册（联想三个钩子 DLL **只导入 W 版**，只挂 A 版等于没挂）。

### 程序与文件改名

`JiYuTrainer.exe` → **`lxtctrainer.exe`**；`JiYuTrainerHooks.dll` → **`LxtcTrainerHooks.dll`**。
同步改动：两个工程的 `<TargetName>`、`.rc` 内嵌 `IDR_DLL_HOOKS` 路径、`App.cpp` 的模块释放路径与便携模式临时目录/批处理名、`TrainerWorker.cpp` 的卸载模块名、`JiYuTrainerHooks.cpp` 的 `VLoad` 名称判定与自身进程名判定。
> 注意：**改名后 `.rc` 内嵌路径必须同步**，否则注入后完全不生效（旧版就踩过这个坑）。

### 用户可见文案

「极域」→「联想」（8 个源文件 + 2 个 UTF-16LE 资源文件）。出局功能（UDP/密码/更新器）与文档未改；窗口类名/标题等内部标识保持原样（已验证的 IPC 链路不动）。

---

## 二、实测结论

环境：授权实验机、Windows 10 64 位、**`lxtctrainer.exe` 与 `Student.exe` 在同一沙箱内**、管理员运行。

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 注入 | ✅ 成功 | 日志 `InjectDll: DLL loaded (module 0x…)`、`向 Student.exe [pid] 注入DLL成功` |
| 病毒 → 主程序回报 | ✅ 通 | 主程序日志 `[I] Receive ctl success message`，界面「已控制」，失联弹窗停止；小窗 `[V] main window hwnd from msg : 0x…` |
| 键盘解锁 | ✅ | 小窗 `[V] call LockKeyboard.dll!EnableKeyboard to unlock input.` |
| 鼠标解锁 | ✅ | 小窗 `[K] WH_KEYBOARD_LL(W) -> 0x…`；鼠标可正常点/拖 |
| 主程序卡顿 | ✅ 已消除 | 注入等待 1.5s + 解锁改到独立线程 |
| 广播窗口降级 | ⏳ 待验证 | 需教师端发起「教师演示 / 窗口广播」 |
| 黑屏安静隐藏 | ⏳ 待验证 | 需教师端下发黑屏 |

---

## 三、⚠️ 关键注意事项（都是实际踩过的坑）

1. **`lxtctrainer.exe` 与 `Student.exe` 必须在同一域**：若把 `Student.exe` 放进沙箱（Sandboxie 等）而工具在沙箱外，跨沙箱的窗口/消息会被隔离 → 病毒找不到主程序（「意外与主程序失去联系」不断弹出）、`hk:mhwnd` 也送不到。**两边同一个沙箱，或两边都不放沙箱**（键盘/鼠标解锁不受影响，它在目标进程内部完成）。
2. **模块只在"文件不存在"时解压**：更新 `lxtctrainer.exe` 后**必须删除旧的 `LxtcTrainerHooks.dll`**，否则仍在用旧模块。
3. **不要点「重启主程序」**：它会以 `-r2` 再拉起一个 `lxtctrainer.exe`，撞上单实例检查会表现为「重复出现 / 未响应」。
4. **小窗会自动"消失"**：2.6 秒后被滑到屏幕上方之外；`DoNotShowVirusWindow=FALSE` 只是不真正隐藏——把鼠标移到屏幕最上沿点一下即可拉回。
5. **自带驱动是 32 位**：64 位 Windows 上 `XTestDriverCanUse()` 直接返回 FALSE（日志 `驱动不支持 64位 系统`），驱动类功能（强杀/自保护）不可用；**实测证明注入与键鼠解锁不依赖驱动**。
6. **文件日志会"停"在 `初始化正常`**：之后 logger 切到 UI 回调，不再写文件。后续排查请看**工具界面日志列表**与**病毒小窗**。

### 编码铁律（务必遵守）

| 文件 | 编码 |
| --- | --- |
| `JiYuTrainerHooks/JiYuTrainerHooks.cpp` | **UTF-8 带 BOM** |
| `JiYuTrainer/TrainerWorker.cpp`、`App.cpp`、`JiYuTrainerUI/*.cpp`、`*.html` | **GBK**（无 BOM） |
| `JiYuTrainer.rc`、`JiYuTrainerHooks.rc`、`JiYuTrainerUI.rc` | **UTF-16LE 带 BOM** |
| `MainWindow.h`、`NtHlp.cpp` | 纯 ASCII |

用编辑器保存时**必须保持原编码**，否则中文会损坏并导致编译报错（`C4819` / `C2001`）。

---

## 四、编译

| 组件 | 说明 |
| --- | --- |
| Visual Studio 2019（或 2022 + **v142** 工具集） | 需勾选「使用 C++ 的桌面开发」 |
| Windows 10 SDK | 工程 `WindowsTargetPlatformVersion=10.0` |

**不需要**：WDK（驱动已预编译并内嵌）、额外第三方库（curl / mhook / MemoryModule / XZip / Sciter 已随仓库提供）。

1. 用 Visual Studio 打开 `JiYuTrainer.sln`；
2. 配置 **Release**、平台 **x86**；
3. 生成 `JiYuTrainer` 项目（依赖会自动先构建 Hooks 模块）；
4. 产物：`Release/lxtctrainer.exe`（已内嵌所需模块）+ `Release/LxtcTrainerHooks.dll`。

命令行：

```
msbuild JiYuTrainer.sln /t:JiYuTrainer /p:Configuration=Release /p:Platform=x86 /m
```

> `.rc` 内嵌 `..\Release\JiYuTrainerDriver.sys`、`..\Release\LxtcTrainerHooks.dll`、`..\Release\sciter.zip`——这三个文件必须先存在（驱动与 sciter.zip 随仓库提供，DLL 由 Hooks 工程生成）。

---

## 五、运行

1. 以**管理员身份**运行 `lxtctrainer.exe`（建议与 `Student.exe` 放同一沙箱/同域）；
2. 确认主窗口已出现，**再**启动 `student.exe`；
3. 界面显示**「已控制」**即注入并回报成功；
4. 教师端发起屏幕广播时，广播窗口会被转为窗口模式。

常用命令行参数（与上游一致）：

```
lxtctrainer.exe -config        打开高级配置
lxtctrainer.exe -hidden        静默模式
lxtctrainer.exe -killst        直接结束学生端进程
lxtctrainer.exe -bugreport -bugfile <path>   上报崩溃信息
```

---

## 六、目标环境（学生端画像）

| 项目 | 值 |
| --- | --- |
| 广播渲染进程 | `MultiRender.exe`（窗口类 `MultiRenderWindowClass`）|
| 主客户端进程 | `Student.exe`（同时是键鼠锁宿主）|
| 黑屏安静进程 | `BlackSlient.exe`（独立进程，按 PID 处理其窗口，不注入）|
| 默认安装目录 | `C:\Program Files (x86)\Lenovo teaching system\` |
| 广播窗口标题 | 教师演示 / 窗口广播（串表 `chs.config`）|
| 权限 | 需管理员运行（跨进程注入）|

学生端组件（纯静态分析，未运行样本）：

| 组件 | 角色 |
| --- | --- |
| `Loader.exe` | 启动器：拉起 `Student.exe` + `ProcessProtect.exe` + `Multi.exe` |
| `Student.exe` | 主客户端（DuiLib；加载 `StudentLogic.dll`、`HookManager.dll`、`systemoper1.dll`）|
| `Multi.exe` / `MultiRender.exe` | 广播管理进程 / **广播渲染进程**（类名 `MultiRenderWindowClass`）|
| `BlackSlient.exe` | 黑屏安静（独立进程）|
| `ProcessProtect.exe` | 进程保护（**用户态看门狗**，实测**不阻碍注入**）|
| `LockKeyboard.dll` | 键鼠锁：**只导出 `DisableKeyboard` / `EnableKeyboard`**，底层 `\\.\Kbd` |
| `HookManager.dll` / `HookProcDll.dll` / `HookLib.dll` | 键鼠钩子，**只导入 `SetWindowsHookExW`** |
| `inspect.sys` | 网络流量重定向驱动（WFP，与保护/注入无关）|

要点：所有可执行文件均为 **32 位（x86）**。

---

## 七、已知限制

- 仅支持 32 位**目标**（注入方与被注入进程位数需一致）。
- 自带内核驱动为 32 位，64 位系统下不可用（注入与键鼠解锁不依赖它）。
- 「关黑屏」采用独立进程直接处理：定位 `BlackSlient.exe`，按 PID 匹配其顶层窗口并隐藏；每轮检测自动重新隐藏。
- 以下能力为**原极域专有**，对联想教学系统默认无效：卸载密码破解（`TopDomain`/`Knock1`）、屏蔽极域驱动（`TDNetFilter`/`TDKeybd`/`TDProcHook`）、`LibTDAjust.dll` 桩替换、UDP 发包（`DMOC` 协议）。
- 自动更新指向原作者的服务器，已停用。
- `Release/` 下的 `JiYuTrainerOldForWinXP.exe`、`JiYuTrainerUpdater.exe` 为上游遗留/占位文件，与本次适配无关。

---

## 八、许可

本项目沿用上游 [MIT License](../LICENSE)。分发时请保留原作者的版权声明：

- 上游项目：[imengyu/JiYuTrainer](https://github.com/imengyu/JiYuTrainer)
- 感谢上游作者「快乐的梦鱼」及所有贡献者提供的开源实现。

---

## 待办 / 后续

- [x] 注入、病毒回报、键盘/鼠标解锁、去卡顿 —— 已实测通过。
- [ ] 广播窗口降级实测（需教师端发起「教师演示 / 窗口广播」）。
- [ ] 黑屏安静隐藏实测（需教师端下发黑屏）。
- [ ] 若需在 64 位系统使用驱动类功能，需另行移植 x64 驱动。
