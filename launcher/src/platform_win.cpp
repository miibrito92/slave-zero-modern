// Windows platform layer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>

#include <atomic>
#include <thread>

#include "platform.h"

static std::atomic<int> g_games{0};

namespace {

fs::path RegPath(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return fs::path(buf);
    return {};
}

}  // namespace

namespace platform {

const char* Name() { return "Windows"; }
bool IsWindows() { return true; }
bool InsideGamescope() { return false; }
bool GameRunning() { return g_games > 0; }

fs::path ConfigDir() {
    wchar_t* appdata = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) p = fs::path(appdata) / "SlaveZeroLauncher";
    CoTaskMemFree(appdata);
    return p.empty() ? ExecutableDir() : p;
}

fs::path ExecutableDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

DisplayInfo PrimaryDisplay() {
    DisplayInfo d;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        d.width = (int)dm.dmPelsWidth;
        d.height = (int)dm.dmPelsHeight;
        d.refresh = (int)dm.dmDisplayFrequency;
    }
    return d;
}

std::vector<fs::path> DetectGameDirs() {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        std::error_code ec;
        if (!d.empty() && fs::exists(d / "SlaveZero.exe", ec)) out.push_back(d);
    };
    add(RegPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games\\1207662553", L"path"));
    add(RegPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207662553", L"path"));
    add(L"C:\\GOG Games\\Slave Zero");
    add(L"C:\\Program Files (x86)\\GOG Galaxy\\Games\\Slave Zero");
    return out;
}

std::vector<Runner> DetectRunners() { return {}; }
fs::path PrefixForGameDir(const fs::path&) { return {}; }

bool Launch(const LaunchOptions& o, std::string& err) {
    std::wstring exe = (o.gameDir / "SlaveZero.exe").wstring();
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    SetEnvironmentVariableW(L"SZM_LAUNCHER", L"1");  // the in-game menu can ask us to restart the game
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, o.gameDir.c_str(), &si, &pi)) {
        err = "CreateProcess error " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    ++g_games;
    std::thread([h = pi.hProcess] {  // the launcher hides while the game runs
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        --g_games;
    }).detach();
    return true;
}

void OpenFolder(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void PickFolder(std::function<void(const fs::path&)> done) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Select the Slave Zero game folder");
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                done(fs::path(path));
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void Poll() {}
bool SpawnInProgress() { return false; }

static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool Download(const std::string& url, const fs::path& dest, std::string& err) {
    HRESULT hr = URLDownloadToFileW(nullptr, Widen(url).c_str(), dest.c_str(), 0, nullptr);
    if (FAILED(hr)) { err = "download failed (HRESULT " + std::to_string((unsigned long)hr) + ")"; return false; }
    return true;
}

int RunProcess(const std::vector<std::string>& argv, const fs::path& cwd, std::string& err) {
    std::wstring cmd;
    for (auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"' + Widen(a) + L'"';
    }
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        err = "could not start " + argv[0];
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

}  // namespace platform
