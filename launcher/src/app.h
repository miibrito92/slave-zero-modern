// Launcher core: settings model, install/repair, launch, and the ImGui interface.
#pragma once
#include <string>
#include <vector>

#include "platform.h"

struct ImFont;

struct Settings {
    // renderer (szModern.ini)
    int renderer = 0;  // 0 modern (szmodern.dll), 1 classic Glide (nGlide)
    bool windowed = false;
    int winW = 1920, winH = 1080;
    float renderScale = 1.0f;
    bool widescreen = true;
    bool vsync = true;
    int frameLimit = 0;
    int msaa = 4;
    int aniso = 16;
    bool mips = true;
    bool trueColor = true;
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
    bool closeOnPlay = true;
    std::string extraEnv;
};

class App {
public:
    void Init();
    // Draws the whole UI. Returns false when the launcher should exit.
    bool Frame();
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
    char pathBuf_[1024] = {};
    ImFont* body_ = nullptr;
    ImFont* title_ = nullptr;
    ImFont* big_ = nullptr;
};
