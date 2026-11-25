// Platform services the launcher core needs. Implemented in platform_win.cpp / platform_linux.cpp.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct DisplayInfo {
    int width = 1920, height = 1080, refresh = 60;
};

// A way to run the Windows game (Linux only: Proton builds or Wine installs).
struct Runner {
    enum Kind { Proton, Wine } kind = Wine;
    std::string name;  // shown in the UI
    fs::path path;     // Proton: tool directory (contains "proton"); Wine: wine binary
};

struct LaunchOptions {
    fs::path gameDir;
    // Linux only
    Runner runner;
    fs::path prefix;
    bool gamescope = false;
    bool mangohud = false;
    bool gamemode = false;
    int outW = 0, outH = 0, refresh = 0;  // for gamescope
    std::string extraEnv;                 // "KEY=value KEY2=value"
};

namespace platform {
const char* Name();
bool IsWindows();
fs::path ConfigDir();                       // directory for launcher.ini (may be the game dir)
fs::path ExecutableDir();
DisplayInfo PrimaryDisplay();
std::vector<fs::path> DetectGameDirs();     // best guesses, most likely first
std::vector<Runner> DetectRunners();        // empty on Windows
fs::path PrefixForGameDir(const fs::path& gameDir);  // Wine prefix containing the game (Linux)
bool Launch(const LaunchOptions& o, std::string& err);
void OpenFolder(const fs::path& p);
bool Download(const std::string& url, const fs::path& dest, std::string& err);
// Runs a program to completion with no console window; returns its exit code (-1 if it could not start).
int RunProcess(const std::vector<std::string>& argv, const fs::path& cwd, std::string& err);
// Shows a folder picker; result is delivered later via the callback on the UI thread's next poll.
void PickFolder(std::function<void(const fs::path&)> done);
void Poll();  // deliver pending async results
// True while another thread is starting a process. The UI thread must not submit GPU work then:
// on Linux the new process closes its inherited DRM descriptor, and amdgpu waits for the GPU queue
// to drain, which never happens while frames keep coming.
bool SpawnInProgress();
}  // namespace platform
