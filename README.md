# Slave Zero — Modern Renderer & Launcher

A drop-in replacement renderer and a cross-platform launcher for **Slave Zero** (1999, GOG version).
No game files are included: you need your own copy of the game.

## What it does

- **szmodern.dll** — a Direct3D 11 implementation of the game's `hwAPI` renderer plugin
  interface (the same interface `3dfx.dll` and `d3d.dll` implement). On Linux it runs on Vulkan
  through DXVK/Proton.
  - native resolution (4K and beyond), MSAA, anisotropic filtering, generated mipmaps, supersampling
  - widescreen Hor+ (the engine's view frustum is widened in memory; the HUD stays correct)
  - fixes the crosshair / lock-on drift at high resolutions by keeping the engine on a virtual
    640x480 screen while rasterising at full resolution
  - 32-bit true-colour textures, sharpening, gamma, frame limiter, borderless or windowed
  - screen filters: scanlines, CRT (aperture grille), curved CRT, retro 640x480 pixels, VHS — F9 cycles in game
  - widescreen polish: 4:3 menus/intro, optional HUD at the screen edges
  - optional HD texture pack: the game's own textures upscaled 4x with Real-ESRGAN, plus per-pixel
    lighting for the player mech from the height maps the game ships (its original bump-mapping assets)
  - in-game settings menu: **F10** (configurable) or Back + Start on a gamepad. Mouse, keyboard and
    gamepad navigation; the game is paused and gets no input while it is open. Most settings apply
    live; window mode, aspect ratio and texture options are marked and applied with "Restart game"
- **Launcher** — Windows `.exe` and Linux AppImage. Finds the game (GOG registry, Lutris, Heroic, Steam libraries),
  installs/repairs the renderer, and exposes the settings: resolution, display mode, frame rate,
  anti-aliasing, screen filters, controls and audio, plus Proton/Wine, gamescope, MangoHud and GameMode
  on Linux. Its "Build HD textures" button downloads Real-ESRGAN and generates the texture pack locally.
  With "Start the game directly" it skips its window (start it with `--launcher` to see it again); it
  stays in the background while the game runs to restart the game or reopen when the in-game menu asks.
  On Linux inside gamescope (Steam Game Mode) it tags the game's window with `STEAM_GAME` so gamescope
  shows and focuses it.
- **Steam copy:** the Steam release is the same game (identical executable and data). Set the
  launch options to `"/path/SlaveZero-Launcher-x86_64.AppImage" %command%` (Linux) or
  `"C:\path\SlaveZero-Launcher.exe" %command%` (Windows). On Linux the launcher then runs Steam's own
  command for the Steam copy, so Steam's Proton, prefix, overlay and playtime apply. Steam's install
  script (`gencfg.bat`) writes the data paths in `szOptions.ini` as DOS short paths on a drive letter that
  only exists while the script runs, which is why the Steam copy cannot find its data under Proton; the
  launcher rewrites them to the install's real location on every save.
- A 2-byte compatibility patch for `SlaveZero.exe`: its CPU-speed probe executes `cli`/`sti`, which
  faults on Windows NT and Wine. The launcher applies it and keeps `SlaveZero.exe.original`.

## Building

Requirements: a Linux host with `g++`, SDL3, Python 3, ImageMagick, and
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) plus
[appimagetool](https://github.com/AppImage/appimagetool) under `~/.local/opt/sz-tools/`.

```sh
./build.sh                 # build/szmodern.dll (32-bit Windows DLL)
launcher/build.sh          # launcher/build/SlaveZero-Launcher-x86_64.AppImage and SlaveZero-Launcher.exe
```

`tools/install.sh` / `tools/uninstall.sh` install the renderer into a game folder without the launcher.
`tools/make-texture-pack.py "<game dir>"` builds the HD texture pack from the command line (Pillow, NumPy,
realesrgan-ncnn-vulkan). The pack is derived from the game's art, so it is generated on the user's machine
and never distributed.

## How it works

`docs/hwapi.md` documents the reverse-engineered renderer interface: the render context,
primitive and vertex layouts, texture formats, the HUD's hard-coded 640x480 assumptions, and the
frustum hook used for widescreen. `tools/decompile.sh` regenerates the Ghidra output it is based on.

## Third-party components

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) — `launcher/third_party/imgui`
- [miniz](https://github.com/richgel999/miniz) (MIT) — `launcher/third_party/miniz`
- [stb_image / stb_image_write](https://github.com/nothings/stb) (public domain / MIT) — `src/third_party`, `launcher/third_party/stb`
- [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN) (BSD-3-Clause) — downloaded at runtime, not bundled
- [Orbitron](https://fonts.google.com/specimen/Orbitron) and [Exo 2](https://fonts.google.com/specimen/Exo+2)
  (SIL Open Font License) — `launcher/third_party/fonts`

## License

[MIT](LICENSE) for the code in this repository; third-party components keep their own licenses (above).
Contributions are welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).

Slave Zero is a trademark of its respective owners. This project is not affiliated with them, GOG or Valve.
No game files are included; you need your own copy of the game.
