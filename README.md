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

This fork extends the wrapper with several visual upgrades configured in `d3d9.ini`. All defaults are tuned to give a clear quality bump out of the box without manual tuning.

## `[GRAPHICS]` section

- **`FXAA`** (default `1`) — post-process anti-aliasing combined with adaptive sharpening. FXAA smooths jagged edges, the sharpening pass recovers texture detail. Near-zero performance cost.
- **`AnisotropicFiltering`** (default `16`) — forces full trilinear + anisotropic filtering on every texture sampler regardless of the game's choice. Fixes streaky/blurry ground textures at oblique angles. Free on any modern GPU.
- **`TextureLODBias`** (default `-1.0`) — biases mip selection toward sharper levels. Combined with the wrapper's automatic mipmap regeneration (the game ships textures without mip chains), this fixes blurry faces and surfaces at medium distance. Going below `-1.5` may cause shimmer at very long range.
- **`SSAAFactor`** (default `1`) — set to `2`, `3` or `4` to render at NxN the back buffer resolution then downsample with bilinear filtering at present time. The strongest AA option (true supersampling), eliminates virtually all aliasing including thin sub-pixel features. Very expensive: `2x` quadruples GPU load. Disables MSAA when active. Recommended only with a high-end GPU.
- **`Antialiasing`** (default `0`) — MSAA on the back buffer (2/4/8/16x). Off by default because MSAA can interact poorly with the game's intermediate render targets. Prefer FXAA or SSAA.
- **`VSync`** (default `0`) — caps frame rate to your monitor's refresh rate.

## `[RESOLUTION]` section

- **`Width` / `Height`** (default `0`) — `0` means use the game's default resolution (typically your screen's native). Override only if you specifically want a different render resolution. With borderless fullscreen the result is stretched to your screen, so picking a value lower than your screen's native produces a blurry upscale.

## `[FORCEWINDOWED]` section

- **`FreeMouse`** (default `1`) — neutralizes the game's `ClipCursor` and `SetCapture` calls so the cursor can leave the window freely. Enables Alt+Tab, multi-monitor cursor movement, screenshot tools, and the Windows key. No impact on gameplay.

## Mipmap regeneration

The game ships character and surface textures without mip chains, which causes severe blur and shimmer at distance regardless of LOD bias. The wrapper detects single-mip textures at creation time and regenerates a full mip chain via `D3DXFilterTexture(TRIANGLE | DITHER | SRGB)` after the first upload. This is automatic and has no ini option — it always runs.

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
