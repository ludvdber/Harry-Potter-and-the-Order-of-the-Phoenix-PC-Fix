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
extern float fSharpness;
extern bool bColorGrading;
extern float fVibrance;
extern float fVignette;
extern float fLift;
extern float fGamma;
extern float fGain;
extern float fTemperature;
extern float fTint;
extern float fContrast;
extern float fSplitTone;
extern bool  bSSAO;
extern float fSSAOStrength;
extern float fSSAORadius;
extern float fSSAOMinDelta;
extern float fSSAOMaxDelta;
extern bool  bBloom;
extern float fBloomStrength;
extern float fBloomThreshold;
extern bool  bGodRays;
extern float fGodRaysStrength;
extern float fGodRaysDecay;
extern bool  g_intzSupported;
extern IDirect3DTexture9* g_sceneDepthTex;
extern UINT  g_sceneDepthW;
extern UINT  g_sceneDepthH;
extern IDirect3DSurface9* g_sceneDepthSurf;
extern IDirect3DSurface9* g_curDepthReal;
extern UINT g_sceneDepthRTW;
extern UINT g_sceneDepthRTH;
extern bool g_aoDoneThisFrame; // set by the depth-unbind AO pass; tells the Present pass to skip SSAO
extern void WrapperLog(const char* fmt, ...);

// FXAA + adaptive sharpening + color grading + screen-space AO pixel shader.
// Runs on the final frame, single combined pass for performance.
// Constants: c0 = (rcpFrameX, rcpFrameY, frameW, frameH)
//            c1 = (lift, gamma, gain, vibrance)
//            c2 = (vignetteStrength, sharpness, temperature, tint)
//            c3 = (ssaoStrength, ssaoRadiusPx, ssaoMinDelta, ssaoMaxDelta)
//            c4 = (depthUVScaleX, depthUVScaleY, _, _) — maps screen UVs onto the depth
//                 texture when it is larger than the back buffer (post-Reset window-sized depth)
//            c5 = (bloomStrength, godRayStrength, _, _) — 0 disables that effect's composite
//            c6 = (contrast, splitToneStrength, _, _) — S-curve contrast; teal/orange split toning
// Samplers: s0 = scene color, s1 = depth (INTZ when available), s2 = bloom, s3 = god rays
static const char* g_fxaaPS = R"(
sampler2D scene    : register(s0);
sampler2D depthTex : register(s1);
sampler2D bloomTex : register(s2);
sampler2D rayTex   : register(s3);
float4 rcpFrame    : register(c0);
float4 grade1      : register(c1);
float4 grade2      : register(c2);
float4 ssaoP       : register(c3);
float4 dScale      : register(c4);
float4 lightP      : register(c5);
float4 gradeC      : register(c6);
float luma(float3 c){return dot(c,float3(0.299,0.587,0.114));}
float ssaoFactor(float2 uv, float lumaRange){
    if (ssaoP.x < 0.001) return 1.0;
    // UI-bleed protection: the game draws transparent UI without writing depth, so the depth
    // buffer under a dialog still holds 3D scene depth. Without this guard, AO would bleed
    // background object silhouettes through the UI as gray outlines. Real 3D surfaces have
    // texture / lighting variance at 1px radius; flat UI overlays don't.
    if (lumaRange < 0.01) return 1.0;
    float zC = tex2D(depthTex, uv * dScale.xy).r;
    if (zC > 0.9995) return 1.0;
    // Perspective Z is non-linear: a 50 cm contact distance produces a delta of ~0.003 near
    // the camera and ~0.00002 at far distance. Scaling thresholds by (1 - zC) approximates the
    // perspective compression, so the same MinDelta/MaxDelta ini values stay meaningful at
    // every distance (no halos at near silhouettes, no missed AO at far walls).
    float depthScale = max(1.0 - zC, 0.001);
    float minD = ssaoP.z * depthScale;
    float maxD = ssaoP.w * depthScale;
    // 12-tap golden-angle spiral over the whole sampling disk (radii sqrt-spaced for even
    // area coverage, outermost tap = ssaoRadiusPx), rotated per pixel by interleaved
    // gradient noise. A static kernel repeats the same pattern on every pixel, which shows
    // up as banding/ringing around objects; per-pixel rotation converts that error into
    // high-frequency noise the eye ignores (and the FXAA pass in this same shader smooths).
    // Disk coverage (vs the old single-radius ring) gives the AO volume in corners instead
    // of a thin contact line.
    static const float2 off[12] = {
        float2( 0.2887,  0.0000), float2(-0.3010,  0.2758), float2( 0.0437, -0.4981),
        float2( 0.3514,  0.4582), float2(-0.6356, -0.1124), float2( 0.5967, -0.3795),
        float2(-0.1986,  0.7375), float2(-0.3750, -0.7253), float2( 0.8134,  0.2974),
        float2(-0.8438,  0.3485), float2( 0.4045, -0.8677), float2( 0.2997,  0.9540)
    };
    float2 px = uv * rcpFrame.zw;
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float sn, cs;
    sincos(ang, sn, cs);
    float occ = 0.0;
    for (int i=0; i<12; i++) {
        float2 r = float2(off[i].x*cs - off[i].y*sn, off[i].x*sn + off[i].y*cs);
        float2 sUV = (uv + r * rcpFrame.xy * ssaoP.y) * dScale.xy;
        float zS = tex2D(depthTex, sUV).r;
        float d = zC - zS;
        if (d > minD && d < maxD) occ += 1.0;
    }
    return saturate(1.0 - (occ / 12.0) * ssaoP.x);
}
float3 grade(float3 col,float2 uv){
    col = saturate(col * grade1.z + grade1.x);
    col = pow(max(col, 1e-5), grade1.y);
    // White balance: temperature (grade2.z, + = warmer: more red / less blue) and tint
    // (grade2.w, + = magenta: less green). Cheap channel scaling, then re-normalise luma so the
    // shift only re-tints rather than brightening/darkening the image.
    float lumPre = luma(col);
    col *= float3(1.0 + 0.15*grade2.z, 1.0 - 0.10*grade2.w, 1.0 - 0.15*grade2.z);
    float lumPost = max(luma(col), 1e-4);
    col *= lumPre / lumPost;
    col = saturate(col);
    // Contrast S-curve (gradeC.x): blend toward smoothstep, which deepens shadows AND lifts
    // highlights around the 0.5 pivot — adds depth/pop to HP5's flat midtones without crushing.
    col = lerp(col, col*col*(3.0 - 2.0*col), gradeC.x);
    float lum = luma(col);
    float mx = max(col.r, max(col.g, col.b));
    float mn = min(col.r, min(col.g, col.b));
    float sat = mx - mn;
    col = lerp(lum.xxx, col, 1.0 + grade1.w * (1.0 - sat));
    // Split toning (gradeC.y = strength): push shadows toward cool teal and highlights toward warm
    // orange — the classic cinematic "teal & orange". Tints are roughly luma-neutral (a positive
    // channel paired with a negative one) so they re-colour rather than brighten. Midtones (0.45..
    // 0.55) get neither weight, so faces/mid-greys stay natural.
    float lSplit = luma(col);
    float3 shTint = float3(-0.10, 0.04, 0.16);
    float3 hiTint = float3( 0.16, 0.05, -0.10);
    float shW = saturate(1.0 - lSplit * 1.8);
    float hiW = saturate((lSplit - 0.45) * 1.8);
    col = saturate(col + (shTint*shW + hiTint*hiW) * gradeC.y);
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
    float range = lMax - lMin;
    float thresh = max(0.0625, lMax*0.125);
    // --- sharpen candidate (flat-detail enhancement, strength = grade2.y from ini Sharpness) ---
    // Overshoot-limited unsharp mask. Plain (m-avg)*amount paints a bright/dark 1px fringe at
    // medium-contrast edges (the "extra pixel on the width" the user reported); clamping the
    // result to the local neighbourhood +/- a small slack keeps real sharpening but forbids the
    // fringe from exceeding what the surrounding pixels already span.
    float3 nbHi = max(max(nw,ne),max(sw,se));
    float3 nbLo = min(min(nw,ne),min(sw,se));
    float3 sharp = clamp(m + (m-avg)*grade2.y, nbLo - 0.05, nbHi + 0.05);
    // --- FXAA candidate ---
    float2 dir=float2(-((lNW+lNE)-(lSW+lSE)),(lNW+lSW)-(lNE+lSE));
    float rcp=1.0/(min(abs(dir.x),abs(dir.y))+max((lNW+lNE+lSW+lSE)*0.03125,0.0078125));
    dir=clamp(dir*rcp,-12,12)*o;
    float3 A=0.5*(tex2D(scene,uv+dir*(1.0/3.0-0.5)).rgb+tex2D(scene,uv+dir*(2.0/3.0-0.5)).rgb);
    float3 B=A*0.5+0.25*(tex2D(scene,uv-dir*0.5).rgb+tex2D(scene,uv+dir*0.5).rgb);
    float lB=luma(B);
    float3 fxaa = (lB<lMin||lB>lMax)?A:B;
    // --- smooth blend instead of a hard flat/edge branch ---
    // The old binary `if(range<thresh)` made pixels sitting on the threshold flip between
    // sharpened and FXAA'd from frame to frame as scene luma changed (e.g. a flickering torch),
    // so the edge fringe appeared to crawl even with a still camera. A smoothstep transition
    // removes the toggle: which path a pixel takes no longer flips on a tiny luma change.
    float edge = smoothstep(thresh*0.6, thresh*1.4, range);
    float3 result = lerp(sharp, fxaa, edge);
    result *= ssaoFactor(uv, range);
    // Additive composite of the light passes (bloom + god rays). Both buffers are pre-blurred
    // half-res, sampled bilinear; the branch is skipped (no texture fetch) when both strengths
    // are 0, so a build with the lighting pass off pays nothing and never reads s2/s3.
    if (lightP.x + lightP.y > 0.0001) {
        result += tex2D(bloomTex, uv).rgb * lightP.x + tex2D(rayTex, uv).rgb * lightP.y;
    }
    return float4(grade(result, uv), 1);
}
)";

// Standalone SSAO shader for the "AO at depth-unbind" pass (option B). Identical occlusion math
// to the combined shader's ssaoFactor, but it runs on the pure 3D scene the instant the game
// unbinds scene depth (before any UI is drawn), so it needs NO luma-variance UI-bleed guard —
// there is no UI in the buffer yet. Output is sceneColor * ao; FXAA + grading still run at Present.
// Constants/samplers match the combined shader: s0 scene, s1 depth, c0 rcp+dims, c3 ssao, c4 uvscale.
static const char* g_aoPS = R"(
sampler2D scene    : register(s0);
sampler2D depthTex : register(s1);
float4 rcpFrame    : register(c0);
float4 ssaoP       : register(c3);
float4 dScale      : register(c4);
float4 main(float2 uv:TEXCOORD0):COLOR0{
    float3 col = tex2D(scene, uv).rgb;
    if (ssaoP.x < 0.001) return float4(col, 1);
    float zC = tex2D(depthTex, uv * dScale.xy).r;
    if (zC > 0.9995) return float4(col, 1);
    float depthScale = max(1.0 - zC, 0.001);
    float minD = ssaoP.z * depthScale;
    float maxD = ssaoP.w * depthScale;
    float2 off[12] = {
        float2( 0.2887,  0.0000), float2(-0.3010,  0.2758), float2( 0.0437, -0.4981),
        float2( 0.3514,  0.4582), float2(-0.6356, -0.1124), float2( 0.5967, -0.3795),
        float2(-0.1986,  0.7375), float2(-0.3750, -0.7253), float2( 0.8134,  0.2974),
        float2(-0.8438,  0.3485), float2( 0.4045, -0.8677), float2( 0.2997,  0.9540)
    };
    float2 px = uv * rcpFrame.zw;
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
    float sn, cs;
    sincos(ang, sn, cs);
    float occ = 0.0;
    for (int i=0; i<12; i++) {
        float2 r = float2(off[i].x*cs - off[i].y*sn, off[i].x*sn + off[i].y*cs);
        float2 sUV = (uv + r * rcpFrame.xy * ssaoP.y) * dScale.xy;
        float zS = tex2D(depthTex, sUV).r;
        float d = zC - zS;
        if (d > minD && d < maxD) occ += 1.0;
    }
    float ao = saturate(1.0 - (occ / 12.0) * ssaoP.x);
    return float4(col * ao, 1);
}
)";

// ---- Light pass shaders (bloom + god rays), all run at half resolution ----------------------
// Bright-pass: isolate pixels brighter than the threshold, keeping their colour. Rendered into a
// half-res target (bilinear downsample from the full-res scene happens for free). c0.x = threshold.
static const char* g_brightPS = R"(
sampler2D scene : register(s0);
float4 p : register(c0);
float4 main(float2 uv:TEXCOORD0):COLOR0{
    float3 c = tex2D(scene, uv).rgb;
    float l = dot(c, float3(0.299,0.587,0.114));
    float k = max(l - p.x, 0.0) / max(l, 1e-4);
    return float4(c * k, 1);
}
)";

// Separable 9-tap Gaussian. c0.xy = texel step along the blur axis (one axis non-zero per pass).
static const char* g_blurPS = R"(
sampler2D src : register(s0);
float4 dir : register(c0);
float4 main(float2 uv:TEXCOORD0):COLOR0{
    float w0=0.227027, w1=0.1945946, w2=0.1216216, w3=0.054054, w4=0.016216;
    float3 c = tex2D(src, uv).rgb * w0;
    c += (tex2D(src, uv + dir.xy*1.0).rgb + tex2D(src, uv - dir.xy*1.0).rgb) * w1;
    c += (tex2D(src, uv + dir.xy*2.0).rgb + tex2D(src, uv - dir.xy*2.0).rgb) * w2;
    c += (tex2D(src, uv + dir.xy*3.0).rgb + tex2D(src, uv - dir.xy*3.0).rgb) * w3;
    c += (tex2D(src, uv + dir.xy*4.0).rgb + tex2D(src, uv - dir.xy*4.0).rgb) * w4;
    return float4(c, 1);
}
)";

// God rays / crepuscular light shafts (GPU Gems 3 radial light scattering). Marches 32 samples
// from the pixel toward the detected light screen position, accumulating the bright-pass with a
// per-step decay. c0 = (lightU, lightV, decay, weight), c1 = (exposure, density, _, _).
static const char* g_rayPS = R"(
sampler2D src : register(s0);
float4 lp : register(c0);
float4 ex : register(c1);
float4 main(float2 uv:TEXCOORD0):COLOR0{
    float2 delta = (uv - lp.xy) * (ex.y / 32.0);
    float2 c = uv;
    float3 col = 0.0;
    float illum = 1.0;
    for (int i=0;i<32;i++){
        c -= delta;
        col += tex2D(src, c).rgb * illum * lp.w;
        illum *= lp.z;
    }
    return float4(col * (ex.x / 32.0), 1);
}
)";

static IDirect3DTexture9*    s_fxaaTex  = nullptr;
static IDirect3DSurface9*    s_fxaaSurf = nullptr;
static IDirect3DPixelShader9* s_fxaaPS  = nullptr;
static IDirect3DPixelShader9* s_aoPS    = nullptr;
static UINT s_fxaaW = 0, s_fxaaH = 0;
static bool s_fxaaInitFailed = false;

// Light-pass resources (half-res). All created together when Bloom or GodRays is enabled.
static const int PROBE_SIZE = 64;
static IDirect3DTexture9*     s_brightTex = nullptr; static IDirect3DSurface9* s_brightSurf = nullptr;
static IDirect3DTexture9*     s_blur0Tex  = nullptr; static IDirect3DSurface9* s_blur0Surf  = nullptr;
static IDirect3DTexture9*     s_blur1Tex  = nullptr; static IDirect3DSurface9* s_blur1Surf  = nullptr;
static IDirect3DTexture9*     s_rayTex    = nullptr; static IDirect3DSurface9* s_raySurf    = nullptr;
static IDirect3DTexture9*     s_probeTex  = nullptr; static IDirect3DSurface9* s_probeSurf  = nullptr;
static IDirect3DSurface9*     s_probeSysSurf = nullptr;
static IDirect3DPixelShader9* s_brightPS = nullptr;
static IDirect3DPixelShader9* s_blurPS   = nullptr;
static IDirect3DPixelShader9* s_rayPS    = nullptr;
static int  s_lightW = 0, s_lightH = 0;
static bool s_lightingReady = false;
static float s_frameBloomStr = 0.0f; // per-frame composite strengths, set by RenderLighting
static float s_frameRayStr   = 0.0f;

// Small shared helpers for the multi-pass light pipeline.
static void SetLinearClamp(IDirect3DDevice9* dev, DWORD s)
{
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
}

static void DrawFSQuad(IDirect3DDevice9* dev, float w, float h)
{
    struct V { float x, y, z, rhw, u, v; };
    V q[4] = {
        { -0.5f,   -0.5f,   0, 1, 0, 0 },
        { w-0.5f,  -0.5f,   0, 1, 1, 0 },
        { -0.5f,   h-0.5f,  0, 1, 0, 1 },
        { w-0.5f,  h-0.5f,  0, 1, 1, 1 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

void FreeLighting()
{
    if (s_brightSurf) { s_brightSurf->Release(); s_brightSurf = nullptr; }
    if (s_brightTex)  { s_brightTex->Release();  s_brightTex  = nullptr; }
    if (s_blur0Surf)  { s_blur0Surf->Release();  s_blur0Surf  = nullptr; }
    if (s_blur0Tex)   { s_blur0Tex->Release();   s_blur0Tex   = nullptr; }
    if (s_blur1Surf)  { s_blur1Surf->Release();  s_blur1Surf  = nullptr; }
    if (s_blur1Tex)   { s_blur1Tex->Release();   s_blur1Tex   = nullptr; }
    if (s_raySurf)    { s_raySurf->Release();    s_raySurf    = nullptr; }
    if (s_rayTex)     { s_rayTex->Release();     s_rayTex     = nullptr; }
    if (s_probeSurf)  { s_probeSurf->Release();  s_probeSurf  = nullptr; }
    if (s_probeTex)   { s_probeTex->Release();   s_probeTex   = nullptr; }
    if (s_probeSysSurf) { s_probeSysSurf->Release(); s_probeSysSurf = nullptr; }
    if (s_brightPS)   { s_brightPS->Release();   s_brightPS   = nullptr; }
    if (s_blurPS)     { s_blurPS->Release();     s_blurPS     = nullptr; }
    if (s_rayPS)      { s_rayPS->Release();      s_rayPS      = nullptr; }
    s_lightW = s_lightH = 0;
    s_lightingReady = false;
    s_frameBloomStr = s_frameRayStr = 0.0f;
}

void FreeFXAA()
{
    if (s_fxaaPS)
        WrapperLog("FXAA: resources released (device Reset)\n");
    FreeLighting();
    if (s_fxaaSurf) { s_fxaaSurf->Release(); s_fxaaSurf = nullptr; }
    if (s_fxaaTex)  { s_fxaaTex->Release();  s_fxaaTex  = nullptr; }
    if (s_fxaaPS)   { s_fxaaPS->Release();   s_fxaaPS   = nullptr; }
    if (s_aoPS)     { s_aoPS->Release();     s_aoPS     = nullptr; }
    s_fxaaW = s_fxaaH = 0;
    if (g_sceneDepthTex) { g_sceneDepthTex->Release(); g_sceneDepthTex = nullptr; }
    g_sceneDepthW = g_sceneDepthH = 0;
    // Compare-only diagnostic pointers — would dangle once the texture above is released.
    g_sceneDepthSurf = nullptr;
    g_curDepthReal = nullptr;
    g_sceneDepthRTW = g_sceneDepthRTH = 0;
    // A failure can be transient (device mid-Reset); each Reset earns a fresh init attempt,
    // otherwise FXAA/grading/SSAO would stay disabled for the rest of the session.
    s_fxaaInitFailed = false;
}

static IDirect3DPixelShader9* CompilePS(IDirect3DDevice9* dev, const char* src, const char* name)
{
    ID3DXBuffer* pCode = nullptr;
    ID3DXBuffer* pErr  = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(D3DXCompileShader(src, (UINT)strlen(src), nullptr, nullptr,
                                     "main", "ps_3_0", 0, &pCode, &pErr, nullptr)))
    {
        dev->CreatePixelShader((DWORD*)pCode->GetBufferPointer(), &ps);
        pCode->Release();
    }
    else
    {
        WrapperLog("Lighting init: %s shader compile failed: %s\n", name,
            pErr ? (char*)pErr->GetBufferPointer() : "?");
    }
    if (pErr) pErr->Release();
    return ps;
}

static IDirect3DTexture9* CreateRT(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface9** outSurf)
{
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &tex, nullptr)))
        return nullptr;
    tex->GetSurfaceLevel(0, outSurf);
    return tex;
}

// Creates the half-res bloom/god-ray targets + the brightest-pixel probe (RT + system-mem copy).
// Non-fatal: on any failure the lighting pass stays off and FXAA continues normally.
static void InitLighting(IDirect3DDevice9* dev, UINT W, UINT H, D3DFORMAT fmt)
{
    s_lightingReady = false;
    if (!bBloom && !bGodRays) return;

    s_lightW = (int)(W / 2); s_lightH = (int)(H / 2);
    if (s_lightW < 4 || s_lightH < 4) return;

    s_brightTex = CreateRT(dev, s_lightW, s_lightH, fmt, &s_brightSurf);
    s_blur0Tex  = CreateRT(dev, s_lightW, s_lightH, fmt, &s_blur0Surf);
    s_blur1Tex  = CreateRT(dev, s_lightW, s_lightH, fmt, &s_blur1Surf);
    s_rayTex    = CreateRT(dev, s_lightW, s_lightH, fmt, &s_raySurf);
    s_probeTex  = CreateRT(dev, PROBE_SIZE, PROBE_SIZE, fmt, &s_probeSurf);
    dev->CreateOffscreenPlainSurface(PROBE_SIZE, PROBE_SIZE, fmt, D3DPOOL_SYSTEMMEM, &s_probeSysSurf, nullptr);

    // Clear to black so the composite never reads uninitialised GPU memory: when only one of
    // bloom/god rays is active the other buffer is still bound and sampled (then *0). Garbage
    // could be NaN, and NaN*0 = NaN would corrupt the pixel — a black start guarantees 0.
    if (s_brightSurf) dev->ColorFill(s_brightSurf, nullptr, 0);
    if (s_blur0Surf)  dev->ColorFill(s_blur0Surf,  nullptr, 0);
    if (s_blur1Surf)  dev->ColorFill(s_blur1Surf,  nullptr, 0);
    if (s_raySurf)    dev->ColorFill(s_raySurf,    nullptr, 0);

    s_brightPS = CompilePS(dev, g_brightPS, "bright");
    s_blurPS   = CompilePS(dev, g_blurPS,   "blur");
    s_rayPS    = CompilePS(dev, g_rayPS,    "ray");

    bool ok = s_brightTex && s_blur1Tex && s_brightPS && s_blurPS;
    if (bGodRays) ok = ok && s_rayTex && s_rayPS && s_probeSurf && s_probeSysSurf;
    if (!ok)
    {
        WrapperLog("Lighting init: resource/shader creation failed -> lighting pass disabled\n");
        FreeLighting();
        return;
    }
    s_lightingReady = true;
    WrapperLog("Lighting init: %dx%d half-res  Bloom=%d (str=%.2f thr=%.2f)  GodRays=%d (str=%.2f decay=%.3f)\n",
        s_lightW, s_lightH, (int)bBloom, fBloomStrength, fBloomThreshold,
        (int)bGodRays, fGodRaysStrength, fGodRaysDecay);
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

    // AO-only shader for the depth-unbind pass (option B). Non-fatal if it fails: the Present
    // pass keeps its own SSAO as a fallback (gated on g_aoDoneThisFrame staying false).
    ID3DXBuffer* pAOCode = nullptr;
    ID3DXBuffer* pAOErr  = nullptr;
    if (SUCCEEDED(D3DXCompileShader(g_aoPS, (UINT)strlen(g_aoPS), nullptr, nullptr,
                                     "main", "ps_3_0", 0, &pAOCode, &pAOErr, nullptr)))
    {
        dev->CreatePixelShader((DWORD*)pAOCode->GetBufferPointer(), &s_aoPS);
        pAOCode->Release();
    }
    else
    {
        WrapperLog("AO-unbind: shader compile failed: %s (falling back to SSAO-at-Present)\n",
            pAOErr ? (char*)pAOErr->GetBufferPointer() : "?");
    }
    if (pAOErr) pAOErr->Release();

    s_fxaaW = W; s_fxaaH = H;
    WrapperLog("FXAA init: %dx%d fmt=%d (SSAO kernel: 12-tap spiral, per-pixel rotation)\n", W, H, (int)fmt);

    InitLighting(dev, W, H, fmt);
    return true;
}

// Option B: apply SSAO the instant the game unbinds scene depth (3D done, UI not yet drawn).
// Darkens the back buffer in place; the game then composites UI on top untouched, and the
// Present pass runs FXAA + grading on the result (its own SSAO suppressed via g_aoDoneThisFrame).
// Reuses s_fxaaTex/s_fxaaSurf as the scene-copy scratch (the two passes never overlap in time).
void DeviceSSAOAtUnbind(IDirect3DDevice9* dev)
{
    if (!bFXAA || !bSSAO || !g_intzSupported || !g_sceneDepthTex) return;
    if (!s_aoPS || !s_fxaaTex || !s_fxaaSurf) return; // resources not ready yet (first frame)

    IDirect3DSurface9* pBB = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) || !pBB) return;
    D3DSURFACE_DESC desc;
    pBB->GetDesc(&desc);
    if (desc.Width != s_fxaaW || desc.Height != s_fxaaH) { pBB->Release(); return; }

    if (FAILED(dev->StretchRect(pBB, nullptr, s_fxaaSurf, nullptr, D3DTEXF_NONE))) { pBB->Release(); return; }

    IDirect3DStateBlock9* pSB = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &pSB)) || !pSB) { pBB->Release(); return; }

    // Render target is already the back buffer (verified by the caller); leave depth-stencil bound
    // (Z disabled below makes it irrelevant) so we don't disturb the game's about-to-unbind state.
    dev->SetRenderTarget(0, pBB);
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
    dev->SetPixelShader(s_aoPS);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, s_fxaaTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    dev->SetTexture(1, g_sceneDepthTex);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    float rcpF[4] = { 1.0f / s_fxaaW, 1.0f / s_fxaaH, (float)s_fxaaW, (float)s_fxaaH };
    dev->SetPixelShaderConstantF(0, rcpF, 1);
    float ssaoP[4] = { fSSAOStrength, fSSAORadius, fSSAOMinDelta, fSSAOMaxDelta };
    dev->SetPixelShaderConstantF(3, ssaoP, 1);
    float dsc[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    if (g_sceneDepthW && g_sceneDepthH && g_sceneDepthRTW && g_sceneDepthRTH)
    {
        float sx = (float)g_sceneDepthRTW / (float)g_sceneDepthW;
        float sy = (float)g_sceneDepthRTH / (float)g_sceneDepthH;
        if (sx <= 1.0f && sy <= 1.0f) { dsc[0] = sx; dsc[1] = sy; }
    }
    dev->SetPixelShaderConstantF(4, dsc, 1);

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
    pBB->Release();

    g_aoDoneThisFrame = true;
    static bool s_logged = false;
    if (!s_logged) { s_logged = true; WrapperLog("SSAO: running at scene depth-unbind, pre-UI (option B)\n"); }
}

// Analyses the bright-pass buffer: downsamples the half-res bright target to a 64x64 RT, reads it
// back to system memory and scans it on the CPU (4096 texels — negligible). Returns the screen-UV
// of the brightest texel (god-ray origin), its luma, and the FRACTION of the frame that is bright.
// That fraction is the menu/over-bright guard: a 2D menu or a near-white close-up has a huge bright
// area (would bloom into a white blob), while a normal scene only has small light sources.
static bool AnalyzeBright(IDirect3DDevice9* dev, float& outU, float& outV, float& outMax, float& outFrac)
{
    if (!s_probeSurf || !s_probeSysSurf || !s_brightSurf) return false;

    // GetRenderTargetData forces a CPU/GPU sync, so we only re-probe every 3rd frame and reuse the
    // cached result in between — neither the light position nor the bright fraction moves fast.
    static int   s_tick = 0;
    static bool  s_haveCache = false;
    static float s_cU = 0.5f, s_cV = 0.5f, s_cMax = 0.0f, s_cFrac = 0.0f;
    if (s_haveCache && (s_tick++ % 3) != 0)
    {
        outU = s_cU; outV = s_cV; outMax = s_cMax; outFrac = s_cFrac;
        return true;
    }

    if (FAILED(dev->StretchRect(s_brightSurf, nullptr, s_probeSurf, nullptr, D3DTEXF_LINEAR))) return false;
    if (FAILED(dev->GetRenderTargetData(s_probeSurf, s_probeSysSurf))) return false;

    D3DLOCKED_RECT lr;
    if (FAILED(s_probeSysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    float best = -1.0f; int bx = PROBE_SIZE / 2, by = PROBE_SIZE / 2; int brightCount = 0;
    BYTE* base = (BYTE*)lr.pBits;
    for (int y = 0; y < PROBE_SIZE; y++)
    {
        DWORD* row = (DWORD*)(base + y * lr.Pitch);
        for (int x = 0; x < PROBE_SIZE; x++)
        {
            DWORD px = row[x];
            float r = (float)((px >> 16) & 0xFF);
            float g = (float)((px >> 8)  & 0xFF);
            float b = (float)( px        & 0xFF);
            float l = 0.299f * r + 0.587f * g + 0.114f * b;
            if (l > best) { best = l; bx = x; by = y; }
            if (l > 15.0f) brightCount++; // ~0.06 of full scale = a meaningfully bright texel
        }
    }
    s_probeSysSurf->UnlockRect();
    outU = (bx + 0.5f) / (float)PROBE_SIZE;
    outV = (by + 0.5f) / (float)PROBE_SIZE;
    outMax = best / 255.0f;
    outFrac = (float)brightCount / (float)(PROBE_SIZE * PROBE_SIZE);
    s_cU = outU; s_cV = outV; s_cMax = outMax; s_cFrac = outFrac; s_haveCache = true;
    return true;
}

// Runs the bright-pass, the bloom blur and the god-ray radial blur into the half-res targets.
// Leaves bloom in s_blur1Tex and god rays in s_rayTex; sets the per-frame composite strengths
// (s_frameBloomStr / s_frameRayStr) the FXAA pass then reads. Must run inside ApplyFXAA's state
// block (the block restores the game's render target / shaders afterwards).
static void RenderLighting(IDirect3DDevice9* dev)
{
    s_frameBloomStr = 0.0f;
    s_frameRayStr   = 0.0f;
    if (!s_lightingReady || !s_fxaaTex) return;

    float lw = (float)s_lightW, lh = (float)s_lightH;
    float texX = 1.0f / lw, texY = 1.0f / lh;
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)s_lightW, (DWORD)s_lightH, 0.0f, 1.0f };

    dev->SetDepthStencilSurface(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    // --- bright-pass: full-res scene -> half-res bright target ---
    dev->SetRenderTarget(0, s_brightSurf);
    dev->SetViewport(&vp);
    dev->SetPixelShader(s_brightPS);
    dev->SetTexture(0, s_fxaaTex);
    SetLinearClamp(dev, 0);
    float bp[4] = { fBloomThreshold, 0, 0, 0 };
    dev->SetPixelShaderConstantF(0, bp, 1);
    DrawFSQuad(dev, lw, lh);

    // Analyse the bright-pass once (light position + bright fraction). The fraction drives the
    // over-bright guard below; the position is the god-ray origin.
    float lu = 0.5f, lv = 0.5f, lmax = 0.0f, lfrac = 0.0f;
    bool haveProbe = AnalyzeBright(dev, lu, lv, lmax, lfrac);
    // Menu / over-bright guard: a 2D menu or a near-white close-up has a huge bright area that
    // would bloom into a white blob. Fade the whole light pass out as the bright fraction climbs:
    // full strength up to 30% of the frame bright, down to 0 by 60%. Normal scenes (small light
    // sources) stay well under 30% and keep full effect.
    float guard = 1.0f;
    if (haveProbe)
    {
        guard = 1.0f - (lfrac - 0.30f) / 0.30f;
        if (guard < 0.0f) guard = 0.0f;
        if (guard > 1.0f) guard = 1.0f;
    }

    // --- bloom: two separable-Gaussian iterations with wide tap spacing ---
    // A single tight 9-tap blur only reaches ~8px and reads as a 1px halo, not a glow. Two
    // iterations (the 2nd twice as wide) spread the light far enough onto surrounding pixels to
    // actually look like bloom. blur0/blur1 ping-pong; the final glow ends up in s_blur1Tex.
    if (bBloom && guard > 0.01f)
    {
        dev->SetPixelShader(s_blurPS);
        const float spread = 2.5f;
        struct Pass { IDirect3DSurface9* dst; IDirect3DTexture9* src; float dx, dy; };
        Pass passes[4] = {
            { s_blur0Surf, s_brightTex, texX * spread,        0.0f },
            { s_blur1Surf, s_blur0Tex,  0.0f,                 texY * spread },
            { s_blur0Surf, s_blur1Tex,  texX * spread * 2.0f, 0.0f },
            { s_blur1Surf, s_blur0Tex,  0.0f,                 texY * spread * 2.0f },
        };
        for (int p = 0; p < 4; p++)
        {
            dev->SetRenderTarget(0, passes[p].dst);
            dev->SetViewport(&vp);
            dev->SetTexture(0, passes[p].src);
            SetLinearClamp(dev, 0);
            float d[4] = { passes[p].dx, passes[p].dy, 0, 0 };
            dev->SetPixelShaderConstantF(0, d, 1);
            DrawFSQuad(dev, lw, lh);
        }
        s_frameBloomStr = fBloomStrength * guard;
    }

    // --- god rays: radial-blur from the detected brightest on-screen source ---
    if (bGodRays && haveProbe && lmax > 0.04f && guard > 0.01f)
    {
        dev->SetRenderTarget(0, s_raySurf);
        dev->SetViewport(&vp);
        dev->SetPixelShader(s_rayPS);
        dev->SetTexture(0, s_brightTex);
        SetLinearClamp(dev, 0);
        float c0[4] = { lu, lv, fGodRaysDecay, 0.95f };
        float c1[4] = { 1.0f, 1.0f, 0, 0 }; // exposure=1 (normalized), density=1; strength applied at composite
        dev->SetPixelShaderConstantF(0, c0, 1);
        dev->SetPixelShaderConstantF(1, c1, 1);
        DrawFSQuad(dev, lw, lh);
        s_frameRayStr = fGodRaysStrength * guard;
    }
}

static void ApplyFXAA(IDirect3DDevice9* dev, IDirect3DSurface9* pBB)
{
    // Copy the finished scene into our texture
    if (FAILED(dev->StretchRect(pBB, nullptr, s_fxaaSurf, nullptr, D3DTEXF_NONE)))
        return;

    // Save full device state, apply FXAA pass, restore. Without a state block we can't
    // restore the game's state afterwards, so skip the pass entirely rather than corrupt it.
    IDirect3DStateBlock9* pSB = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &pSB)) || !pSB)
        return;

    // Light passes (bloom + god rays) render into their own half-res targets first; the FXAA
    // pass below composites the results. No-op (and leaves strengths at 0) when lighting is off.
    RenderLighting(dev);

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
    float rcpF[4] = { 1.0f / s_fxaaW, 1.0f / s_fxaaH, (float)s_fxaaW, (float)s_fxaaH };
    dev->SetPixelShaderConstantF(0, rcpF, 1);
    // c1 = (lift, gamma, gain, vibrance), c2 = (vignette, sharpness, temperature, tint). Neutral
    // when ColorGrading=0, but sharpness rides in c2.y independently — it is part of the FXAA
    // pass, not grading, so it applies whenever FXAA is on.
    float grade1[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    float grade2[4] = { 0.0f, fSharpness, 0.0f, 0.0f };
    if (bColorGrading)
    {
        grade1[0] = fLift;
        grade1[1] = (fGamma > 0.01f) ? fGamma : 1.0f;
        grade1[2] = fGain;
        grade1[3] = fVibrance;
        grade2[0] = fVignette;
        grade2[2] = fTemperature;
        grade2[3] = fTint;
    }
    dev->SetPixelShaderConstantF(1, grade1, 1);
    dev->SetPixelShaderConstantF(2, grade2, 1);
    float gradeC[4] = { bColorGrading ? fContrast : 0.0f, bColorGrading ? fSplitTone : 0.0f, 0.0f, 0.0f };
    dev->SetPixelShaderConstantF(6, gradeC, 1);

    // SSAO: use the cached scene-depth INTZ texture (populated at CreateDepthStencilSurface time).
    // We do NOT use GetDepthStencilSurface here because the game often unbinds depth before Present
    // (UI rendering); the cached texture still holds the last-rendered scene depth contents.
    // Skip SSAO here when the depth-unbind pass (option B) already darkened this frame's scene.
    // If that pass didn't run (g_aoDoneThisFrame still false — e.g. a scene with no clean
    // scene-depth unbind), fall back to doing SSAO in this combined pass, UI-bleed guard included.
    bool ssaoActive = false;
    if (bSSAO && g_intzSupported && g_sceneDepthTex && !g_aoDoneThisFrame)
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
    // c4: screen-UV -> depth-UV scale. Identity until the game has bound the cached depth at
    // least once (g_sceneDepthRTW measured in SetDepthStencilSurface); identity also covers
    // the common case where depth and back buffer have the same size.
    float dsc[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    if (ssaoActive && g_sceneDepthW && g_sceneDepthH && g_sceneDepthRTW && g_sceneDepthRTH)
    {
        float sx = (float)g_sceneDepthRTW / (float)g_sceneDepthW;
        float sy = (float)g_sceneDepthRTH / (float)g_sceneDepthH;
        if (sx <= 1.0f && sy <= 1.0f)
        {
            dsc[0] = sx;
            dsc[1] = sy;
        }
    }
    dev->SetPixelShaderConstantF(4, dsc, 1);

    // c5: light-pass composite strengths. RenderLighting() set s_frameBloomStr / s_frameRayStr
    // (0 when that effect is off or, for god rays, when no bright source was on screen this frame).
    float lightP[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (s_lightingReady && (s_frameBloomStr > 0.0f || s_frameRayStr > 0.0f))
    {
        lightP[0] = s_frameBloomStr;
        lightP[1] = s_frameRayStr;
        dev->SetTexture(2, s_blur1Tex);
        SetLinearClamp(dev, 2);
        dev->SetTexture(3, s_rayTex);
        SetLinearClamp(dev, 3);
    }
    dev->SetPixelShaderConstantF(5, lightP, 1);

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
