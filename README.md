# LtsTrainer

> 联想教学系统（Lenovo teaching system / NeiCloud）学生端助手
>
> 基于开源项目 [JiYuTrainer](https://github.com/imengyu/JiYuTrainer) 改造，保留其界面与使用方式，目标从极域电子教室切换为联想教学系统。

## 声明

- 本项目仅供**学习、研究**，请在**已授权的实验环境**中使用。
- 请勿用于违反法律、校规或单位规定的行为；使用后果由使用者自行承担。
- 本项目沿用上游 MIT 许可，二次分发请保留原作者署名（见文末）。

## 功能

- **广播窗口化**：教师演示窗口变为普通窗口（有标题栏、可移动缩放、不置顶、光标正常）。
- **黑屏解除**：自动结束黑屏安静进程并隐藏其窗口。
- **键鼠解锁**：解除键盘锁进程与鼠标底层钩子，锁定状态下依然有效。
- **网络限制拦截**：阻止学生端下发网络过滤，正常上网不受影响。
- **防被清场**：以管理员权限运行，避免被"程序使用限制"批量结束。
- **自动提权**：普通方式启动时会自动请求管理员权限。
- 保留原项目的托盘图标、快捷键、日志窗口、配置界面等能力。

## 使用

1. 运行 `Release\LtsTrainer.exe`（单文件分发，所需模块已内置于 exe，首次运行自动释放）。
2. 弹出 UAC 时选择"是"（程序需要管理员权限才能注入与解锁）。
3. **建议先启动本工具，再启动学生端**；若学生端已在锁定/黑屏状态，后启动也能自动解除。
4. 运行后在 exe 同目录生成 `LtsTrainer.ini`（配置）与 `LtsTrainer.log`（日志，关闭程序后写入完整）。

常用命令行参数：

```
LtsTrainer.exe -config        打开高级配置
LtsTrainer.exe -hidden        静默启动
LtsTrainer.exe -killst        直接结束学生端进程
LtsTrainer.exe -bugreport     生成问题反馈信息
```

> 提示：更新版本后请先删除旧的 `LtsTrainerHooks.dll`（程序仅在文件不存在时释放模块）。

## 开发

### 工程结构

| 工程 | 说明 |
| --- | --- |
| `JiYuTrainer` | 主程序：目标定位、注入、窗口与进程处理 |
| `JiYuTrainerHooks` | 注入模块：在目标进程内做窗口接管与解锁 |
| `JiYuTrainerUI` | 界面（Sciter） |
| `JiYuTrainerUpdater` | 更新器（上游遗留，默认不启用） |
| `JiYuTrainerDriver` | 旧版内核驱动，仅 32 位系统可用，非必需 |

### 编译

- Visual Studio 2019（或 2022 + **v142** 工具集），安装"使用 C++ 的桌面开发"
- Windows 10 SDK
- 配置 **Release / x86**（目标学生端为 32 位）

```
msbuild JiYuTrainer.sln /t:JiYuTrainer /p:Configuration=Release /p:Platform=x86 /m
```

产物：`Release\LtsTrainer.exe` + `Release\LtsTrainerHooks.dll`。

### 文件编码约定

| 文件 | 编码 |
| --- | --- |
| `JiYuTrainerHooks\JiYuTrainerHooks.cpp` | UTF-8 带 BOM |
| 其余 `.cpp` / `.h` / `.html` | GBK |
| `.rc` 资源文件 | UTF-16LE 带 BOM |

用编辑器保存时请保持原编码，否则中文会损坏并导致编译报错。

## 许可

[MIT License](LICENSE)

- 上游项目：[imengyu/JiYuTrainer](https://github.com/imengyu/JiYuTrainer)
- 感谢上游作者「快乐的梦鱼」及所有贡献者。
