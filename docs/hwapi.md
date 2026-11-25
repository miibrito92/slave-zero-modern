# Slave Zero `hwAPI` renderer interface — reverse-engineering notes

Source: Ghidra 12.2 decompilation of the shipped `3dfx.dll` (Glide3) and `d3d.dll`
(DirectDraw/D3D6), plus `SlaveZero.exe` and `Scripts/wHud.dll`. Decompiled output
lives in `ref/decomp/` (git-ignored; regenerate with `tools/decompile.sh`).

## How the engine loads a renderer

`SlaveZero.exe` `LoadLibrary`s the DLL named by `Renderer DLL=` in `szOptions.ini`
and resolves 33 `hwAPI_*` exports with `GetProcAddress` into a function table that
lives **inside the render-context struct** (`rc`). All exports are `__cdecl`, and
`rc` is always the first argument.

| rc slot | export |
|---|---|
| `+0x1e0` | `hwAPI_BlitPoly` (2D/HUD quads) |
| `+0x264` | `hwAPI_cam2ScreenCoord` |
| `+0x1e8`…`+0x264` | remaining hwAPI pointers (see the exe `GetProcAddress` chain near decomp line 24900) |

## Render context `rc` (engine-owned, passed to every call)

| offset | meaning |
|---|---|
| `+0x08` | → perspective / viewport struct `persp` |
| `+0x0c` | → renderer-private state (we own this; allocated in `StartUp`) |
| `+0x20`, `+0x24` | float viewport offset x, y (pixels) |
| `+0x58` | shadow-map/lightmap draw mode (0/1/3) |
| `+0x60` | debug-stats overlay flag |
| `+0x64` | float gamma |
| `+0x70` | → options struct `opt` |
| `+0x74` | "same frame" flag used by `ClearViewSurface` |
| `+0x84` | 256-byte scratch string buffer |
| `+0x214` | callback `void* Alloc(size)` |
| `+0x21c` | callback `Free(ptr)` |
| `+0x22c` | callback `Error(const char* msg, int fatal)` |
| `+0x234` | callback `Log(fmt, ...)` (writes `szDebug.txt`) |
| `+0x268` | callback: resolve animated-texture frame index |
| `+0x27c` | callback: `scrn_printf`-style on-screen text |

### `persp` (`rc+8`)
`+0x00` int width, `+0x04` int height (what `GetScreenDimensions` returns to scripts),
`+0x08` float width, `+0x0c` float height, `+0x20` float scaleX, `+0x24` float scaleY,
`+0x2c` float centerX, `+0x30` float centerY.

### `opt` (`rc+0x70`)
`+0x1030/+0x1034` requested width/height, `+0x1154` multitexture/lightmaps enabled,
`+0x1160` wait-for-vsync, `+0x1174` use hardware gamma ramp, `+0x1180` float gamma,
`+0x11b0` fog enabled.

## Projection is done by the renderer, not the engine

Engine vertices arrive in **camera space**. The renderer projects them:

```
sx = oow * x * persp.scaleX + persp.centerX + rc.offX
sy = persp.height - (oow * y * persp.scaleY + persp.centerY) + rc.offY
```

Engine vertex record (pointed to by an array of pointers in each primitive):
`+0x14` x, `+0x18` y (camera space), `+0x20/+0x24` u,v (texture 0),
`+0x28/+0x2c` u,v (lightmap), `+0x30/+0x34/+0x38` r,g,b in 0..1, `+0x4c` oow (=1/z).

`hwAPI_cam2ScreenCoord(rc, camPos[3], *outX, *outY)` uses the same formula with
`oow = 1/camPos.z`, minus the viewport offset. The exe calls it from
`XformWorld2ScreenCoord` (scripts' lock-on / target boxes) and the lens-flare code.

**Consequence:** a new renderer fully controls the 3D projection. The output
resolution, aspect and FOV can be chosen independently of what the engine thinks.

## Primitives — `hwAPI_ProcessBatchOfPrimatives(rc, prim, ?, flush)`

`prim` header: `+0x02` u16 byte size, `+0x04` u16 first-vertex index, `+0x06` u16
vertex count (convex polygon, fan-triangulated), `+0x08` u32 flags, `+0x18` →
texture, `+0x1c` → lightmap ("shadow map"), `+0x24` → anim-frame index,
`+0x30` → vertex pointer array.

Flags (`prim+8`) select the pipeline:

| bit | type | Glide combine |
|---|---|---|
| `0x40000000` | 1: texture only (fullbright) | `grColorCombine(3,8,1,1,0)` |
| `0x20000000` | 3: texture × vertex colour | `grColorCombine(3,1,0,1,0)` |
| `0x80000000` | 2: untextured, vertex colour | `grColorCombine(1,0,0,2,0)` |
| `0x10000000` | 4: texture × lightmap (2 TMUs or 2 passes) | `grTexCombine` |
| `0x00e00000` | any set → translucent (Z-sorted, drawn at `EndFrame`, no Z write) |
| `0x00800000` | constant alpha in the low byte |
| `0x00400000` / `0x00200000` | texture alpha / additive variants |
| `0x0f000000` >> 24 | pass sub-mode used by the lightmap path (1, 2, 3) |

Opaque polygons are binned into 4 per-type buffers of 3000 bytes and flushed when
full or when `flush` = 1 or 2. Translucent polygons go into a binary tree keyed on
average depth and are drawn back-to-front in `hwAPI_EndFrame`. 2D quads from
`BlitPoly` go into a second tree (keyed on depth) drawn after.

Depth: W-buffer (`grDepthBufferMode(2)`), compare LEQUAL. Sky: `DrawSkyBegin` sets
compare ALWAYS and turns fog off; `DrawSkyEnd` restores.
Fog: linear table fog on W (`hwAPI_BuildFogTable(rc, ?, near, far, disable)`), colour
from the private struct.

## Textures

`hwAPI_AllocInitTexturePLS` / `InitTexturePLS` build a per-mip "PLS" record
(`SizeofPLS` → 0x48 bytes Glide, 0x54 bytes D3D). Glide accepts only 16-bit formats:
RGB565 (default), ARGB1555 (`tex.flags & 0x80`), ARGB4444 (`& 0x100`).
Texture header: `+0x24` width, `+0x28` height, `+0x2c` flags, `+0x30` → pixel data
(mips contiguous), `+0x34` → PLS array, `+0x3c` u16 frame count (animated textures).

The D3D renderer additionally reports, at runtime:
`CanUseTrueColor` (32-bit textures), `CanBumpmap` / `GetBumpmappingStyle`, and
`CanUseTextureCompression` (DXT). The Glide renderer returns 0 for all of them;
nGlide inherits that limit. `textures.zip` ships `*_bump*.tex` maps.

## The crosshair / lock-on bug (root cause)

`Scripts/wHud.dll` hard-codes a 640x480 screen:

- The resting reticle position is `(320 - w/2, 240 - h/2)` in *pixels*
  (constants at VA `0x100060d4` / `0x100060d8`), later divided by the *real*
  screen size from `GetScreenDimensions`. At any resolution other than 640x480 the
  reticle lands off-centre.
- The eight lock-on marker slots are initialised to `(320, 240)`.
- Other HUD scale factors use `1/640` and `1/480` (`0x10006080`, `0x10006084`,
  `0x100060a8`, `0x100060d0`).

**Fix strategy:** report a virtual 640x480 to the engine (`persp` width/height,
`cam2ScreenCoord`) and scale to the real swap-chain size inside the renderer, at
full float precision. Every hard-coded HUD assumption then holds, and 3D is still
rasterised natively at 4K.

## Findings from building szmodern.dll

- Neither shipped renderer ever enables hardware fog (Glide only calls `grFogMode(0)`,
  D3D sets `FOGENABLE=0`), so fog is not a renderer concern.
- Lightmaps are ARGB1555. The D3D renderer blends them `DESTCOLOR, SRCCOLOR` (2x modulate);
  Glide with two TMUs multiplies 1x. `LightmapScale` in szModern.ini picks.
- Vertex `+0x1c` is camera-space Z (the D3D renderer derives its depth from it).
- `hwAPI_EnumerateDisplayModes(mode records[], int* count)` appends 0x310-byte records. The
  exe loads `<game dir>\<Renderer Name>` of the selected `szHardware.ini` entry.
- `hwAPI_BlitPoly(rc, tex, frameIdx*, x, y, depth, w, h, alpha, u0, v0, u1, v1, lit, r, g, b)`;
  height is scaled by 4/3, and texture V is flipped (top edge uses v1).
- The exe's CPU-speed probe (VA `0x41d849`) wraps `rdtsc` in `cli`/`sti`, which raises a
  privileged-instruction fault on NT and Wine; `tools/install.sh` NOPs those two bytes.

## Widescreen (Hor+)

`FUN_0040c750` builds the perspective from the display size: focal = max(w,h)/2 / tan(45°),
centre = w/2, h/2, and the frustum terms `+0x34/+0x38` (focal / half-width) and
`+0x44/+0x48` (half-width / focal). `FUN_0040cb70` then builds the frustum planes from them,
and the world clippers (`FUN_00410ba0`, `FUN_00411f60`, `FUN_00415960`) use both pairs.
szmodern.dll redirects the four `call 0x40c750` sites (0x40c947, 0x435283, 0x438078,
0x43939f) in memory to a wrapper that runs the original and then widens those four terms by
k = (aspect / (4/3)). Focal length and centre stay unchanged, so the engine still believes in a
640x480 screen (HUD, crosshair, `cam2ScreenCoord`), and the renderer maps that 640x480 to the
centre 4:3 of a wider scene. Full-screen blits (x≈0, w≈1) are stretched to the whole width.

## True colour

With `CanUseTrueColor` = 1 and `Use True Color Textures=1`, the engine skips its 32→16-bit
conversion (`FUN_00438740`): textures whose file flags have `0x200` stay 32-bit `0xAARRGGBB`.
~330 of 1415 shipped textures are 32-bit (mostly effects with smooth alpha); the rest were
authored as ARGB1555. Bump maps exist only for the player mech (`a_Slave0_live_*_bump1..4`).

## Frame timer (pause)

`FUN_0042d310` is the engine's per-frame timer, called once per frame from `0x40a3e8`. Game time
is counted in 1/120 s ticks (`FUN_0042d290` returns accumulated QPC seconds × 120). In real-time
mode (`[0x4dbd00] == 0`) it busy-waits until at least one tick has passed (`[0x4457d0]` = 1.0, which
is why the game never exceeds 120 fps), clamps the step to `[0x4457d8]` = 120 ticks, and stores:

| Address    | Meaning                         |
|------------|---------------------------------|
| `0x4e84b0` | game time (float, ticks)        |
| `0x4e8490` | previous frame's game time      |
| `0x4e8494` | this frame's step (float, ticks)|

The overlay redirects the call. While paused it sets the step to 0 and moves both times to the
current clock, so the game keeps rendering without advancing, and resumes without a jump. Freezing
the clock functions instead does not work: the busy-wait never ends and the game stops rendering.

## Input

Keyboard and mouse come from `Scripts/wInputKBM.dll` (DirectInput plus `GetAsyncKeyState`,
`GetKeyState`, `GetCursorPos`, `SetCursorPos`). The overlay blocks them by patching those imports in
the game's modules and the `Acquire` / `GetDeviceState` / `GetDeviceData` slots of dinput's device
vtable (shared by all devices, so hooking a throwaway device covers the game's).
