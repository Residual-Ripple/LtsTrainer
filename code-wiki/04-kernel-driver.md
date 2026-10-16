# 04 · 内核驱动 JiYuTrainerDriver

> 工程目录：`JiYuTrainerDriver/`　产物：`JiYuTrainerDriver.sys`
> 设备名：`\Device\JKRK`，符号链接：`\??\JKRK`（Ring3 通过 `\\.\JKRK` 访问）
> 语言：C（DDK/WDK 风格，`ntifs.h`），仅 **32 位**（`XTestDriverCanUse` 明确拒绝 64 位系统）。
> 作用：当 Ring3 杀不掉有驱动保护的极域进程时，提供 Ring0 的进程保护、强制结束、关机/重启与进程监控。

---

## 1. DriverEntry：驱动初始化

`JiYuTrainerDriver/Driver.c`

```c
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegPath)
{
    IoCreateDevice(..., L"\\Device\\JKRK", FILE_DEVICE_UNKNOWN, ..., &deviceObject);
    pDriverObject->DriverUnload                        = DriverUnload;
    pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IOControlDispatch; // 处理 IOCTL
    pDriverObject->MajorFunction[IRP_MJ_CREATE]         = CreateDispatch;

    IoCreateSymbolicLink(L"\\??\\JKRK", L"\\Device\\JKRK"); // 暴露给 Ring3

    LoadFunctions();     // 用 MmGetSystemRoutineAddress 动态取内核函数地址

    // 绕过内核回调校验：给驱动 LDR 项打标志，避免 MmVerifyCallbackFunction 拒绝注册
    ldr = (PLDR_DATA_TABLE_ENTRY32)pDriverObject->DriverSection;
    ldr->Flags |= 0x20;

    KxPsMonitorInit();          // 注册进程创建/退出通知
    KxInitProtectProcess();     // 注册进程句柄回调（ObRegisterCallbacks）
    return ntStatus;
}
```

`DriverUnload` 反向清理：删符号链接、`KxUnInitProtectProcess`、`KxPsMonitorUnInit`、`IoDeleteDevice`。

---

## 2. 动态取函数：LoadFunctions

内核不导出全部需要的函数，所以用 `MmGetSystemRoutineAddress` 按名字取址，存入全局函数指针（`_PsSuspendProcess`、`_ObRegisterCallbacks`、`_NtShutdownSystem`、`memcpy` 等）：

```c
_PsLookupProcessByProcessId = MmGetSystemRoutineAddress(L"PsLookupProcessByProcessId");
_ObRegisterCallbacks        = MmGetSystemRoutineAddress(L"ObRegisterCallbacks");
_PsSetCreateProcessNotifyRoutineEx = MmGetSystemRoutineAddress(L"PsSetCreateProcessNotifyRoutineEx");
...
```

`LoadUnExpFunctions()`（`UnExp.c`）进一步用**特征码扫描**定位**未导出**的 `PspTerminateThreadByPointer` / `PspExitThread`：

- `KxSearchFeatureCodeForAddress`：从某地址起在 `Search_MaxLength` 内逐字节比对特征码；
- `KxGetPspTerminateThreadByPointerAddressX_7Or8Or10`：从 `PsTerminateSystemThread` 入口找 `XX E8`（call 指令），解析相对偏移还原出 `PspTerminateThreadByPointer` 的绝对地址；
- `KxGetPspExitThread_32_64`：在 `PspTerminateThreadByPointer` 附近找 `E8 ... CC`（call 后跟 int3）模式反推 `PspExitThread`。

> 这是内核研究中常见的"**特征码定位未导出函数**"手法，同一个二进制在不同 Windows 版本上偏移不同，因此用模式匹配而非硬编码地址。

---

## 3. IOCTL 分发：IOControlDispatch

Ring3 通过 `DeviceIoControl(hKDrv, CTL_xxx, ...)` 下发命令（控制码定义见 `IoCtl.h`）：

| 控制码 | 语义 | 处理 |
|---|---|---|
| `CTL_INITPARAM` | 初始化参数 | 记录 `isWin7/isWinXP/systemVersion`；`LoadUnExpFunctions()`；XP 时 `KxHookInXP()` |
| `CTL_INITSELFPROTECT` | 保护指定进程 | `KxShadowSSDTHook()` + `KxProtectProcessWithPid(pid)` |
| `CTL_OPEN_PROCESS` | 打开进程句柄 | `PsLookupProcessByProcessId` + `ObOpenObjectByPointer` 返回句柄 |
| `CTL_OPEN_THREAD` | 打开线程句柄 | 同上（线程版） |
| `CTL_KILL_PROCESS` | 强制结束进程 | 先试 `ZwTerminateProcess`；`ACCESS_DENIED` 时 `KillProcess()` |
| `CTL_KILL_PROCESS_SPARE_NO_EFFORT` | 暴力结束 | `ZeroKill(pid)`：把目标进程地址空间整段写成 `0xCC` |
| `CTL_KILL_THREAD` | 结束线程 | `PspTerminateThreadByPointer(thread, 0, TRUE)` |
| `CTL_SUSPEND_PROCESS` / `CTL_RESUME_PROCESS` | 挂起/恢复 | `_PsSuspendProcess` / `_PsResumeProcess` |
| `CTL_SHUTDOWN` / `CTL_REBOOT` | 关机 / 重启 | `CompuleShutdown()` / `CompuleReBoot()` |
| `CTL_CLIENT_QUIT` | 客户端退出 | `KxUnProtectProcessWithPid(PsGetCurrentProcessId())` 解除对本进程保护 |
| `CTL_UNINIT` | 反初始化 | `KxShadowSSDTUnHook()` 摘 Hook |

返回值写回 `Irp->IoStatus.Status`（对 Ring3 表现为 `GetLastError`）。

### 3.1 ZeroKill：暴力摧毁进程

```c
NTSTATUS ZeroKill(ULONG_PTR PID) {
    PsLookupProcessByProcessId((HANDLE)PID, &Eprocess);
    KeStackAttachProcess(Eprocess, pKs);          // 附加到目标地址空间
    for (i = 0; i <= 0x7fffffff; i += 0x1000)     // 逐页
        if (MmIsAddressValid((PVOID)i)) { ProbeForWrite(...); memset((PVOID)i, 0xcc, 0x1000); }
    KeUnstackDetachProcess(pKs);
    ObOpenObjectByPointer(...); ZwTerminateProcess(handle, ...); ZwClose(handle);
}
```

即把目标进程内存全部覆盖成 `0xCC`（int3）再结束，属于"宁为玉碎"式的暴力手段（源码注释也提示可能引发系统不稳定）。

### 3.2 KillProcess：APC 终止线程

```c
NTSTATUS KillProcess(PEPROCESS pEProcess) {
    _PsSuspendProcess(pEProcess);
    for (tid = 8; tid < 65536; tid += 4) {          // 遍历线程 id
        PsLookupThreadByThreadId(tid, &pEThread);
        if (IoThreadToProcess(pEThread) == pEProcess)
            PspExitThread ? KxTerminateThreadApc(pEThread) : PspTerminateThreadByPointer(...);
        ObDereferenceObject(pEThread);
    }
    _PsResumeProcess(pEProcess);
}
```

`KxTerminateThreadApc` 用 `KeInitializeApc + KeInsertQueueApc` 向目标线程投递一个内核 APC，回调 `KernelKillThreadRoutine` 里调用 `PspExitThread(STATUS_SUCCESS)` 让线程自我了断。

### 3.3 CompuleShutdown / CompuleReBoot：汇编小段

```c
VOID CompuleShutdown() {
    _NtShutdownSystem(ShutdownPowerOff);            // 正规关机
    // 再补一段 shellcode：mov ax,2001h; mov dx,1004h; out dx,ax; ret  （强制断电）
}
VOID CompuleReBoot() {
    // shellcode：mov al,0FEh; out 64h,al; ret  （写 8042 键盘控制器触发复位）
}
```

关机/重启在正常 `NtShutdownSystem` 之后追加直接操作硬件的 shellcode 兜底。

---

## 4. 进程保护：Protect.c

### 4.1 保护进程列表

用单向链表 `PROTECT_PROC_STORAGE` 保存被保护 PID：

```c
KxInitProtectProcessList()   // 初始化头结点
KxProtectProcessWithPid(pid) // 追加到链表尾部
KxUnProtectProcessWithPid(pid)
KxIsProcessProtect(pid)      // 查询
```

### 4.2 ObRegisterCallbacks（现代系统主手段）

```c
NTSTATUS KxInitProtectProcess() {
    OB_CALLBACK_REGISTRATION obReg; OB_OPERATION_REGISTRATION opReg;
    obReg.Version = _ObGetFilterVersion();
    RtlInitUnicodeString(&obReg.Altitude, L"321000");
    opReg.ObjectType = PsProcessType;
    opReg.Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    opReg.PreOperation = &KxObPreCall;
    _ObRegisterCallbacks(&obReg, &obHandle);
}
```

回调 `KxObPreCall`：若目标是被保护进程，则从"期望访问权限"里**剥掉**危险位：

```c
DesiredAccess &= ~PROCESS_TERMINATE;   // 不许结束
DesiredAccess &= ~PROCESS_VM_OPERATION;// 不许改内存
DesiredAccess &= ~PROCESS_VM_READ;     // 不许读内存
DesiredAccess &= ~PROCESS_VM_WRITE;    // 不许写内存
```

这样别人即使能 `OpenProcess`，也拿不到能杀进程/注入的权限。

### 4.3 XP 兼容：SSDT Hook

XP 不支持 `ObRegisterCallbacks`，改用 SSDT Hook（`KxHookInXP`）：

```c
OldZwTerminateProcess = KeServiceDescriptorTable[0].Base[SYSCALL_INDEX(ZwTerminateProcess)];
OldZwOpenProcess      = KeServiceDescriptorTable[0].Base[SYSCALL_INDEX(ZwOpenProcess)];
EnableWriteProtect(&old);                       // 关 CR0.WP（写保护）
KeServiceDescriptorTable[0].Base[SYSCALL_INDEX(ZwTerminateProcess)] = Hook_ZwTerminateProcess;
DisableWriteProtect(old);                       // 恢复
```

- `EnableWriteProtect` / `DisableWriteProtect`：内联汇编 `cli`/`sti` + 改 `CR0` 第 16 位（WP），让内核可写只读内存页；
- `Hook_ZwOpenProcess`：若目标是受保护进程 → `STATUS_ACCESS_DENIED`；
- `Hook_ZwTerminateProcess`：若目标是受保护进程且不是"自己关自己" → 拒绝。

### 4.4 Shadow SSDT

`KxShadowSSDTHook`（`KeServiceDescriptorTableShadow`，用于 win32k 表）当前仅计算地址并置标志，未实际挂钩（保留能力）。

---

## 5. 进程监控：Monitor.c

```c
KxPsMonitorInit()   → _PsSetCreateProcessNotifyRoutineEx(KxCreateProcessNotifyEx, FALSE)
KxPsMonitorUnInit() → 同函数 Remove=TRUE 注销

VOID KxCreateProcessNotifyEx(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
    if (CreateInfo == NULL) {                  // 进程退出
        if (KxIsProcessProtect(ProcessId)) KxUnProtectProcessWithPid(ProcessId); // 清理链表
    }
}
```

作用：进程退出时自动把它从保护列表移除，避免 PID 复用导致误保护。

---

## 6. 驱动接口总览（Ring3 视角）

```cpp
// DriverLoader.cpp
XLoadDriver()      → MLoadKernelDriver("JiYuTrainerDriver", path) + XOpenDriver() + KFSendDriverinitParam()
XInitSelfProtect() → KFInstallSelfProtect()
XUnLoadDriver()    → KFBeforeUnInitDriver() + MUnLoadKernelDriver("JiYuTrainerDriver")

// KernelUtils.cpp → 逐个 DeviceIoControl
KForceKill(pid)    → CTL_KILL_PROCESS
KFShutdown()       → CTL_SHUTDOWN
KFReboot()         → CTL_REBOOT
```

完整映射见 [02 · §6 驱动装载与调用](./02-host-main-program.md#6-驱动装载与调用)。

---

## 7. 平台限制与注意事项（源码可见的约束）

| 约束 | 出处 |
|---|---|
| 不支持 64 位系统 | `XTestDriverCanUse()`：`if (Is64BitOS()) return FALSE` |
| 需管理员权限（XP 除外） | 同上 `IsRunasAdmin()` 判定 |
| XP/Win7 分别处理未导出函数偏移 | `LoadUnExpFunctions()` |
| 驱动源码编译错误可忽略 | README：仓库已附带编译好的 `JiYuTrainerDriver.sys` |
| ZeroKill / 强制断电可能蓝屏 | 源码注释与 README 均提示风险 |
| 需绕过 `MmVerifyCallbackFunction` | `ldr->Flags |= 0x20`，用于旧系统回调注册 |

---

## 8. 小结（学习要点）

- 内核模块的典型骨架：**创建设备与符号链接 → 分发 IRP → 动态取函数 → 注册回调**。
- **未导出函数定位**（特征码/相对偏移反推）是内核开发的通用技巧。
- **进程保护**有两条路：现代系统用 `ObRegisterCallbacks` 剥离句柄权限，旧系统用 SSDT Hook。
- **强杀**层层加码：`NtTerminateProcess` → `ZwTerminateProcess` → APC 终止线程 → 暴力 `memset 0xCC`。
- CR0.WP 写保护开关是绕过"只读页不可写"这一限制的经典手段。

---

**上一篇** → [03 · Hooks 注入模块](./03-hooks-injection-module.md)　|　**下一篇** → [05 · UI 层](./05-ui-layer.md)
