# Contributing

Ideas, bug reports and pull requests are welcome.

## Ground rules

- **No game files.** Never commit anything from Slave Zero itself: executables, DLLs, `Data/*.zip`,
  textures, the generated HD texture pack, or decompiled game code (`ref/` is git-ignored for this
  reason). Everyone needs their own copy of the game (GOG or Steam).
- Keep changes compatible with both the GOG and Steam releases. They ship the same `SlaveZero.exe`,
  and the renderer checks the bytes it hooks before patching anything.
- Linux (Proton/Wine) and Windows both matter. If you could only test one, say so in the pull request.

## Building

See [Building](README.md#building) in the README. In short:

```sh
./build.sh            # renderer: build/szmodern.dll (32-bit, llvm-mingw)
launcher/build.sh     # launchers: AppImage and .exe (embed the renderer built above)
```

## Where things are

| Path | What |
|---|---|
| `src/szmodern.cpp` | The Direct3D 11 renderer (the game's `hwAPI` plugin interface) |
| `src/overlay.cpp` | In-game settings menu (F10), input blocking and pause |
| `src/shaders.h` | Scene shader and post-processing filters |
| `docs/hwapi.md` | Reverse-engineering notes: plugin interface, data layouts, hooked addresses |
| `launcher/src/` | Launcher core (`app.cpp`) and per-platform code (`platform_*.cpp`) |
| `tools/` | Install/uninstall scripts, headless test, texture-pack builder, decompiler export |

Read `docs/hwapi.md` before changing the renderer: most of its behaviour depends on offsets and
addresses recovered from the original renderers and the executable.

## Testing

The renderer can be checked without touching the screen. `tools/headless-test.sh` runs the game in a
headless gamescope session and takes screenshots:

```sh
tools/install.sh                                   # put build/szmodern.dll into the game folder
tools/headless-test.sh "<shot frames>" <exit frame> [timeout] ["frame:vkhex[:hold],..."]
# e.g. reach Mission 1 and screenshot it:
tools/headless-test.sh "1250" 1300 240 "100:1b,300:0d,750:0d,900:0d,1000:25,1100:0d"
```

Environment switches understood by the renderer (tests only): `SZM_SHOTS`, `SZM_EXIT`, `SZM_KEYS`,
`SZM_LOGTEX`, `SZM_TEST_RESTART`, `SZM_TEST_APPLY`. The launcher has `SZL_SHOT`, `SZL_TAB`, `SZL_SCALE`,
`SZL_REPORT` and `SZL_AUTOPLAY`. Tests rewrite `szModern.ini` in the game folder, so back it up first.

`szModern.log` next to the game records what the renderer did; please attach it to bug reports.

## License

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
