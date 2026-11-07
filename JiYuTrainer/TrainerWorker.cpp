#include "stdafx.h"
#include "TrainerWorker.h"
#include "JiYuTrainer.h"
#include "AppPublic.h"
#include "NtHlp.h"
#include "PathHelper.h"
#include "StringHlp.h"
#include "MsgCenter.h"
#include "StringSplit.h"
#include "DriverLoader.h"
#include "KernelUtils.h"
#include "SysHlp.h"
#include "SettingHlp.h"
#include "RegHlp.h"
#include <vector>
#include <rpc.h>
#include <fwpmu.h>
#pragma comment(lib, "fwpuclnt.lib")

extern JTApp *currentApp;
extern LoggerInternal * currentLogger;

TrainerWorkerInternal * TrainerWorkerInternal::currentTrainerWorker = nullptr;

extern NtQuerySystemInformationFun NtQuerySystemInformation;

using namespace std;

#define TIMER_RESET_PID 40115
#define TIMER_CK 40116

PSYSTEM_PROCESSES current_system_process = NULL;
static bool NameHasKey(const WCHAR* name, int nlen, const WCHAR* key)
{
	int kl = 0;
	while (key[kl]) kl++;
	for (int i = 0; i + kl <= nlen; i++) {
		int j = 0;
		for (; j < kl; j++) {
			WCHAR a = name[i + j], b = key[j];
			if (a >= L'A' && a <= L'Z') a = (WCHAR)(a + 32);
			if (b >= L'A' && b <= L'Z') b = (WCHAR)(b + 32);
			if (a != b) break;
		}
		if (j == kl) return true;
	}
	return false;
}
static bool KillOneProcess(DWORD pid)
{
	HANDLE hProcess = NULL;
	NTSTATUS status = MOpenProcessNt(pid, &hProcess);
	if (NT_SUCCESS(status) && hProcess) {
		status = MTerminateProcessNt(0, hProcess);
		CloseHandle(hProcess);
		if (NT_SUCCESS(status)) return true;
	}
	status = STATUS_UNSUCCESSFUL;
	if (KForceKill(pid, &status)) return true;
	return false;
}
static bool WfpNameHit(LPCWSTR s)
{
	if (!s || !*s) return false;
	WCHAR buf[512];
	wcsncpy_s(buf, 512, s, _TRUNCATE);
	_wcslwr_s(buf, 512);
	return wcsstr(buf, L"inspect") != NULL || wcsstr(buf, L"zds") != NULL ||
		wcsstr(buf, L"wfp-traffic") != NULL || wcsstr(buf, L"neicloud") != NULL ||
		wcsstr(buf, L"mmc") != NULL || wcsstr(buf, L"��������") != NULL;
}
static bool GuidInList(const std::vector<GUID>& list, const GUID& g)
{
	for (size_t i = 0; i < list.size(); i++) {
		if (memcmp(&list[i], &g, sizeof(GUID)) == 0) return true;
	}
	return false;
}
static int KClearWfpFilters()
{
	HANDLE engine = NULL;
	FWPM_SESSION0 session;
	ZeroMemory(&session, sizeof(session));
	if (FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &engine) != ERROR_SUCCESS)
		return -1;

	std::vector<GUID> suspectProv;
	FWPM_PROVIDER0** provs = NULL;
	UINT32 provCount = 0;
	HANDLE hEnum = NULL;
	if (FwpmProviderCreateEnumHandle0(engine, NULL, &hEnum) == ERROR_SUCCESS) {
		if (FwpmProviderEnum0(engine, hEnum, 0xFFFFFFFF, &provs, &provCount) == ERROR_SUCCESS) {
			for (UINT32 i = 0; i < provCount; i++) {
				if (WfpNameHit(provs[i]->displayData.name) || WfpNameHit(provs[i]->displayData.description) ||
					WfpNameHit(provs[i]->serviceName))
					suspectProv.push_back(provs[i]->providerKey);
			}
			FwpmFreeMemory0((void**)&provs);
		}
		FwpmProviderDestroyEnumHandle0(engine, hEnum);
	}

	std::vector<GUID> suspectSub;
	FWPM_SUBLAYER0** subs = NULL;
	UINT32 subCount = 0;
	hEnum = NULL;
	if (FwpmSubLayerCreateEnumHandle0(engine, NULL, &hEnum) == ERROR_SUCCESS) {
		if (FwpmSubLayerEnum0(engine, hEnum, 0xFFFFFFFF, &subs, &subCount) == ERROR_SUCCESS) {
			for (UINT32 i = 0; i < subCount; i++) {
				if (WfpNameHit(subs[i]->displayData.name) || WfpNameHit(subs[i]->displayData.description) ||
					(subs[i]->providerKey && GuidInList(suspectProv, *subs[i]->providerKey)))
					suspectSub.push_back(subs[i]->subLayerKey);
			}
			FwpmFreeMemory0((void**)&subs);
		}
		FwpmSubLayerDestroyEnumHandle0(engine, hEnum);
	}

	int removed = 0;
	FWPM_FILTER0** filters = NULL;
	UINT32 filtCount = 0;
	hEnum = NULL;
	if (FwpmFilterCreateEnumHandle0(engine, NULL, &hEnum) == ERROR_SUCCESS) {
		if (FwpmFilterEnum0(engine, hEnum, 0xFFFFFFFF, &filters, &filtCount) == ERROR_SUCCESS) {
			for (UINT32 i = 0; i < filtCount; i++) {
				FWPM_FILTER0* f = filters[i];
				bool match = false;
				if (f->providerKey && GuidInList(suspectProv, *f->providerKey)) match = true;
				if (GuidInList(suspectSub, f->subLayerKey)) match = true;
				if (WfpNameHit(f->displayData.name) || WfpNameHit(f->displayData.description)) match = true;
				if (match && FwpmFilterDeleteById0(engine, f->filterId) == ERROR_SUCCESS) removed++;
			}
			FwpmFreeMemory0((void**)&filters);
		}
		FwpmFilterDestroyEnumHandle0(engine, hEnum);
	}

	for (size_t i = 0; i < suspectSub.size(); i++)
		FwpmSubLayerDeleteByKey0(engine, &suspectSub[i]);

	FwpmEngineClose0(engine);
	return removed;
}
static int KStopInspectService()
{
	SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm) return -1;
	int stopped = 0;
	SC_HANDLE svc = OpenServiceW(scm, L"Inspect", SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (svc) {
		SERVICE_STATUS st;
		ZeroMemory(&st, sizeof(st));
		if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) stopped = 1;
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return stopped;
}
static void KClearNetworkLimit()
{
	static bool tried = false;
	int removed = KClearWfpFilters();
	int stopped = KStopInspectService();
	if (!tried || removed > 0 || stopped > 0) {
		tried = true;
		currentLogger->Log(L"[NetGuard] Inspect service stop=%d, WFP filters removed=%d", stopped, removed);
	}
}
static void KillHelperProcesses()
{
	if (!current_system_process) return;
	static const WCHAR* kHelperKeys[] = { L"blacksl", L"pdelkeyb", L"devicecontrol", L"processprotect" };
	static DWORD lastKilledPid = 0;
	bool done = false;
	for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
		done = p->NextEntryOffset == 0;
		if (!p->ImageName.Length) continue;
		DWORD pid = (DWORD)p->ProcessId;
		if (pid <= 4) continue;
		for (int i = 0; i < 4; i++) {
			if (NameHasKey(p->ImageName.Buffer, p->ImageName.Length / (int)sizeof(WCHAR), kHelperKeys[i])) {
				if (KillOneProcess(pid) && lastKilledPid != pid) {
					currentLogger->Log(L"[Kill] helper process terminated : %s [%d]", p->ImageName.Buffer, pid);
					lastKilledPid = pid;
				}
				break;
			}
		}
	}
}

TrainerWorkerInternal::TrainerWorkerInternal()
{
	currentTrainerWorker = this;

	//For message box center
	hMsgBoxHook = SetWindowsHookEx(
		WH_CBT,        // Type of hook 
		CBTProc,        // Hook procedure (see below)
		NULL,         // Module handle. Must be NULL (see docs)
		GetCurrentThreadId()  // Only install for THIS thread!!!
	);
}
TrainerWorkerInternal::~TrainerWorkerInternal()
{
	if (hDesktop) {
		CloseDesktop(hDesktop);
		hDesktop = NULL;
	}

	UnhookWindowsHookEx(hMsgBoxHook);

	StopInternal();
	ClearProcess();

	currentTrainerWorker = nullptr;
}

void TrainerWorkerInternal::Init()
{
	hDesktop = OpenDesktop(L"Default", 0, FALSE, DESKTOP_ENUMERATE);
	UpdateScreenSize();

	if (LocateStudentMainLocation()) currentLogger->Log(L"�Ѷ�λ������ӽ���λ�ã� %s", _StudentMainPath.c_str());
	else currentLogger->LogWarn(L"�޷���λ������ӽ���λ��");

	UpdateState();
	UpdateStudentMainInfo(false);

	InitSettings();
}
void TrainerWorkerInternal::InitSettings()
{
	SettingHlp*settings = currentApp->GetSettings();
	setAutoIncludeFullWindow = settings->GetSettingBool(L"AutoIncludeFullWindow");
	setCkInterval = currentApp->GetSettings()->GetSettingInt(L"CKInterval", 3100);
	setAllowGbTop = settings->GetSettingBool(L"AllowGbTop", false);
	if (setCkInterval < 1000 || setCkInterval > 10000) setCkInterval = 3000;

	if (_StudentMainControlled)
		SendMessageToVirus(L"hk:reset");
}
void TrainerWorkerInternal::UpdateScreenSize()
{
	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void TrainerWorkerInternal::Start()
{
	if (!_Running) 
	{
		_Running = true;

		//Settings
		
		KClearNetworkLimit();
		SetTimer(hWndMain, TIMER_CK, setCkInterval, TimerProc);
		SetTimer(hWndMain, TIMER_RESET_PID, 2000, TimerProc);

		UpdateState();

	}
}
void TrainerWorkerInternal::Stop()
{
	if (_Running) {
		StopInternal();
		UpdateState();
	}
}
void TrainerWorkerInternal::StopInternal() {
	if (_Running) {
		_Running = false;
		KillTimer(hWndMain, TIMER_CK);
		KillTimer(hWndMain, TIMER_RESET_PID);
	}
}

void TrainerWorkerInternal::SetUpdateInfoCallback(TrainerWorkerCallback *callback)
{
	if (callback) {
		_Callback = callback;
		hWndMain = callback->GetMainHWND();
	}
}

void TrainerWorkerInternal::HandleMessageFromVirus(LPCWSTR buf)
{
	wstring act(buf);
	vector<wstring> arr;
	SplitString(act, arr, L":");
	if (arr.size() >= 2)
	{
		if (arr[0] == L"hkb")
		{
			if (arr[1] == L"succ") {
				_StudentMainControlled = true;
				currentLogger->LogInfo(L"Receive ctl success message ");
				if (_Callback) _Callback->OnBeforeSendStartConf();
				UpdateState();

				//HS 
				FAST_STR_BINDER(str, L"hs:%ld", 32, (ULONG_PTR)hWndMain);
				SendMessageToVirus(str);
			}
			else if (arr[1] == L"immck") {
				RunCk();
				currentLogger->LogInfo(L"Receive  immck message ");
			}
			else if (arr[1] == L"jyk") {
				if (arr.size() >= 3) {
					_StudentMainRunningLock = arr[2] == L"1";
					UpdateState();
				}
			}
			else if (arr[1] == L"algbtop") {
				currentLogger->LogInfo(L"Receive allow gbTop message ");
				_Callback->OnAllowGbTop();
			}
			else if (arr[1] == L"gbmfull") {
				gbFullManual = true;
				ManualFull(gbFullManual);
			}
			else if (arr[1] == L"gbmnofull") {
				if (_FakeFull) FakeFull(false);
				gbFullManual = false;
				ManualFull(gbFullManual);
			}
			else if (arr[1] == L"gbuntop") ManualTop(false);
			else if (arr[1] == L"gbtop") ManualTop(true);
			else if (arr[1] == L"showhelp" && _Callback) _Callback->OnShowHelp();
			else if (arr[1] == L"wtf") {
				//�����˴����Ŀ�꣬��������״̬
				DWORD pid = _wtol(arr[2].c_str());
				AddFilterdPid(pid);

				if (_StudentMainFileLocatedByProcess)
					_StudentMainFileLocated = false;
				_StudentMainPid = 0;

				currentLogger->Log(L"��������ֵ� Student.exe");

				UpdateState();
				UpdateStudentMainInfo(false);
			}
		}
		else if (arr[0] == L"wcd")
		{
			//wwcd
			int wcdc = _wtoi(arr[1].c_str());
			if (wcdc % 20 == 0)
				currentLogger->LogInfo(L"Receive watch dog message %d ", wcdc);
		}
		else if (arr[0] == L"vback") {
			wstring strBuff = arr[1];
			if (arr.size() > 2) {
				for (UINT i = 2; i < arr.size(); i++)
					strBuff += L":" + arr[i];
			}
			currentLogger->Log(L"Receive virus echo : %s", strBuff.c_str());
		}
	}
}
void TrainerWorkerInternal::SendMessageToVirus(LPCWSTR buf)
{
	MsgCenterSendToVirus(buf, hWndMain);
}

bool TrainerWorkerInternal::Kill(bool autoWork)
{
	if (_StudentMainPid <= 4) {
		currentLogger->LogError(L"δ�ҵ�����������");
		return false;
	}
	if (_StudentMainControlled){
		bool vkill = autoWork;
		if(!vkill){
			MSGBOXPARAMSW mbp = { 0 };
			mbp.hwndOwner = hWndMain;
			mbp.lpszCaption = L"LtsTrainer - ��ʾ";
			mbp.lpszText = L"���Ƿ�ϣ��ʹ�ò������б��ƣ�";
			mbp.cbSize = sizeof(MSGBOXPARAMSW);
			mbp.dwStyle = MB_ICONEXCLAMATION | MB_TOPMOST | MB_YESNOCANCEL;
			int drs = MessageBoxIndirect(&mbp);
			if (drs == IDCANCEL) return false;
			else if (drs == IDYES) vkill = true;
		}
		if (vkill) {
			//Stop sginal
			SendMessageToVirus(L"ss2:0");
			return true;
		}
	}

	HANDLE hProcess;
	NTSTATUS status = MOpenProcessNt(_StudentMainPid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			return true;
		}
		else {
			currentLogger->LogError(L"�򿪽��̴���0x%08X�����ֶ�����", status);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		_StudentMainPid = 0;
		_StudentMainControlled = false;
		UpdateState();
		UpdateStudentMainInfo(!autoWork);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) goto FORCEKILL;
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			currentLogger->LogError(L"�������̴���0x%08X�����ֶ�����", status);
			if (!autoWork)
				MessageBox(hWndMain, L"�޷�����������ӽ��ң�����Ҫʹ�����������ֶ�����", L"LtsTrainer - ����", MB_ICONERROR);;
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
	}

FORCEKILL:
	if (XDriverLoaded() && MessageBox(hWndMain, L"��ͨ�޷��������룬�Ƿ���������������룿\n���������ܲ��ȶ��������á���Ҳ����ʹ�� PCHunter �Ȱ�ȫ��������ǿɱ��", L"LtsTrainer - ��ʾ", MB_ICONEXCLAMATION | MB_YESNO | MB_TOPMOST) == IDYES)
	{
		if (KForceKill(_StudentMainPid, &status)) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
		else if(!autoWork) MessageBox(hWndMain, L"����Ҳ�޷���������ʹ�� PCHunter �������ɣ�", L"����", MB_ICONEXCLAMATION);
	}
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::KillStAuto()
{
	RunCk();
	RunResetPid();

	return Kill(true);
}
bool TrainerWorkerInternal::Rerun(bool autoWork)
{
	if (!_StudentMainFileLocated) {
		currentLogger->LogWarn(L"δ�ҵ�������ӽ���");
		if (!autoWork && _Callback)
			_Callback->OnSimpleMessageCallback(L"<h5>�����޷��ڴ˼�������ҵ�������ӽ��ң�����Ҫ�ֶ�����</h5>");
		return false;
	}
	return  SysHlp::RunApplication(_StudentMainPath.c_str(), NULL);
}
void* TrainerWorkerInternal::RunOperation(TrainerWorkerOp op) 
{
	switch (op)
	{
	case TrainerWorkerOpVirusBoom: {
		MsgCenterSendToVirus(L"ss:0", hWndMain);
		return nullptr;
	}
	case TrainerWorkerOpVirusQuit: {
		MsgCenterSendToVirus((LPWSTR)L"hk:ckend", hWndMain);
		return nullptr;
	}
	case TrainerWorkerOp1: {
		WCHAR s[300]; swprintf_s(s, L"hk:path:%s", currentApp->GetFullPath());
		MsgCenterSendToVirus(s, hWndMain);
		swprintf_s(s, L"hk:mhwnd:%d", (LONG)hWndMain);
		MsgCenterSendToVirus(s, hWndMain);
		swprintf_s(s, L"hk:inipath:%s", currentApp->GetPartFullPath(PART_INI));
		MsgCenterSendToVirus(s, hWndMain);
		break;
	}
	case TrainerWorkerOpForceUnLoadVirus: {
		UnLoadAllVirus();
		break;
	}
	case TrainerWorkerOp2: {
		if (ReadTopDomanPassword(false))
			return (LPVOID)_TopDomainPassword.c_str();
		return nullptr;
	}
	case TrainerWorkerOp3: {
		if (ReadTopDomanPassword(true)) 
			return (LPVOID)_TopDomainPassword.c_str();
		return nullptr;
	}
	case TrainerWorkerOp4: {
		return (void*)UnLoadJiYuProtectDriver();
	}
	case TrainerWorkerOp5: {
		return (void*) UnLoadNetFilterDriver();
	}
	}
	return nullptr;
}

bool TrainerWorkerInternal::RunCk()
{
	_LastResolveWindowCount = 0;
	_LastResoveBroadcastWindow = false;
	_LastResoveBlackScreenWindow = false;
	_FirstBlackScreenWindow = false;

	EnumDesktopWindows(hDesktop, EnumWindowsProc, (LPARAM)this);

	MsgCenterSendHWNDS(hWndMain);

	return _LastResolveWindowCount > 0;
}
void TrainerWorkerInternal::RunResetPid()
{
	FlushProcess();

	//CK GET STAT DELAY
	if (_NextLoopGetCkStat) {
		_NextLoopGetCkStat = false;
				{ //Adapter: feed the freshly injected virus with our main window hwnd and paths
			WCHAR s[512];
			swprintf_s(s, L"hk:mhwnd:%d", (LONG)hWndMain);
			MsgCenterSendToVirus(s, hWndMain);
			swprintf_s(s, L"hk:path:%s", currentApp->GetFullPath());
			MsgCenterSendToVirus(s, hWndMain);
			swprintf_s(s, L"hk:inipath:%s", currentApp->GetPartFullPath(PART_INI));
			MsgCenterSendToVirus(s, hWndMain);
		}
		SendMessageToVirus(L"hk:ckstat");
	}

	//Find jiyu main process
	DWORD newPid = 0;
		DWORD bsPid = 0;
	if (LocateBlackSlient(&bsPid)) _BlackSlientPid = bsPid;
	else _BlackSlientPid = 0;
		KillHelperProcesses();
	static int netGuardTick = 0;
	if (++netGuardTick >= 5) { netGuardTick = 0; KClearNetworkLimit(); }

if (LocateStudentMain(&newPid)) { //�ҵ�����

		if (_StudentMainPid != newPid)
		{
			_StudentMainPid = newPid;

			if (InstallVirus()) {
				_VirusInstalled = true;
				_NextLoopGetCkStat = true;

				currentLogger->Log(L"�� Student.exe [%d] ע��DLL�ɹ�", newPid);
			}
			else  currentLogger->LogError(L"�� Student.exe [%d] ע��DLLʧ��", newPid);

			currentLogger->Log(L"������ Student.exe [%d]", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}
	}
	else { //û���ҵ�

		if (_StudentMainPid != 0)
		{
			_StudentMainPid = 0;

			currentLogger->Log(L"���������� Student.exe ���˳�", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}

	}

	newPid = 0;
	if (LocateMasterHelper(&newPid)) {
		if (_MasterHelperPid != newPid)
		{
			_MasterHelperPid = newPid;
			if (InstallVirusForMaster()) currentLogger->Log(L"�� MultiRender.exe [%d] ע��DLL�ɹ�", newPid);
			else  currentLogger->LogError(L"�� MultiRender.exe [%d] ע��DLLʧ��", newPid);
		}
	}
	else {
		_MasterHelperPid = 0;
	}
	newPid = 0;
	if (LocateMultiManager(&newPid)) {
		if (_MultiManagerPid != newPid)
		{
			_MultiManagerPid = newPid;
			if (InstallVirusForManager()) currentLogger->Log(L"[Inject] Multi.exe [%d] hooked", newPid);
			else  currentLogger->LogError(L"[Inject] Multi.exe [%d] hook failed", newPid);
		}
	}
	else {
		_MultiManagerPid = 0;
	}
}

bool TrainerWorkerInternal::FlushProcess()
{
	ClearProcess();

	DWORD dwSize = 0;
	NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &dwSize);
	if (status == STATUS_INFO_LENGTH_MISMATCH && dwSize > 0)
	{
		current_system_process = (PSYSTEM_PROCESSES)malloc(dwSize);
		status = NtQuerySystemInformation(SystemProcessInformation, current_system_process, dwSize, 0);
		if (!NT_SUCCESS(status)) {
			currentLogger->LogError2(L"NtQuerySystemInformation failed ! 0x%08X", status);
			return false;
		}
	}

	return true;
}
void TrainerWorkerInternal::ClearProcess()
{
	if (current_system_process) {
		free(current_system_process);
		current_system_process = NULL;
	}
}
bool TrainerWorkerInternal::FindProcess(LPCWSTR processName, DWORD * outPid)
{
	return false;
}
bool TrainerWorkerInternal::KillProcess(DWORD pid, bool force)
{
	HANDLE hProcess;
	NTSTATUS status = MOpenProcessNt(_StudentMainPid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"�Ҳ������� [%d] ", pid);
			return true;
		}
		else {
			currentLogger->LogError2(L"�򿪽��� [%d] ����0x%08X�����ֶ�����", pid);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		currentLogger->Log(L"���� [%d] �����ɹ�", pid);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) {
			if (force) goto FORCEKILL;
			else currentLogger->LogError2(L"�������� [%d] ���󣺾ܾ����ʡ��ɳ���ʹ����������", pid);
			CloseHandle(hProcess);
		}
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"�������� [%d] ����0x%08X�����ֶ�����", pid);
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			currentLogger->LogError2(L"�Ҳ������� [%d] ", pid);
			CloseHandle(hProcess);
			return true;
		}
	}
FORCEKILL:
	if (XDriverLoaded())
	{
		if (KForceKill(_StudentMainPid, &status)) {
			currentLogger->Log(L"���� [%d] ǿ�ƽ����ɹ�", pid);
			CloseHandle(hProcess);
			return true;
		}
		else {
			currentLogger->LogError2(L"����ǿ�ƽ������� [%d] ����0x%08X", pid);
		}
	}
	else currentLogger->Log(L"����δ���أ��޷�ǿ�ƽ�������");
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::ReadTopDomanPassword(BOOL forceKnock)
{
	_TopDomainPassword.clear();

	HKEY hKey;
	LRESULT lastError;

	if (forceKnock) goto READ_EX;
	//��ͨע�����ȡ��������4.0�汾

	WCHAR Data[32];
	if (MRegReadKeyString64And32(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\TopDomain\\e-Learning Class Standard\\1.00",
		L"SOFTWARE\\Wow6432Node\\TopDomain\\e-Learning Class Standard\\1.00", L"UninstallPasswd", Data, 32)) {
		
		if (StrEqual(Data, L"Passwd[123456]")) goto READ_EX; //6.0�Ժ��ȡ�����ˣ�����ʾPasswd[123456]�����µķ�����ȡ
		else {
			_TopDomainPassword = Data;
			_TopDomainPassword = _TopDomainPassword.substr(6, _TopDomainPassword.size() - 6);
			return true;
		}

	}
	else currentLogger->LogWarn2(L"MRegReadKeyString64And32 Failed : %s (%d)", PRINT_LAST_ERROR_STR);

	//HKEY_LOCAL_MACHINE\SOFTWARE\TopDomain\e-Learning Class\Student Knock1
READ_EX:

	lastError = RegOpenKeyEx(HKEY_LOCAL_MACHINE, SysHlp::Is64BitOS() ?
		L"SOFTWARE\\Wow6432Node\\TopDomain\\e-Learning Class\\Student" : 
		L"SOFTWARE\\TopDomain\\e-Learning Class\\Student", 0, KEY_WOW64_64KEY | KEY_READ, &hKey);
	if (lastError == ERROR_SUCCESS) {

		DWORD dwType = REG_BINARY;
		BYTE Data[120];
		DWORD cbData = 120;
		lastError = RegQueryValueEx(hKey, L"Knock1", 0, &dwType, (LPBYTE)Data, &cbData);
		if (lastError == ERROR_SUCCESS) {

			RegCloseKey(hKey);

			WCHAR ss[34] = { 0 };
			if (UnDecryptJiyuKnock(Data, cbData, ss)) {
				_TopDomainPassword = ss;
				return true;
			}
			else {
				currentLogger->LogWarn2(L"UnDecryptJiyuKnock failed !");
				return false;
			}
		}
		else currentLogger->LogWarn2(L"RegQueryValueEx Failed : %d", lastError);
		RegCloseKey(hKey);
	}
	else currentLogger->LogWarn2(L"RegOpenKeyEx Failed : %d", lastError);
	return false;
}
bool TrainerWorkerInternal::AppointStudentMainLocation(LPCWSTR fullPath) {
	if (Path::GetFileName(fullPath) != L"Student.exe")	return false;
	if (Path::Exists(fullPath)) 
	{
		_StudentMainPath = fullPath;
		_StudentMainFileLocated = true;
		_StudentMainFileLocatedByProcess = false;

		currentApp->GetSettings()->SetSettingStr(L"StudentMainPath", fullPath);
		currentLogger->Log(L"�ɹ��ֶ���λ������ӽ���λ�ã� %s", fullPath);

		UpdateStudentMainInfo(false);
		return true;
	}
	return false;
}
bool TrainerWorkerInternal::LocateStudentMainLocation()
{
	//ע������� ���� ·��
	WCHAR Data[MAX_PATH];
	if (MRegReadKeyString64And32(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\e-Learning Class V6.0",
		L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\e-Learning Class V6.0", 
		L"DisplayIcon", Data, MAX_PATH)) {

		if (Path::Exists(Data)) {
			_StudentMainPath = Data;
			_StudentMainFileLocated = true;
			return true;
		}
		else currentLogger->Log(L"��ȡע��� [DisplayIcon] �����һ����Ч��������ӽ���·�� : %s", Data);
	}
	else currentLogger->LogWarn2(L"MRegReadKeyString64And32 Failed : %s (%d)", PRINT_LAST_ERROR_STR);

	//��ȡ�û�ָ����·��
	wstring appointStudentMainPath  = currentApp->GetSettings()->GetSettingStr(L"StudentMainPath", L"", MAX_PATH);
	if (!StrEmepty(appointStudentMainPath.c_str()) && Path::Exists(appointStudentMainPath)) {
		wstring fileName = Path::GetFileName(appointStudentMainPath);
		if (StrEqual(fileName.c_str(), L"Student.exe")) {
			_StudentMainPath = appointStudentMainPath;
			_StudentMainFileLocated = true;
			return true;
		}
	}

	//ֱ�ӳ��Բ���
		//Lenovo teaching system install paths (primary target)
	static LPCWSTR ltsPaths[] = { L"C:\\Program Files (x86)\\Lenovo teaching system\\Student.exe", L"C:\\Program Files (x86)\\Lenovo teaching system\\MultiRender.exe", L"C:\\Program Files (x86)\\Lenovo teaching system\\Multi.exe", nullptr };
	for (int li = 0; ltsPaths[li]; li++) {
		if (Path::Exists(ltsPaths[li])) {
			_StudentMainPath = ltsPaths[li];
			_StudentMainFileLocated = true;
			_StudentMainFileLocatedByProcess = false;
			return true;
		}
	}
LPCWSTR mabeInHere[6] = {
		L"c:\\Program Files\\Mythware\\������ù���ϵͳ����V6.0 2016 ������\\StudentMain.exe",
		L"C:\\Program Files\\Mythware\\e-Learning Class\\StudentMain.exe",
		L"C:\\Program Files (x86)\\Mythware\\������ù���ϵͳ����V6.0 2016 ������\\StudentMain.exe",
		L"C:\\Program Files (x86)\\Mythware\\e - Learning Class\\StudentMain.exe",
		L"C:\\e-Learning Class\\StudentMain.exe",
		L"c:\\������ù���ϵͳ����V6.0 2016 ������\\StudentMain.exe",
	};
	for (int i = 0; i < 6; i++) {
		if (Path::Exists(mabeInHere[i])) {
			_StudentMainPath = mabeInHere[i];
			_StudentMainFileLocated = true;
			_StudentMainFileLocatedByProcess = false;
			return true;
		}
	}

	return false;
}
bool TrainerWorkerInternal::LocateStudentMain(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"Student.exe"))
			{
				if (CheckPidFilterd((DWORD)p->ProcessId)) continue;

				HANDLE hProcess;
				if (NT_SUCCESS(MOpenProcessNt((DWORD)p->ProcessId, &hProcess))) {
					WCHAR buffer[MAX_PATH];
					if (MGetProcessFullPathEx(hProcess, buffer)) {
						//���exe��ͬĿ¼���Ƿ���� LibTDMaster.dll ���������ų�
						PathRemoveFileSpec(buffer);
						wcscat_s(buffer, L"\\StudentLogic.dll");
						if (!PathFileExists(buffer)) {
							AddFilterdPid((DWORD)p->ProcessId);
							currentLogger->Log(L"��ֵ� Student.exe [%d]", (DWORD)p->ProcessId);
							continue;
						}

						//Exe ȷ��λ��
						if (!_StudentMainFileLocated) {
							currentLogger->Log(L"ͨ������ Student.exe [%d] ��λ��λ�ã� %s", (DWORD)p->ProcessId, _StudentMainPath);
							_StudentMainPath = buffer;
							_StudentMainFileLocated = true;
							_StudentMainFileLocatedByProcess = true;
						}
					}
					CloseHandle(hProcess);
				}

				if (outFirstPid) *outFirstPid = (DWORD)p->ProcessId;

				ClearFilterdPid();
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
		ClearFilterdPid();
	}
	return false;
}
bool TrainerWorkerInternal::LocateMultiManager(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"Multi.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}bool TrainerWorkerInternal::LocateMasterHelper(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"MultiRender.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}
bool TrainerWorkerInternal::CheckPidFilterd(DWORD pid)
{
	for (auto it = incorrectStudentMainPids.begin(); it != incorrectStudentMainPids.end(); it++) {
		if ((*it).pid == pid)
			return true;
	}
	return false;
}
void TrainerWorkerInternal::AddFilterdPid(DWORD pid)
{
	IncorrectStudentMainFilterData data;
	data.checked = true;
	data.pid = pid;
	incorrectStudentMainPids.push_back(data);
}
void TrainerWorkerInternal::ClearFilterdPid()
{
	if (current_system_process)
	{
		for (auto it = incorrectStudentMainPids.begin(); it != incorrectStudentMainPids.end(); it++) {
			auto pid = (*it).pid;
			bool founded = false;
			bool done = false;
			for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
				if ((DWORD)p->ProcessId == pid)
				{
					founded = true;
					break;
				}
				done = p->NextEntryOffset == 0;
			}
			if (!founded) 
				it = incorrectStudentMainPids.erase(it);
		}
	}
}


void TrainerWorkerInternal::UpdateStudentMainInfo(bool byUser)
{
	if (_Callback)
		_Callback->OnUpdateStudentMainInfo(_StudentMainPid > 4, _StudentMainPath.c_str(), _StudentMainPid, byUser);
}
void TrainerWorkerInternal::UpdateState()
{
	if (_Callback) 
	{
		TrainerWorkerCallback::TrainerStatus status;
		if (_StudentMainPid > 4) {
			if (_StudentMainControlled) {
				_StatusTextMain = L"�ѿ���������ӽ���";

				if (!_Running) {
					_StatusTextMain += L" ��������δ����";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_StudentMainRunningLock)
				{
					_StatusTextMore = L"��Ϊ������������ӽ���<br/>�����Է��ļ������Ĺ���";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlledAndUnLocked;
				}
				else {
					_StatusTextMore = L"�����Է��ļ������Ĺ���";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlled;
				}
			}
			else {
				_StatusTextMain = L"�޷�����������ӽ���";
				if (!_Running) {
					_StatusTextMain = L"������δ����";
					_StatusTextMore = L"�����ֶ�ֹͣ������<br / >��ǰ������������κβ���";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_VirusInstalled) {
					_StatusTextMore = L"���Ѳ������룬��δ��������<br / ><span style=\"color:#f41702\">��������Ҫ������������</span>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusUnknowProblem;
				}
				else {
					_StatusTextMore = L"��������ӽ��Ҳ��벡��ʧ��<br / >����������鿴 <a id=\"link_log\">��־</a>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControllFailed;
				}
			}
		}
		else {
			_StatusTextMain = L"������ӽ���δ����";
			if (!_Running) {
				_StatusTextMain = L"������ӽ���δ���� ���ҿ�����δ����";
				_StatusTextMore = L"�����ֶ�ֹͣ������<br / >��ǰ���������������";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
			}
			else if (_StudentMainFileLocated) {
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotRunning;
				_StatusTextMore = L"���ڴ˼�������ҵ�������ӽ���<br / >����Ե�� <b>�·���ť</b> ������";
			}
			else {
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotFound;
				_StatusTextMore = L"δ�ڴ˼�������ҵ�������ӽ���";
			}
		}

		_Callback->OnUpdateState(status, _StatusTextMain.c_str(), _StatusTextMore.c_str());
	}
}

bool TrainerWorkerInternal::LocateBlackSlient(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"BlackSlient.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}
bool TrainerWorkerInternal::InstallVirus()
{
	return InjectDll(_StudentMainPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InstallVirusForManager()
{
	return InjectDll(_MultiManagerPid, currentApp->GetPartFullPath(PART_HOOKER));
}bool TrainerWorkerInternal::InstallVirusForMaster()
{
	return InjectDll(_MasterHelperPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InjectDll(DWORD pid, LPCWSTR dllPath)
{
	HANDLE hRemoteProcess;
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hRemoteProcess);
	if (!NT_SUCCESS(ntStatus)) {
		currentLogger->LogError2(L"InjectDll: open process failed 0x%08X", ntStatus);
		return FALSE;
	}

	FARPROC pLoadLibraryW = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
	if (!pLoadLibraryW) {
		currentLogger->LogError2(L"InjectDll: cannot resolve LoadLibraryW");
		CloseHandle(hRemoteProcess);
		return FALSE;
	}

	SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(WCHAR);
	LPVOID pRemotePath = VirtualAllocEx(hRemoteProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!pRemotePath) {
		currentLogger->LogError2(L"InjectDll: VirtualAllocEx failed %d", GetLastError());
		CloseHandle(hRemoteProcess);
		return FALSE;
	}

	SIZE_T written = 0;
	if (!WriteProcessMemory(hRemoteProcess, pRemotePath, (LPVOID)dllPath, pathBytes, &written) || written != pathBytes) {
		currentLogger->LogError2(L"InjectDll: WriteProcessMemory failed %d (%u/%u)", GetLastError(), (unsigned)written, (unsigned)pathBytes);
		VirtualFreeEx(hRemoteProcess, pRemotePath, 0, MEM_RELEASE);
		CloseHandle(hRemoteProcess);
		return FALSE;
	}

	HANDLE hRemoteThread = CreateRemoteThread(hRemoteProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibraryW, pRemotePath, 0, NULL);
	if (!hRemoteThread) {
		currentLogger->LogError2(L"InjectDll: CreateRemoteThread failed %d", GetLastError());
		VirtualFreeEx(hRemoteProcess, pRemotePath, 0, MEM_RELEASE);
		CloseHandle(hRemoteProcess);
		return FALSE;
	}

	DWORD waitResult = WaitForSingleObject(hRemoteThread, 1500);
	DWORD exitCode = 0;
	GetExitCodeThread(hRemoteThread, &exitCode);
	CloseHandle(hRemoteThread);
	VirtualFreeEx(hRemoteProcess, pRemotePath, 0, MEM_RELEASE);
	CloseHandle(hRemoteProcess);

	if (waitResult == WAIT_TIMEOUT) {
		currentLogger->LogWarn(L"InjectDll: remote thread still busy, assume DLL loaded : %s", dllPath);
		return TRUE;
	}
	if (exitCode == 0) {
		currentLogger->LogError2(L"InjectDll: remote LoadLibraryW returned NULL, DLL not loaded : %s", dllPath);
		return FALSE;
	}
	currentLogger->Log(L"InjectDll: DLL loaded (module 0x%08X) : %s", exitCode, dllPath);
	return TRUE;
}
bool TrainerWorkerInternal::UnInjectDll(DWORD pid, LPCWSTR moduleName)
{
	HANDLE hProcess;
	//�򿪽���
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hProcess);
	if (!NT_SUCCESS(ntStatus)) {
		currentLogger->LogError2(L"ж�ز���ʧ�ܣ��򿪽���ʧ�ܣ�0x%08X", ntStatus);
		return FALSE;
	}
	DWORD pszLibFileRemoteSize = sizeof(wchar_t) * (lstrlen(moduleName) + 1);
	wchar_t *pszLibFileRemote;
	//ʹ��VirtualAllocEx������Զ�̽��̵��ڴ��ַ�ռ����DLL�ļ����ռ�
	pszLibFileRemote = (wchar_t *)VirtualAllocEx(hProcess, NULL, pszLibFileRemoteSize, MEM_COMMIT, PAGE_READWRITE);
	//ʹ��WriteProcessMemory������DLL��·����д�뵽Զ�̽��̵��ڴ�ռ�
	WriteProcessMemory(hProcess, pszLibFileRemote, (void *)moduleName, pszLibFileRemoteSize, NULL);

	DWORD dwHandle;
	DWORD dwID;
	LPVOID pFunc = GetProcAddress(GetModuleHandle(TEXT("Kernel32")), "GetModuleHandleW");
	HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pFunc, pszLibFileRemote, 0, &dwID);
	if (!hThread) {
		currentLogger->LogError2(L"ж�ز���ʧ�ܣ�����Զ���߳�ʧ�ܣ�%d", GetLastError());
		return FALSE;
	}

	// �ȴ�GetModuleHandle�������
	WaitForSingleObject(hThread, INFINITE);
	// ���GetModuleHandle�ķ���ֵ
	GetExitCodeThread(hThread, &dwHandle);
	// �ͷ�Ŀ�����������Ŀռ�
	VirtualFreeEx(hProcess, pszLibFileRemote, pszLibFileRemoteSize, MEM_DECOMMIT);
	CloseHandle(hThread);
	// ʹĿ����̵���FreeLibrary��ж��DLL
	pFunc = GetProcAddress(GetModuleHandle(TEXT("Kernel32")), "FreeLibrary"); ;
	hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pFunc, (LPVOID)dwHandle, 0, &dwID);
	if (!hThread) {
		currentLogger->LogError2(L"ж�ز���ʧ�ܣ�����Զ���߳�ʧ�ܣ�%d", GetLastError());
		return FALSE;
	}
	
	// �ȴ�FreeLibraryж�����
	WaitForSingleObject(hThread, INFINITE);
	CloseHandle(hThread);
	CloseHandle(hProcess);

	return true;
}
bool TrainerWorkerInternal::UnLoadAllVirus()
{
	if (_MultiManagerPid > 4) {
		if (UnInjectDll(_MultiManagerPid, L"LtsTrainerHooks.dll"))
			currentLogger->Log(L"[Unload] Multi.exe unhooked");
	}
	if (_MasterHelperPid > 4) {
		if (UnInjectDll(_MasterHelperPid, L"LtsTrainerHooks.dll"))
			currentLogger->Log(L"��ǿ��ж�� MasterHelper ����");
		//KillProcess(_MasterHelperPid, false);
	}
	if (_StudentMainPid > 4)
		if (UnInjectDll(_StudentMainPid, L"LtsTrainerHooks.dll"))
			currentLogger->Log(L"��ǿ��ж�� StudentMain ����");

	return false;
}
bool TrainerWorkerInternal::UnLoadNetFilterDriver()
{
	return MUnLoadDriverServiceWithMessage(L"TDNetFilter");
}
bool TrainerWorkerInternal::UnLoadJiYuProtectDriver()
{
	return false;
}

bool TrainerWorkerInternal::SwitchFakeFull()
{
	if (_FakeFull) { 
		_FakeFull = false; 
		SendMessageToVirus(L"hk:fkfull:false");
		FakeFull(_FakeFull);
	}
	else if(_StudentMainRunningLock) { 
		_FakeFull = true; 
		SendMessageToVirus(L"hk:fkfull:true");
		FakeFull(_FakeFull);
	}

	return _FakeFull;
}
void TrainerWorkerInternal::FakeFull(bool fk) {
	if (_CurrentBroadcastWnd)
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBroadcastWnd, GWL_STYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBroadcastWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			/*HWND jiYuGBDeskRdWnd = FindWindowExW(currentGbWnd, NULL, NULL, L"TDDesk Render Window");
			if (jiYuGBDeskRdWnd != NULL) {

			}*/
			_FakeBroadcastFull = true;
			currentLogger->Log(L"�����㲥���ڼ�װȫ��״̬");
		}
		else {
			_FakeBroadcastFull = false;
			FixWindow(_CurrentBroadcastWnd, (LPWSTR)L"��Ļ�㲥");
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			SetWindowPos(_CurrentBroadcastWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
			currentLogger->Log(L"ȡ���㲥���ڼ�װȫ��״̬");
		}
	}
	if (_CurrentBlackScreenWnd) 
	{
		if (fk) {
			SetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBlackScreenWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			_FakeBlackScreenFull = true;
			currentLogger->Log(L"�����������ڼ�װȫ��״̬");
		}
		else {
			_FakeBlackScreenFull = false;
			FixWindow(_CurrentBlackScreenWnd, (LPWSTR)L"BlackScreen Window");
			currentLogger->Log(L"ȡ���������ڼ�װȫ��״̬");
		}
	}
	if (!fk && !_CurrentBlackScreenWnd && !_CurrentBroadcastWnd && (_FakeBlackScreenFull || _FakeBroadcastFull)) {
		_FakeBroadcastFull = false;
		_FakeBlackScreenFull = false;
	}
}
void TrainerWorkerInternal::ManualFull(bool fk)
{
	currentLogger->Log(L"receive ManualFull %s", fk ? L"true" : L"false");
	if (_CurrentBroadcastWnd) 
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBroadcastWnd, GWL_STYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW) | WS_SYSMENU);
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBroadcastWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
		}
		else {
			FixWindow(_CurrentBroadcastWnd, (LPWSTR)L"��Ļ�㲥");
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			SetWindowPos(_CurrentBroadcastWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
		}
	}
}
void TrainerWorkerInternal::ManualTop(bool fk)
{
	currentLogger->Log(L"receive ManualTop %s", fk ? L"true" : L"false");
	if (_CurrentBroadcastWnd)
	{
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		else {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) ^ WS_EX_TOPMOST);
			SetWindowPos(_CurrentBroadcastWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}
}
bool TrainerWorkerInternal::ChecIsJIYuWindow(HWND hWnd, LPDWORD outPid, LPDWORD outTid) {
	if (_StudentMainPid == 0) return false;
	DWORD pid = 0, tid = GetWindowThreadProcessId(hWnd, &pid);
	if (outPid) *outPid = pid;
	if (outTid) *outTid = tid;
	return pid == _StudentMainPid || pid == _MasterHelperPid || pid == _MultiManagerPid;
}
bool TrainerWorkerInternal::IsBlackSlientWindow(HWND hWnd) {
	if (_BlackSlientPid <= 4) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hWnd, &pid);
	return pid == _BlackSlientPid;
}
bool TrainerWorkerInternal::IsMultiRenderClassWindow(HWND hWnd) {
	WCHAR cls[128];
	if (!GetClassNameW(hWnd, cls, 128)) return false;
	return StringHlp::StrEqualW(cls, L"MultiRenderWindowClass");
}
bool TrainerWorkerInternal::CheckIsTargetWindow(LPWSTR text, HWND hWnd) {
	bool b = false;
	
	if (StrEqual(text, L"BlackScreen Window") || IsBlackSlientWindow(hWnd)) {
		b = true;
		_LastResoveBlackScreenWindow = true;
		if (!_FirstBlackScreenWindow) {
			_FirstBlackScreenWindow = true;
			if (_Callback) _Callback->OnResolveBlackScreenWindow();
		}
		_CurrentBlackScreenWnd = hWnd;
		if (_FakeBlackScreenFull) return false;
	}
	else if (CheckWindowTextIsGb(text) || IsMultiRenderClassWindow(hWnd)) {
		b = true;
		_LastResoveBroadcastWindow = true;
		_CurrentBroadcastWnd = hWnd;
		if (_FakeBroadcastFull) return false;
	}
	return b;
}
void TrainerWorkerInternal::FixWindow(HWND hWnd, LPWSTR text)
{
	_LastResolveWindowCount++;

	LONG oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);

	if (StrEqual(text, L"BlackScreen Window"))
	{
		oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
			SetWindowPos(hWnd, 0, 20, 20, 90, 150, SWP_NOZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
			ShowWindow(hWnd, SW_HIDE);
		}
	}
	//Un top
	if (CheckWindowTextIsGb(text))
	{
		if (!setAllowGbTop && (oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_TOPMOST);
			SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		if (!gbFullManual) 
		{
			//Set border and sizeable
			SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));
		}
	}
	else if(setAutoIncludeFullWindow)
	{
		if ((oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_TOPMOST);
			SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}

		//Set border and sizeable
		SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));

	}

	SetWindowPos(hWnd, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE | SWP_DRAWFRAME | SWP_NOACTIVATE);
}
bool TrainerWorkerInternal::CheckWindowTextIsGb(const wchar_t* text) {
	return StringHlp::StrContainsW(text, L"�㲥", nullptr) || StringHlp::StrContainsW(text, L"��ʾ", nullptr)
		|| StringHlp::StrContainsW(text, L"����", nullptr)
		|| StringHlp::StrEqualW(text, L"��Ļ�ݲ��Ҵ���");
}

BOOL CALLBACK TrainerWorkerInternal::EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
	TrainerWorkerInternal *self =(TrainerWorkerInternal *)lParam;
	if (IsWindowVisible(hWnd) && self->ChecIsJIYuWindow(hWnd, NULL, NULL)) {
		WCHAR text[50];
		GetWindowText(hWnd, text, 50);
		if (StrEqual(text, L"LtsTrainer Virus Window")) return TRUE;

		RECT rc;
		GetWindowRect(hWnd, &rc);
		if (self->CheckIsTargetWindow(text, hWnd)) {
			//JiYu window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
		else if (self->setAutoIncludeFullWindow && rc.top == 0 && rc.left == 0 && rc.right == self->screenWidth && rc.bottom == self->screenHeight) {
			//Full window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
	}
	return TRUE;
}
VOID CALLBACK TrainerWorkerInternal::TimerProc(HWND hWnd, UINT message, UINT_PTR iTimerID, DWORD dwTime)
{
	if (currentTrainerWorker != nullptr) 
	{
		if (iTimerID == TIMER_RESET_PID) {
			currentTrainerWorker->RunResetPid();
		}
		if (iTimerID == TIMER_CK) {
			currentTrainerWorker->RunCk();
		}
	}
}
LRESULT CALLBACK TrainerWorkerInternal::CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode < 0)
		return CallNextHookEx(currentTrainerWorker->hMsgBoxHook, nCode, wParam, lParam);
	switch (nCode)
	{
	case HCBT_ACTIVATE: {
		// ����wParam�о���message box�ľ��
		HWND hWnd = (HWND)wParam;
		HWND hWndOwner = GetWindow(hWnd, GW_OWNER);

		// �����Ѿ�����message box�ľ�������������ǾͿ��Զ���message box��!
		if (hWndOwner && hWndOwner  == currentTrainerWorker->hWndMain)
		{
			//�����ڸ����ھ���
			RECT rect; GetWindowRect(hWnd, &rect);
			RECT rectParent; GetWindowRect(hWndOwner, &rectParent);
			rect.left = ((rectParent.right - rectParent.left) - (rect.right - rect.left)) / 2 + rectParent.left;
			rect.top = ((rectParent.bottom - rectParent.top) - (rect.bottom - rect.top)) / 2 + rectParent.top;
			SetWindowPos(hWnd, 0, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}
		return 0;
	}
	}
	// Call the next hook, if there is one
	return CallNextHookEx(currentTrainerWorker->hMsgBoxHook, nCode, wParam, lParam);
}

bool UnDecryptJiyuKnock(BYTE* Data, DWORD cbData, WCHAR* ss)
{
	//������Ĵ���
	__try {
		DWORD v5; // esi
		DWORD v6; // ecx
		BYTE *v7; // eax
		DWORD v8; // edx
		BYTE *i;
		v5 = cbData;
		v6 = cbData >> 2;
		v7 = Data;
		if (cbData >> 2)
		{
			v8 = cbData >> 2;
			do
			{
				*(DWORD *)v7 ^= 0x50434C45u;
				v7 += 4;
				--v8;
			} while (v8);
		}
		for (i = Data; v6; --v6)
		{
			*(DWORD *)i ^= 0x454C4350u;
			i += 4;
		}
		WORD v4[34];
		v4[0] = 0;
		memset(&v4[1], 0, 0x40u);

		int a1 = (int)&v4;

		int v13; // edi
		BYTE *v14; // eax
		__int16 v15; // cx
		v13 = a1 - Data[0];
		v14 = &Data[Data[0]];
		do
		{
			v15 = *(WORD *)v14;
			*(WORD *)&v14[v13 - (DWORD)Data] = *(WORD *)v14;
			v14 += 2;
		} while (v15);


		for (int i = 0; i < 32; i++) ss[i] = v4[i];
		return true;
	}
	__except (1) {
		return false;
	}
}
