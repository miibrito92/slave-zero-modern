// Linux platform layer: finds the game and Proton/Wine, launches through umu-run / proton / wine.
#include <SDL3/SDL.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>

#include "platform.h"
#include "steam.h"

extern char** environ;

static const std::thread::id g_uiThread = std::this_thread::get_id();  // static init runs on the main thread

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

// posix_spawn (clone with CLONE_VM|CLONE_VFORK) instead of fork(): the launcher holds GPU
// mappings and several threads, and a forked copy can stall in the kernel before exec.
std::atomic<int> g_spawning{0};
std::atomic<int> g_games{0};

int Spawn(char* const* argv, char* const* envp, const std::string& dir, bool newSession, bool quiet, pid_t& pid) {
    struct Busy {
        Busy() { ++g_spawning; }
        ~Busy() { --g_spawning; }
    } busy;
    // give the UI thread a moment to finish the frame it may be submitting
    if (std::this_thread::get_id() != g_uiThread) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    if (!dir.empty()) posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (quiet) {
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (newSession) flags |= POSIX_SPAWN_SETSID;
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, flags);
    int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return rc;
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

// Steam Game Mode shows and focuses only X11 windows whose STEAM_GAME property matches the running
// game. Steam's overlay library normally sets it inside the game, but umu removes that library in a
// gamescope session (and only tags windows itself for Flatpak installs), so the game ran unseen and
// without input. While the game runs, tag its windows from here. Xlib is loaded at run time.
static void TagGameWindowsForSteam() {
    unsigned long appId = 0;
    auto num = [](const char* s) -> unsigned long long {
        if (!s || !*s || std::strspn(s, "0123456789") != std::strlen(s)) return 0;
        return std::strtoull(s, nullptr, 10);
    };
    if (unsigned long long a = num(std::getenv("SteamAppId"))) appId = (unsigned long)a;
    else if (unsigned long long g = num(std::getenv("SteamGameId"))) appId = (unsigned long)(g > 0xffffffffull ? g >> 32 : g);
    if (!appId) return;
    void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
#define X11FN(name) auto p##name = reinterpret_cast<decltype(&name)>(dlsym(lib, #name))
    X11FN(XOpenDisplay); X11FN(XCloseDisplay); X11FN(XInternAtom); X11FN(XQueryTree); X11FN(XGetClassHint);
    X11FN(XGetWindowProperty); X11FN(XChangeProperty); X11FN(XFree); X11FN(XFlush);
#undef X11FN
    if (!pXOpenDisplay || !pXCloseDisplay || !pXInternAtom || !pXQueryTree || !pXGetClassHint || !pXGetWindowProperty ||
        !pXChangeProperty || !pXFree || !pXFlush)
        return;
    const char* sgi = std::getenv("SteamGameId");
    const std::string appClass = std::string("steam_app_") + (sgi && *sgi ? sgi : std::to_string(appId));
    std::thread([=] {
        Display* d = pXOpenDisplay(nullptr);
        if (!d) return;
        Atom steamGame = pXInternAtom(d, "STEAM_GAME", False);
        auto isGame = [&](Window w) {
            XClassHint ch{};
            if (!pXGetClassHint(d, w, &ch)) return false;
            std::string s = std::string(ch.res_name ? ch.res_name : "") + " " + (ch.res_class ? ch.res_class : "");
            if (ch.res_name) pXFree(ch.res_name);
            if (ch.res_class) pXFree(ch.res_class);
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            // Proton names the class steam_app_<SteamGameId>; plain Wine uses the exe name
            return s.find("slavezero") != std::string::npos || s.find(appClass) != std::string::npos;
        };
        auto tag = [&](Window w) {
            Atom type = 0;
            int fmt = 0;
            unsigned long n = 0, after = 0;
            unsigned char* data = nullptr;
            bool has = pXGetWindowProperty(d, w, steamGame, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success &&
                       type == XA_CARDINAL && n == 1 && data && *reinterpret_cast<unsigned long*>(data) == appId;
            if (data) pXFree(data);
            if (has) return;
            unsigned long v = appId;
            pXChangeProperty(d, w, steamGame, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&v), 1);
        };
        std::function<void(Window, int)> walk = [&](Window w, int depth) {
            Window root = 0, parent = 0, *kids = nullptr;
            unsigned int nk = 0;
            if (!pXQueryTree(d, w, &root, &parent, &kids, &nk)) return;
            for (unsigned int i = 0; i < nk; ++i) {
                if (isGame(kids[i])) tag(kids[i]);
                else if (depth < 2) walk(kids[i], depth + 1);  // window managers may reparent
            }
            if (kids) pXFree(kids);
        };
        Window root = DefaultRootWindow(d);
        // poll while the game runs (plus 30 s, in case the first process hands off to another)
        for (int idle = 0; idle < 60; ++idle) {
            if (g_games > 0) idle = 0;
            walk(root, 0);
            pXFlush(d);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        pXCloseDisplay(d);
    }).detach();
}

namespace platform {

const char* Name() { return "Linux"; }
bool IsWindows() { return false; }
bool InsideGamescope() {
    const char* gs = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    const char* de = std::getenv("XDG_CURRENT_DESKTOP");
    return (gs && *gs) || (de && std::strcmp(de, "gamescope") == 0);
}

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
    // Steam libraries (native and Flatpak Steam)
    for (const fs::path& lib : SteamLibraries({Home() / ".local/share/Steam", Home() / ".steam/steam",
                                               Home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                                               Home() / ".var/app/com.valvesoftware.Steam/data/Steam"}))
        add(lib / "steamapps/common/Slave Zero");
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

bool IsSteamInstall(const fs::path& gameDir) { return PathIsSteamInstall(gameDir); }

fs::path PrefixForGameDir(const fs::path& gameDir) {
    // Steam copy: Steam's own prefix for the game, <library>/steamapps/compatdata/<appid>/pfx
    if (IsSteamInstall(gameDir)) return gameDir.lexically_normal().parent_path().parent_path() / "compatdata" / kSteamAppId / "pfx";
    for (fs::path p = gameDir; p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
        if (p.filename() == "drive_c") return p.parent_path();
    return {};
}

bool Launch(const LaunchOptions& o, std::string& err) {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    fs::path exe = o.gameDir / "SlaveZero.exe";
    bool viaSteam = !o.steamCommand.empty();
    if (o.prefix.empty() && !viaSteam) { err = "the game folder is not inside a Wine prefix"; return false; }

    if (o.gamescope && !InsideGamescope()) {
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
    env.push_back({"SZM_LAUNCHER", "1"});  // the in-game menu can ask us to restart the game

    if (viaSteam) {
        // Steam's own command (its runtime, Proton and prefix); Steam already set up the environment
        argv.insert(argv.end(), o.steamCommand.begin(), o.steamCommand.end());
    } else if (o.runner.kind == Runner::Proton) {
        fs::path umu = FindInPath("umu-run");
        if (!umu.empty()) {
            env.push_back({"PROTONPATH", o.runner.path.string()});
            env.push_back({"WINEPREFIX", o.prefix.string()});
            // Under Steam, keep the game's Steam ID: umu copies GAMEID into SteamGameId, which Proton
            // uses to tag the game's windows. Steam Game Mode only shows windows tagged with the
            // running game's ID, so "umu-0" leaves the game invisible behind the launcher.
            std::string gameId = "umu-0";
            if (const char* sgi = std::getenv("SteamGameId"); sgi && *sgi && std::strspn(sgi, "0123456789") == std::strlen(sgi) &&
                                                               std::strcmp(sgi, "0") != 0)
                gameId = std::string("umu-") + sgi;
            env.push_back({"GAMEID", gameId});
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
    if (!viaSteam) argv.push_back(exe.string());
    {
        std::stringstream ss(o.extraEnv);
        std::string kv;
        while (ss >> kv) {
            auto eq = kv.find('=');
            if (eq != std::string::npos && eq > 0) env.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
    }

    // build argv/envp before forking: the launcher is multithreaded, so the child may only
    // call async-signal-safe functions until exec
    const char* appdir = std::getenv("APPDIR");
    std::vector<std::string> envs;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        std::string key = kv.substr(0, kv.find('='));
        bool overridden = false;
        for (auto& p : env) overridden |= p.first == key;
        if (overridden) continue;
        if (key == "LD_LIBRARY_PATH" && appdir && kv.find(appdir) != std::string::npos) continue;  // AppImage libs
        envs.push_back(kv);
    }
    for (auto& [k, v] : env) envs.push_back(k + "=" + v);
    std::vector<char*> a, ev;
    for (auto& s2 : argv) a.push_back(const_cast<char*>(s2.c_str()));
    a.push_back(nullptr);
    for (auto& s2 : envs) ev.push_back(const_cast<char*>(s2.c_str()));
    ev.push_back(nullptr);
    std::string dir = o.gameDir.string();

    pid_t pid = 0;
    int rc = Spawn(a.data(), ev.data(), dir, true, false, pid);
    if (rc != 0) { err = std::strerror(rc); return false; }
    ++g_games;
    std::thread([pid] {  // reap when the game exits
        waitpid(pid, nullptr, 0);
        --g_games;
    }).detach();
    if (InsideGamescope()) TagGameWindowsForSteam();
    return true;
}

int RunProcess(const std::vector<std::string>& argv, const fs::path& cwd, std::string& err) {
    std::vector<char*> a;
    for (auto& s2 : argv) a.push_back(const_cast<char*>(s2.c_str()));
    a.push_back(nullptr);
    pid_t pid = 0;
    int rc = Spawn(a.data(), environ, cwd.string(), false, true, pid);
    if (rc != 0) { err = "could not run " + argv[0] + ": " + std::strerror(rc); return -1; }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { err = std::strerror(errno); return -1; }
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) == 127) err = "could not run " + argv[0];
        return WEXITSTATUS(status);
    }
    err = "terminated by a signal";
    return -1;
}

bool Download(const std::string& url, const fs::path& dest, std::string& err) {
    int rc = RunProcess({"curl", "-fsSL", "--retry", "2", "-o", dest.string(), url}, {}, err);
    if (rc != 0 && err.empty()) err = "download failed (curl exit " + std::to_string(rc) + ")";
    return rc == 0;
}

void OpenFolder(const fs::path& p) {
    std::string path = p.string();
    char* a[] = {const_cast<char*>("xdg-open"), const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (Spawn(a, environ, {}, true, true, pid) == 0) std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}

void PickFolder(std::function<void(const fs::path&)> done) {
    g_pickDone = std::move(done);
    SDL_ShowOpenFolderDialog(OnFolderPicked, nullptr, g_window, Home().c_str(), false);
}

bool SpawnInProgress() { return g_spawning > 0; }
bool GameRunning() { return g_games > 0; }

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
