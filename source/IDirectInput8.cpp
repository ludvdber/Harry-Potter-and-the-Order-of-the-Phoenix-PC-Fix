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

// ---- m_IDirectInputDevice8A ----

HRESULT m_IDirectInputDevice8A::QueryInterface(REFIID riid, LPVOID* ppvObj) { return ProxyInterface->QueryInterface(riid, ppvObj); }
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
	HRESULT hr = ProxyInterface->GetDeviceState(c, d);
	if (IsInputLost(hr))
	{
		if (!g_loggedReAcquireA) { g_loggedReAcquireA = true; WrapperLog("DirectInput[A] auto-re-Acquire: GetDeviceState hr=0x%X, retrying\n", hr); }
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceState(c, d);
	}
	return hr;
}
HRESULT m_IDirectInputDevice8A::GetDeviceData(DWORD c, LPDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f)
{
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

HRESULT m_IDirectInputDevice8W::QueryInterface(REFIID riid, LPVOID* ppvObj) { return ProxyInterface->QueryInterface(riid, ppvObj); }
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
	HRESULT hr = ProxyInterface->GetDeviceState(c, d);
	if (IsInputLost(hr))
	{
		if (!g_loggedReAcquireW) { g_loggedReAcquireW = true; WrapperLog("DirectInput[W] auto-re-Acquire: GetDeviceState hr=0x%X, retrying\n", hr); }
		if (SUCCEEDED(ProxyInterface->Acquire()))
			hr = ProxyInterface->GetDeviceState(c, d);
	}
	return hr;
}
HRESULT m_IDirectInputDevice8W::GetDeviceData(DWORD c, LPDIDEVICEOBJECTDATA r, LPDWORD p, DWORD f)
{
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

HRESULT m_IDirectInput8A::QueryInterface(REFIID riid, LPVOID* ppvObj) { return ProxyInterface->QueryInterface(riid, ppvObj); }
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

HRESULT m_IDirectInput8W::QueryInterface(REFIID riid, LPVOID* ppvObj) { return ProxyInterface->QueryInterface(riid, ppvObj); }
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

	auto it = originals.find("DirectInput8Create");
	if (it != originals.end())
		oDirectInput8Create = (DirectInput8Create_fn)it->second.get();
	else
		oDirectInput8Create = (DirectInput8Create_fn)GetProcAddress(dinput8, "DirectInput8Create");

	WrapperLog("DirectInput: DirectInput8Create hook %s\n", oDirectInput8Create ? "installed" : "FAILED to resolve");
}
