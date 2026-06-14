# Harry Potter and the Order of the Phoenix — Enhanced PC Fix

![hp5](https://github.com/user-attachments/assets/d18c2a80-bd34-463b-b372-4d20f4a19b30)

An enhanced **Direct3D 9 wrapper** for *Harry Potter and the Order of the Phoenix* (EA Bright Light, 2007). Drops in as `d3d9.dll` next to `hp.exe` and layers modern visual quality on top of the 2007 renderer: real MSAA with auto-fallback, forced anisotropic filtering, automatic mipmap regeneration, FXAA + tunable adaptive sharpening, screen-space ambient occlusion, a bloom + god-rays lighting pass, and a full color grade (white balance, contrast, split toning) — plus a clean fix for the Alt+Tab freeze the game has shipped with since launch.

All visual effects run as post-process passes (no texture replacement) and every value lives in `d3d9.ini`, read once at launch.

Based on [Chip-Biscuit's HP5 PC Fix](https://github.com/Chip-Biscuit/Harry-Potter-and-the-Order-of-the-Phoenix-PC-Fix) (which itself extends Elisha Riedlinger's generic D3D9 proxy wrapper). All additions described below are **new in this fork** — they do not exist in the upstream releases. License: [Unlicense](LICENSE) (public domain).

---

## What this fork adds

| Feature | Default | What it does |
|---|---|---|
| **MSAA with cascade fallback** | `8×` | Real hardware MSAA on the back buffer. The driver is probed and the wrapper cascades `16 → 8 → 4 → 2 → off` until it finds a level the GPU supports for both back buffer and depth-stencil. No more silent "asked for 16x, got nothing". |
| **Forced anisotropic filtering** | `16×` | Forced on every sampler regardless of what the game asks for. Fixes streaky ground textures at oblique angles. Free on any modern GPU. |
| **Automatic mipmap regeneration** | always on | HP5 ships character and surface textures **without mip chains** — a major cause of distant blur and shimmer that no amount of LOD bias can fix. The wrapper detects single-mip textures at creation, rebuilds the full chain via `D3DXFilterTexture(TRIANGLE \| DITHER \| SRGB)` after first upload, and the game runs as if it had been authored with mips from day one. |
| **Texture LOD bias** | `-1.0` | Biases mip selection toward sharper levels. Combined with the mipmap regen above, recovers detail on faces and walls at medium distance. |
| **FXAA + tunable adaptive sharpening** | on | Single `ps_3_0` post-process pass that does luma-based edge AA and sharpens flat detail. The flat-vs-edge decision is a smooth `smoothstep` blend (not a hard branch, which used to make the sharpen fringe crawl on edges) and the overshoot is neighbourhood-clamped so it can't paint a 1px halo. Strength via `Sharpness`. Also hosts color grading, SSAO and the lighting composite so it's one shader, one draw call. |
| **Optional SSAA** | off | `SSAAFactor=2/3/4` renders at NxN back-buffer resolution and lets the driver bilinear-downsample. Strongest AA option, very expensive (2× quadruples GPU load). Disables MSAA. |
| **Screen-space ambient occlusion** | off | 12-tap golden-angle spiral depth comparison, per-pixel rotated. Auto depth-stencil is overridden to INTZ (FOURCC, every D3D10+ GPU) so depth becomes sampleable, and `CreateDepthStencilSurface` is intercepted so game-created scene depth gets the same treatment. AO runs as its own pass the moment the game unbinds scene depth (pre-UI), with a fallback inside the FXAA shader. Depth thresholds are perspective-scaled so a fixed delta works at all distances. |
| **Bloom + god-rays lighting pass** | off | Half-resolution light pipeline composited in the FXAA pass: a bright-pass + two-iteration Gaussian bloom, plus crepuscular light shafts radiating from the brightest **on-screen** source (auto-detected via a 64×64 readback). An automatic over-bright guard fades both out on 2D menus / near-white screens so they don't wash out. |
| **Full color grade folded into the FXAA pass** | on, subtle | ASC-CDL Lift / Gain → Gamma → white balance (Temperature / Tint) → contrast S-curve → Vibrance → split toning (teal/orange) → Vignette, all sharing the FXAA shader so it costs zero extra draw call. Defaults give HP5's flat 2007 look a modern pop without making it look like a different game. |
| **`SetMaximumFrameLatency(1)`** | always on | Cuts input lag from the Windows default of 3 frames down to 1, on any device that exposes `IDirect3DDevice9Ex` (every Vista+ driver). Perceptible above 60 FPS. |
| **DirectInput8 proxy + Alt+Tab mouse re-Acquire** | always on | Proxies `IDirectInput8A/W` and `IDirectInputDevice8A/W`. Mouse devices are tracked and force-re-Acquired on `WM_ACTIVATEAPP(TRUE)`. Mouse comes back immediately on Alt+Tab return — no relaunching the game. |
| **Per-launch diagnostics log** | always on | Every launch writes `d3d9_wrapper.log` next to `hp.exe` listing exactly what engaged on your hardware: INTZ support, MSAA cascade result, AF/LOD overrides, FXAA shader compile status, DI8 wrapper status. Tail this file first if anything looks off. |

Plus a handful of safety/QoL fixes:

- `Antialiasing` no longer silently disables when the asked-for level isn't supported — it cascades down.
- MSAA is **never** injected into `CreateRenderTarget` (would break the game's intermediate RTs that it samples as textures).
- `g_sceneDepthTex` is cached separately from the live depth-stencil binding because HP5 unbinds depth before Present (for UI rendering) — using `GetDepthStencilSurface` at Present time would return null.
- `EnableHooks=1` warning section (see [Footgun](#footgun--leave-enablehooks0) below).

---

## Installation

1. Install the game from your own copy or an archival source. Apply the NoDVD if needed (SecuROM).
2. **Launch the game once**, open Video Options, set resolution to **640×480**, quit. This step is required by the wrapper chain — without it, `d3d9.dll` won't take effect on first run.
3. Download the latest archive from this fork's **Releases** page.
4. Extract into the game folder so that next to `hp.exe` you have:
   - `d3d9.dll` (our wrapper)
   - `d3d9_original.dll` (upstream wrapper — the chain layer below ours)
   - `d3d9.ini` (defaults tuned for HP5)
5. *(Optional)* If you want the 60/120 FPS patch or the FOV / aspect-ratio overrides, drop in `fps.dll` from the [upstream releases](https://github.com/Chip-Biscuit/Harry-Potter-and-the-Order-of-the-Phoenix-PC-Fix/releases). Those features live in `fps.dll`, not in `d3d9.dll`.
6. Launch. All settings are configurable in `d3d9.ini` — see below.

After tweaking `d3d9.ini`, relaunch the game (the file is read once at DLL load — no hot-reload).

---

## `d3d9.ini` reference

### `[GRAPHICS]` — anti-aliasing and texture filtering

- **`FXAA`** (default `1`) — post-process AA + adaptive sharpening. Also gates color grading, SSAO and the lighting pass — turning FXAA off disables all of them. Near-zero cost.
- **`Sharpness`** (default `0.40`) — strength of the FXAA pass's adaptive sharpening. `0` = pure AA, no sharpening. Overshoot is clamped to the local neighbourhood so it can't crawl on edges. Range `0.0..1.0`.
- **`Antialiasing`** (default `8`) — MSAA level on the back buffer (`2`/`4`/`8`/`16`, or `0` = off). Cascades down if your GPU doesn't support the requested level. Set to `0` if you observe artifacts on character rendering.
- **`AnisotropicFiltering`** (default `16`) — forces full trilinear + anisotropic filtering on every sampler regardless of the game's choice. Free on any modern GPU.
- **`TextureLODBias`** (default `-1.0`) — mip-selection bias. Combined with automatic mipmap regen, fixes blurry faces and surfaces at medium distance. Below `-1.5` may cause shimmer at very long range. Range `-3.0..0.0`.
- **`SSAAFactor`** (default `1`) — set to `2`/`3`/`4` to render at NxN back-buffer resolution then bilinear-downsample. Strongest AA available (true supersampling). Disables MSAA. `2×` quadruples GPU load.
- **`VSync`** (default `0`) — caps to monitor refresh, costs 1 frame of latency.

### Color grading (folded into the FXAA pass)

The 2007 base render is technically clean but visually flat. The wrapper applies a full grade in the same shader as FXAA — no extra draw call. Order: Lift/Gain → Gamma → white balance → contrast → vibrance → split toning → vignette.

- **`ColorGrading`** (default `1`) — `0` bypasses everything below (neutral pass-through), `1` applies the values.
- **`Vibrance`** (default `0.40`) — boosts saturation of *low*-saturation pixels (skin, robes, stone) without over-saturating already-colorful pixels. Range `0.0..1.0`.
- **`Vignette`** (default `0.05`) — radial darkening at screen corners for cinematic focus. Range `0.0..1.0`.
- **`Lift`** (default `0.00`) — raises/lowers shadows. Negative = deeper blacks, positive = faded film look. Range `-0.10..+0.10`.
- **`Gamma`** (default `1.00`) — midtone curve. `<1` brightens, `>1` darkens. Range `0.5..2.0`.
- **`Gain`** (default `1.05`) — overall multiplier before gamma. `>1` = brighter and punchier. Range `0.5..2.0`.
- **`Temperature`** (default `0.00`) — white balance. Negative = cooler/bluer, positive = warmer/orange. Luma-preserving (re-tints without changing brightness). Range `-1.0..+1.0`.
- **`Tint`** (default `0.00`) — white balance on the green↔magenta axis. A slight positive value cuts HP5's yellow-green cast. Range `-1.0..+1.0`.
- **`Contrast`** (default `0.00`) — smoothstep S-curve. Deepens shadows and lifts highlights around mid-grey, adding pop and perceived depth to flat scenes. Range `0.0..1.0`.
- **`SplitTone`** (default `0.00`) — cinematic split toning: cool/teal shadows + warm/orange highlights, midtones left neutral. `0.15–0.25` is a tasteful touch; higher pushes shadows visibly blue. Range `0.0..1.0`.

### Lighting pass — bloom + god rays

Off by default. A half-resolution light pipeline (bright-pass → two-iteration Gaussian bloom; radial light shafts from the brightest on-screen source) composited additively in the FXAA pass. An automatic guard measures how much of the frame is bright and fades both effects out on 2D menus and near-white close-ups so they don't bloom into a white blob.

- **`Bloom`** (default `0`) — `0` off, `1` on. Soft glow around bright areas (windows, lamps, spells).
- **`BloomStrength`** (default `0.35`) — how strongly the glow is added. `0.35` subtle, `0.8+` strong. Range `0.0..2.0`.
- **`BloomThreshold`** (default `0.55`) — luma above which a pixel glows (shared with god rays). Higher = only the brightest sources. Range `0.0..1.0`.
- **`GodRays`** (default `0`) — `0` off, `1` on. Light shafts from the brightest on-screen source. Needs a window / the sun in frame; no bright source on screen = no rays.
- **`GodRaysStrength`** (default `0.45`) — how strongly the shafts are added. Range `0.0..2.0`.
- **`GodRaysDecay`** (default `0.96`) — per-step falloff along each ray. Higher = longer shafts. Range `0.80..0.999`.

### `ShadowMapScale` (experimental, off)

`ShadowMapScale=2/4` renders the game's square shadow-map render targets at NxN resolution for sharper shadow edges. **Experimental and left off** — HP5 reuses the exact same surface signature for water reflections and projected passes, which this also upscales and visibly glitches. Tested on HP5: leave at `1`.

### Screen-space ambient occlusion (SSAO)

Off by default — opt-in because it requires an INTZ-capable GPU (every D3D10+ card supports it, falls back silently otherwise). When on, the wrapper overrides the auto depth-stencil format and intercepts `CreateDepthStencilSurface` so it can sample scene depth, then does a 12-tap golden-angle spiral comparison (per-pixel rotated to break up banding). AO runs as its own pass the instant the game unbinds scene depth — before any UI is drawn — and falls back to running inside the FXAA shader if a scene has no clean depth unbind.

- **`SSAO`** (default `0`) — `0` off, `1` on. Adds 12 depth taps per pixel; negligible cost on any GPU made in the last decade.
- **`SSAOStrength`** (default `0.80`) — `0..1`. How much occlusion darkens occluded pixels.
- **`SSAORadius`** (default `6.0`) — `1..16` pixels. Sampling radius. Larger = softer/wider halo, smaller = tight contact AO.
- **`SSAOMinDelta`** (default `0.001`) — perspective-scaled depth threshold below which samples are ignored (avoids self-occlusion noise on flat surfaces).
- **`SSAOMaxDelta`** (default `0.30`) — perspective-scaled depth threshold above which samples are ignored (avoids halos at silhouettes). Tighten if you see halos around characters.

A luma-variance check from the FXAA pass is reused to skip AO on uniform-color regions — this is what prevents UI dialogs (drawn alpha-blended without writing depth) from showing scene-object silhouettes bleeding through.

### Frame latency

`SetMaximumFrameLatency(1)` is called automatically right after device creation on any `IDirect3DDevice9Ex` device. Cuts input lag from 3 frames to 1. No ini option — always on.

### `[FORCEWINDOWED]` — windowed mode and Alt+Tab

- **`FreeMouse`** (default `1`) — neutralizes the game's `ClipCursor` and `SetCapture` calls so the cursor can leave the window freely. Enables multi-monitor cursor movement, screenshot tools, the Windows key, and screenshots.
- **`DoNotNotifyOnTaskSwitch`** (default `1`) — swallows the `WM_ACTIVATEAPP(FALSE)` message HP5 reacts to on focus loss. Without this, Alt+Tabbing away can freeze the game on return. Works without `EnableHooks` because the wrapper subclasses HP5's window directly via `SetWindowLongPtr`.

### Alt+Tab behavior

With the defaults above, Alt+Tab no longer freezes the game.

- **Mouse recovers immediately** when you come back. The DirectInput8 proxy forces a re-Acquire on `WM_ACTIVATEAPP(TRUE)`.
- **Keyboard recovers on its own after ~30 seconds.** HP5's main window only receives `WM_NCACTIVATE` after the first activation — `WM_ACTIVATE` / `WM_ACTIVATEAPP` / `WM_SETFOCUS` never make it to our wndproc subclass on Alt+Tab return, so we can't trigger a keyboard re-Acquire from a Win32 hook. The game's own DirectInput polling eventually times out and re-acquires the keyboard. The delay is constant. If you Alt+Tab once in a while, just wait; if you Alt+Tab very often, relaunching is faster than waiting.

Six approaches to accelerate keyboard recovery were investigated and rejected — each one either failed to fire, broke the mouse path, or introduced a different regression. The 30-second wait is the documented tradeoff and is acceptable for normal play.

### `[RESOLUTION]`

- **`Width` / `Height`** (default `0`) — `0` = use the game's own resolution. **Recommended: leave these at `0` and pick your resolution in the game's own options menu** (with `DPIAware=1` it now lists your real native modes). Forcing a resolution here that differs from the game's own setting — especially a different aspect ratio, or `-1` (monitor native) — desyncs the game's reflection/projection rendering from the back buffer and produces **translucent "ghost duplicate" characters**. Set the resolution in-game and these two stay in sync. `-1` is kept for advanced use but documented as may-ghost.

### `[MAIN]`

Mostly upstream — see comments in `d3d9.ini` for `FPSLimit`, `FPSLimitMode`, `FullScreenRefreshRateInHz`, `DisplayFPSCounter`, `ForceWindowedMode`. Two additions in this fork:

- **`ScreenshotKey`** (default `123` = F12) — virtual-key code of a screenshot hotkey that saves the final, post-processed frame as a PNG to `<gamedir>/screenshots/`. Exists because Win+PrtScr is unreliable over HP5's input handling. `44` = PrtScr, `0` = off.
- **`DPIAware`** (default `1`) — opts the process out of Windows display scaling at DLL attach. Without it, on a scaled display (e.g. 150/160%) the borderless window is shrunk then blur-stretched by Windows and `GetMonitorInfo` reports a scaled-down desktop, so the game's resolution menu lists wrong modes. Leave at `1`.

### `[LAUNCHER]`

Upstream section, used only by the standalone launcher binary — see `d3d9.ini` comments for `AppExe` / `AppArgs`.

---

## Logging

Every launch writes a fresh `d3d9_wrapper.log` next to `hp.exe`. The log is opened in `"w"` mode so it's overwritten each time — if you need to keep an interesting one, rename it before relaunching.

Things to look for:
- `INTZ supported: yes/no` — gates SSAO availability.
- `ValidateMSAASupport: MSAA N OK` or `downgraded to M` — tells you what AA level actually engaged.
- `FXAA shader compiled OK` — if missing, the post-process pass didn't initialize.
- `DirectInput8Create wrapped` and `Acquire(mouse) on activate` — confirms DI8 proxy is live.

Tail this file first if any feature seems silently absent — most "it didn't work" cases are visible there.

---

## Footgun — leave `EnableHooks=0`

The upstream wrapper exposes `EnableHooks=1` to install a process-wide IAT hook chain (`RegisterClassA/W/Ex`, `LoadLibraryA/W/Ex`, `FreeLibrary`, `GetProcAddress` across `hp.exe` + `ole32.dll` + `d3d9.dll`).

**On HP5 specifically, this crashes the game at `DLL_PROCESS_ATTACH`.** Symptom: the log stops on `FreeMouse hooks installed` and you never see a window. Leave `EnableHooks=0` (the default).

`DoNotNotifyOnTaskSwitch=1` works without `EnableHooks` even though the upstream comment says otherwise — the wrapper subclasses HP5's window directly, no IAT chain needed.

---

## Building from source

Toolchain: **Visual Studio 2022**, C++. Targets Win32 (the game is 32-bit) and Win64.

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
    build/d3d9-wrapper.sln /p:Configuration=Release /p:Platform=Win32 /v:minimal
```

Output: `data/d3d9.dll` (Win32) or `data/x64/d3d9.dll` (Win64). Drop it into the game folder along with `d3d9.ini`.

No tests, no lint config. CI (`appveyor.yml`) builds Release for both platforms.

---

## Credits

- **Upstream HP5 fix**: [Chip-Biscuit](https://github.com/Chip-Biscuit) — original repository this fork is built on. The `fps.dll` (60/120 FPS, FOV, aspect ratio overrides) remains a Chip-Biscuit project and is distributed via the [upstream releases](https://github.com/Chip-Biscuit/Harry-Potter-and-the-Order-of-the-Phoenix-PC-Fix/releases).
- **Base D3D9 wrapper**: Elisha Riedlinger / "13 AG" — the generic Direct3D 9 proxy this stack ultimately descends from.
- **This fork's enhancements** (MSAA cascade, AF/LOD forcing, mipmap regeneration, FXAA + tunable sharpening, full color grade with white balance / contrast / split toning, SSAO with the INTZ pipeline + pre-UI depth-unbind pass, bloom + god-rays lighting pass with over-bright guard, F12 screenshots, DPI-aware opt-out, frame latency cap, DirectInput8 proxy + Alt+Tab mouse re-Acquire): added in this fork.