# 06 · 更新器 JiYuTrainerUpdater

> 工程目录：`JiYuTrainerUpdater/`　Win32 配置下为**静态库**（链接进主程序），x64 配置下为 Application。
> 作用：检查服务器是否有新版本、下载新版 exe、并触发安装（自身亦可作为独立安装器 `JiYuTrainerUpdater.exe`）。
> 网络层：第三方 **curl**（同步与异步下载）。

---

## 1. 更新服务地址与协议

`JiYuTrainerUpdater.cpp`

```cpp
#define UPDATE_HOST "http://imengyu.top/services/update/JiYuTrainer/"
```

采用简单的 **HTTP GET + 纯文本响应**协议（非 JSON）：

| 请求 | 响应 | 含义 |
|---|---|---|
| `?checkupdate=<当前版本>` | `newupdate` / `latest` / 其他 | 有新版 / 已最新 / 服务异常 |
| `?getupdateinfo` | 文本 | 新版更新说明 |
| `?getnewver` | 版本号文本 | 最新版本号 |
| `?getupdate` | 文件名 | 新版可执行文件名（拼成下载 URL） |

---

## 2. 导出接口（供 UI 调用）

| 导出函数 | 说明 |
|---|---|
| `JUpdater_CheckInternet()` | `InternetGetConnectedState` 判断是否有网络 |
| `JUpdater_CheckUpdate(bool byUser)` | 检查更新；非用户触发时受"每日一次"限制 |
| `JUpdater_GetUpdateNew(buffer, maxCount)` | 取更新说明 |
| `JUpdater_GetUpdateNewVer()` | 取最新版本号 |
| `JUpdater_DownLoadUpdateFile(callback, lparam)` | 起线程下载新版文件 |
| `JUpdater_Updatering()` | 是否正在下载 |
| `JUpdater_CancelDownLoadUpdateFile()` | 取消下载（`TerminateThread` + 删临时文件） |
| `JUpdater_RunInstallion()` | 卸载注入 DLL 后以 `-install-full` 提权运行新版 exe |

`CheckServerForUpdateInfo(bool byUser)` 发起检查：

```cpp
string url = UPDATE_HOST + "?checkupdate=" + CURRENT_VERSION;
auto res = curl_get_req(url, response);          // 同步 GET
if (res != CURLE_OK) return UPDATE_STATUS_COULD_NOT_CONNECT;
if (response == "newupdate") return UPDATE_STATUS_HAS_UPDATE;
if (response == "latest")    return UPDATE_STATUS_LATEST;
return UPDATE_STATUS_NOT_SUPPORT;
```

---

## 3. 每日一次检查节流

`CheckLastUpdateDate(iniPath)`：

```cpp
GetPrivateProfileString("JTSettings", "LastCheckUpdateTime", "00/00", ...);  // 上次检查日期
GetPrivateProfileString("JTSettings", "AlwaysCheckUpdate", "FALSE", ...);
// 若今天已检查过且未开 AlwaysCheckUpdate → 返回 true（跳过），否则记录今天日期
```

`JUpdater_CheckUpdate(byUser=false)` 会先过这道闸，避免每次启动都请求服务器。

---

## 4. 下载流程

`JUpdater_DownLoadUpdateFile()`：

```cpp
updateFilePath = <当前目录>\JiYuTrainerUpdater.exe;   // 下载到固定文件名
string url = UPDATE_HOST + "?getupdate";             // 先取文件名
curl_get_req(url, resp);
downloadFileUrl = UPDATE_HOST + resp;
updateThread = CreateThread(..., UpdateDownloadThread, ...);
```

`UpdateDownloadThread`（下载线程）：

```cpp
updateing = true; Sleep(1300);
CURL *curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, url);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
curl_easy_setopt(curl, CURLOPT_WRITEDATA, updateFile);
curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, UpdateProgressFunc);  // 进度回调
curl_easy_perform(curl);
curl_easy_cleanup(curl);
callBack(0, lparam, UPDATE_STATUS_FINISHED);
```

进度回调 `UpdateProgressFunc` 把 `已下载/总大小` 换算成百分比，通过 `callBack` 推给 `UpdaterWindow` 的进度条。

---

## 5. 安装/升级：JUpdater_RunInstallion

```cpp
if (Path::Exists(updateFilePath)) {
    // 先强制卸载已注入的 DLL
    if (appCurrent->GetTrainerWorker())
        appCurrent->GetTrainerWorker()->RunOperation(TrainerWorkerOpForceUnLoadVirus);
    // 提权运行新版，并带上源路径参数： -install-full -f <当前exe>
    SysHlp::RunApplicationPriviledge(updateFilePath, appCurrent->MakeFromSourceArg("-install-full"));
}
```

`MakeFromSourceArg()`（`App.cpp`）会把当前安装源路径塞进参数：`-f <fullSourceInstallerPath> -install-full`，新版程序据此完成"覆盖旧 exe → 删除旧文件 → 以 `-rc` 清理更新器"的落地（主程序 `CheckAndInstall` 里的 `appIsInstaller` 分支）。

---

## 6. 更新状态码

用于回调传递下载/检查状态（`JiYuTrainerUpdater.h`）：

| 常量 | 含义 |
|---|---|
| `UPDATE_STATUS_LATEST` | 已是最新 |
| `UPDATE_STATUS_HAS_UPDATE` | 有新版本 |
| `UPDATE_STATUS_CHECKED` | 今日已检查过（节流命中） |
| `UPDATE_STATUS_COULD_NOT_CONNECT` | 网络错误 |
| `UPDATE_STATUS_NOT_SUPPORT` | 服务返回异常 |
| `UPDATE_STATUS_DWONLAODING` | 下载中（进度） |
| `UPDATE_STATUS_FINISHED` | 下载完成 |
| `UPDATE_STATUS_COULD_NOT_CREATE_FILE` | 无法创建文件 |

---

## 7. 与 UI 的配合

- `MainWindow::UpdateThread` 在启动（`setAutoUpdate` 为真）后台检查更新，命中则 `GetUpdateInfo()` 弹提示；
- `link_checkupdate` → `JUpdater_CheckInternet` + `JUpdater_CheckUpdate(true)`；
- 点击"立即更新" → `UpdaterWindow` → `JUpdater_DownLoadUpdateFile(...)` → 下载完成 `JUpdater_RunInstallion()`。

---

## 8. 小结（学习要点）

- **纯文本 HTTP 协议**：用最少依赖实现"检查/说明/版本/下载"四类查询，简单但够用。
- **同步 + 异步**：检查用同步 `curl_get_req`（阻塞短），下载用独立线程 + 进度回调。
- **自我更新落盘**：先释放远端文件为 `JiYuTrainerUpdater.exe`，再以提权 + `-install-full` 参数启动它完成覆盖，避免"自己替换自己"的文件占用问题（主程序用 `.bat` 延迟删除旧文件）。

---

**上一篇** → [05 · UI 层](./05-ui-layer.md)　|　**下一篇** → [07 · 工具库与第三方依赖](./07-utils-and-thirdparty.md)
