// Renderer settings (szModern.ini), shared by the renderer and the in-game overlay.
#pragma once
#include <vector>

struct Config {
    int windowed = 0;           // 0 = borderless fullscreen on the monitor, 1 = normal window
    int width = 0, height = 0;  // window client size when windowed (0 = 1920x1080)
    int vsync = 1;
    int widescreen = 1;  // hor+ widescreen (patches the engine's frustum width in memory)
    int hudEdges = 0;    // widescreen: move corner HUD elements to the screen edges
    int menuPillarbox = 1;  // widescreen: show menus/intro as 4:3
    int trueColor = 1;   // ask the engine for 32-bit source textures
    int texturePack = 1; // load szmodern/textures/<name>.png replacements when present
    int normalMaps = 1;  // per-pixel lighting where the pack has <name>_n.png
    int frameLimit = 0;
    int msaa = 4;
    float renderScale = 1.0f;
    int anisotropy = 16;
    int generateMips = 1;
    float lightmapScale = 2.0f;
    int opaqueCutout = 1;
    float sharpen = 0.3f;
    int filter = 0;            // 0 none, 1 scanlines, 2 CRT, 3 CRT curved, 4 retro pixels, 5 VHS
    float filterStrength = 0.7f;
    int scanlines = 480;
    float gammaOverride = 0.0f;
    float decalBias = 1.0f / 4096.0f;
    int log = 1;
    // in-game overlay
    int overlayKey = 0x79;     // virtual key that opens the overlay (F10)
    int overlayHint = 1;       // show "F10: settings" for a few seconds at startup
    int pauseInOverlay = 1;    // freeze the game while the overlay is open
    int startDirectly = 0;     // [Launcher] StartDirectly: the launcher skips its window
    // test automation (environment): screenshot frames, exit frame
    std::vector<int> shotFrames;
    int exitFrame = 0;
    struct Key { int frame; int vk; int hold; };
    std::vector<Key> keys;  // SZM_KEYS="frame:vk[:hold],..." (vk hex; 01/02 = mouse buttons), tests only
};

extern Config g_cfg;
extern char g_gameDir[260];
extern char g_dllPath[260];
void Log(const char* fmt, ...);
void SaveConfig();  // writes the user-facing settings back to szModern.ini
