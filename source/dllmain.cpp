/**
* Copyright (C) 2020 Elisha Riedlinger
*
* This software is  provided 'as-is', without any express  or implied  warranty. In no event will the
* authors be held liable for any damages arising from the use of this software.
* Permission  is granted  to anyone  to use  this software  for  any  purpose,  including  commercial
* applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*   1. The origin of this software must not be misrepresented; you must not claim that you  wrote the
*      original  software. If you use this  software  in a product, an  acknowledgment in the product
*      documentation would be appreciated but is not required.
*   2. Altered source versions must  be plainly  marked as such, and  must not be  misrepresented  as
*      being the original software.
*   3. This notice may not be removed or altered from any source distribution.
*/

#include "d3d9.h"
#include "d3dx9.h"
#include "iathook.h"
#include "helpers.h"
#include "IDirectInput8.h"
#include <cstdio>
#include <unordered_map>

#pragma comment(lib, "d3dx9.lib")
#pragma comment(lib, "winmm.lib") // needed for timeBeginPeriod()/timeEndPeriod()

static FILE* g_log = nullptr;
void WrapperLog(const char* fmt, ...)
{
	if (!g_log) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fflush(g_log);
}

// Surface-creation census for the shadow-map investigation: log every render-target / depth
// surface the game creates so shadow-map candidates (small square depth targets recreated per
// level) can be identified from the log. Capped — the interesting creations happen at startup
// and level loads; a per-frame allocator would otherwise flood the file.
void LogSurfaceCensus(const char* kind, UINT W, UINT H, UINT fmt, DWORD usage, UINT pool, UINT msaa)
{
	static int s_lines = 0;
	if (s_lines > 400) return;
	if (++s_lines > 400) { WrapperLog("Census: 400-line cap reached, further surface creations not logged\n"); return; }
	WrapperLog("Census %-6s %ux%u fmt=0x%X usage=0x%X pool=%u msaa=%u\n", kind, W, H, fmt, usage, pool, msaa);
}

Direct3DShaderValidatorCreate9Proc m_pDirect3DShaderValidatorCreate9;
PSGPErrorProc m_pPSGPError;
PSGPSampleTextureProc m_pPSGPSampleTexture;
D3DPERF_BeginEventProc m_pD3DPERF_BeginEvent;
D3DPERF_EndEventProc m_pD3DPERF_EndEvent;
D3DPERF_GetStatusProc m_pD3DPERF_GetStatus;
D3DPERF_QueryRepeatFrameProc m_pD3DPERF_QueryRepeatFrame;
D3DPERF_SetMarkerProc m_pD3DPERF_SetMarker;
D3DPERF_SetOptionsProc m_pD3DPERF_SetOptions;
D3DPERF_SetRegionProc m_pD3DPERF_SetRegion;
DebugSetLevelProc m_pDebugSetLevel;
DebugSetMuteProc m_pDebugSetMute;
Direct3D9EnableMaximizedWindowedModeShimProc m_pDirect3D9EnableMaximizedWindowedModeShim;
Direct3DCreate9Proc m_pDirect3DCreate9;
Direct3DCreate9ExProc m_pDirect3DCreate9Ex;

HWND g_hFocusWindow = NULL;
HMODULE g_hWrapperModule = NULL;

HMODULE d3d9dll = NULL;

bool bForceWindowedMode;
bool bUsePrimaryMonitor;
bool bCenterWindow;
bool bAlwaysOnTop;
bool bDoNotNotifyOnTaskSwitch;
bool bDisplayFPSCounter;
bool bEnableHooks;
bool bCaptureMouse;
bool bFreeMouse;
float fFPSLimit;
int nFullScreenRefreshRateInHz;
int nForceWindowStyle;
int nAntialiasing;
int nAnisotropicFiltering;
bool bVSync;
float fTextureLODBias;
int nResolutionWidth;
int nResolutionHeight;
int nBackBufferWidth = 0;
int nBackBufferHeight = 0;
bool bFXAA;
float fSharpness = 0.25f;
int nSSAAFactor = 1;
bool bColorGrading;
float fVibrance;
float fVignette;
float fLift;
float fGamma;
float fGain;
bool  bSSAO;
float fSSAOStrength;
float fSSAORadius;
float fSSAOMinDelta;
float fSSAOMaxDelta;
int nScreenshotKey;
bool  g_intzChecked = false;
bool  g_intzSupported = false;
IDirect3DTexture9* g_sceneDepthTex = nullptr;
UINT  g_sceneDepthW = 0, g_sceneDepthH = 0;
// Diagnostics shared with IDirect3DDevice9Ex.cpp. g_sceneDepthSurf/g_curDepthReal are raw
// compare-only pointers (no ref held — g_sceneDepthTex's ref keeps the surface alive).
IDirect3DSurface9* g_sceneDepthSurf = nullptr;
IDirect3DSurface9* g_curDepthReal = nullptr;
// Dimensions of the render target the game binds alongside the scene depth. After a Reset the
// game can create a window-sized depth surface (e.g. 2560x1440) while our back buffer stays at
// the ini resolution — the depth content then only fills the RT-sized top-left region, and the
// SSAO shader must scale its UVs by RT/depth to sample it correctly.
UINT g_sceneDepthRTW = 0, g_sceneDepthRTH = 0;
int g_dsBindCount = 0;        // per-frame SetDepthStencilSurface(non-null) calls
int g_dsNullUnbindCount = 0;  // per-frame SetDepthStencilSurface(NULL) calls
int g_dsSceneUnbindOnBB = 0;  // per-frame scene-depth unbinds while RT0 == back buffer
int g_mipRegenCount = 0;      // cumulative successful mip-chain regenerations
bool g_aoDoneThisFrame = false; // set when the depth-unbind AO pass ran this frame (option B)

// ---- Shadow-map upscaling (ShadowMapScale > 1) ----
// HP5 renders its projected shadows into small square X8R8G8B8 render-target textures
// (128/256/512, paired with same-size depth — see the Census log lines). We hand the game an
// NxN-times larger texture; since it believes the original size, every viewport it sets on that
// target is scaled up by the same factor (SetViewport interception). Shadow lookups use
// normalized UVs, so sampling needs no change. NOTE: the game reuses this same surface signature
// for non-shadow render-to-texture (reflections/projected passes); we cannot tell them apart, so
// this is EXPERIMENTAL — it may also upscale those. Default OFF in the shipped ini.
int nShadowMapScale = 1;
// Keyed by *real* (unwrapped) pointers. Single render thread — no locking needed for HP5.
static std::unordered_map<IDirect3DSurface9*, std::pair<UINT, UINT>> g_upscaledRTBySurf; // surf -> original WxH
static std::unordered_map<IDirect3DTexture9*, IDirect3DSurface9*> g_upscaledRTByTex;     // tex -> its level-0 surf
bool g_curRTUpscaled = false; // RT0 currently bound is an upscaled shadow target
UINT g_curRTOrigW = 0, g_curRTOrigH = 0;

void TrackUpscaledRT(IDirect3DTexture9* realTex, IDirect3DSurface9* realSurf, UINT origW, UINT origH)
{
	g_upscaledRTBySurf[realSurf] = { origW, origH };
	g_upscaledRTByTex[realTex] = realSurf;
}

// Called when the game fully releases an upscaled texture — the entry must go away, otherwise
// a later allocation reusing the same address would be mistaken for a shadow target.
void UntrackUpscaledTexture(IDirect3DTexture9* realTex)
{
	auto it = g_upscaledRTByTex.find(realTex);
	if (it != g_upscaledRTByTex.end())
	{
		g_upscaledRTBySurf.erase(it->second);
		g_upscaledRTByTex.erase(it);
	}
}

bool LookupUpscaledRT(IDirect3DSurface9* realSurf, UINT* origW, UINT* origH)
{
	if (!realSurf) return false;
	auto it = g_upscaledRTBySurf.find(realSurf);
	if (it == g_upscaledRTBySurf.end()) return false;
	*origW = it->second.first;
	*origH = it->second.second;
	return true;
}

// DEFAULT-pool resources are all released across a Reset/CreateDevice; anything still tracked
// at that point is (about to be) a dangling key.
void ClearUpscaledRTs()
{
	g_upscaledRTBySurf.clear();
	g_upscaledRTByTex.clear();
	g_curRTUpscaled = false;
	g_curRTOrigW = g_curRTOrigH = 0;
}

char WinDir[MAX_PATH + 1];

// List of registered window classes and procedures
// WORD classAtom, ULONG_PTR WndProcPtr
std::vector<std::pair<WORD, ULONG_PTR>> WndProcList;

void HookModule(HMODULE hmod);
LRESULT WINAPI CustomWndProcA(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CustomWndProcW(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

class FrameLimiter
{
private:
	static inline double TIME_Frequency = 0.0;
	static inline double TIME_Ticks = 0.0;
	static inline double TIME_Frametime = 0.0;

public:
	static inline ID3DXFont* pFPSFont = nullptr;
	static inline ID3DXFont* pTimeFont = nullptr;

public:
	enum FPSLimitMode { FPS_NONE, FPS_REALTIME, FPS_ACCURATE };
	static void Init(FPSLimitMode mode)
	{
		LARGE_INTEGER frequency;

		QueryPerformanceFrequency(&frequency);
		static constexpr auto TICKS_PER_FRAME = 1;
		auto TICKS_PER_SECOND = (TICKS_PER_FRAME * fFPSLimit);
		if (mode == FPS_ACCURATE)
		{
			TIME_Frametime = 1000.0 / (double)fFPSLimit;
			TIME_Frequency = (double)frequency.QuadPart / 1000.0; // ticks are milliseconds
		}
		else // FPS_REALTIME
		{
			TIME_Frequency = (double)frequency.QuadPart / (double)TICKS_PER_SECOND; // ticks are 1/n frames (n = fFPSLimit)
		}
		Ticks();
	}
	static DWORD Sync_RT()
	{
		DWORD lastTicks, currentTicks;
		LARGE_INTEGER counter;

		QueryPerformanceCounter(&counter);
		lastTicks = (DWORD)TIME_Ticks;
		TIME_Ticks = (double)counter.QuadPart / TIME_Frequency;
		currentTicks = (DWORD)TIME_Ticks;

		return (currentTicks > lastTicks) ? currentTicks - lastTicks : 0;
	}
	static DWORD Sync_SLP()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		double millis_current = (double)counter.QuadPart / TIME_Frequency;
		double millis_delta = millis_current - TIME_Ticks;
		if (TIME_Frametime <= millis_delta)
		{
			TIME_Ticks = millis_current;
			return 1;
		}
		else if (TIME_Frametime - millis_delta > 2.0) // > 2ms
			Sleep(1); // Sleep for ~1ms
		else
			Sleep(0); // yield thread's time-slice (does not actually sleep)

		return 0;
	}
	static void ShowFPS(LPDIRECT3DDEVICE9EX device)
	{
		static std::list<int> m_times;

		//https://github.com/microsoft/VCSamples/blob/master/VC2012Samples/Windows%208%20samples/C%2B%2B/Windows%208%20app%20samples/Direct2D%20geometry%20realization%20sample%20(Windows%208)/C%2B%2B/FPSCounter.cpp#L279
		LARGE_INTEGER frequency;
		LARGE_INTEGER time;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&time);

		if (m_times.size() == 50)
			m_times.pop_front();
		m_times.push_back(static_cast<int>(time.QuadPart));

		uint32_t fps = 0;
		if (m_times.size() >= 2)
			fps = static_cast<uint32_t>(0.5f + (static_cast<float>(m_times.size() - 1) * static_cast<float>(frequency.QuadPart)) / static_cast<float>(m_times.back() - m_times.front()));

		static int space = 0;
		if (!pFPSFont || !pTimeFont)
		{
			D3DDEVICE_CREATION_PARAMETERS cparams;
			RECT rect;
			device->GetCreationParameters(&cparams);
			GetClientRect(cparams.hFocusWindow, &rect);

			D3DXFONT_DESC fps_font;
			ZeroMemory(&fps_font, sizeof(D3DXFONT_DESC));
			fps_font.Height = rect.bottom / 20;
			fps_font.Width = 0;
			fps_font.Weight = 400;
			fps_font.MipLevels = 0;
			fps_font.Italic = 0;
			fps_font.CharSet = DEFAULT_CHARSET;
			fps_font.OutputPrecision = OUT_DEFAULT_PRECIS;
			fps_font.Quality = ANTIALIASED_QUALITY;
			fps_font.PitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
			wchar_t FaceName[] = L"Arial";
			memcpy(&fps_font.FaceName, &FaceName, sizeof(FaceName));

			D3DXFONT_DESC time_font = fps_font;
			time_font.Height = rect.bottom / 35;
			space = fps_font.Height + 5;

			if (D3DXCreateFontIndirect(device, &fps_font, &pFPSFont) != D3D_OK)
				return;

			if (D3DXCreateFontIndirect(device, &time_font, &pTimeFont) != D3D_OK)
				return;
		}
		else
		{
			auto DrawTextOutline = [](ID3DXFont* pFont, FLOAT X, FLOAT Y, D3DXCOLOR dColor, CONST PCHAR cString, ...)
				{
					const D3DXCOLOR BLACK(D3DCOLOR_XRGB(0, 0, 0));
					CHAR cBuffer[101] = "";

					va_list oArgs;
					va_start(oArgs, cString);
					_vsnprintf((cBuffer + strlen(cBuffer)), (sizeof(cBuffer) - strlen(cBuffer)), cString, oArgs);
					va_end(oArgs);

					RECT Rect[5] =
					{
						{ X - 1, Y, X + 500.0f, Y + 50.0f },
						{ X, Y - 1, X + 500.0f, Y + 50.0f },
						{ X + 1, Y, X + 500.0f, Y + 50.0f },
						{ X, Y + 1, X + 500.0f, Y + 50.0f },
						{ X, Y, X + 500.0f, Y + 50.0f },
					};

					if (dColor != BLACK)
					{
						for (auto i = 0; i < 4; i++)
							pFont->DrawText(NULL, cBuffer, -1, &Rect[i], DT_NOCLIP, BLACK);
					}

					pFont->DrawText(NULL, cBuffer, -1, &Rect[4], DT_NOCLIP, dColor);
				};

			static char str_format_fps[] = "%02d";
			static char str_format_time[] = "%.01f ms";
			static const D3DXCOLOR YELLOW(D3DCOLOR_XRGB(0xF7, 0xF7, 0));
			DrawTextOutline(pFPSFont, 10, 10, YELLOW, str_format_fps, fps);
			DrawTextOutline(pTimeFont, 10, space, YELLOW, str_format_time, (1.0f / fps) * 1000.0f);
		}
	}

private:
	static void Ticks()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		TIME_Ticks = (double)counter.QuadPart / TIME_Frequency;
	}
};

FrameLimiter::FPSLimitMode mFPSLimitMode = FrameLimiter::FPSLimitMode::FPS_NONE;

extern void DeviceFXAAPresent(IDirect3DDevice9* dev);

// Screenshot hotkey (ini [MAIN] ScreenshotKey, default F12, 0 = off). Runs after the FXAA pass
// so the PNG shows exactly what reaches the screen, post-processing included. Exists because
// OS-level capture (Win+PrtScr) is unreliable over this game's input handling.
static void MaybeTakeScreenshot(IDirect3DDevice9* dev)
{
	if (!nScreenshotKey)
		return;
	static bool s_wasDown = false;
	bool down = (GetAsyncKeyState(nScreenshotKey) & 0x8000) != 0;
	bool fire = down && !s_wasDown;
	s_wasDown = down;
	if (!fire)
		return;

	IDirect3DSurface9* bb = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	D3DSURFACE_DESC desc;
	bb->GetDesc(&desc);

	// The back buffer can be multisampled (MSAA) — not lockable/saveable directly.
	// StretchRect into a plain render target performs the resolve.
	IDirect3DSurface9* tmp = nullptr;
	if (SUCCEEDED(dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &tmp, nullptr)) && tmp
		&& SUCCEEDED(dev->StretchRect(bb, nullptr, tmp, nullptr, D3DTEXF_NONE)))
	{
		char dir[MAX_PATH];
		GetModuleFileNameA(g_hWrapperModule, dir, MAX_PATH);
		strcpy(strrchr(dir, '\\'), "\\screenshots");
		CreateDirectoryA(dir, nullptr);
		SYSTEMTIME st;
		GetLocalTime(&st);
		char file[MAX_PATH];
		_snprintf(file, MAX_PATH, "%s\\hp5_%04u%02u%02u_%02u%02u%02u.png", dir,
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		HRESULT hr = D3DXSaveSurfaceToFileA(file, D3DXIFF_PNG, tmp, nullptr, nullptr);
		if (SUCCEEDED(hr))
			WrapperLog("Screenshot saved: %s\n", file);
		else
			WrapperLog("Screenshot FAILED (hr=0x%08X): %s\n", (unsigned)hr, file);
	}
	if (tmp) tmp->Release();
	bb->Release();
}

// Frame diagnostics: reports the per-frame depth bind/unbind structure (data needed to decide
// whether the future "AO at depth-unbind" pass is viable) plus cumulative health counters.
// Detailed lines for the 10 frames after the scene depth texture first appears, then one
// summary line every 1800 presents so a long session stays readable.
static void PresentDiagTick()
{
	static unsigned s_frame = 0;
	static unsigned s_winBinds = 0, s_winNull = 0, s_winSceneBB = 0;
	static int s_detail = 0;
	static IDirect3DTexture9* s_lastDepthTex = nullptr;

	s_frame++;
	s_winBinds += g_dsBindCount;
	s_winNull += g_dsNullUnbindCount;
	s_winSceneBB += g_dsSceneUnbindOnBB;

	if (g_sceneDepthTex != s_lastDepthTex)
	{
		s_lastDepthTex = g_sceneDepthTex;
		if (g_sceneDepthTex)
		{
			s_detail = 10;
			WrapperLog("Diag: scene depth texture appeared (%ux%u), detailing next 10 frames\n", g_sceneDepthW, g_sceneDepthH);
		}
	}

	if (s_detail > 0)
	{
		s_detail--;
		WrapperLog("Diag frame %u: dsBinds=%d dsNullUnbinds=%d sceneDepthUnbinds(RT=backbuffer)=%d\n",
			s_frame, g_dsBindCount, g_dsNullUnbindCount, g_dsSceneUnbindOnBB);
	}
	else if (s_frame % 1800 == 0)
	{
		WrapperLog("Diag @%u frames: avg/frame dsBinds=%.1f dsNullUnbinds=%.1f sceneUnbindsOnBB=%.1f | mipRegen total=%d | sceneDepth=%s\n",
			s_frame, s_winBinds / 1800.0, s_winNull / 1800.0, s_winSceneBB / 1800.0,
			g_mipRegenCount, g_sceneDepthTex ? "cached" : "none");
		s_winBinds = s_winNull = s_winSceneBB = 0;
	}

	g_dsBindCount = g_dsNullUnbindCount = g_dsSceneUnbindOnBB = 0;
	// Re-arm the depth-unbind AO pass for the next frame.
	g_aoDoneThisFrame = false;
}

HRESULT m_IDirect3DDevice9Ex::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
{
	if (mFPSLimitMode == FrameLimiter::FPSLimitMode::FPS_REALTIME)
		while (!FrameLimiter::Sync_RT());
	else if (mFPSLimitMode == FrameLimiter::FPSLimitMode::FPS_ACCURATE)
		while (!FrameLimiter::Sync_SLP());

	DeviceFXAAPresent(ProxyInterface);
	MaybeTakeScreenshot(ProxyInterface);
	PresentDiagTick();
	return ProxyInterface->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT m_IDirect3DDevice9Ex::PresentEx(THIS_ CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags)
{
	if (mFPSLimitMode == FrameLimiter::FPSLimitMode::FPS_REALTIME)
		while (!FrameLimiter::Sync_RT());
	else if (mFPSLimitMode == FrameLimiter::FPSLimitMode::FPS_ACCURATE)
		while (!FrameLimiter::Sync_SLP());

	DeviceFXAAPresent(ProxyInterface);
	MaybeTakeScreenshot(ProxyInterface);
	PresentDiagTick();
	return ProxyInterface->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT m_IDirect3DDevice9Ex::EndScene()
{
	if (bDisplayFPSCounter)
		FrameLimiter::ShowFPS(ProxyInterface);

	return ProxyInterface->EndScene();
}

void CaptureMouse(HWND hWnd)
{
	RECT window_rect;
	GetWindowRect(hWnd, &window_rect);
	if (window_rect.left < 0)
		window_rect.left = 0;
	if (window_rect.top < 0)
		window_rect.top = 0;
	SetCapture(hWnd);
	ClipCursor(&window_rect);
}

void ForceWindowed(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode = NULL)
{
	HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : g_hFocusWindow;
	HMONITOR monitor = MonitorFromWindow((!bUsePrimaryMonitor && hwnd) ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
	MONITORINFO info;
	info.cbSize = sizeof(MONITORINFO);
	GetMonitorInfo(monitor, &info);
	int DesktopResX = info.rcMonitor.right - info.rcMonitor.left;
	int DesktopResY = info.rcMonitor.bottom - info.rcMonitor.top;

	int left = (int)info.rcMonitor.left;
	int top = (int)info.rcMonitor.top;

	if (nForceWindowStyle != 1) // not borderless fullscreen
	{
		left += (int)(((float)DesktopResX / 2.0f) - ((float)pPresentationParameters->BackBufferWidth / 2.0f));
		top += (int)(((float)DesktopResY / 2.0f) - ((float)pPresentationParameters->BackBufferHeight / 2.0f));
	}

	pPresentationParameters->Windowed = 1;

	// This must be set to default (0) on windowed mode as per D3D9 spec
	pPresentationParameters->FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;

	// If exists, this must match the rate in PresentationParameters
	if (pFullscreenDisplayMode != NULL)
		pFullscreenDisplayMode->RefreshRate = D3DPRESENT_RATE_DEFAULT;

	// This flag is not available on windowed mode as per D3D9 spec
	pPresentationParameters->PresentationInterval &= ~D3DPRESENT_DONOTFLIP;

	if (hwnd != NULL)
	{
		int cx, cy;
		UINT uFlags = SWP_SHOWWINDOW;
		LONG lOldStyle = GetWindowLong(hwnd, GWL_STYLE);
		LONG lOldExStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
		LONG lNewStyle, lNewExStyle;

		lOldExStyle &= ~(WS_EX_TOPMOST);

		if (nForceWindowStyle == 1)
		{
			cx = DesktopResX;
			cy = DesktopResY;
		}
		else
		{
			cx = pPresentationParameters->BackBufferWidth;
			cy = pPresentationParameters->BackBufferHeight;

			if (!bCenterWindow)
				uFlags |= SWP_NOMOVE;
		}

		switch (nForceWindowStyle)
		{
		case 1: // borderless fullscreen
		case 4: // borderless window (no style)
			lNewStyle = lOldStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_DLGFRAME);
			lNewStyle |= (lOldStyle & WS_CHILD) ? 0 : WS_POPUP;
			lNewExStyle = lOldExStyle & ~(WS_EX_CONTEXTHELP | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW);
			lNewExStyle |= WS_EX_APPWINDOW;
			break;
		case 2: // window
			lNewStyle = (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
			lNewExStyle = (WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
			break;
		case 3: // resizable window
			lNewStyle = (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX);
			lNewExStyle = (WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
			break;
		}

		if (nForceWindowStyle)
		{
			if (lNewStyle != lOldStyle)
			{
				SetWindowLong(hwnd, GWL_STYLE, lNewStyle);
				uFlags |= SWP_FRAMECHANGED;
			}
			if (lNewExStyle != lOldExStyle)
			{
				SetWindowLong(hwnd, GWL_EXSTYLE, lNewExStyle);
				uFlags |= SWP_FRAMECHANGED;
			}
		}
		SetWindowPos(hwnd, bAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, left, top, cx, cy, uFlags);

		if (bDoNotNotifyOnTaskSwitch || bCaptureMouse)
		{
			if (bCaptureMouse)
				CaptureMouse(hwnd);

			WORD wClassAtom = GetClassWord(hwnd, GCW_ATOM);
			if (wClassAtom != 0)
			{
				bool found = false;
				for (unsigned int i = 0; i < WndProcList.size(); i++) {
					if (WndProcList[i].first == wClassAtom) {
						found = true;
						break;
					}
				}
				if (!found)
				{
					LONG_PTR wndproc = GetWindowLongPtr(hwnd, GWLP_WNDPROC);
					if (wndproc && !IsBadCodePtr((FARPROC)wndproc))
					{
						WndProcList.emplace_back(wClassAtom, wndproc);
						SetWindowLongPtr(hwnd, GWLP_WNDPROC, IsWindowUnicode(hwnd) ? (LONG_PTR)CustomWndProcW : (LONG_PTR)CustomWndProcA);
						WrapperLog("ForceWindowed: wndproc subclassed (focus-loss swallowing %s)\n",
							bDoNotNotifyOnTaskSwitch ? "armed" : "off");
					}
				}
			}
		}
	}
}

void ForceFullScreenRefreshRateInHz(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	if (!pPresentationParameters->Windowed)
	{
		std::vector<int> list;
		DISPLAY_DEVICE dd;
		dd.cb = sizeof(DISPLAY_DEVICE);
		DWORD deviceNum = 0;
		while (EnumDisplayDevices(NULL, deviceNum, &dd, 0))
		{
			DISPLAY_DEVICE newdd = { 0 };
			newdd.cb = sizeof(DISPLAY_DEVICE);
			DWORD monitorNum = 0;
			DEVMODE dm = { 0 };
			while (EnumDisplayDevices(dd.DeviceName, monitorNum, &newdd, 0))
			{
				for (auto iModeNum = 0; EnumDisplaySettings(NULL, iModeNum, &dm) != 0; iModeNum++)
					list.emplace_back(dm.dmDisplayFrequency);
				monitorNum++;
			}
			deviceNum++;
		}

		std::sort(list.begin(), list.end());
		if (nFullScreenRefreshRateInHz > list.back() || nFullScreenRefreshRateInHz < list.front() || nFullScreenRefreshRateInHz < 0)
			pPresentationParameters->FullScreen_RefreshRateInHz = list.back();
		else
			pPresentationParameters->FullScreen_RefreshRateInHz = nFullScreenRefreshRateInHz;
	}
}

// INTZ FOURCC: a depth-stencil format that's also sample-able as a regular texture.
// Supported by every D3D10-class GPU (so any GPU made since ~2007).
#ifndef D3DFMT_INTZ
#define D3DFMT_INTZ ((D3DFORMAT)MAKEFOURCC('I','N','T','Z'))
#endif

bool CheckINTZSupport(IDirect3D9* pD3D9, UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT /*ignored*/)
{
	if (g_intzChecked) return g_intzSupported;
	g_intzChecked = true;
	if (!pD3D9) { g_intzSupported = false; return false; }

	// CheckDeviceFormat needs the *display* adapter format (typical X8R8G8B8), NOT the back-buffer
	// format (which can be A8R8G8B8 — an invalid display format that makes the check fail).
	D3DDISPLAYMODE mode = { 0 };
	D3DFORMAT adapterFmt = D3DFMT_X8R8G8B8;
	if (SUCCEEDED(pD3D9->GetAdapterDisplayMode(Adapter, &mode)))
		adapterFmt = mode.Format;

	HRESULT hr = pD3D9->CheckDeviceFormat(Adapter, DevType, adapterFmt,
		D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_INTZ);
	g_intzSupported = SUCCEEDED(hr);
	WrapperLog("INTZ depth-as-texture support: %s (adapterFmt=0x%X, hr=0x%X)\n",
		g_intzSupported ? "YES" : "NO", (UINT)adapterFmt, (UINT)hr);
	return g_intzSupported;
}

void ApplyGraphicsSettings(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	if (!pPresentationParameters)
		return;

	WrapperLog("ApplyGraphicsSettings: game requested %ux%u Windowed=%d\n",
		pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
		pPresentationParameters->Windowed);

	// SSAO needs a sampleable depth buffer. Override AutoDepthStencilFormat to INTZ
	// so D3D9 creates the auto depth as a texture we can read in our shader at present time.
	if (bSSAO && g_intzSupported && pPresentationParameters->EnableAutoDepthStencil)
	{
		WrapperLog("ApplyGraphicsSettings: overriding AutoDepthStencilFormat 0x%X -> INTZ for SSAO\n",
			(UINT)pPresentationParameters->AutoDepthStencilFormat);
		pPresentationParameters->AutoDepthStencilFormat = D3DFMT_INTZ;
	}

	// Width/Height = -1: render at the native resolution of the monitor hosting the game window
	// (no driver upscale blur in borderless mode). Re-queried at every CreateDevice/Reset so a
	// monitor change is picked up.
	int resW = nResolutionWidth, resH = nResolutionHeight;
	if (resW == -1 || resH == -1)
	{
		HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : g_hFocusWindow;
		HMONITOR monitor = MonitorFromWindow((!bUsePrimaryMonitor && hwnd) ? hwnd : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
		MONITORINFOEXA info;
		info.cbSize = sizeof(info);
		if (GetMonitorInfoA(monitor, (LPMONITORINFO)&info))
		{
			// EnumDisplaySettings reports the physical display mode — immune to DPI
			// virtualization, which shrinks rcMonitor for non-DPI-aware processes
			// (2560x1440 at 160% Windows scaling reads as 1600x900 otherwise).
			DEVMODEA dm = {};
			dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight)
			{
				resW = (int)dm.dmPelsWidth;
				resH = (int)dm.dmPelsHeight;
			}
			else
			{
				resW = info.rcMonitor.right - info.rcMonitor.left;
				resH = info.rcMonitor.bottom - info.rcMonitor.top;
			}
			WrapperLog("ApplyGraphicsSettings: native resolution detected: %dx%d\n", resW, resH);
		}
	}

	if (resW > 0 && resH > 0)
	{
		pPresentationParameters->BackBufferWidth = resW;
		pPresentationParameters->BackBufferHeight = resH;
		WrapperLog("ApplyGraphicsSettings: overriding resolution to %dx%d\n", resW, resH);
	}

	// SSAA: gonfle le back buffer; le driver fait le bilinear downsample au Present (windowed mode).
	// MSAA et SSAA combinés = explosion mémoire et redondant — on désactive MSAA si SSAA actif.
	if (nSSAAFactor > 1)
	{
		pPresentationParameters->BackBufferWidth  *= nSSAAFactor;
		pPresentationParameters->BackBufferHeight *= nSSAAFactor;
		WrapperLog("ApplyGraphicsSettings: SSAA %dx -> internal back buffer %ux%u\n",
			nSSAAFactor,
			pPresentationParameters->BackBufferWidth,
			pPresentationParameters->BackBufferHeight);
		if (nAntialiasing > 0)
		{
			WrapperLog("ApplyGraphicsSettings: SSAA active, disabling MSAA %d\n", nAntialiasing);
			nAntialiasing = 0;
		}
		// SwapEffect must be DISCARD for the driver to bilinear-stretch back buffer to window
		pPresentationParameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
	}

	nBackBufferWidth  = (int)pPresentationParameters->BackBufferWidth;
	nBackBufferHeight = (int)pPresentationParameters->BackBufferHeight;

	if (nAntialiasing > 0)
	{
		pPresentationParameters->MultiSampleType = (D3DMULTISAMPLE_TYPE)nAntialiasing;
		pPresentationParameters->MultiSampleQuality = 0;
		// MSAA requires D3DSWAPEFFECT_DISCARD
		pPresentationParameters->SwapEffect = D3DSWAPEFFECT_DISCARD;
	}

	if (bVSync)
	{
		pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	}
}

// Validate MSAA against the GPU. If unsupported, downgrade silently to D3DMULTISAMPLE_NONE
// so CreateDevice/Reset doesn't fail. Returns true if a downgrade occurred.
// Cascade MSAA level down (16 → 8 → 4 → 2 → NONE) until the driver supports it for both the
// back buffer and the auto depth-stencil. Avoids the silent "disabled entirely" surprise when
// a user asks for 16x and the driver caps at 8x.
bool ValidateMSAASupport(IDirect3D9* pD3D9, UINT Adapter, D3DDEVTYPE DeviceType, D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	if (!pD3D9 || !pPresentationParameters || pPresentationParameters->MultiSampleType == D3DMULTISAMPLE_NONE)
		return false;

	D3DFORMAT bbFormat = pPresentationParameters->BackBufferFormat;
	if (bbFormat == D3DFMT_UNKNOWN)
		bbFormat = D3DFMT_X8R8G8B8;

	const D3DMULTISAMPLE_TYPE requested = pPresentationParameters->MultiSampleType;
	D3DMULTISAMPLE_TYPE level = requested;
	while (level != D3DMULTISAMPLE_NONE)
	{
		HRESULT hr = pD3D9->CheckDeviceMultiSampleType(
			Adapter, DeviceType, bbFormat,
			pPresentationParameters->Windowed,
			level, NULL);

		if (SUCCEEDED(hr) && pPresentationParameters->EnableAutoDepthStencil)
		{
			hr = pD3D9->CheckDeviceMultiSampleType(
				Adapter, DeviceType, pPresentationParameters->AutoDepthStencilFormat,
				pPresentationParameters->Windowed,
				level, NULL);
		}

		if (SUCCEEDED(hr))
		{
			pPresentationParameters->MultiSampleType = level;
			pPresentationParameters->MultiSampleQuality = 0;
			if (level == requested)
				WrapperLog("ValidateMSAASupport: MSAA %d OK\n", (int)level);
			else
				WrapperLog("ValidateMSAASupport: MSAA %d unsupported, downgraded to %d\n", (int)requested, (int)level);
			return false;
		}

		// Drop to next supported level: 16 -> 8 -> 4 -> 2 -> NONE
		switch (level)
		{
		case D3DMULTISAMPLE_16_SAMPLES: level = D3DMULTISAMPLE_8_SAMPLES; break;
		case D3DMULTISAMPLE_8_SAMPLES:  level = D3DMULTISAMPLE_4_SAMPLES; break;
		case D3DMULTISAMPLE_4_SAMPLES:  level = D3DMULTISAMPLE_2_SAMPLES; break;
		case D3DMULTISAMPLE_2_SAMPLES:  level = D3DMULTISAMPLE_NONE;      break;
		default:                        level = D3DMULTISAMPLE_NONE;      break;
		}
	}

	WrapperLog("ValidateMSAASupport: MSAA %d unsupported, no level worked, disabling\n", (int)requested);
	pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
	pPresentationParameters->MultiSampleQuality = 0;
	return true;
}

HRESULT m_IDirect3D9Ex::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
{
	g_hFocusWindow = hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow;
	if (bForceWindowedMode)
	{
		ForceWindowed(pPresentationParameters);
	}

	if (nFullScreenRefreshRateInHz)
		ForceFullScreenRefreshRateInHz(pPresentationParameters);

	D3DFORMAT bbFmt = (pPresentationParameters && pPresentationParameters->BackBufferFormat != D3DFMT_UNKNOWN)
		? pPresentationParameters->BackBufferFormat : D3DFMT_X8R8G8B8;
	CheckINTZSupport(ProxyInterface, Adapter, DeviceType, bbFmt);
	D3DFORMAT origAutoDepth = pPresentationParameters ? pPresentationParameters->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
	ApplyGraphicsSettings(pPresentationParameters);
	ValidateMSAASupport(ProxyInterface, Adapter, DeviceType, pPresentationParameters);

	if (bDisplayFPSCounter)
	{
		if (FrameLimiter::pFPSFont)
			FrameLimiter::pFPSFont->Release();
		if (FrameLimiter::pTimeFont)
			FrameLimiter::pTimeFont->Release();
		FrameLimiter::pFPSFont = nullptr;
		FrameLimiter::pTimeFont = nullptr;
	}

	HRESULT hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);

	if (FAILED(hr) && pPresentationParameters && pPresentationParameters->AutoDepthStencilFormat == D3DFMT_INTZ)
	{
		WrapperLog("CreateDevice failed with INTZ depth, retrying with original 0x%X\n", (UINT)origAutoDepth);
		pPresentationParameters->AutoDepthStencilFormat = origAutoDepth;
		hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
	}

	if (FAILED(hr) && pPresentationParameters && pPresentationParameters->MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
		pPresentationParameters->MultiSampleQuality = 0;
		hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
	}

	if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
	{
		IDirect3DDevice9Ex* pEx = nullptr;
		if (SUCCEEDED((*ppReturnedDeviceInterface)->QueryInterface(IID_IDirect3DDevice9Ex, (void**)&pEx)))
		{
			pEx->SetMaximumFrameLatency(1);
			pEx->Release();
			WrapperLog("CreateDevice: SetMaximumFrameLatency(1) applied\n");
		}
		*ppReturnedDeviceInterface = new m_IDirect3DDevice9Ex((IDirect3DDevice9Ex*)*ppReturnedDeviceInterface, this, IID_IDirect3DDevice9);
	}

	return hr;
}

extern void FreeFXAA();

HRESULT m_IDirect3DDevice9Ex::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	FreeFXAA();
	ClearUpscaledRTs();

	if (bForceWindowedMode)
		ForceWindowed(pPresentationParameters);

	if (nFullScreenRefreshRateInHz)
		ForceFullScreenRefreshRateInHz(pPresentationParameters);

	D3DFORMAT origAutoDepth = pPresentationParameters ? pPresentationParameters->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
	ApplyGraphicsSettings(pPresentationParameters);

	if (bDisplayFPSCounter)
	{
		if (FrameLimiter::pFPSFont)
			FrameLimiter::pFPSFont->OnLostDevice();
		if (FrameLimiter::pTimeFont)
			FrameLimiter::pTimeFont->OnLostDevice();
	}

	auto hRet = ProxyInterface->Reset(pPresentationParameters);

	if (FAILED(hRet) && pPresentationParameters && pPresentationParameters->AutoDepthStencilFormat == D3DFMT_INTZ)
	{
		WrapperLog("Reset failed with INTZ depth, retrying with original 0x%X\n", (UINT)origAutoDepth);
		pPresentationParameters->AutoDepthStencilFormat = origAutoDepth;
		hRet = ProxyInterface->Reset(pPresentationParameters);
	}

	if (FAILED(hRet) && pPresentationParameters && pPresentationParameters->MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
		pPresentationParameters->MultiSampleQuality = 0;
		hRet = ProxyInterface->Reset(pPresentationParameters);
	}

	if (bDisplayFPSCounter)
	{
		if (SUCCEEDED(hRet))
		{
			if (FrameLimiter::pFPSFont)
				FrameLimiter::pFPSFont->OnResetDevice();
			if (FrameLimiter::pTimeFont)
				FrameLimiter::pTimeFont->OnResetDevice();
		}
	}

	return hRet;
}

HRESULT m_IDirect3D9Ex::CreateDeviceEx(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, IDirect3DDevice9Ex** ppReturnedDeviceInterface)
{
	g_hFocusWindow = hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow;
	if (bForceWindowedMode)
	{
		ForceWindowed(pPresentationParameters, pFullscreenDisplayMode);
	}

	if (nFullScreenRefreshRateInHz)
		ForceFullScreenRefreshRateInHz(pPresentationParameters);

	D3DFORMAT bbFmt = (pPresentationParameters && pPresentationParameters->BackBufferFormat != D3DFMT_UNKNOWN)
		? pPresentationParameters->BackBufferFormat : D3DFMT_X8R8G8B8;
	CheckINTZSupport(ProxyInterface, Adapter, DeviceType, bbFmt);
	D3DFORMAT origAutoDepth = pPresentationParameters ? pPresentationParameters->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
	ApplyGraphicsSettings(pPresentationParameters);
	ValidateMSAASupport(ProxyInterface, Adapter, DeviceType, pPresentationParameters);

	if (bDisplayFPSCounter)
	{
		if (FrameLimiter::pFPSFont)
			FrameLimiter::pFPSFont->Release();
		if (FrameLimiter::pTimeFont)
			FrameLimiter::pTimeFont->Release();
		FrameLimiter::pFPSFont = nullptr;
		FrameLimiter::pTimeFont = nullptr;
	}

	HRESULT hr = ProxyInterface->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);

	if (FAILED(hr) && pPresentationParameters && pPresentationParameters->AutoDepthStencilFormat == D3DFMT_INTZ)
	{
		WrapperLog("CreateDeviceEx failed with INTZ depth, retrying with original 0x%X\n", (UINT)origAutoDepth);
		pPresentationParameters->AutoDepthStencilFormat = origAutoDepth;
		hr = ProxyInterface->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
	}

	if (FAILED(hr) && pPresentationParameters && pPresentationParameters->MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
		pPresentationParameters->MultiSampleQuality = 0;
		hr = ProxyInterface->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
	}

	if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
	{
		(*ppReturnedDeviceInterface)->SetMaximumFrameLatency(1);
		WrapperLog("CreateDeviceEx: SetMaximumFrameLatency(1) applied\n");
		*ppReturnedDeviceInterface = new m_IDirect3DDevice9Ex(*ppReturnedDeviceInterface, this, IID_IDirect3DDevice9Ex);
	}

	return hr;
}

HRESULT m_IDirect3DDevice9Ex::ResetEx(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode)
{
	FreeFXAA();
	ClearUpscaledRTs();

	if (bForceWindowedMode)
		ForceWindowed(pPresentationParameters, pFullscreenDisplayMode);

	if (nFullScreenRefreshRateInHz)
		ForceFullScreenRefreshRateInHz(pPresentationParameters);

	D3DFORMAT origAutoDepth = pPresentationParameters ? pPresentationParameters->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
	ApplyGraphicsSettings(pPresentationParameters);

	if (bDisplayFPSCounter)
	{
		if (FrameLimiter::pFPSFont)
			FrameLimiter::pFPSFont->OnLostDevice();
		if (FrameLimiter::pTimeFont)
			FrameLimiter::pTimeFont->OnLostDevice();
	}

	auto hRet = ProxyInterface->ResetEx(pPresentationParameters, pFullscreenDisplayMode);

	if (FAILED(hRet) && pPresentationParameters && pPresentationParameters->AutoDepthStencilFormat == D3DFMT_INTZ)
	{
		WrapperLog("ResetEx failed with INTZ depth, retrying with original 0x%X\n", (UINT)origAutoDepth);
		pPresentationParameters->AutoDepthStencilFormat = origAutoDepth;
		hRet = ProxyInterface->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
	}

	if (FAILED(hRet) && pPresentationParameters && pPresentationParameters->MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
		pPresentationParameters->MultiSampleQuality = 0;
		hRet = ProxyInterface->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
	}

	if (bDisplayFPSCounter)
	{
		if (SUCCEEDED(hRet))
		{
			if (FrameLimiter::pFPSFont)
				FrameLimiter::pFPSFont->OnResetDevice();
			if (FrameLimiter::pTimeFont)
				FrameLimiter::pTimeFont->OnResetDevice();
		}
	}

	return hRet;
}

LRESULT WINAPI CustomWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, int idx)
{
	if (hWnd == g_hFocusWindow || _fnIsTopLevelWindow(hWnd)) // skip child windows like buttons, edit boxes, etc.
	{
		if (bAlwaysOnTop)
		{
			if ((GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
				SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
		}
		switch (uMsg)
		{
		case WM_ACTIVATE:
			if (bCaptureMouse && LOWORD(wParam) != WA_INACTIVE)
				CaptureMouse(hWnd);
			break;
		case WM_NCACTIVATE:
			if (bCaptureMouse && LOWORD(wParam) != WA_INACTIVE)
				CaptureMouse(hWnd);
			break;
		case WM_ACTIVATEAPP:
			// On focus return, force DI8 mouse re-Acquire — the game's polling otherwise
			// stays in a "ghost acquired" state and the cursor stops responding.
			if (wParam == TRUE)
			{
				WrapperLog("AltTab: WM_ACTIVATEAPP(TRUE) received, re-acquiring mice\n");
				DirectInputReAcquireMice();
			}
			// Swallow the deactivation message to prevent HP5's freeze on focus loss.
			if (bDoNotNotifyOnTaskSwitch && wParam == FALSE)
			{
				WrapperLog("AltTab: WM_ACTIVATEAPP(FALSE) swallowed\n");
				return 0;
			}
			if (bCaptureMouse && wParam == TRUE)
				CaptureMouse(hWnd);
			break;
		case WM_KILLFOCUS:
			break;
		case WM_SETFOCUS:
		case WM_MOUSEACTIVATE:
			if (bCaptureMouse)
				CaptureMouse(hWnd);
			break;
		default:
			break;
		}
	}
	WNDPROC OrigProc = WNDPROC(WndProcList[idx].second);
	return OrigProc(hWnd, uMsg, wParam, lParam);
}

LRESULT WINAPI CustomWndProcA(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	WORD wClassAtom = GetClassWord(hWnd, GCW_ATOM);
	if (wClassAtom)
	{
		for (unsigned int i = 0; i < WndProcList.size(); i++) {
			if (WndProcList[i].first == wClassAtom) {
				return CustomWndProc(hWnd, uMsg, wParam, lParam, i);
			}
		}
	}
	// We should never reach here, but having safeguards anyway is good
	return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

LRESULT WINAPI CustomWndProcW(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	WORD wClassAtom = GetClassWord(hWnd, GCW_ATOM);
	if (wClassAtom)
	{
		for (unsigned int i = 0; i < WndProcList.size(); i++) {
			if (WndProcList[i].first == wClassAtom) {
				return CustomWndProc(hWnd, uMsg, wParam, lParam, i);
			}
		}
	}
	// We should never reach here, but having safeguards anyway is good
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

typedef ATOM(__stdcall* RegisterClassA_fn)(const WNDCLASSA*);
typedef ATOM(__stdcall* RegisterClassW_fn)(const WNDCLASSW*);
typedef ATOM(__stdcall* RegisterClassExA_fn)(const WNDCLASSEXA*);
typedef ATOM(__stdcall* RegisterClassExW_fn)(const WNDCLASSEXW*);
RegisterClassA_fn oRegisterClassA = NULL;
RegisterClassW_fn oRegisterClassW = NULL;
RegisterClassExA_fn oRegisterClassExA = NULL;
RegisterClassExW_fn oRegisterClassExW = NULL;
ATOM __stdcall hk_RegisterClassA(WNDCLASSA* lpWndClass)
{
	if (!IsValueIntAtom(DWORD(lpWndClass->lpszClassName))) { // argument is a class name
		if (IsSystemClassNameA(lpWndClass->lpszClassName)) { // skip system classes like buttons, common controls, etc.
			return oRegisterClassA(lpWndClass);
		}
	}
	ULONG_PTR pWndProc = ULONG_PTR(lpWndClass->lpfnWndProc);
	lpWndClass->lpfnWndProc = CustomWndProcA;
	WORD wClassAtom = oRegisterClassA(lpWndClass);
	if (wClassAtom != 0)
	{
		WndProcList.emplace_back(wClassAtom, pWndProc);
	}
	return wClassAtom;
}
ATOM __stdcall hk_RegisterClassW(WNDCLASSW* lpWndClass)
{
	if (!IsValueIntAtom(DWORD(lpWndClass->lpszClassName))) { // argument is a class name
		if (IsSystemClassNameW(lpWndClass->lpszClassName)) { // skip system classes like buttons, common controls, etc.
			return oRegisterClassW(lpWndClass);
		}
	}
	ULONG_PTR pWndProc = ULONG_PTR(lpWndClass->lpfnWndProc);
	lpWndClass->lpfnWndProc = CustomWndProcW;
	WORD wClassAtom = oRegisterClassW(lpWndClass);
	if (wClassAtom != 0)
	{
		WndProcList.emplace_back(wClassAtom, pWndProc);
	}
	return wClassAtom;
}
ATOM __stdcall hk_RegisterClassExA(WNDCLASSEXA* lpWndClass)
{
	if (!IsValueIntAtom(DWORD(lpWndClass->lpszClassName))) { // argument is a class name
		if (IsSystemClassNameA(lpWndClass->lpszClassName)) { // skip system classes like buttons, common controls, etc.
			return oRegisterClassExA(lpWndClass);
		}
	}
	ULONG_PTR pWndProc = ULONG_PTR(lpWndClass->lpfnWndProc);
	lpWndClass->lpfnWndProc = CustomWndProcA;
	WORD wClassAtom = oRegisterClassExA(lpWndClass);
	if (wClassAtom != 0)
	{
		WndProcList.emplace_back(wClassAtom, pWndProc);
	}
	return wClassAtom;
}
ATOM __stdcall hk_RegisterClassExW(WNDCLASSEXW* lpWndClass)
{
	if (!IsValueIntAtom(DWORD(lpWndClass->lpszClassName))) { // argument is a class name
		if (IsSystemClassNameW(lpWndClass->lpszClassName)) { // skip system classes like buttons, common controls, etc.
			return oRegisterClassExW(lpWndClass);
		}
	}
	ULONG_PTR pWndProc = ULONG_PTR(lpWndClass->lpfnWndProc);
	lpWndClass->lpfnWndProc = CustomWndProcW;
	WORD wClassAtom = oRegisterClassExW(lpWndClass);
	if (wClassAtom != 0)
	{
		WndProcList.emplace_back(wClassAtom, pWndProc);
	}
	return wClassAtom;
}

typedef HWND(__stdcall* GetForegroundWindow_fn)(void);
GetForegroundWindow_fn oGetForegroundWindow = NULL;

HWND __stdcall hk_GetForegroundWindow()
{
	if (g_hFocusWindow && IsWindow(g_hFocusWindow))
		return g_hFocusWindow;
	return oGetForegroundWindow();
}

typedef HWND(__stdcall* GetActiveWindow_fn)(void);
GetActiveWindow_fn oGetActiveWindow = NULL;

HWND __stdcall hk_GetActiveWindow(void)
{
	HWND hWndActive = oGetActiveWindow();
	if (g_hFocusWindow && hWndActive == NULL && IsWindow(g_hFocusWindow))
	{
		if (GetCurrentThreadId() == GetWindowThreadProcessId(g_hFocusWindow, NULL))
			return g_hFocusWindow;
	}
	return hWndActive;
}

typedef HWND(__stdcall* GetFocus_fn)(void);
GetFocus_fn oGetFocus = NULL;

HWND __stdcall hk_GetFocus(void)
{
	HWND hWndFocus = oGetFocus();
	if (g_hFocusWindow && hWndFocus == NULL && IsWindow(g_hFocusWindow))
	{
		if (GetCurrentThreadId() == GetWindowThreadProcessId(g_hFocusWindow, NULL))
			return g_hFocusWindow;
	}
	return hWndFocus;
}

// Free-mouse hooks: prevent the game from clipping the cursor to the window or
// capturing exclusive mouse input. Lets the user move to other monitors / take
// screenshots / press the Windows key without fighting the game.
typedef BOOL(WINAPI* ClipCursor_fn)(const RECT*);
typedef HWND(WINAPI* SetCapture_fn)(HWND);
ClipCursor_fn oClipCursor = NULL;
SetCapture_fn oSetCapture = NULL;

BOOL WINAPI hk_ClipCursor(const RECT* lpRect)
{
	// Pretend success without actually clipping
	return TRUE;
}

HWND WINAPI hk_SetCapture(HWND hWnd)
{
	// Pretend no previous capture without actually capturing
	return NULL;
}

typedef HMODULE(__stdcall* LoadLibraryA_fn)(LPCSTR lpLibFileName);
LoadLibraryA_fn oLoadLibraryA;

HMODULE __stdcall hk_LoadLibraryA(LPCSTR lpLibFileName)
{
	HMODULE hmod = oLoadLibraryA(lpLibFileName);
	if (hmod)
	{
		HookModule(hmod);
	}
	return hmod;
}

typedef HMODULE(__stdcall* LoadLibraryW_fn)(LPCWSTR lpLibFileName);
LoadLibraryW_fn oLoadLibraryW;

HMODULE __stdcall hk_LoadLibraryW(LPCWSTR lpLibFileName)
{
	HMODULE hmod = oLoadLibraryW(lpLibFileName);
	if (hmod)
	{
		HookModule(hmod);
	}
	return hmod;
}

typedef HMODULE(__stdcall* LoadLibraryExA_fn)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExA_fn oLoadLibraryExA;

HMODULE __stdcall hk_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE hmod = oLoadLibraryExA(lpLibFileName, hFile, dwFlags);
	if (hmod && ((dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) == 0))
	{
		HookModule(hmod);
	}
	return hmod;
}

typedef HMODULE(__stdcall* LoadLibraryExW_fn)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExW_fn oLoadLibraryExW;

HMODULE __stdcall hk_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE hmod = oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
	if (hmod && ((dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) == 0))
	{
		HookModule(hmod);
	}
	return hmod;
}

typedef BOOL(__stdcall* FreeLibrary_fn)(HMODULE hLibModule);
FreeLibrary_fn oFreeLibrary;

BOOL __stdcall hk_FreeLibrary(HMODULE hLibModule)
{
	if (hLibModule == g_hWrapperModule)
		return TRUE; // We will stay in memory, thank you very much

	return oFreeLibrary(hLibModule);
}

FARPROC __stdcall hk_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
	__try
	{
		if (!lstrcmpA(lpProcName, "RegisterClassA"))
		{
			if (oRegisterClassA == NULL)
				oRegisterClassA = (RegisterClassA_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_RegisterClassA;
		}
		if (!lstrcmpA(lpProcName, "RegisterClassW"))
		{
			if (oRegisterClassW == NULL)
				oRegisterClassW = (RegisterClassW_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_RegisterClassW;
		}
		if (!lstrcmpA(lpProcName, "RegisterClassExA"))
		{
			if (oRegisterClassExA == NULL)
				oRegisterClassExA = (RegisterClassExA_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_RegisterClassExA;
		}
		if (!lstrcmpA(lpProcName, "RegisterClassExW"))
		{
			if (oRegisterClassExW == NULL)
				oRegisterClassExW = (RegisterClassExW_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_RegisterClassExW;
		}
		if (!lstrcmpA(lpProcName, "GetForegroundWindow"))
		{
			if (oGetForegroundWindow == NULL)
				oGetForegroundWindow = (GetForegroundWindow_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_GetForegroundWindow;
		}
		if (!lstrcmpA(lpProcName, "GetActiveWindow"))
		{
			if (oGetActiveWindow == NULL)
				oGetActiveWindow = (GetActiveWindow_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_GetActiveWindow;
		}
		if (!lstrcmpA(lpProcName, "GetFocus"))
		{
			if (oGetFocus == NULL)
				oGetFocus = (GetFocus_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_GetFocus;
		}
		if (!lstrcmpA(lpProcName, "LoadLibraryA"))
		{
			if (oLoadLibraryA == NULL)
				oLoadLibraryA = (LoadLibraryA_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_LoadLibraryA;
		}
		if (!lstrcmpA(lpProcName, "LoadLibraryW"))
		{
			if (oLoadLibraryW == NULL)
				oLoadLibraryW = (LoadLibraryW_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_LoadLibraryW;
		}
		if (!lstrcmpA(lpProcName, "LoadLibraryExA"))
		{
			if (oLoadLibraryExA == NULL)
				oLoadLibraryExA = (LoadLibraryExA_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_LoadLibraryExA;
		}
		if (!lstrcmpA(lpProcName, "LoadLibraryExW"))
		{
			if (oLoadLibraryExW == NULL)
				oLoadLibraryExW = (LoadLibraryExW_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_LoadLibraryExW;
		}
		if (!lstrcmpA(lpProcName, "FreeLibrary"))
		{
			if (oFreeLibrary == NULL)
				oFreeLibrary = (FreeLibrary_fn)GetProcAddress(hModule, lpProcName);
			return (FARPROC)hk_FreeLibrary;
		}
	}
	__except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
	}

	return GetProcAddress(hModule, lpProcName);
}

void HookModule(HMODULE hmod)
{
	char modpath[MAX_PATH + 1];
	if (hmod == g_hWrapperModule) // don't hook ourselves
		return;

	if (GetModuleFileNameA(hmod, modpath, MAX_PATH)) {
		if (!_strnicmp(modpath, WinDir, strlen(WinDir))) { // skip system modules
			return;
		}
	}

	// user32.dll imports
	auto originalsUser32 = IATHook::Replace(
		hmod, "user32.dll",
		std::make_tuple("RegisterClassA", (void*)hk_RegisterClassA),
		std::make_tuple("RegisterClassW", (void*)hk_RegisterClassW),
		std::make_tuple("RegisterClassExA", (void*)hk_RegisterClassExA),
		std::make_tuple("RegisterClassExW", (void*)hk_RegisterClassExW),
		std::make_tuple("GetForegroundWindow", (void*)hk_GetForegroundWindow),
		std::make_tuple("GetActiveWindow", (void*)hk_GetActiveWindow),
		std::make_tuple("GetFocus", (void*)hk_GetFocus)
	);

	if (oRegisterClassA == NULL) { auto it = originalsUser32.find("RegisterClassA");   if (it != originalsUser32.end())  oRegisterClassA = (RegisterClassA_fn)it->second.get(); }
	if (oRegisterClassW == NULL) { auto it = originalsUser32.find("RegisterClassW");   if (it != originalsUser32.end())  oRegisterClassW = (RegisterClassW_fn)it->second.get(); }
	if (oRegisterClassExA == NULL) { auto it = originalsUser32.find("RegisterClassExA"); if (it != originalsUser32.end())  oRegisterClassExA = (RegisterClassExA_fn)it->second.get(); }
	if (oRegisterClassExW == NULL) { auto it = originalsUser32.find("RegisterClassExW"); if (it != originalsUser32.end())  oRegisterClassExW = (RegisterClassExW_fn)it->second.get(); }
	if (oGetForegroundWindow == NULL) { auto it = originalsUser32.find("GetForegroundWindow"); if (it != originalsUser32.end()) oGetForegroundWindow = (GetForegroundWindow_fn)it->second.get(); }
	if (oGetActiveWindow == NULL) { auto it = originalsUser32.find("GetActiveWindow");     if (it != originalsUser32.end()) oGetActiveWindow = (GetActiveWindow_fn)it->second.get(); }
	if (oGetFocus == NULL) { auto it = originalsUser32.find("GetFocus");            if (it != originalsUser32.end()) oGetFocus = (GetFocus_fn)it->second.get(); }

	// kernel32.dll imports
	auto originalsKernel32 = IATHook::Replace(
		hmod, "kernel32.dll",
		std::make_tuple("LoadLibraryA", (void*)hk_LoadLibraryA),
		std::make_tuple("LoadLibraryW", (void*)hk_LoadLibraryW),
		std::make_tuple("LoadLibraryExA", (void*)hk_LoadLibraryExA),
		std::make_tuple("LoadLibraryExW", (void*)hk_LoadLibraryExW),
		std::make_tuple("FreeLibrary", (void*)hk_FreeLibrary),
		std::make_tuple("GetProcAddress", (void*)hk_GetProcAddress)
	);

	if (oLoadLibraryA == NULL) { auto it = originalsKernel32.find("LoadLibraryA");   if (it != originalsKernel32.end()) oLoadLibraryA = (LoadLibraryA_fn)it->second.get(); }
	if (oLoadLibraryW == NULL) { auto it = originalsKernel32.find("LoadLibraryW");   if (it != originalsKernel32.end()) oLoadLibraryW = (LoadLibraryW_fn)it->second.get(); }
	if (oLoadLibraryExA == NULL) { auto it = originalsKernel32.find("LoadLibraryExA"); if (it != originalsKernel32.end()) oLoadLibraryExA = (LoadLibraryExA_fn)it->second.get(); }
	if (oLoadLibraryExW == NULL) { auto it = originalsKernel32.find("LoadLibraryExW"); if (it != originalsKernel32.end()) oLoadLibraryExW = (LoadLibraryExW_fn)it->second.get(); }
	if (oFreeLibrary == NULL) { auto it = originalsKernel32.find("FreeLibrary");    if (it != originalsKernel32.end()) oFreeLibrary = (FreeLibrary_fn)it->second.get(); }
}

void HookImportedModules()
{
	HMODULE hModule;
	HMODULE hm;

	hModule = GetModuleHandle(0);

	PIMAGE_DOS_HEADER img_dos_headers = (PIMAGE_DOS_HEADER)hModule;
	PIMAGE_NT_HEADERS img_nt_headers = (PIMAGE_NT_HEADERS)((BYTE*)img_dos_headers + img_dos_headers->e_lfanew);
	PIMAGE_IMPORT_DESCRIPTOR img_import_desc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)img_dos_headers + img_nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
	if (img_dos_headers->e_magic != IMAGE_DOS_SIGNATURE)
		return;

	for (IMAGE_IMPORT_DESCRIPTOR* iid = img_import_desc; iid->Name != 0; iid++) {
		char* mod_name = (char*)((size_t*)(iid->Name + (size_t)hModule));
		hm = GetModuleHandleA(mod_name);
		// ual check
		if (hm && !(GetProcAddress(hm, "DirectInput8Create") != NULL && GetProcAddress(hm, "DirectSoundCreate8") != NULL && GetProcAddress(hm, "InternetOpenA") != NULL)) {
			HookModule(hm);
		}
	}
}

bool WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
		g_hWrapperModule = hModule;

		// Open log file next to this DLL
		{
			char logPath[MAX_PATH];
			GetModuleFileNameA(hModule, logPath, MAX_PATH);
			strcpy(strrchr(logPath, '\\'), "\\d3d9_wrapper.log");
			g_log = fopen(logPath, "w");
			WrapperLog("d3d9 wrapper loaded\n");
		}

		// Load dll — try d3d9_original.dll (Chip-Biscuit's patched binary) first,
		// fall back to the system d3d9.dll if not present.
		char path[MAX_PATH];
		GetModuleFileNameA(hModule, path, MAX_PATH);
		strcpy(strrchr(path, '\\'), "\\d3d9_original.dll");
		d3d9dll = LoadLibraryA(path);
		if (!d3d9dll)
		{
			WrapperLog("d3d9_original.dll not found, using system d3d9.dll\n");
			GetSystemDirectoryA(path, MAX_PATH);
			strcat_s(path, "\\d3d9.dll");
			d3d9dll = LoadLibraryA(path);
		}
		else
		{
			WrapperLog("Loaded: %s\n", path);
		}

		if (d3d9dll)
		{
			// Get function addresses
			m_pDirect3DShaderValidatorCreate9 = (Direct3DShaderValidatorCreate9Proc)GetProcAddress(d3d9dll, "Direct3DShaderValidatorCreate9");
			m_pPSGPError = (PSGPErrorProc)GetProcAddress(d3d9dll, "PSGPError");
			m_pPSGPSampleTexture = (PSGPSampleTextureProc)GetProcAddress(d3d9dll, "PSGPSampleTexture");
			m_pD3DPERF_BeginEvent = (D3DPERF_BeginEventProc)GetProcAddress(d3d9dll, "D3DPERF_BeginEvent");
			m_pD3DPERF_EndEvent = (D3DPERF_EndEventProc)GetProcAddress(d3d9dll, "D3DPERF_EndEvent");
			m_pD3DPERF_GetStatus = (D3DPERF_GetStatusProc)GetProcAddress(d3d9dll, "D3DPERF_GetStatus");
			m_pD3DPERF_QueryRepeatFrame = (D3DPERF_QueryRepeatFrameProc)GetProcAddress(d3d9dll, "D3DPERF_QueryRepeatFrame");
			m_pD3DPERF_SetMarker = (D3DPERF_SetMarkerProc)GetProcAddress(d3d9dll, "D3DPERF_SetMarker");
			m_pD3DPERF_SetOptions = (D3DPERF_SetOptionsProc)GetProcAddress(d3d9dll, "D3DPERF_SetOptions");
			m_pD3DPERF_SetRegion = (D3DPERF_SetRegionProc)GetProcAddress(d3d9dll, "D3DPERF_SetRegion");
			m_pDebugSetLevel = (DebugSetLevelProc)GetProcAddress(d3d9dll, "DebugSetLevel");
			m_pDebugSetMute = (DebugSetMuteProc)GetProcAddress(d3d9dll, "DebugSetMute");
			m_pDirect3D9EnableMaximizedWindowedModeShim = (Direct3D9EnableMaximizedWindowedModeShimProc)GetProcAddress(d3d9dll, "Direct3D9EnableMaximizedWindowedModeShim");
			m_pDirect3DCreate9 = (Direct3DCreate9Proc)GetProcAddress(d3d9dll, "Direct3DCreate9");
			m_pDirect3DCreate9Ex = (Direct3DCreate9ExProc)GetProcAddress(d3d9dll, "Direct3DCreate9Ex");

			// ini
			HMODULE hm = NULL;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Direct3DCreate9, &hm);
			GetModuleFileNameA(hm, path, sizeof(path));
			strcpy(strrchr(path, '\\'), "\\d3d9.ini");
			// Opt out of Windows DPI scaling before the game creates its window. Without this,
			// display scaling (e.g. 160% on a 1440p laptop screen) makes the borderless window
			// undersized-then-DWM-stretched (extra blur) and lies to GetMonitorInfo about the
			// desktop size. Must run at DLL attach — the game's window doesn't exist yet.
			if (GetPrivateProfileInt("MAIN", "DPIAware", 1, path) != 0)
			{
				typedef BOOL(WINAPI* SetDpiCtx_fn)(HANDLE);
				HMODULE user32 = GetModuleHandleA("user32.dll");
				SetDpiCtx_fn pSetCtx = user32 ? (SetDpiCtx_fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
				BOOL dpiOk = FALSE;
				if (pSetCtx)
					dpiOk = pSetCtx((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Win10 1703+)
				if (!dpiOk)
					dpiOk = SetProcessDPIAware(); // Vista+ fallback (system-DPI aware)
				WrapperLog("DPIAware: %s\n", dpiOk ? "enabled (DPI virtualization off)" : "FAILED");
			}

			bForceWindowedMode = GetPrivateProfileInt("MAIN", "ForceWindowedMode", 0, path) != 0;
			fFPSLimit = static_cast<float>(GetPrivateProfileInt("MAIN", "FPSLimit", 0, path));
			nFullScreenRefreshRateInHz = GetPrivateProfileInt("MAIN", "FullScreenRefreshRateInHz", 0, path);
			bDisplayFPSCounter = GetPrivateProfileInt("MAIN", "DisplayFPSCounter", 0, path);
			nScreenshotKey = GetPrivateProfileInt("MAIN", "ScreenshotKey", VK_F12, path);
			bEnableHooks = GetPrivateProfileInt("MAIN", "EnableHooks", 0, path);
			bUsePrimaryMonitor = GetPrivateProfileInt("FORCEWINDOWED", "UsePrimaryMonitor", 0, path) != 0;
			bCenterWindow = GetPrivateProfileInt("FORCEWINDOWED", "CenterWindow", 1, path) != 0;
			bAlwaysOnTop = GetPrivateProfileInt("FORCEWINDOWED", "AlwaysOnTop", 0, path) != 0;
			bDoNotNotifyOnTaskSwitch = GetPrivateProfileInt("FORCEWINDOWED", "DoNotNotifyOnTaskSwitch", 0, path) != 0;
			nForceWindowStyle = GetPrivateProfileInt("FORCEWINDOWED", "ForceWindowStyle", 0, path);
			bCaptureMouse = GetPrivateProfileInt("FORCEWINDOWED", "CaptureMouse", 0, path) != 0;
			bFreeMouse = GetPrivateProfileInt("FORCEWINDOWED", "FreeMouse", 1, path) != 0;
			nAntialiasing = GetPrivateProfileInt("GRAPHICS", "Antialiasing", 0, path);
			nAnisotropicFiltering = GetPrivateProfileInt("GRAPHICS", "AnisotropicFiltering", 0, path);
			bVSync = GetPrivateProfileInt("GRAPHICS", "VSync", 0, path) != 0;
			bFXAA = GetPrivateProfileInt("GRAPHICS", "FXAA", 0, path) != 0;
			{
				char szBias[32];
				GetPrivateProfileStringA("GRAPHICS", "TextureLODBias", "0", szBias, sizeof(szBias), path);
				fTextureLODBias = static_cast<float>(atof(szBias));
			}
			nSSAAFactor = GetPrivateProfileInt("GRAPHICS", "SSAAFactor", 1, path);
			if (nSSAAFactor < 1) nSSAAFactor = 1;
			if (nSSAAFactor > 4) nSSAAFactor = 4;
			nShadowMapScale = GetPrivateProfileInt("GRAPHICS", "ShadowMapScale", 1, path);
			if (nShadowMapScale < 1) nShadowMapScale = 1;
			if (nShadowMapScale > 8) nShadowMapScale = 8;
			bColorGrading = GetPrivateProfileInt("GRAPHICS", "ColorGrading", 1, path) != 0;
			{
				char szF[32];
				GetPrivateProfileStringA("GRAPHICS", "Vibrance",  "0.15", szF, sizeof(szF), path); fVibrance = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "Vignette",  "0.40", szF, sizeof(szF), path); fVignette = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "Lift",      "0.00", szF, sizeof(szF), path); fLift     = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "Gamma",     "1.00", szF, sizeof(szF), path); fGamma    = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "Gain",      "1.05", szF, sizeof(szF), path); fGain     = (float)atof(szF);
					GetPrivateProfileStringA("GRAPHICS", "Sharpness", "0.25", szF, sizeof(szF), path); fSharpness = (float)atof(szF);
					if (fSharpness < 0.0f) fSharpness = 0.0f;
					if (fSharpness > 1.0f) fSharpness = 1.0f;
			}
			bSSAO = GetPrivateProfileInt("GRAPHICS", "SSAO", 0, path) != 0;
			{
				char szF[32];
				GetPrivateProfileStringA("GRAPHICS", "SSAOStrength", "0.50", szF, sizeof(szF), path); fSSAOStrength = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "SSAORadius",   "6.0",  szF, sizeof(szF), path); fSSAORadius   = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "SSAOMinDelta", "0.0005", szF, sizeof(szF), path); fSSAOMinDelta = (float)atof(szF);
				GetPrivateProfileStringA("GRAPHICS", "SSAOMaxDelta", "0.05",   szF, sizeof(szF), path); fSSAOMaxDelta = (float)atof(szF);
			}
			nResolutionWidth = GetPrivateProfileInt("RESOLUTION", "Width", 0, path);
			nResolutionHeight = GetPrivateProfileInt("RESOLUTION", "Height", 0, path);

			WrapperLog("Ini: %s\n", path);
			WrapperLog("  ForceWindowedMode=%d ForceWindowStyle=%d EnableHooks=%d DoNotNotify=%d\n",
				bForceWindowedMode, nForceWindowStyle, bEnableHooks, bDoNotNotifyOnTaskSwitch);
			WrapperLog("  Resolution=%dx%d  AF=%d  LODBias=%.2f  MSAA=%d  FXAA=%d  Sharpness=%.2f  VSync=%d  SSAA=%dx  ShadowMapScale=%dx\n",
				nResolutionWidth, nResolutionHeight, nAnisotropicFiltering, fTextureLODBias, nAntialiasing, (int)bFXAA, fSharpness, (int)bVSync, nSSAAFactor, nShadowMapScale);
			WrapperLog("  ColorGrading=%d  Vibrance=%.2f  Vignette=%.2f  Lift=%.2f  Gamma=%.2f  Gain=%.2f\n",
				(int)bColorGrading, fVibrance, fVignette, fLift, fGamma, fGain);
			WrapperLog("  SSAO=%d  Strength=%.2f  Radius=%.1f  MinDelta=%.4f  MaxDelta=%.4f\n",
				(int)bSSAO, fSSAOStrength, fSSAORadius, fSSAOMinDelta, fSSAOMaxDelta);

			if (fFPSLimit > 0.0f)
			{
				// Default matches data/d3d9.ini: ACCURATE (sleep-yield). REALTIME busy-waits a full core.
				FrameLimiter::FPSLimitMode mode = (GetPrivateProfileInt("MAIN", "FPSLimitMode", 2, path) == 1) ? FrameLimiter::FPSLimitMode::FPS_REALTIME : FrameLimiter::FPSLimitMode::FPS_ACCURATE;
				if (mode == FrameLimiter::FPSLimitMode::FPS_ACCURATE)
					timeBeginPeriod(1);

				FrameLimiter::Init(mode);
				mFPSLimitMode = mode;
				WrapperLog("FrameLimiter: %.0f fps, mode=%s\n", fFPSLimit,
					mode == FrameLimiter::FPSLimitMode::FPS_ACCURATE ? "ACCURATE (sleep-yield)" : "REALTIME (busy-wait)");
			}
			else
				mFPSLimitMode = FrameLimiter::FPSLimitMode::FPS_NONE;

			if (bFreeMouse && !bCaptureMouse)
			{
				HMODULE mainModule = GetModuleHandleA(nullptr);
				auto originals = IATHook::Replace(
					mainModule, "user32.dll",
					std::make_tuple("ClipCursor", (void*)hk_ClipCursor),
					std::make_tuple("SetCapture", (void*)hk_SetCapture)
				);
				if (oClipCursor == NULL) { auto it = originals.find("ClipCursor"); if (it != originals.end()) oClipCursor = (ClipCursor_fn)it->second.get(); }
				if (oSetCapture == NULL) { auto it = originals.find("SetCapture"); if (it != originals.end()) oSetCapture = (SetCapture_fn)it->second.get(); }
				WrapperLog("FreeMouse hooks installed (ClipCursor=%p, SetCapture=%p)\n", oClipCursor, oSetCapture);
			}

			InstallDirectInputHook();

			if (bEnableHooks && (bDoNotNotifyOnTaskSwitch || bCaptureMouse))
			{
				GetSystemWindowsDirectoryA(WinDir, MAX_PATH);

				HMODULE mainModule = GetModuleHandleA(nullptr);

				// Hook main module user32.dll imports
				{
					auto originalsUser32 = IATHook::Replace(
						mainModule, "user32.dll",
						std::make_tuple("RegisterClassA", (void*)hk_RegisterClassA),
						std::make_tuple("RegisterClassW", (void*)hk_RegisterClassW),
						std::make_tuple("RegisterClassExA", (void*)hk_RegisterClassExA),
						std::make_tuple("RegisterClassExW", (void*)hk_RegisterClassExW),
						std::make_tuple("GetForegroundWindow", (void*)hk_GetForegroundWindow),
						std::make_tuple("GetActiveWindow", (void*)hk_GetActiveWindow),
						std::make_tuple("GetFocus", (void*)hk_GetFocus)
					);

					if (oRegisterClassA == NULL) { auto it = originalsUser32.find("RegisterClassA");   if (it != originalsUser32.end())  oRegisterClassA = (RegisterClassA_fn)it->second.get(); }
					if (oRegisterClassW == NULL) { auto it = originalsUser32.find("RegisterClassW");   if (it != originalsUser32.end())  oRegisterClassW = (RegisterClassW_fn)it->second.get(); }
					if (oRegisterClassExA == NULL) { auto it = originalsUser32.find("RegisterClassExA"); if (it != originalsUser32.end())  oRegisterClassExA = (RegisterClassExA_fn)it->second.get(); }
					if (oRegisterClassExW == NULL) { auto it = originalsUser32.find("RegisterClassExW"); if (it != originalsUser32.end())  oRegisterClassExW = (RegisterClassExW_fn)it->second.get(); }
					if (oGetForegroundWindow == NULL) { auto it = originalsUser32.find("GetForegroundWindow"); if (it != originalsUser32.end()) oGetForegroundWindow = (GetForegroundWindow_fn)it->second.get(); }
					if (oGetActiveWindow == NULL) { auto it = originalsUser32.find("GetActiveWindow");     if (it != originalsUser32.end()) oGetActiveWindow = (GetActiveWindow_fn)it->second.get(); }
					if (oGetFocus == NULL) { auto it = originalsUser32.find("GetFocus");            if (it != originalsUser32.end()) oGetFocus = (GetFocus_fn)it->second.get(); }
				}

				// Hook main module kernel32.dll imports (including GetProcAddress)
				{
					auto originalsKernel32 = IATHook::Replace(
						mainModule, "kernel32.dll",
						std::make_tuple("LoadLibraryA", (void*)hk_LoadLibraryA),
						std::make_tuple("LoadLibraryW", (void*)hk_LoadLibraryW),
						std::make_tuple("LoadLibraryExA", (void*)hk_LoadLibraryExA),
						std::make_tuple("LoadLibraryExW", (void*)hk_LoadLibraryExW),
						std::make_tuple("FreeLibrary", (void*)hk_FreeLibrary),
						std::make_tuple("GetProcAddress", (void*)hk_GetProcAddress)
					);

					if (oLoadLibraryA == NULL) { auto it = originalsKernel32.find("LoadLibraryA");   if (it != originalsKernel32.end()) oLoadLibraryA = (LoadLibraryA_fn)it->second.get(); }
					if (oLoadLibraryW == NULL) { auto it = originalsKernel32.find("LoadLibraryW");   if (it != originalsKernel32.end()) oLoadLibraryW = (LoadLibraryW_fn)it->second.get(); }
					if (oLoadLibraryExA == NULL) { auto it = originalsKernel32.find("LoadLibraryExA"); if (it != originalsKernel32.end()) oLoadLibraryExA = (LoadLibraryExA_fn)it->second.get(); }
					if (oLoadLibraryExW == NULL) { auto it = originalsKernel32.find("LoadLibraryExW"); if (it != originalsKernel32.end()) oLoadLibraryExW = (LoadLibraryExW_fn)it->second.get(); }
					if (oFreeLibrary == NULL) { auto it = originalsKernel32.find("FreeLibrary");    if (it != originalsKernel32.end()) oFreeLibrary = (FreeLibrary_fn)it->second.get(); }
				}

				// Ensure d3d9.dll's IAT calls route through our hooks as well
				if (d3d9dll)
				{
					IATHook::Replace(d3d9dll, "kernel32.dll",
						std::make_tuple("GetProcAddress", (void*)hk_GetProcAddress)
					);

					auto u32_d3d9 = IATHook::Replace(d3d9dll, "user32.dll",
						std::make_tuple("GetForegroundWindow", (void*)hk_GetForegroundWindow)
					);
					if (oGetForegroundWindow == NULL) {
						auto it = u32_d3d9.find("GetForegroundWindow");
						if (it != u32_d3d9.end()) oGetForegroundWindow = (GetForegroundWindow_fn)it->second.get();
					}
				}

				// Hook ole32.dll's imports of user32 where applicable
				if (HMODULE ole32 = GetModuleHandleA("ole32.dll"))
				{
					auto originalsOleUser32 = IATHook::Replace(
						ole32, "user32.dll",
						std::make_tuple("RegisterClassA", (void*)hk_RegisterClassA),
						std::make_tuple("RegisterClassW", (void*)hk_RegisterClassW),
						std::make_tuple("RegisterClassExA", (void*)hk_RegisterClassExA),
						std::make_tuple("RegisterClassExW", (void*)hk_RegisterClassExW),
						std::make_tuple("GetActiveWindow", (void*)hk_GetActiveWindow)
					);

					if (oRegisterClassA == NULL) { auto it = originalsOleUser32.find("RegisterClassA");   if (it != originalsOleUser32.end())  oRegisterClassA = (RegisterClassA_fn)it->second.get(); }
					if (oRegisterClassW == NULL) { auto it = originalsOleUser32.find("RegisterClassW");   if (it != originalsOleUser32.end())  oRegisterClassW = (RegisterClassW_fn)it->second.get(); }
					if (oRegisterClassExA == NULL) { auto it = originalsOleUser32.find("RegisterClassExA"); if (it != originalsOleUser32.end())  oRegisterClassExA = (RegisterClassExA_fn)it->second.get(); }
					if (oRegisterClassExW == NULL) { auto it = originalsOleUser32.find("RegisterClassExW"); if (it != originalsOleUser32.end())  oRegisterClassExW = (RegisterClassExW_fn)it->second.get(); }
					if (oGetActiveWindow == NULL) { auto it = originalsOleUser32.find("GetActiveWindow");  if (it != originalsOleUser32.end())  oGetActiveWindow = (GetActiveWindow_fn)it->second.get(); }
				}

				HookImportedModules();
			}
		}
	}
	break;
	case DLL_PROCESS_DETACH:
	{
		if (mFPSLimitMode == FrameLimiter::FPSLimitMode::FPS_ACCURATE)
			timeEndPeriod(1);

		if (d3d9dll)
			FreeLibrary(d3d9dll);
	}
	break;
	}
	return true;
}

HRESULT WINAPI Direct3DShaderValidatorCreate9()
{
	if (!m_pDirect3DShaderValidatorCreate9)
	{
		return E_FAIL;
	}

	return m_pDirect3DShaderValidatorCreate9();
}

HRESULT WINAPI PSGPError()
{
	if (!m_pPSGPError)
	{
		return E_FAIL;
	}

	return m_pPSGPError();
}

HRESULT WINAPI PSGPSampleTexture()
{
	if (!m_pPSGPSampleTexture)
	{
		return E_FAIL;
	}

	return m_pPSGPSampleTexture();
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName)
{
	if (!m_pD3DPERF_BeginEvent)
	{
		return NULL;
	}

	return m_pD3DPERF_BeginEvent(col, wszName);
}

int WINAPI D3DPERF_EndEvent()
{
	if (!m_pD3DPERF_EndEvent)
	{
		return NULL;
	}

	return m_pD3DPERF_EndEvent();
}

DWORD WINAPI D3DPERF_GetStatus()
{
	if (!m_pD3DPERF_GetStatus)
	{
		return NULL;
	}

	return m_pD3DPERF_GetStatus();
}

BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
	if (!m_pD3DPERF_QueryRepeatFrame)
	{
		return FALSE;
	}

	return m_pD3DPERF_QueryRepeatFrame();
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)
{
	if (!m_pD3DPERF_SetMarker)
	{
		return;
	}

	return m_pD3DPERF_SetMarker(col, wszName);
}

void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
{
	if (!m_pD3DPERF_SetOptions)
	{
		return;
	}

	return m_pD3DPERF_SetOptions(dwOptions);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)
{
	if (!m_pD3DPERF_SetRegion)
	{
		return;
	}

	return m_pD3DPERF_SetRegion(col, wszName);
}

HRESULT WINAPI DebugSetLevel(DWORD dw1)
{
	if (!m_pDebugSetLevel)
	{
		return E_FAIL;
	}

	return m_pDebugSetLevel(dw1);
}

void WINAPI DebugSetMute()
{
	if (!m_pDebugSetMute)
	{
		return;
	}

	return m_pDebugSetMute();
}

int WINAPI Direct3D9EnableMaximizedWindowedModeShim(BOOL mEnable)
{
	if (!m_pDirect3D9EnableMaximizedWindowedModeShim)
	{
		return NULL;
	}

	return m_pDirect3D9EnableMaximizedWindowedModeShim(mEnable);
}

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
	if (!m_pDirect3DCreate9)
	{
		return nullptr;
	}

	IDirect3D9* pD3D9 = m_pDirect3DCreate9(SDKVersion);

	if (pD3D9)
	{
		return new m_IDirect3D9Ex((IDirect3D9Ex*)pD3D9, IID_IDirect3D9);
	}

	return nullptr;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	if (!m_pDirect3DCreate9Ex)
	{
		return E_FAIL;
	}

	HRESULT hr = m_pDirect3DCreate9Ex(SDKVersion, ppD3D);

	if (SUCCEEDED(hr) && ppD3D)
	{
		*ppD3D = new m_IDirect3D9Ex(*ppD3D, IID_IDirect3D9Ex);
	}

	return hr;
}
