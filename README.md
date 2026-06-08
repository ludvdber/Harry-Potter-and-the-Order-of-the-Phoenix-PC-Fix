<div align="center">

# 🎮 Game Specific Patches & DLL Wrappers  

***created and maintained by***

[![Chip-Biscuit Website](https://img.shields.io/badge/Chip--Biscuit-Website-blue?style=for-the-badge)](https://chip-biscuit.github.io/)

Reverse Engineering • Programming • Patching • Game Improvements • DLL Creation 

[![Downloads](https://img.shields.io/github/downloads/Chip-Biscuit/Harry-Potter-and-the-Order-of-the-Phoenix-PC-Fix/total?label=Total%20Downloads)](https://github.com/Chip-Biscuit/Harry-Potter-and-the-Order-of-the-Phoenix-PC-Fix/releases)

<sub>click the Total Downloads button above to take you to the releases page and download the zip at the bottom</sub>

</div>

# Harry Potter and the Order of the Phoenix PC Fix

![hp5](https://github.com/user-attachments/assets/d18c2a80-bd34-463b-b372-4d20f4a19b30)

# Requirements before using fix
IMPORTANT READ THE READ ME FILE INCLUDED WITH THE DOWNLOAD BEFORE USING THE FIX.

# Official video guide

Watch the official fix guide video here on how to install the game and use the fix

<a href="https://www.youtube.com/watch?v=VeZ-Envz9ME&t=58s">
  <img src="https://github.com/user-attachments/assets/468a7fa8-f32a-4607-aee9-11b7bafe738e" alt="hp5" width="480" height="360">
</a><br>

###### <i>Click the image above to watch the video</i>

# Instructions
You must launch the game once before you use this fix. You will either need to start a new game or continue a save file and go to the video settings and make sure your resolution is 640 x 480 in order for the d3d9.dll file to work.

Go to releases, download the Harry Potter and the Order of the Phoenix Fix that fits the requirements you need, then extract the d3d9.dll, d3d9.ini and the fps.dll files into your game folder next to the hp.exe file and you are good to go! You can edit the settings you wish to use in the d3d9.ini file.

# Resolution/Aspect Ratio
The default for resolution is set to (1920 x 1080). Put the resolution that you wish to use in both Width and Height. If you boot up the game after setting the resolution you want in the d3d9.ini file and the resolution has not changed automatically then go to the video options menu in game and select your resolution from in there.

The default for aspect ratio is (0) which is (16:9). Choose the aspect ratio that is correct for you from the selection within the d3d9.ini file and replace (0) with the number correct for you.

# FPS
Choose one of the releases of either (60fps or 120fps) to use. If your monitor is 60hz then choose the 60fps release. If your monitor is 120hz or more choose the 120fps release.

# FOV
You can choose in the d3d9.ini file of either 1, 2 or 3 each will zoom the FOV/camera in the game out slightly more. Choose which option you want to use and it will be reflected in the game, 0 is off or original game cameras zoom FOV.

# Visual Quality Enhancements

This fork extends the wrapper with several visual upgrades configured in `d3d9.ini`. All defaults are tuned to give a clear quality bump out of the box without manual tuning. Drop the new `d3d9.dll` and `d3d9.ini` next to `hp.exe` and you're set.

## `[GRAPHICS]` — anti-aliasing and texture filtering

- **`FXAA`** (default `1`) — post-process anti-aliasing combined with adaptive sharpening. FXAA smooths jagged edges; the sharpening pass recovers texture detail. Near-zero performance cost. Also hosts the color grading and SSAO passes below — turning FXAA off disables both.
- **`Antialiasing`** (default `8`) — MSAA on the back buffer (2/4/8/16x). The wrapper queries the driver and **cascades down** (16 → 8 → 4 → 2 → off) until it finds a level the GPU supports — no more "asked for 16x, silently disabled" surprises. Set to `0` if you observe artifacts on character rendering.
- **`AnisotropicFiltering`** (default `16`) — forces full trilinear + anisotropic filtering on every texture sampler regardless of the game's choice. Fixes streaky/blurry ground textures at oblique angles. Free on any modern GPU.
- **`TextureLODBias`** (default `-1.0`) — biases mip selection toward sharper levels. Combined with the wrapper's automatic mipmap regeneration (the game ships textures without mip chains), this fixes blurry faces and surfaces at medium distance. Going below `-1.5` may cause shimmer at very long range.
- **`SSAAFactor`** (default `1`) — set to `2`, `3` or `4` to render at NxN the back buffer resolution then downsample with bilinear filtering at present time. The strongest AA option (true supersampling), eliminates virtually all aliasing including thin sub-pixel features. Very expensive: `2x` quadruples GPU load. Disables MSAA when active. Recommended only with a high-end GPU.
- **`VSync`** (default `0`) — caps frame rate to your monitor's refresh rate.

### Color grading (folded into the FXAA pass)

The 2007 base render is technically clean but visually flat — washed greens, no cinematic depth. The wrapper applies a subtle ASC-CDL-style grade in the same shader pass as FXAA, so it costs zero extra draw call.

- **`ColorGrading`** (default `1`) — `0` = bypass (neutral pass-through), `1` = apply the values below.
- **`Vibrance`** (default `0.25`) — boosts saturation of *low*-saturation pixels (skin, robes, stone) without over-saturating already-colorful pixels. Range `0.0..1.0`.
- **`Vignette`** (default `0.00`) — radial darkening at screen corners for cinematic focus. Range `0.0..1.0`.
- **`Lift`** (default `0.00`) — raises shadows. Positive = lifted blacks (faded film look). Range `-0.10..+0.10`.
- **`Gamma`** (default `1.00`) — midtone curve. `<1` brightens mids, `>1` darkens them. Range `0.5..2.0`.
- **`Gain`** (default `1.05`) — overall multiplier before gamma. `>1` = brighter and punchier. Range `0.5..2.0`.

### Frame latency

On any GPU that exposes `IDirect3DDevice9Ex` (every Vista+ driver), the wrapper calls `SetMaximumFrameLatency(1)` right after device creation. This shaves one frame of input lag versus the Windows default of 3 — perceptible on any game above 60 FPS. No ini option, always on.

### Screen-space ambient occlusion (SSAO)

Off by default. When `SSAO = 1`, the wrapper:

1. At device creation, checks the GPU for the `INTZ` depth-as-texture format (every D3D10+ GPU supports it).
2. Overrides `AutoDepthStencilFormat` to `INTZ` so the auto depth-stencil becomes a sampleable texture instead of an opaque surface, and intercepts `CreateDepthStencilSurface` to do the same for game-created depth (HP5 uses this path).
3. In the FXAA pass, performs an 8-tap radial depth comparison around each pixel; pixels with neighbors closer to the camera (= occluders) within a tunable delta range get darkened.
4. Composites with the FXAA + grading output in the same shader pass — no extra draw call.

Falls back silently if INTZ isn't supported.

- **`SSAO`** (default `0`) — `0` off, `1` on. Adds ~8 depth taps per pixel; cost is negligible on any GPU made in the last decade.
- **`SSAOStrength`** (default `0.80`) — `0..1`. How much occlusion darkens occluded pixels.
- **`SSAORadius`** (default `6.0`) — `1..16` pixels. Sampling radius — larger = softer/wider halo, smaller = tight contact AO.
- **`SSAOMinDelta`** (default `0.001`) — perspective-relative depth threshold. Below this, samples are ignored (avoids self-occlusion noise on flat surfaces).
- **`SSAOMaxDelta`** (default `0.30`) — perspective-relative depth threshold. Above this, samples are ignored (avoids halos around foreground objects). Tighten if you see halos around characters.

## `[FORCEWINDOWED]` — windowed mode and Alt+Tab

- **`FreeMouse`** (default `1`) — neutralizes the game's `ClipCursor` and `SetCapture` calls so the cursor can leave the window freely. Enables Alt+Tab, multi-monitor cursor movement, screenshot tools, and the Windows key. No impact on gameplay.
- **`DoNotNotifyOnTaskSwitch`** (default `1`) — swallows the `WM_ACTIVATEAPP(FALSE)` message HP5 reacts to on focus loss. Without this, Alt+Tabbing away can freeze the game on return. Works without `EnableHooks` because the wrapper subclasses HP5's window directly via `SetWindowLongPtr`.

### Alt+Tab behavior

With the defaults above, Alt+Tab no longer freezes the game.

- **Mouse recovers immediately** when you come back. The wrapper proxies DirectInput8 and forces a re-Acquire on `WM_ACTIVATEAPP(TRUE)`.
- **Keyboard recovers on its own after ~30 seconds.** HP5's main window only receives `WM_NCACTIVATE` after the first activation (not `WM_ACTIVATE` / `WM_ACTIVATEAPP` / `WM_SETFOCUS`), so we can't trigger a keyboard re-Acquire from a Win32 hook — but the game's own DirectInput polling eventually times out and re-acquires the keyboard. The delay is constant. If you Alt+Tab once in a while, just wait; if you Alt+Tab often, relaunching is faster than waiting.

## `[RESOLUTION]` section

- **`Width` / `Height`** (default `0`) — `0` means use the game's default resolution (typically your screen's native). Override only if you specifically want a different render resolution. With borderless fullscreen the result is stretched to your screen, so picking a value lower than your screen's native produces a blurry upscale.

## Mipmap regeneration

The game ships character and surface textures without mip chains, which causes severe blur and shimmer at distance regardless of LOD bias. The wrapper detects single-mip textures at creation time and regenerates a full mip chain via `D3DXFilterTexture(TRIANGLE | DITHER | SRGB)` after the first upload. This is automatic and has no ini option — it always runs.

## Logging

Every game launch writes a fresh `d3d9_wrapper.log` next to `hp.exe` describing which features actually engaged on your hardware (INTZ support, MSAA cascade result, AF/LOD bias overrides, etc.). Tail this file first if anything looks off.

## Footgun — leave `EnableHooks=0`

The original wrapper exposes `EnableHooks=1` to install a process-wide IAT hook chain. On HP5 specifically, this crashes `hp.exe` at `DLL_PROCESS_ATTACH` — the chain rewrites `RegisterClassA/W/Ex`, `LoadLibraryA/W/Ex`, `FreeLibrary`, `GetProcAddress` across `hp.exe` + `ole32.dll` + `d3d9.dll`, and the game never launches. Symptom: the log stops on `FreeMouse hooks installed` and you see no window. Leave `EnableHooks=0` (the default).

# Vote to see the game return via GOG Dreamlist
If you are interested in potentially seeing this game easily available to purchase and use today then go and vote on the games GOG Dreamlist to help make this become a reality, you can vote for the game here and write a message about the game if you wish – https://www.gog.com/dreamlist/game/harry-potter-and-the-order-of-the-phoenix-2007

# Issues/Problems
If you have any issues, with the fixes then please go to discord for help linked below. https://discord.gg/eVJ7sQH7Cc

# Credits
Credit to Elisha Riedlinger for the base wrapper and 13 AG.

---

### Fix Enhancers  
https://fixenhancers.wixsite.com/fix-enhancers

“Creating compatibility fixes and enhancements for legacy PC games.”

# Chip
- founder
- reverse engineer
- programmer
- developer
- Game Preservationist
  
<img width="250" height="500" alt="my logoo" src="https://github.com/user-attachments/assets/9bb13d3f-0734-4f1d-b68f-14114b13744a" />


# JokerAlex21 
- founder
- admin
- tester 

<img width="250" height="250" alt="YouTube_Logo" src="https://github.com/user-attachments/assets/5c7204ca-4bca-4673-8117-965732e7ee6d" />
