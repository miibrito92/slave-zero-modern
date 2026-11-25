// Launcher core: settings model, install/repair, launch, and the ImGui interface.
#pragma once
#include <string>
#include <vector>

#include "packbuilder.h"
#include "platform.h"

struct ImFont;

struct Settings {
    // renderer (szModern.ini)
    int renderer = 0;  // 0 modern (szmodern.dll), 1 classic Glide (nGlide)
    bool windowed = false;
    int winW = 1920, winH = 1080;
    float renderScale = 1.0f;
    bool widescreen = true;
    bool hudEdges = false;
    bool menus4x3 = true;
    bool vsync = true;
    int frameLimit = 0;
    int msaa = 4;
    int aniso = 16;
    bool mips = true;
    bool trueColor = true;
    bool texturePack = true;
    bool normalMaps = true;
    float lightmapScale = 2.0f;
    bool cutout = true;
    float sharpen = 0.3f;
    int filter = 0;  // 0 off, 1 scanlines, 2 CRT, 3 CRT curved, 4 retro pixels, 5 VHS
    float filterStrength = 0.7f;
    int scanlines = 480;
    // game (szOptions.ini)
    float gamma = 1.866667f;
    float mouseSens = 1.0f;
    bool invertMouse = false;
    bool crosshair = true;
    bool audioOn = true;
    bool musicOn = true;
    float musicVol = 1.0f;
    float sfxVol = 1.0f;
    bool eax = false;
    bool forceFeedback = false;
    // launcher (launcher.ini)
    int runnerIndex = 0;
    bool gamescope = false;
    bool mangohud = false;
    bool gamemode = false;
    bool closeOnPlay = true;   // close the launcher when the game exits (else show it again)
    bool startDirectly = false;  // szModern.ini [Launcher] StartDirectly: skip this window
    std::string extraEnv;
};

class App {
public:
    void Init();
    // Runs every loop iteration, visible or not: starts the game when skipping the window and
    // handles the game exiting. Returns false when the launcher should exit.
    bool Tick();
    // Draws the whole UI. Returns false when the launcher should exit.
    bool Frame();
    // The window stays hidden while the game runs, and when starting the game directly.
    bool WindowHidden() const;
    void ForceWindow() { forceWindow_ = true; }
    // Linux: the arguments Steam passed (%command% in the launch options). Kept only if they start
    // Slave Zero; used when the selected game folder is that Steam copy.
    void SetSteamCommand(const std::vector<std::string>& args);
    void SetFonts(ImFont* body, ImFont* title, ImFont* big) { body_ = body; title_ = title; big_ = big; }

private:
    enum class Status { Ok, Missing, Outdated };
    void SetGameDir(const fs::path& dir);
    void LoadSettings();
    bool SaveSettings();
    void RefreshStatus();
    bool Install(std::string& msg);
    void Play();
    void Notify(const std::string& msg, bool error = false);
    void OnGameExited();

    void DrawHeader();
    void DrawGameFolder();
    void DrawDisplayTab();
    void DrawGraphicsTab();
    void DrawGameTab();
    void DrawLinuxTab();
    void DrawFooter();

    fs::path gameDir_;
    bool gameValid_ = false;
    Status rendererStatus_ = Status::Missing;
    Status exeStatus_ = Status::Missing;
    Settings s_;
    DisplayInfo display_;
    std::vector<Runner> runners_;
    fs::path prefix_;
    std::string runnerSaved_;
    std::string message_;
    bool messageError_ = false;
    double messageTime_ = 0;
    bool quit_ = false;
    bool dirty_ = false;
    bool forceWindow_ = false;   // --launcher on the command line
    std::vector<std::string> steamCmd_;
    fs::path steamCmdDir_;       // game folder the Steam command starts
    std::vector<fs::path> installs_;  // detected copies of the game
    bool UsesSteamCommand() const;
    bool hiddenStart_ = false;   // starting the game directly; window not shown yet
    bool pendingPlay_ = false;
    bool gameWasRunning_ = false;
    char pathBuf_[1024] = {};
    ImFont* body_ = nullptr;
    ImFont* title_ = nullptr;
    ImFont* big_ = nullptr;
    PackBuilder pack_;
};
