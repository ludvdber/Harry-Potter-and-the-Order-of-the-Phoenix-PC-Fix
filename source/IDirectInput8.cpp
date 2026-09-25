#include <windows.h>
#include <vector>
#include <algorithm>
#include "IDirectInput8.h"
#include "iathook.h"

void WrapperLog(const char* fmt, ...);

typedef HRESULT(WINAPI* DirectInput8Create_fn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_fn oDirectInput8Create = nullptr;

// Logged once the first time we silently recover an input-lost device, so the user can confirm
// the fix is firing during an Alt+Tab test. Spamming the log every poll is pointless.
static bool g_loggedReAcquireA = false;
static bool g_loggedReAcquireW = false;

static inline bool IsInputLost(HRESULT hr) { return hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED; }

// ---- Device tracking for forced re-Acquire on focus return ----
// Stores raw underlying pointers (not our wrappers) so we can call Acquire directly without
// re-entering our wrappers' methods. We classify by kind at CreateDevice time so the focus-return
// re-Acquire from CustomWndProc only touches mice — HP5's keyboard cycle cannot be recovered
// externally (the game's polling stays in a state we can't unstick without breaking it).
enum DIKind { DIK_Keyboard, DIK_Mouse_Or_Other };

static CRITICAL_SECTION g_diLock;
static bool g_diLockInit = false;
static std::vector<std::pair<LPDIRECTINPUTDEVICE8A, DIKind>> g_diDevsA;
static std::vector<std::pair<LPDIRECTINPUTDEVICE8W, DIKind>> g_diDevsW;

static void InitDILockOnce()
{
	if (!g_diLockInit) { InitializeCriticalSection(&g_diLock); g_diLockInit = true; }
}

static DIKind ClassifyDeviceGuid(REFGUID rguid)
{
	return (rguid == GUID_SysKeyboard) ? DIK_Keyboard : DIK_Mouse_Or_Other;
}

static void TrackDeviceA(LPDIRECTINPUTDEVICE8A dev, DIKind kind)
{
	InitDILockOnce();
	EnterCriticalSection(&g_diLock);
	g_diDevsA.push_back({ dev, kind });
	LeaveCriticalSection(&g_diLock);
}
static void UntrackDeviceA(LPDIRECTINPUTDEVICE8A dev)
{
	if (!g_diLockInit) return;
	EnterCriticalSection(&g_diLock);
	g_diDevsA.erase(std::remove_if(g_diDevsA.begin(), g_diDevsA.end(),
		[dev](const std::pair<LPDIRECTINPUTDEVICE8A, DIKind>& p) { return p.first == dev; }),
		g_diDevsA.end());
	LeaveCriticalSection(&g_diLock);
}
static void TrackDeviceW(LPDIRECTINPUTDEVICE8W dev, DIKind kind)
{
	InitDILockOnce();
	EnterCriticalSection(&g_diLock);
	g_diDevsW.push_back({ dev, kind });
	LeaveCriticalSection(&g_diLock);
}
static void UntrackDeviceW(LPDIRECTINPUTDEVICE8W dev)
{
	if (!g_diLockInit) return;
	EnterCriticalSection(&g_diLock);
	g_diDevsW.erase(std::remove_if(g_diDevsW.begin(), g_diDevsW.end(),
		[dev](const std::pair<LPDIRECTINPUTDEVICE8W, DIKind>& p) { return p.first == dev; }),
		g_diDevsW.end());
	LeaveCriticalSection(&g_diLock);
}

// First call of each kind per device, with its kind: tells which device the game polls
// (state) and which it reads as events (buffered data). Diagnostic, cheap after the first call.
static void NoteFirstCall(void* dev, const char* call, DWORD arg)
{
	static struct { void* dev; const char* call; } seen[64];
	static int count = 0;
	InitDILockOnce();
	EnterCriticalSection(&g_diLock);
	for (int i = 0; i < count; i++)
		if (seen[i].dev == dev && seen[i].call == call) { LeaveCriticalSection(&g_diLock); return; }
	if (count < 64) { seen[count].dev = dev; seen[count].call = call; count++; }
	const char* kind = "?";
	for (auto& d : g_diDevsA) if ((void*)d.first == dev) kind = d.second == DIK_Keyboard ? "keyboard" : "mouse/other";
	for (auto& d : g_diDevsW) if ((void*)d.first == dev) kind = d.second == DIK_Keyboard ? "keyboard" : "mouse/other";
	LeaveCriticalSection(&g_diLock);
	WrapperLog("DirectInput: %s %p first %s (%lu)\n", kind, dev, call, arg);
}

// A key released while the game is in the background never reaches DirectInput, which then
// keeps reporting it down after the return. Measured on HP6 (2026-09-25): W stayed down 4.7 s
// after an Alt+Tab taken while walking, until the key was pressed again, and Harry walked on
// his own. Keys that DirectInput reports down at the return are therefore shown to the game
// as up until DirectInput sees them released, unless Windows says they are really held now.
static void FilterStaleKeys(BYTE* keys, BYTE* stale, bool returned, void* dev)
{
	if (returned)
	{
		int n = 0;
		for (int k = 0; k < 256; k++) { stale[k] = (keys[k] & 0x80) ? 1 : 0; n += stale[k]; }
		if (n) WrapperLog("DirectInput: keyboard %p, %d key(s) still reported down at the return, held back until released\n", dev, n);
	}
	for (int k = 0; k < 256; k++)
	{
		if (!stale[k]) continue;
		if (!(keys[k] & 0x80)) { stale[k] = 0; continue; }
		// DIK codes are scan codes; the extended ones carry 0x80 instead of the E0 prefix.
		const UINT sc = (k & 0x80) ? (0xE000 | (k & 0x7F)) : (UINT)k;
		const UINT vk = MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX);
		if (vk && (GetAsyncKeyState((int)vk) & 0x8000)) { stale[k] = 0; continue; }
		keys[k] = 0;
	}
}

void DirectInputReAcquireMice()
{
	if (!g_diLockInit) return;
	int n = 0, ok = 0;
	EnterCriticalSection(&g_diLock);
	for (auto& p : g_diDevsA) if (p.second == DIK_Mouse_Or_Other) { n++; p.first->Unacquire(); if (SUCCEEDED(p.first->Acquire())) ok++; }
	for (auto& p : g_diDevsW) if (p.second == DIK_Mouse_Or_Other) { n++; p.first->Unacquire(); if (SUCCEEDED(p.first->Acquire())) ok++; }
	LeaveCriticalSection(&g_diLock);
	// Fires once per Alt+Tab return — cheap to log, confirms mouse recovery is working.
	WrapperLog("DirectInput: re-acquired mice %d/%d ok\n", ok, n);
}

// ---- Re-Acquire on return to the foreground, detected by polling ----
extern volatile LONG g_presentCount; // dllmain.cpp
static volatile LONG g_fgGeneration = 0;
static volatile LONG g_wasForeground = 1;

// The foreground window, asked of user32 itself rather than through our import table: with
// DoNotNotifyOnTaskSwitch = 1 the wrapper below us patches import tables so the game keeps
// believing it is in front. Through our own import, the watcher saw the game in front during a
// whole Alt+Tab (2026-09-25); Frida, calling the export directly, saw the truth.
using GetForegroundWindowFn = HWND(WINAPI*)();
static GetForegroundWindowFn RealGetForegroundWindow()
{
	static GetForegroundWindowFn fn = (GetForegroundWindowFn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetForegroundWindow");
	return fn ? fn : GetForegroundWindow;
}

// The system call behind it (win32u, Windows 10+). With DoNotNotifyOnTaskSwitch = 1, user32's
// own GetForegroundWindow ALSO kept answering "the game" during an Alt+Tab (2026-09-25): the
// patch is in the function itself, not only in import tables. No user32 patch reaches this one.
static GetForegroundWindowFn KernelGetForegroundWindow()
{
	static GetForegroundWindowFn fn = []() -> GetForegroundWindowFn {
		HMODULE w = GetModuleHandleA("win32u.dll");
		if (!w) w = LoadLibraryA("win32u.dll");
		return w ? (GetForegroundWindowFn)GetProcAddress(w, "NtUserGetForegroundWindow") : nullptr;
	}();
	return fn ? fn : RealGetForegroundWindow();
}

static DWORD ForegroundPid(HWND w)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	return pid;
}

static bool ProcessOwnsForeground()
{
	return ForegroundPid(KernelGetForegroundWindow()()) == GetCurrentProcessId();
}

static LONG UpdateForegroundState()
{
	const LONG fg = ProcessOwnsForeground() ? 1 : 0;
	const LONG before = InterlockedExchange(&g_wasForeground, fg);
	if (fg && !before)
	{
		const LONG gen = InterlockedIncrement(&g_fgGeneration);
		WrapperLog("DirectInput: back in the foreground (return #%ld, frame %ld), re-acquiring devices\n", gen, g_presentCount);
		return gen;
	}
	if (!fg && before) WrapperLog("DirectInput: left the foreground (frame %ld)\n", g_presentCount);
	return g_fgGeneration;
}

// The loss of focus must be seen even if the game stops reading its devices while unfocused;
// sampling only inside GetDeviceState never observed it. A watcher thread samples the
// foreground on its own; the device reads only compare generations.
static DWORD WINAPI ForegroundWatcher(LPVOID)
{
	for (;;)
	{
		UpdateForegroundState();
		Sleep(100);
	}
}

LONG DirectInputForegroundGeneration()
{
	static volatile LONG s_started = 0;
	if (InterlockedExchange(&s_started, 1) == 0)
	{
		DWORD tid = 0;
		HANDLE h = CreateThread(nullptr, 0, ForegroundWatcher, nullptr, 0, &tid);
		WrapperLog("DirectInput: foreground watcher %s (tid %lu)\n", h ? "started" : "FAILED to start", tid);
		if (h) CloseHandle(h);
	}
	return UpdateForegroundState();
}

bool m_IDirectInputDevice8A::ReAcquireIfForegroundReturned()
{
	const LONG gen = DirectInputForegroundGeneration();
	if (gen == SeenForegroundGeneration) return false;
	SeenForegroundGeneration = gen;
	ProxyInterface->Unacquire();
	const HRESULT hr = ProxyInterface->Acquire();
	WrapperLog("DirectInput[A]: device %p re-acquired after foreground return, hr=0x%X\n", ProxyInterface, hr);
	return true;
}

bool m_IDirectInputDevice8W::ReAcquireIfForegroundReturned()
{
	const LONG gen = DirectInputForegroundGeneration();
	if (gen == SeenForegroundGeneration) return false;
	SeenForegroundGeneration = gen;
	ProxyInterface->Unacquire();
	const HRESULT hr = ProxyInterface->Acquire();
	WrapperLog("DirectInput[W]: device %p re-acquired after foreground return, hr=0x%X\n", ProxyInterface, hr);
	return true;
}

// ---- m_IDirectInputDevice8A ----

HRESULT m_IDirectInputDevice8A::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
	HRESULT hr = ProxyInterface->QueryInterface(riid, ppvObj);
	// Same object asked for again (HP6 re-queries its devices): hand back the wrapper.
	// Returning the raw pointer silently took every later call - GetDeviceState
	// included - out of the wrapper, so nothing done here ever ran.
	if (SUCCEEDED(hr) && ppvObj && *ppvObj == (LPVOID)ProxyInterface) *ppvObj = this;
	return hr;
}
ULONG   m_IDirectInputDevice8A::AddRef() { return ProxyInterface->AddRef(); }
ULONG   m_IDirectInputDevice8A::Release()
{
	ULONG r = ProxyInterface->Release();
	if (r == 0) { UntrackDeviceA(ProxyInterface); delete this; }
	return r;
}

HRESULT m_IDirectInputDevice8A::GetCapabilities(LPDIDEVCAPS p) { return ProxyInterface->GetCapabilities(p); }
HRESULT m_IDirectInputDevice8A::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA c, LPVOID r, DWORD f) { return ProxyInterface->EnumObjects(c, r, f); }
HRESULT m_IDirectInputDevice8A::GetProperty(REFGUID g, LPDIPROPHEADER p) { return ProxyInterface->GetProperty(g, p); }
HRESULT m_IDirectInputDevice8A::SetProperty(REFGUID g, LPCDIPROPHEADER p) { return ProxyInterface->SetProperty(g, p); }
HRESULT m_IDirectInputDevice8A::Acquire() { return ProxyInterface->Acquire(); }
HRESULT m_IDirectInputDevice8A::Unacquire() { return ProxyInterface->Unacquire(); }
HRESULT m_IDirectInputDevice8A::GetDeviceState(DWORD c, LPVOID d)
{
	NoteFirstCall(ProxyInterface, "GetDeviceState", c);
	const bool returned = ReAcquireIfForegroundReturned();
	HRESULT hr = ProxyInterface->GetDeviceState(c, d);
	if (IsInputLost(hr))
	{
		if (!g_loggedReAcquireA) { g_loggedReAcquireA = true; WrapperLog("DirectInput[A] auto-re-Acquire: GetDeviceState hr=0x%X, retrying\n", hr); }
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceState(c, d);
	}
	if (c == 256 && SUCCEEDED(hr)) FilterStaleKeys((BYTE*)d, StaleKeys, returned, ProxyInterface);
	return hr;
}
HRESULT m_IDirectInputDevice8A::GetDeviceData(DWORD c, LPDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f)
{
	NoteFirstCall(ProxyInterface, "GetDeviceData", p ? *p : 0);
	ReAcquireIfForegroundReturned();
	HRESULT hr = ProxyInterface->GetDeviceData(c, r, p, f);
	if (IsInputLost(hr))
	{
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceData(c, r, p, f);
	}
	return hr;
}
HRESULT m_IDirectInputDevice8A::SetDataFormat(LPCDIDATAFORMAT d) { return ProxyInterface->SetDataFormat(d); }
HRESULT m_IDirectInputDevice8A::SetEventNotification(HANDLE h) { return ProxyInterface->SetEventNotification(h); }
HRESULT m_IDirectInputDevice8A::SetCooperativeLevel(HWND h, DWORD f) { return ProxyInterface->SetCooperativeLevel(h, f); }
HRESULT m_IDirectInputDevice8A::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA p, DWORD o, DWORD h) { return ProxyInterface->GetObjectInfo(p, o, h); }
HRESULT m_IDirectInputDevice8A::GetDeviceInfo(LPDIDEVICEINSTANCEA p) { return ProxyInterface->GetDeviceInfo(p); }
HRESULT m_IDirectInputDevice8A::RunControlPanel(HWND o, DWORD f) { return ProxyInterface->RunControlPanel(o, f); }
HRESULT m_IDirectInputDevice8A::Initialize(HINSTANCE h, DWORD v, REFGUID g) { return ProxyInterface->Initialize(h, v, g); }
HRESULT m_IDirectInputDevice8A::CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* p, LPUNKNOWN u) { return ProxyInterface->CreateEffect(g, e, p, u); }
HRESULT m_IDirectInputDevice8A::EnumEffects(LPDIENUMEFFECTSCALLBACKA c, LPVOID r, DWORD t) { return ProxyInterface->EnumEffects(c, r, t); }
HRESULT m_IDirectInputDevice8A::GetEffectInfo(LPDIEFFECTINFOA p, REFGUID g) { return ProxyInterface->GetEffectInfo(p, g); }
HRESULT m_IDirectInputDevice8A::GetForceFeedbackState(LPDWORD p) { return ProxyInterface->GetForceFeedbackState(p); }
HRESULT m_IDirectInputDevice8A::SendForceFeedbackCommand(DWORD f) { return ProxyInterface->SendForceFeedbackCommand(f); }
HRESULT m_IDirectInputDevice8A::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK c, LPVOID r, DWORD f) { return ProxyInterface->EnumCreatedEffectObjects(c, r, f); }
HRESULT m_IDirectInputDevice8A::Escape(LPDIEFFESCAPE e) { return ProxyInterface->Escape(e); }
HRESULT m_IDirectInputDevice8A::Poll()
{
	HRESULT hr = ProxyInterface->Poll();
	if (IsInputLost(hr))
	{
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->Poll();
	}
	return hr;
}
HRESULT m_IDirectInputDevice8A::SendDeviceData(DWORD c, LPCDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f) { return ProxyInterface->SendDeviceData(c, r, p, f); }
HRESULT m_IDirectInputDevice8A::EnumEffectsInFile(LPCSTR n, LPDIENUMEFFECTSINFILECALLBACK c, LPVOID r, DWORD f) { return ProxyInterface->EnumEffectsInFile(n, c, r, f); }
HRESULT m_IDirectInputDevice8A::WriteEffectToFile(LPCSTR n, DWORD e, LPDIFILEEFFECT r, DWORD f) { return ProxyInterface->WriteEffectToFile(n, e, r, f); }
HRESULT m_IDirectInputDevice8A::BuildActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) { return ProxyInterface->BuildActionMap(a, u, f); }
HRESULT m_IDirectInputDevice8A::SetActionMap(LPDIACTIONFORMATA a, LPCSTR u, DWORD f) { return ProxyInterface->SetActionMap(a, u, f); }
HRESULT m_IDirectInputDevice8A::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA p) { return ProxyInterface->GetImageInfo(p); }

// ---- m_IDirectInputDevice8W ----

HRESULT m_IDirectInputDevice8W::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
	HRESULT hr = ProxyInterface->QueryInterface(riid, ppvObj);
	// Same object asked for again (HP6 re-queries its devices): hand back the wrapper.
	// Returning the raw pointer silently took every later call - GetDeviceState
	// included - out of the wrapper, so nothing done here ever ran.
	if (SUCCEEDED(hr) && ppvObj && *ppvObj == (LPVOID)ProxyInterface) *ppvObj = this;
	return hr;
}
ULONG   m_IDirectInputDevice8W::AddRef() { return ProxyInterface->AddRef(); }
ULONG   m_IDirectInputDevice8W::Release()
{
	ULONG r = ProxyInterface->Release();
	if (r == 0) { UntrackDeviceW(ProxyInterface); delete this; }
	return r;
}

HRESULT m_IDirectInputDevice8W::GetCapabilities(LPDIDEVCAPS p) { return ProxyInterface->GetCapabilities(p); }
HRESULT m_IDirectInputDevice8W::EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW c, LPVOID r, DWORD f) { return ProxyInterface->EnumObjects(c, r, f); }
HRESULT m_IDirectInputDevice8W::GetProperty(REFGUID g, LPDIPROPHEADER p) { return ProxyInterface->GetProperty(g, p); }
HRESULT m_IDirectInputDevice8W::SetProperty(REFGUID g, LPCDIPROPHEADER p) { return ProxyInterface->SetProperty(g, p); }
HRESULT m_IDirectInputDevice8W::Acquire() { return ProxyInterface->Acquire(); }
HRESULT m_IDirectInputDevice8W::Unacquire() { return ProxyInterface->Unacquire(); }
HRESULT m_IDirectInputDevice8W::GetDeviceState(DWORD c, LPVOID d)
{
	NoteFirstCall(ProxyInterface, "GetDeviceState", c);
	const bool returned = ReAcquireIfForegroundReturned();
	HRESULT hr = ProxyInterface->GetDeviceState(c, d);
	if (IsInputLost(hr))
	{
		if (!g_loggedReAcquireW) { g_loggedReAcquireW = true; WrapperLog("DirectInput[W] auto-re-Acquire: GetDeviceState hr=0x%X, retrying\n", hr); }
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceState(c, d);
	}
	if (c == 256 && SUCCEEDED(hr)) FilterStaleKeys((BYTE*)d, StaleKeys, returned, ProxyInterface);
	return hr;
}
HRESULT m_IDirectInputDevice8W::GetDeviceData(DWORD c, LPDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f)
{
	NoteFirstCall(ProxyInterface, "GetDeviceData", p ? *p : 0);
	ReAcquireIfForegroundReturned();
	HRESULT hr = ProxyInterface->GetDeviceData(c, r, p, f);
	if (IsInputLost(hr))
	{
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceData(c, r, p, f);
	}
	return hr;
}
HRESULT m_IDirectInputDevice8W::SetDataFormat(LPCDIDATAFORMAT d) { return ProxyInterface->SetDataFormat(d); }
HRESULT m_IDirectInputDevice8W::SetEventNotification(HANDLE h) { return ProxyInterface->SetEventNotification(h); }
HRESULT m_IDirectInputDevice8W::SetCooperativeLevel(HWND h, DWORD f) { return ProxyInterface->SetCooperativeLevel(h, f); }
HRESULT m_IDirectInputDevice8W::GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW p, DWORD o, DWORD h) { return ProxyInterface->GetObjectInfo(p, o, h); }
HRESULT m_IDirectInputDevice8W::GetDeviceInfo(LPDIDEVICEINSTANCEW p) { return ProxyInterface->GetDeviceInfo(p); }
HRESULT m_IDirectInputDevice8W::RunControlPanel(HWND o, DWORD f) { return ProxyInterface->RunControlPanel(o, f); }
HRESULT m_IDirectInputDevice8W::Initialize(HINSTANCE h, DWORD v, REFGUID g) { return ProxyInterface->Initialize(h, v, g); }
HRESULT m_IDirectInputDevice8W::CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* p, LPUNKNOWN u) { return ProxyInterface->CreateEffect(g, e, p, u); }
HRESULT m_IDirectInputDevice8W::EnumEffects(LPDIENUMEFFECTSCALLBACKW c, LPVOID r, DWORD t) { return ProxyInterface->EnumEffects(c, r, t); }
HRESULT m_IDirectInputDevice8W::GetEffectInfo(LPDIEFFECTINFOW p, REFGUID g) { return ProxyInterface->GetEffectInfo(p, g); }
HRESULT m_IDirectInputDevice8W::GetForceFeedbackState(LPDWORD p) { return ProxyInterface->GetForceFeedbackState(p); }
HRESULT m_IDirectInputDevice8W::SendForceFeedbackCommand(DWORD f) { return ProxyInterface->SendForceFeedbackCommand(f); }
HRESULT m_IDirectInputDevice8W::EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK c, LPVOID r, DWORD f) { return ProxyInterface->EnumCreatedEffectObjects(c, r, f); }
HRESULT m_IDirectInputDevice8W::Escape(LPDIEFFESCAPE e) { return ProxyInterface->Escape(e); }
HRESULT m_IDirectInputDevice8W::Poll()
{
	HRESULT hr = ProxyInterface->Poll();
	if (IsInputLost(hr))
	{
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->Poll();
	}
	return hr;
}
HRESULT m_IDirectInputDevice8W::SendDeviceData(DWORD c, LPCDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f) { return ProxyInterface->SendDeviceData(c, r, p, f); }
HRESULT m_IDirectInputDevice8W::EnumEffectsInFile(LPCWSTR n, LPDIENUMEFFECTSINFILECALLBACK c, LPVOID r, DWORD f) { return ProxyInterface->EnumEffectsInFile(n, c, r, f); }
HRESULT m_IDirectInputDevice8W::WriteEffectToFile(LPCWSTR n, DWORD e, LPDIFILEEFFECT r, DWORD f) { return ProxyInterface->WriteEffectToFile(n, e, r, f); }
HRESULT m_IDirectInputDevice8W::BuildActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f) { return ProxyInterface->BuildActionMap(a, u, f); }
HRESULT m_IDirectInputDevice8W::SetActionMap(LPDIACTIONFORMATW a, LPCWSTR u, DWORD f) { return ProxyInterface->SetActionMap(a, u, f); }
HRESULT m_IDirectInputDevice8W::GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW p) { return ProxyInterface->GetImageInfo(p); }

// ---- m_IDirectInput8A ----

HRESULT m_IDirectInput8A::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
	HRESULT hr = ProxyInterface->QueryInterface(riid, ppvObj);
	// Same object asked for again (HP6 re-queries its devices): hand back the wrapper.
	// Returning the raw pointer silently took every later call - GetDeviceState
	// included - out of the wrapper, so nothing done here ever ran.
	if (SUCCEEDED(hr) && ppvObj && *ppvObj == (LPVOID)ProxyInterface) *ppvObj = this;
	return hr;
}
ULONG   m_IDirectInput8A::AddRef() { return ProxyInterface->AddRef(); }
ULONG   m_IDirectInput8A::Release()
{
	ULONG r = ProxyInterface->Release();
	if (r == 0) delete this;
	return r;
}

HRESULT m_IDirectInput8A::CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
	HRESULT hr = ProxyInterface->CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter);
	if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice)
	{
		LPDIRECTINPUTDEVICE8A underlying = *lplpDirectInputDevice;
		DIKind kind = ClassifyDeviceGuid(rguid);
		TrackDeviceA(underlying, kind);
		WrapperLog("DirectInput[A]: device wrapped (%s)\n", kind == DIK_Keyboard ? "keyboard" : "mouse/other");
		*lplpDirectInputDevice = new m_IDirectInputDevice8A(underlying);
	}
	return hr;
}
HRESULT m_IDirectInput8A::EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKA c, LPVOID r, DWORD f) { return ProxyInterface->EnumDevices(t, c, r, f); }
HRESULT m_IDirectInput8A::GetDeviceStatus(REFGUID g) { return ProxyInterface->GetDeviceStatus(g); }
HRESULT m_IDirectInput8A::RunControlPanel(HWND o, DWORD f) { return ProxyInterface->RunControlPanel(o, f); }
HRESULT m_IDirectInput8A::Initialize(HINSTANCE h, DWORD v) { return ProxyInterface->Initialize(h, v); }
HRESULT m_IDirectInput8A::FindDevice(REFGUID g, LPCSTR n, LPGUID p) { return ProxyInterface->FindDevice(g, n, p); }
HRESULT m_IDirectInput8A::EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID r, DWORD f) { return ProxyInterface->EnumDevicesBySemantics(u, a, c, r, f); }
HRESULT m_IDirectInput8A::ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK c, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID r) { return ProxyInterface->ConfigureDevices(c, p, f, r); }

// ---- m_IDirectInput8W ----

HRESULT m_IDirectInput8W::QueryInterface(REFIID riid, LPVOID* ppvObj)
{
	HRESULT hr = ProxyInterface->QueryInterface(riid, ppvObj);
	// Same object asked for again (HP6 re-queries its devices): hand back the wrapper.
	// Returning the raw pointer silently took every later call - GetDeviceState
	// included - out of the wrapper, so nothing done here ever ran.
	if (SUCCEEDED(hr) && ppvObj && *ppvObj == (LPVOID)ProxyInterface) *ppvObj = this;
	return hr;
}
ULONG   m_IDirectInput8W::AddRef() { return ProxyInterface->AddRef(); }
ULONG   m_IDirectInput8W::Release()
{
	ULONG r = ProxyInterface->Release();
	if (r == 0) delete this;
	return r;
}

HRESULT m_IDirectInput8W::CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8W* lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
	HRESULT hr = ProxyInterface->CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter);
	if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice)
	{
		LPDIRECTINPUTDEVICE8W underlying = *lplpDirectInputDevice;
		DIKind kind = ClassifyDeviceGuid(rguid);
		TrackDeviceW(underlying, kind);
		WrapperLog("DirectInput[W]: device wrapped (%s)\n", kind == DIK_Keyboard ? "keyboard" : "mouse/other");
		*lplpDirectInputDevice = new m_IDirectInputDevice8W(underlying);
	}
	return hr;
}
HRESULT m_IDirectInput8W::EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKW c, LPVOID r, DWORD f) { return ProxyInterface->EnumDevices(t, c, r, f); }
HRESULT m_IDirectInput8W::GetDeviceStatus(REFGUID g) { return ProxyInterface->GetDeviceStatus(g); }
HRESULT m_IDirectInput8W::RunControlPanel(HWND o, DWORD f) { return ProxyInterface->RunControlPanel(o, f); }
HRESULT m_IDirectInput8W::Initialize(HINSTANCE h, DWORD v) { return ProxyInterface->Initialize(h, v); }
HRESULT m_IDirectInput8W::FindDevice(REFGUID g, LPCWSTR n, LPGUID p) { return ProxyInterface->FindDevice(g, n, p); }
HRESULT m_IDirectInput8W::EnumDevicesBySemantics(LPCWSTR u, LPDIACTIONFORMATW a, LPDIENUMDEVICESBYSEMANTICSCBW c, LPVOID r, DWORD f) { return ProxyInterface->EnumDevicesBySemantics(u, a, c, r, f); }
HRESULT m_IDirectInput8W::ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK c, LPDICONFIGUREDEVICESPARAMSW p, DWORD f, LPVOID r) { return ProxyInterface->ConfigureDevices(c, p, f, r); }

// ---- DirectInput8Create IAT hook ----

static HRESULT WINAPI hk_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
	if (!oDirectInput8Create) return E_FAIL;
	HRESULT hr = oDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
	if (FAILED(hr) || !ppvOut || !*ppvOut) return hr;

	if (riidltf == IID_IDirectInput8A)
		*ppvOut = new m_IDirectInput8A((LPDIRECTINPUT8A)*ppvOut);
	else if (riidltf == IID_IDirectInput8W)
		*ppvOut = new m_IDirectInput8W((LPDIRECTINPUT8W)*ppvOut);
	// Unknown riid: leave unwrapped, the game will still get a valid DI8 pointer.
	return hr;
}

void InstallDirectInputHook()
{
	HMODULE mainModule = GetModuleHandleA(nullptr);
	HMODULE dinput8 = GetModuleHandleA("dinput8.dll");
	if (!dinput8)
	{
		WrapperLog("DirectInput: dinput8.dll not loaded at attach, hook skipped\n");
		return; // host doesn't use DI8, nothing to do
	}

	auto originals = IATHook::Replace(
		mainModule, "dinput8.dll",
		std::make_tuple("DirectInput8Create", (void*)hk_DirectInput8Create)
	);
	// HP4 ships GofInput.dll, which imports DirectInput8Create too. gof_f.exe does not name it
	// anywhere, so it is probably never loaded; hooked anyway if it is, and the log says which.
	if (HMODULE gof = GetModuleHandleA("GofInput.dll"))
	{
		auto gofOriginals = IATHook::Replace(gof, "dinput8.dll", std::make_tuple("DirectInput8Create", (void*)hk_DirectInput8Create));
		WrapperLog("DirectInput: GofInput.dll loaded, DirectInput8Create %s\n", gofOriginals.empty() ? "not imported" : "hooked");
	}

	auto it = originals.find("DirectInput8Create");
	if (it != originals.end())
		oDirectInput8Create = (DirectInput8Create_fn)it->second.get();
	else
		oDirectInput8Create = (DirectInput8Create_fn)GetProcAddress(dinput8, "DirectInput8Create");

	WrapperLog("DirectInput: DirectInput8Create hook %s\n", oDirectInput8Create ? "installed" : "FAILED to resolve");
}
