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

extern bool bFXAA;
extern void WrapperLog(const char* fmt, ...);

// FXAA pixel shader — runs on the final frame before Present
static const char* g_fxaaPS = R"(
sampler2D scene : register(s0);
float4 rcpFrame : register(c0);
float luma(float3 c){return dot(c,float3(0.299,0.587,0.114));}
float4 main(float2 uv:TEXCOORD0):COLOR0{
    float2 o=rcpFrame.xy;
    float3 nw=tex2D(scene,uv+float2(-o.x,-o.y)).rgb;
    float3 ne=tex2D(scene,uv+float2( o.x,-o.y)).rgb;
    float3 sw=tex2D(scene,uv+float2(-o.x, o.y)).rgb;
    float3 se=tex2D(scene,uv+float2( o.x, o.y)).rgb;
    float3 m =tex2D(scene,uv).rgb;
    float3 avg=(nw+ne+sw+se)*0.25;
    float lNW=luma(nw),lNE=luma(ne),lSW=luma(sw),lSE=luma(se),lM=luma(m);
    float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE)));
    float lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));
    // No edge: apply sharpening to recover texture detail (faces, surfaces)
    if((lMax-lMin)<max(0.0625,lMax*0.125))return float4(clamp(m+(m-avg)*0.40,0,1),1);
    float2 dir=float2(-((lNW+lNE)-(lSW+lSE)),(lNW+lSW)-(lNE+lSE));
    float rcp=1.0/(min(abs(dir.x),abs(dir.y))+max((lNW+lNE+lSW+lSE)*0.03125,0.0078125));
    dir=clamp(dir*rcp,-12,12)*o;
    float3 A=0.5*(tex2D(scene,uv+dir*(1.0/3.0-0.5)).rgb+tex2D(scene,uv+dir*(2.0/3.0-0.5)).rgb);
    float3 B=A*0.5+0.25*(tex2D(scene,uv-dir*0.5).rgb+tex2D(scene,uv+dir*0.5).rgb);
    float lB=luma(B);
    // On edges: pure FXAA, no sharpening (would re-introduce aliasing)
    return float4(lB<lMin||lB>lMax?A:B,1);
}
)";

static IDirect3DTexture9*    s_fxaaTex  = nullptr;
static IDirect3DSurface9*    s_fxaaSurf = nullptr;
static IDirect3DPixelShader9* s_fxaaPS  = nullptr;
static UINT s_fxaaW = 0, s_fxaaH = 0;

void FreeFXAA()
{
    if (s_fxaaSurf) { s_fxaaSurf->Release(); s_fxaaSurf = nullptr; }
    if (s_fxaaTex)  { s_fxaaTex->Release();  s_fxaaTex  = nullptr; }
    if (s_fxaaPS)   { s_fxaaPS->Release();   s_fxaaPS   = nullptr; }
    s_fxaaW = s_fxaaH = 0;
}

static bool InitFXAA(IDirect3DDevice9* dev, UINT W, UINT H, D3DFORMAT fmt)
{
    FreeFXAA();
    if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s_fxaaTex, nullptr)))
    {
        WrapperLog("FXAA init: CreateTexture failed\n");
        return false;
    }
    s_fxaaTex->GetSurfaceLevel(0, &s_fxaaSurf);

    ID3DXBuffer* pCode = nullptr;
    ID3DXBuffer* pErr  = nullptr;
    if (FAILED(D3DXCompileShader(g_fxaaPS, (UINT)strlen(g_fxaaPS), nullptr, nullptr,
                                  "main", "ps_2_b", 0, &pCode, &pErr, nullptr)))
    {
        WrapperLog("FXAA init: shader compile failed: %s\n", pErr ? (char*)pErr->GetBufferPointer() : "?");
        if (pErr) pErr->Release();
        FreeFXAA();
        return false;
    }
    if (pErr) pErr->Release();
    dev->CreatePixelShader((DWORD*)pCode->GetBufferPointer(), &s_fxaaPS);
    pCode->Release();

    s_fxaaW = W; s_fxaaH = H;
    WrapperLog("FXAA init: %dx%d fmt=%d\n", W, H, (int)fmt);
    return true;
}

static void ApplyFXAA(IDirect3DDevice9* dev, IDirect3DSurface9* pBB)
{
    // Copy the finished scene into our texture
    if (FAILED(dev->StretchRect(pBB, nullptr, s_fxaaSurf, nullptr, D3DTEXF_NONE)))
        return;

    // Save full device state, apply FXAA pass, restore
    IDirect3DStateBlock9* pSB = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &pSB);

    dev->SetRenderTarget(0, pBB);
    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    D3DVIEWPORT9 vp = { 0, 0, s_fxaaW, s_fxaaH, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(s_fxaaPS);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, s_fxaaTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER,  D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER,  D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER,  D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU,   D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV,   D3DTADDRESS_CLAMP);
    float rcpF[4] = { 1.0f / s_fxaaW, 1.0f / s_fxaaH, 0, 0 };
    dev->SetPixelShaderConstantF(0, rcpF, 1);

    // Full-screen quad with D3D9 half-pixel offset
    struct V { float x, y, z, w, u, v; };
    float W = (float)s_fxaaW, H = (float)s_fxaaH;
    V q[4] = {
        { -0.5f,    -0.5f,    0, 1,  0, 0 },
        { W-0.5f,   -0.5f,    0, 1,  1, 0 },
        { -0.5f,    H-0.5f,   0, 1,  0, 1 },
        { W-0.5f,   H-0.5f,   0, 1,  1, 1 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));

    pSB->Apply();
    pSB->Release();
}

void DeviceFXAAPresent(IDirect3DDevice9* dev)
{
    if (!bFXAA) return;
    static bool s_fxaaInitFailed = false;
    if (s_fxaaInitFailed) return;
    IDirect3DSurface9* pBB = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)))
    {
        D3DSURFACE_DESC desc;
        pBB->GetDesc(&desc);
        if (s_fxaaW != desc.Width || s_fxaaH != desc.Height || !s_fxaaPS)
        {
            if (!InitFXAA(dev, desc.Width, desc.Height, desc.Format))
            {
                s_fxaaInitFailed = true;
                pBB->Release();
                return;
            }
        }
        if (s_fxaaPS)
            ApplyFXAA(dev, pBB);
        pBB->Release();
    }
}

HRESULT m_IDirect3DSwapChain9Ex::QueryInterface(THIS_ REFIID riid, void** ppvObj)
{
	if ((riid == IID_IDirect3DSwapChain9 || riid == IID_IUnknown) && ppvObj)
	{
		AddRef();

		*ppvObj = this;

		return S_OK;
	}

	HRESULT hr = ProxyInterface->QueryInterface(riid, ppvObj);

	if (SUCCEEDED(hr))
	{
		genericQueryInterface(riid, ppvObj, m_pDeviceEx);
	}

	return hr;
}

ULONG m_IDirect3DSwapChain9Ex::AddRef(THIS)
{
	return ProxyInterface->AddRef();
}

ULONG m_IDirect3DSwapChain9Ex::Release(THIS)
{
	return ProxyInterface->Release();
}

HRESULT m_IDirect3DSwapChain9Ex::Present(THIS_ CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags)
{
	if (bFXAA)
	{
		IDirect3DSurface9* pBB = nullptr;
		if (SUCCEEDED(ProxyInterface->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBB)))
		{
			D3DSURFACE_DESC desc;
			pBB->GetDesc(&desc);
			IDirect3DDevice9* dev = m_pDeviceEx->GetProxyInterface();
			if (s_fxaaW != desc.Width || s_fxaaH != desc.Height || !s_fxaaPS)
				InitFXAA(dev, desc.Width, desc.Height, desc.Format);
			if (s_fxaaPS)
				ApplyFXAA(dev, pBB);
			pBB->Release();
		}
	}
	return ProxyInterface->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT m_IDirect3DSwapChain9Ex::GetFrontBufferData(THIS_ IDirect3DSurface9* pDestSurface)
{
	if (pDestSurface)
	{
		pDestSurface = static_cast<m_IDirect3DSurface9 *>(pDestSurface)->GetProxyInterface();
	}

	return GetFrontBufferData(pDestSurface);
}

HRESULT m_IDirect3DSwapChain9Ex::GetBackBuffer(THIS_ UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
{
	HRESULT hr = ProxyInterface->GetBackBuffer(BackBuffer, Type, ppBackBuffer);

	if (SUCCEEDED(hr) && ppBackBuffer)
	{
		*ppBackBuffer = m_pDeviceEx->ProxyAddressLookupTable->FindAddress<m_IDirect3DSurface9>(*ppBackBuffer);
	}

	return hr;
}

HRESULT m_IDirect3DSwapChain9Ex::GetRasterStatus(THIS_ D3DRASTER_STATUS* pRasterStatus)
{
	return ProxyInterface->GetRasterStatus(pRasterStatus);
}

HRESULT m_IDirect3DSwapChain9Ex::GetDisplayMode(THIS_ D3DDISPLAYMODE* pMode)
{
	return ProxyInterface->GetDisplayMode(pMode);
}

HRESULT m_IDirect3DSwapChain9Ex::GetDevice(THIS_ IDirect3DDevice9** ppDevice)
{
	if (!ppDevice)
	{
		return D3DERR_INVALIDCALL;
	}

	m_pDeviceEx->AddRef();

	*ppDevice = m_pDeviceEx;

	return D3D_OK;
}

HRESULT m_IDirect3DSwapChain9Ex::GetPresentParameters(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	return ProxyInterface->GetPresentParameters(pPresentationParameters);
}

HRESULT m_IDirect3DSwapChain9Ex::GetLastPresentCount(THIS_ UINT* pLastPresentCount)
{
	return ProxyInterface->GetLastPresentCount(pLastPresentCount);
}

HRESULT m_IDirect3DSwapChain9Ex::GetPresentStats(THIS_ D3DPRESENTSTATS* pPresentationStatistics)
{
	return ProxyInterface->GetPresentStats(pPresentationStatistics);
}

HRESULT m_IDirect3DSwapChain9Ex::GetDisplayModeEx(THIS_ D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
{
	return ProxyInterface->GetDisplayModeEx(pMode, pRotation);
}
