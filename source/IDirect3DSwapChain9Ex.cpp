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
extern bool bColorGrading;
extern float fVibrance;
extern float fVignette;
extern float fLift;
extern float fGamma;
extern float fGain;
extern bool  bSSAO;
extern float fSSAOStrength;
extern float fSSAORadius;
extern float fSSAOMinDelta;
extern float fSSAOMaxDelta;
extern bool  g_intzSupported;
extern IDirect3DTexture9* g_sceneDepthTex;
extern UINT  g_sceneDepthW;
extern UINT  g_sceneDepthH;
extern void WrapperLog(const char* fmt, ...);

// FXAA + adaptive sharpening + color grading + screen-space AO pixel shader.
// Runs on the final frame, single combined pass for performance.
// Constants: c0 = (rcpFrameX, rcpFrameY, _, _)
//            c1 = (lift, gamma, gain, vibrance)
//            c2 = (vignetteStrength, _, _, _)
//            c3 = (ssaoStrength, ssaoRadiusPx, ssaoMinDelta, ssaoMaxDelta)
// Samplers: s0 = scene color, s1 = depth (INTZ when available)
static const char* g_fxaaPS = R"(
sampler2D scene    : register(s0);
sampler2D depthTex : register(s1);
float4 rcpFrame    : register(c0);
float4 grade1      : register(c1);
float4 grade2      : register(c2);
float4 ssaoP       : register(c3);
float luma(float3 c){return dot(c,float3(0.299,0.587,0.114));}
float ssaoFactor(float2 uv, float lumaRange){
    if (ssaoP.x < 0.001) return 1.0;
    // UI-bleed protection: the game draws transparent UI without writing depth, so the depth
    // buffer under a dialog still holds 3D scene depth. Without this guard, AO would bleed
    // background object silhouettes through the UI as gray outlines. Real 3D surfaces have
    // texture / lighting variance at 1px radius; flat UI overlays don't.
    if (lumaRange < 0.01) return 1.0;
    float zC = tex2D(depthTex, uv).r;
    if (zC > 0.9995) return 1.0;
    // Perspective Z is non-linear: a 50 cm contact distance produces a delta of ~0.003 near
    // the camera and ~0.00002 at far distance. Scaling thresholds by (1 - zC) approximates the
    // perspective compression, so the same MinDelta/MaxDelta ini values stay meaningful at
    // every distance (no halos at near silhouettes, no missed AO at far walls).
    float depthScale = max(1.0 - zC, 0.001);
    float minD = ssaoP.z * depthScale;
    float maxD = ssaoP.w * depthScale;
    static const float2 off[8] = {
        float2( 1, 0), float2(-1, 0), float2( 0, 1), float2( 0,-1),
        float2( 0.7071, 0.7071), float2(-0.7071, 0.7071),
        float2( 0.7071,-0.7071), float2(-0.7071,-0.7071)
    };
    float occ = 0.0;
    for (int i=0; i<8; i++) {
        float2 sUV = uv + off[i] * rcpFrame.xy * ssaoP.y;
        float zS = tex2D(depthTex, sUV).r;
        float d = zC - zS;
        if (d > minD && d < maxD) occ += 1.0;
    }
    return saturate(1.0 - (occ / 8.0) * ssaoP.x);
}
float3 grade(float3 col,float2 uv){
    col = saturate(col * grade1.z + grade1.x);
    col = pow(max(col, 1e-5), grade1.y);
    float lum = luma(col);
    float mx = max(col.r, max(col.g, col.b));
    float mn = min(col.r, min(col.g, col.b));
    float sat = mx - mn;
    col = lerp(lum.xxx, col, 1.0 + grade1.w * (1.0 - sat));
    float2 d = uv - 0.5;
    float vig = 1.0 - saturate(dot(d, d) * 4.0 * grade2.x);
    col *= vig;
    return col;
}
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
    float3 result;
    if((lMax-lMin)<max(0.0625,lMax*0.125)){
        result = clamp(m+(m-avg)*0.40,0,1);
    } else {
        float2 dir=float2(-((lNW+lNE)-(lSW+lSE)),(lNW+lSW)-(lNE+lSE));
        float rcp=1.0/(min(abs(dir.x),abs(dir.y))+max((lNW+lNE+lSW+lSE)*0.03125,0.0078125));
        dir=clamp(dir*rcp,-12,12)*o;
        float3 A=0.5*(tex2D(scene,uv+dir*(1.0/3.0-0.5)).rgb+tex2D(scene,uv+dir*(2.0/3.0-0.5)).rgb);
        float3 B=A*0.5+0.25*(tex2D(scene,uv-dir*0.5).rgb+tex2D(scene,uv+dir*0.5).rgb);
        float lB=luma(B);
        result = lB<lMin||lB>lMax?A:B;
    }
    result *= ssaoFactor(uv, lMax - lMin);
    return float4(grade(result, uv), 1);
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
    if (g_sceneDepthTex) { g_sceneDepthTex->Release(); g_sceneDepthTex = nullptr; }
    g_sceneDepthW = g_sceneDepthH = 0;
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
                                  "main", "ps_3_0", 0, &pCode, &pErr, nullptr)))
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
    // c1 = (lift, gamma, gain, vibrance), c2 = (vignette, _, _, _). Neutral when ColorGrading=0.
    float grade1[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    float grade2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (bColorGrading)
    {
        grade1[0] = fLift;
        grade1[1] = (fGamma > 0.01f) ? fGamma : 1.0f;
        grade1[2] = fGain;
        grade1[3] = fVibrance;
        grade2[0] = fVignette;
    }
    dev->SetPixelShaderConstantF(1, grade1, 1);
    dev->SetPixelShaderConstantF(2, grade2, 1);

    // SSAO: use the cached scene-depth INTZ texture (populated at CreateDepthStencilSurface time).
    // We do NOT use GetDepthStencilSurface here because the game often unbinds depth before Present
    // (UI rendering); the cached texture still holds the last-rendered scene depth contents.
    bool ssaoActive = false;
    if (bSSAO && g_intzSupported && g_sceneDepthTex)
    {
        ssaoActive = true;
    }
    float ssaoP[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (ssaoActive)
    {
        ssaoP[0] = fSSAOStrength;
        ssaoP[1] = fSSAORadius;
        ssaoP[2] = fSSAOMinDelta;
        ssaoP[3] = fSSAOMaxDelta;
        dev->SetTexture(1, g_sceneDepthTex);
        dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        static bool s_loggedSSAOActive = false;
        if (!s_loggedSSAOActive)
        {
            s_loggedSSAOActive = true;
            WrapperLog("SSAO: first-frame active, depthTex=%p (%ux%u)\n",
                (void*)g_sceneDepthTex, g_sceneDepthW, g_sceneDepthH);
        }
    }
    dev->SetPixelShaderConstantF(3, ssaoP, 1);

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
