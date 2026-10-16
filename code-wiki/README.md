# JiYuTrainer Code Wiki

> 面向 **JiYuTrainer（极域电子教室对抗工具）** 开源仓库的完整代码解读文档。
> 本 Wiki 仅用于**技术学习与研究**：了解 Windows 平台下的进程注入、用户层 API Hook、内核驱动与进程保护、Win32 窗口消息通信等系统编程技术。

---

## 一、项目速览

| 项 | 内容 |
|---|---|
| 项目名 | JiYuTrainer（仓库目录名 `LtsTrainer`） |
| 当前版本 | `1.7.6.0515`（见 `JiYuTrainer/AppPublic.h` 的 `CURRENT_VERSION`） |
| 语言 / 平台 | C / C++、Win32 API、Windows 内核驱动（WDK） |
| 架构 | x86（32 位）为主，驱动支持 XP/Win7/Win8/Win10（XP 有专门分支） |
| 推荐工具链 | Visual Studio 2019（解决方案 `JiYuTrainer.sln`），驱动需 WDK8+ |
| 许可 | MIT License |
| 状态 | 已废弃，不再更新（README 首行声明） |
| 代码规模 | 项目自身源码约 2.4 万行（不含第三方） |

**一句话概括**：它是一个常驻后台的 Windows 程序，通过**远程线程注入**把一个 Hook DLL 塞进极域学生端进程（`StudentMain.exe`），在**用户层 Hook** 掉一批窗口/输入/文件/进程相关 API，把极域广播窗口从"全屏独占"改成"可自由操作的窗口"；同时配一个**内核驱动**做自身进程保护与强制结束；界面用 **Sciter（HTML/CSS 引擎）** 绘制。

---

## 二、目录结构

```
LtsTrainer/
├── JiYuTrainer.sln                 # 解决方案入口
├── JiYuTrainer/                    # ① 主程序（Host / 引导 + 核心逻辑）
├── JiYuTrainerUI/                  # ② UI 层（Sciter HTML 界面，静态库）
├── JiYuTrainerHooks/               # ③ 注入模块（Hook DLL，"病毒"）
├── JiYuTrainerDriver/              # ④ 内核驱动（进程保护 / 强杀）
├── JiYuTrainerUpdater/             # ⑤ 更新器（下载 + 安装引导，静态库）
├── Release/                        # 预编译产物（exe / sys / sciter.zip）
├── *.png / *.ico                   # 图标、截图、帮助图
└── code-wiki/                      # ← 本文档所在目录
```

---

## 三、Wiki 目录

| 序号 | 文档 | 内容 |
|:--:|---|---|
| 0 | [README.md](./README.md) | 索引与速览（本文件） |
| 1 | [01-architecture-overview.md](./01-architecture-overview.md) | 整体架构、五工程模型、编译产物与依赖关系 |
| 2 | [02-host-main-program.md](./02-host-main-program.md) | 主程序 `JiYuTrainer`：入口、`JTApp`、`TrainerWorker` 核心循环 |
| 3 | [03-hooks-injection-module.md](./03-hooks-injection-module.md) | 注入模块 `JiYuTrainerHooks`：Hook 安装、30+ Hook 语义、窗口子类化 |
| 4 | [04-kernel-driver.md](./04-kernel-driver.md) | 内核驱动 `JiYuTrainerDriver`：IOCTL、进程保护、强制结束、监控 |
| 5 | [05-ui-layer.md](./05-ui-layer.md) | UI 层 `JiYuTrainerUI`：Sciter 集成、`MainWindow`、子窗口、内建命令 |
| 6 | [06-updater.md](./06-updater.md) | 更新器 `JiYuTrainerUpdater`：检查更新、下载、安装引导 |
| 7 | [07-utils-and-thirdparty.md](./07-utils-and-thirdparty.md) | 通用工具库（Logger / NtHlp / SysHlp…）与全部第三方依赖 |
| 8 | [08-ipc-and-dataflow.md](./08-ipc-and-dataflow.md) | 进程间通信（WM_COPYDATA 文本协议）与端到端数据流 |
| 9 | [09-build-run-debug.md](./09-build-run-debug.md) | 构建、运行方式、命令行参数、调试与排错 |

---

## 四、阅读建议

- **想看全局**：先读 [01-架构总览](./01-architecture-overview.md)，再看 [08-进程间通信](./08-ipc-and-dataflow.md)。
- **想学 Hook**：直接读 [03-Hooks 注入模块](./03-hooks-injection-module.md)，重点看 `VInstallHooks` 与各 `hkXxx` 桩函数。
- **想学内核驱动**：读 [04-内核驱动](./04-kernel-driver.md)，重点看 `DriverEntry` / `IOControlDispatch` / `Protect.c`。
- **想编译运行**：读 [09-构建与运行](./09-build-run-debug.md)。

> ⚠️ **免责声明**：本文档为静态代码分析成果，仅描述代码"是什么 / 怎么做"，用于学习 Windows 系统编程。
> 请在**已授权的实验环境**中研究，遵守当地法律法规与所在机构规定，勿用于未经授权的场景。
