// Linux platform layer: finds the game and Proton/Wine, launches through umu-run / proton / wine.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

#include "platform.h"

extern SDL_Window* g_window;

namespace {

fs::path Home() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

fs::path FindInPath(const char* exe) {
    const char* p = std::getenv("PATH");
    if (!p) return {};
    std::stringstream ss(p);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        fs::path c = fs::path(dir) / exe;
        if (::access(c.c_str(), X_OK) == 0) return c;
    }
    return {};
}

bool IsGameDir(const fs::path& d) {
    std::error_code ec;
    return fs::exists(d / "SlaveZero.exe", ec);
}

// Natural-ish sort key so GE-Proton10-9 < GE-Proton10-10.
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) ++j2;
            long x = std::stol(a.substr(i, i2 - i)), y = std::stol(b.substr(j, j2 - j));
            if (x != y) return x < y;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() < b.size();
}

std::mutex g_pickMutex;
bool g_pickReady = false;
fs::path g_pickResult;
std::function<void(const fs::path&)> g_pickDone;

void SDLCALL OnFolderPicked(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_pickMutex);
    if (files && files[0]) {
        g_pickResult = fs::u8path(files[0]);
        g_pickReady = true;
    }
}

}  // namespace

namespace platform {

const char* Name() { return "Linux"; }
bool IsWindows() { return false; }

fs::path ConfigDir() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    return (x && *x ? fs::path(x) : Home() / ".config") / "slavezero-launcher";
}

fs::path ExecutableDir() {
    if (const char* ai = std::getenv("APPIMAGE")) return fs::path(ai).parent_path();
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
}

DisplayInfo PrimaryDisplay() {
    DisplayInfo d;
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id)) {
        float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
        d.width = (int)std::lround(m->w * density);
        d.height = (int)std::lround(m->h * density);
        d.refresh = (int)std::lround(m->refresh_rate);
    }
    return d;
}

std::vector<fs::path> DetectGameDirs() {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        if (IsGameDir(d) && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };
    std::error_code ec;
    // Lutris game configs ("exe: /path/SlaveZero.exe")
    for (const fs::path& dir : {Home() / ".local/share/lutris/games", Home() / ".config/lutris/games"}) {
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::ifstream f(e.path());
            std::string line;
            while (std::getline(f, line)) {
                auto p = line.find("SlaveZero.exe");
                auto s = line.find('/');
                if (p != std::string::npos && s != std::string::npos && s < p)
                    add(fs::path(line.substr(s, p - s)));
            }
        }
    }
    // Heroic GOG installs
    {
        std::ifstream f(Home() / ".config/heroic/gog_store/installed.json");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::regex re("\"install_path\"\\s*:\\s*\"([^\"]*Slave[^\"]*)\"", std::regex::icase);
        for (std::sregex_iterator it(all.begin(), all.end(), re), end; it != end; ++it) add(fs::path((*it)[1].str()));
    }
    // shallow scan of usual game locations
    for (const fs::path& root : {Home() / "Games", Home() / ".wine/drive_c", Home() / "GOG Games"}) {
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) {
                std::string n = it->path().filename().string();
                if (n == "windows" || n == "dosdevices" || n == "Program Files" || n == "Program Files (x86)" ||
                    n == "users" || n == "shadercache")
                    it.disable_recursion_pending();
                continue;
            }
            if (it->path().filename() == "SlaveZero.exe") add(it->path().parent_path());
        }
    }
    return out;
}

std::vector<Runner> DetectRunners() {
    std::vector<Runner> protons, wines;
    std::error_code ec;
    for (const fs::path& base : {Home() / ".local/share/Steam/compatibilitytools.d", Home() / ".steam/root/compatibilitytools.d",
                                 Home() / ".var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d",
                                 Home() / ".local/share/Steam/steamapps/common"}) {
        if (!fs::is_directory(base, ec)) continue;
        for (auto& e : fs::directory_iterator(base, ec)) {
            if (!fs::exists(e.path() / "proton", ec)) continue;
            fs::path real = fs::weakly_canonical(e.path(), ec);
            bool dup = false;
            for (auto& r : protons) dup |= fs::weakly_canonical(r.path, ec) == real;
            if (!dup) protons.push_back({Runner::Proton, e.path().filename().string(), e.path()});
        }
    }
    std::sort(protons.begin(), protons.end(), [](const Runner& a, const Runner& b) {
        bool ga = a.name.rfind("GE-Proton", 0) == 0, gb = b.name.rfind("GE-Proton", 0) == 0;
        if (ga != gb) return ga;
        return NaturalLess(b.name, a.name);  // newest first
    });
    fs::path lutrisWine = Home() / ".local/share/lutris/runners/wine";
    if (fs::is_directory(lutrisWine, ec))
        for (auto& e : fs::directory_iterator(lutrisWine, ec))
            if (fs::exists(e.path() / "bin/wine", ec))
                wines.push_back({Runner::Wine, "Wine (Lutris) " + e.path().filename().string(), e.path() / "bin/wine"});
    if (fs::path w = FindInPath("wine"); !w.empty()) wines.push_back({Runner::Wine, "Wine (system)", w});
    protons.insert(protons.end(), wines.begin(), wines.end());
    return protons;
}

fs::path PrefixForGameDir(const fs::path& gameDir) {
    for (fs::path p = gameDir; p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
        if (p.filename() == "drive_c") return p.parent_path();
    return {};
}

bool Launch(const LaunchOptions& o, std::string& err) {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    fs::path exe = o.gameDir / "SlaveZero.exe";
    if (o.prefix.empty()) { err = "the game folder is not inside a Wine prefix"; return false; }

    if (o.gamescope) {
        fs::path gs = FindInPath("gamescope");
        if (gs.empty()) { err = "gamescope is not installed"; return false; }
        argv = {gs.string(), "-W", std::to_string(o.outW), "-H", std::to_string(o.outH), "-w", std::to_string(o.outW),
                "-h", std::to_string(o.outH), "-f"};
        if (o.refresh > 0) { argv.push_back("-r"); argv.push_back(std::to_string(o.refresh)); }
        argv.push_back("--");
    }
    if (o.gamemode) {
        fs::path gm = FindInPath("gamemoderun");
        if (!gm.empty()) argv.push_back(gm.string());
    }
    if (o.mangohud) env.push_back({"MANGOHUD", "1"});

    if (o.runner.kind == Runner::Proton) {
        fs::path umu = FindInPath("umu-run");
        if (!umu.empty()) {
            env.push_back({"PROTONPATH", o.runner.path.string()});
            env.push_back({"WINEPREFIX", o.prefix.string()});
            env.push_back({"GAMEID", "umu-0"});
            env.push_back({"UMU_RUNTIME_UPDATE", "0"});
            argv.push_back(umu.string());
        } else {
            // plain Proton wants STEAM_COMPAT_DATA_PATH to contain "pfx"
            fs::path compat = o.prefix.filename() == "pfx" ? o.prefix.parent_path() : o.prefix;
            std::error_code ec;
            if (!fs::exists(compat / "pfx", ec)) { err = "install umu-launcher to run this prefix with Proton"; return false; }
            env.push_back({"STEAM_COMPAT_DATA_PATH", compat.string()});
            env.push_back({"STEAM_COMPAT_CLIENT_INSTALL_PATH", (Home() / ".local/share/Steam").string()});
            argv.push_back((o.runner.path / "proton").string());
            argv.push_back("waitforexitandrun");
        }
    } else {
        env.push_back({"WINEPREFIX", o.prefix.string()});
        argv.push_back(o.runner.path.string());
    }
    argv.push_back(exe.string());
    {
        std::stringstream ss(o.extraEnv);
        std::string kv;
        while (ss >> kv) {
            auto eq = kv.find('=');
            if (eq != std::string::npos && eq > 0) env.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
    }

    pid_t pid = fork();
    if (pid < 0) { err = std::strerror(errno); return false; }
    if (pid == 0) {
        setsid();
        if (chdir(o.gameDir.c_str()) != 0) _exit(127);
        // don't leak the AppImage's private libraries into the game
        if (const char* appdir = std::getenv("APPDIR")) {
            const char* ld = std::getenv("LD_LIBRARY_PATH");
            if (ld && std::strstr(ld, appdir)) unsetenv("LD_LIBRARY_PATH");
        }
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); }
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execv(a[0], a.data());
        _exit(127);
    }
    // reap in the background so it never becomes a zombie
    signal(SIGCHLD, SIG_IGN);
    return true;
}

void OpenFolder(const fs::path& p) {
    if (fork() == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", p.c_str(), (char*)nullptr);
        _exit(127);
    }
}

void PickFolder(std::function<void(const fs::path&)> done) {
    g_pickDone = std::move(done);
    SDL_ShowOpenFolderDialog(OnFolderPicked, nullptr, g_window, Home().c_str(), false);
}

void Poll() {
    fs::path picked;
    {
        std::lock_guard<std::mutex> lock(g_pickMutex);
        if (!g_pickReady) return;
        g_pickReady = false;
        picked = g_pickResult;
    }
    if (g_pickDone) g_pickDone(picked);
}

}  // namespace platform
