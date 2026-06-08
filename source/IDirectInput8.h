#pragma once

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// Installs an IAT hook on the host process for dinput8.dll!DirectInput8Create.
// Called once from DllMain after dinput8.dll is loaded by the host process.
void InstallDirectInputHook();

// Force re-Acquire on every tracked DI8 mouse device. Called from CustomWndProc on
// WM_ACTIVATEAPP(TRUE) to recover from the "ghost acquired" state that HP5's mouse polling
// gets stuck in after an Alt+Tab return.
//
// Keyboard recovery via the same mechanism does NOT work on HP5 — the focus messages
// WM_ACTIVATE/WM_ACTIVATEAPP/WM_SETFOCUS don't reach our wndproc subclass after the first
// activation (only WM_NCACTIVATE does), so we have no reliable hook to fire a keyboard
// re-Acquire on. Users have to relaunch the game to recover keyboard input after Alt+Tab.
void DirectInputReAcquireMice();

class m_IDirectInputDevice8A : public IDirectInputDevice8A
{
private:
	LPDIRECTINPUTDEVICE8A ProxyInterface;
public:
	m_IDirectInputDevice8A(LPDIRECTINPUTDEVICE8A p) : ProxyInterface(p) {}

	STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	STDMETHOD(GetCapabilities)(LPDIDEVCAPS lpDIDevCaps);
	STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(GetProperty)(REFGUID rguidProp, LPDIPROPHEADER pdiph);
	STDMETHOD(SetProperty)(REFGUID rguidProp, LPCDIPROPHEADER pdiph);
	STDMETHOD(Acquire)();
	STDMETHOD(Unacquire)();
	STDMETHOD(GetDeviceState)(DWORD cbData, LPVOID lpvData);
	STDMETHOD(GetDeviceData)(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);
	STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT lpdf);
	STDMETHOD(SetEventNotification)(HANDLE hEvent);
	STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD dwFlags);
	STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA pdidoi, DWORD dwObj, DWORD dwHow);
	STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA pdidi);
	STDMETHOD(RunControlPanel)(HWND hwndOwner, DWORD dwFlags);
	STDMETHOD(Initialize)(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid);
	STDMETHOD(CreateEffect)(REFGUID rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT* ppdeff, LPUNKNOWN punkOuter);
	STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwEffType);
	STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA pdei, REFGUID rguid);
	STDMETHOD(GetForceFeedbackState)(LPDWORD pdwOut);
	STDMETHOD(SendForceFeedbackCommand)(DWORD dwFlags);
	STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl);
	STDMETHOD(Escape)(LPDIEFFESCAPE pesc);
	STDMETHOD(Poll)();
	STDMETHOD(SendDeviceData)(DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl);
	STDMETHOD(EnumEffectsInFile)(LPCSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK pec, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(WriteEffectToFile)(LPCSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFileEft, DWORD dwFlags);
	STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags);
	STDMETHOD(SetActionMap)(LPDIACTIONFORMATA lpdiActionFormat, LPCSTR lptszUserName, DWORD dwFlags);
	STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA lpdiDevImageInfoHeader);
};

class m_IDirectInputDevice8W : public IDirectInputDevice8W
{
private:
	LPDIRECTINPUTDEVICE8W ProxyInterface;
public:
	m_IDirectInputDevice8W(LPDIRECTINPUTDEVICE8W p) : ProxyInterface(p) {}

	STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	STDMETHOD(GetCapabilities)(LPDIDEVCAPS lpDIDevCaps);
	STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(GetProperty)(REFGUID rguidProp, LPDIPROPHEADER pdiph);
	STDMETHOD(SetProperty)(REFGUID rguidProp, LPCDIPROPHEADER pdiph);
	STDMETHOD(Acquire)();
	STDMETHOD(Unacquire)();
	STDMETHOD(GetDeviceState)(DWORD cbData, LPVOID lpvData);
	STDMETHOD(GetDeviceData)(DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);
	STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT lpdf);
	STDMETHOD(SetEventNotification)(HANDLE hEvent);
	STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD dwFlags);
	STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEW pdidoi, DWORD dwObj, DWORD dwHow);
	STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEW pdidi);
	STDMETHOD(RunControlPanel)(HWND hwndOwner, DWORD dwFlags);
	STDMETHOD(Initialize)(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid);
	STDMETHOD(CreateEffect)(REFGUID rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT* ppdeff, LPUNKNOWN punkOuter);
	STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKW lpCallback, LPVOID pvRef, DWORD dwEffType);
	STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOW pdei, REFGUID rguid);
	STDMETHOD(GetForceFeedbackState)(LPDWORD pdwOut);
	STDMETHOD(SendForceFeedbackCommand)(DWORD dwFlags);
	STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD fl);
	STDMETHOD(Escape)(LPDIEFFESCAPE pesc);
	STDMETHOD(Poll)();
	STDMETHOD(SendDeviceData)(DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD fl);
	STDMETHOD(EnumEffectsInFile)(LPCWSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK pec, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(WriteEffectToFile)(LPCWSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT rgDiFileEft, DWORD dwFlags);
	STDMETHOD(BuildActionMap)(LPDIACTIONFORMATW lpdiaf, LPCWSTR lpszUserName, DWORD dwFlags);
	STDMETHOD(SetActionMap)(LPDIACTIONFORMATW lpdiActionFormat, LPCWSTR lptszUserName, DWORD dwFlags);
	STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERW lpdiDevImageInfoHeader);
};

class m_IDirectInput8A : public IDirectInput8A
{
private:
	LPDIRECTINPUT8A ProxyInterface;
public:
	m_IDirectInput8A(LPDIRECTINPUT8A p) : ProxyInterface(p) {}

	STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	STDMETHOD(CreateDevice)(REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
	STDMETHOD(EnumDevices)(DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(GetDeviceStatus)(REFGUID rguidInstance);
	STDMETHOD(RunControlPanel)(HWND hwndOwner, DWORD dwFlags);
	STDMETHOD(Initialize)(HINSTANCE hinst, DWORD dwVersion);
	STDMETHOD(FindDevice)(REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance);
	STDMETHOD(EnumDevicesBySemantics)(LPCSTR ptszUserName, LPDIACTIONFORMATA lpdiActionFormat, LPDIENUMDEVICESBYSEMANTICSCBA lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSA lpdiCDParams, DWORD dwFlags, LPVOID pvRefData);
};

class m_IDirectInput8W : public IDirectInput8W
{
private:
	LPDIRECTINPUT8W ProxyInterface;
public:
	m_IDirectInput8W(LPDIRECTINPUT8W p) : ProxyInterface(p) {}

	STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppvObj);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	STDMETHOD(CreateDevice)(REFGUID rguid, LPDIRECTINPUTDEVICE8W* lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
	STDMETHOD(EnumDevices)(DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(GetDeviceStatus)(REFGUID rguidInstance);
	STDMETHOD(RunControlPanel)(HWND hwndOwner, DWORD dwFlags);
	STDMETHOD(Initialize)(HINSTANCE hinst, DWORD dwVersion);
	STDMETHOD(FindDevice)(REFGUID rguidClass, LPCWSTR ptszName, LPGUID pguidInstance);
	STDMETHOD(EnumDevicesBySemantics)(LPCWSTR ptszUserName, LPDIACTIONFORMATW lpdiActionFormat, LPDIENUMDEVICESBYSEMANTICSCBW lpCallback, LPVOID pvRef, DWORD dwFlags);
	STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSW lpdiCDParams, DWORD dwFlags, LPVOID pvRefData);
};
