#include "app.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "imgui.h"
#include "ini.h"
#include "payload.h"  // generated: kSzModernDll / kSzModernDllSize

namespace {

const char* kExe = "SlaveZero.exe";
const char* kDll = "szmodern.dll";
const char* kExeBackup = "SlaveZero.exe.original";
constexpr uintmax_t kExeSize = 376832;  // GOG SlaveZero.exe
// Win9x-only cli / sti around the CPU-speed probe (VA 0x41d849 / 0x41d881): fault on NT and Wine.
constexpr struct { size_t off; uint8_t orig; } kExePatch[] = {{0x1cc49, 0xfa}, {0x1cc81, 0xfb}};

const ImVec4 kAccent(0.44f, 0.88f, 0.29f, 1.0f);
const ImVec4 kAccentDim(0.25f, 0.52f, 0.17f, 1.0f);
const ImVec4 kWarn(0.98f, 0.73f, 0.26f, 1.0f);
const ImVec4 kErr(0.96f, 0.36f, 0.32f, 1.0f);
const ImVec4 kMuted(0.55f, 0.63f, 0.56f, 1.0f);

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)data, (std::streamsize)n);
    return bool(f);
}

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Two-column "label | widget" row.
void Label(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool BeginForm(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void Section(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
    ImGui::TextColored(kAccent, "%s", text);
}

template <class T> int IndexOf(const std::vector<T>& v, T x) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == x) return (int)i;
    return -1;
}

}  // namespace

// ============================================================================
// Setup / persistence
// ============================================================================

void App::Init() {
    display_ = platform::PrimaryDisplay();
    runners_ = platform::DetectRunners();

    Ini lc;
    lc.Load(platform::ConfigDir() / "launcher.ini");
    std::string saved = lc.Get("Launcher", "GameDir");
    runnerSaved_ = lc.Get("Linux", "Runner");
    s_.gamescope = lc.GetInt("Linux", "Gamescope", 0) != 0;
    s_.mangohud = lc.GetInt("Linux", "MangoHud", 0) != 0;
    s_.gamemode = lc.GetInt("Linux", "GameMode", 0) != 0;
    s_.extraEnv = lc.Get("Linux", "ExtraEnv");
    s_.closeOnPlay = lc.GetInt("Launcher", "CloseOnPlay", 1) != 0;
    for (size_t i = 0; i < runners_.size(); ++i)
        if (runners_[i].path.string() == runnerSaved_) s_.runnerIndex = (int)i;

    fs::path dir;
    if (!saved.empty() && fs::exists(fs::u8path(saved) / kExe)) dir = fs::u8path(saved);
    else if (fs::exists(platform::ExecutableDir() / kExe)) dir = platform::ExecutableDir();
    else {
        auto found = platform::DetectGameDirs();
        if (!found.empty()) dir = found.front();
    }
    SetGameDir(dir);
}

void App::SetGameDir(const fs::path& dir) {
    gameDir_ = dir;
    std::snprintf(pathBuf_, sizeof pathBuf_, "%s", dir.u8string().c_str());
    gameValid_ = !dir.empty() && fs::exists(dir / kExe);
    prefix_ = platform::IsWindows() ? fs::path() : platform::PrefixForGameDir(dir);
    if (gameValid_) LoadSettings();
    RefreshStatus();
}

void App::LoadSettings() {
    Settings d;  // defaults
    Ini m;
    m.Load(gameDir_ / "szModern.ini");
    s_.windowed = m.GetInt("Display", "Windowed", d.windowed) != 0;
    s_.winW = m.GetInt("Display", "Width", 0);
    s_.winH = m.GetInt("Display", "Height", 0);
    if (s_.winW <= 0 || s_.winH <= 0) { s_.winW = d.winW; s_.winH = d.winH; }
    s_.vsync = m.GetInt("Display", "VSync", 1) != 0;
    s_.frameLimit = m.GetInt("Display", "FrameLimit", d.frameLimit);
    s_.widescreen = m.GetInt("Display", "Widescreen", 1) != 0;
    s_.msaa = m.GetInt("Quality", "MSAA", d.msaa);
    s_.renderScale = m.GetFloat("Quality", "RenderScale", d.renderScale);
    s_.aniso = m.GetInt("Quality", "Anisotropy", d.aniso);
    s_.mips = m.GetInt("Quality", "GenerateMipmaps", 1) != 0;
    s_.trueColor = m.GetInt("Quality", "TrueColor", 1) != 0;
    s_.lightmapScale = m.GetFloat("Quality", "LightmapScale", d.lightmapScale);
    s_.cutout = m.GetInt("Quality", "OpaqueCutout", 1) != 0;
    s_.sharpen = m.GetFloat("Post", "Sharpen", d.sharpen);
    s_.filter = std::min(std::max(m.GetInt("Post", "Filter", d.filter), 0), 5);
    s_.filterStrength = m.GetFloat("Post", "FilterStrength", d.filterStrength);
    s_.scanlines = m.GetInt("Post", "Scanlines", d.scanlines);

    Ini o;
    o.Load(gameDir_ / "szOptions.ini");
    s_.renderer = o.GetInt("Video", "Display Mode Wanted", 0) == 1 ? 1 : 0;
    s_.gamma = o.GetFloat("Settings", "Gamma", d.gamma);
    s_.mouseSens = o.GetFloat("Settings", "Mouse Sensitivity", d.mouseSens);
    s_.invertMouse = o.GetInt("Settings", "Invert Mouse", 0) != 0;
    s_.crosshair = o.GetInt("Settings", "Show Crosshair", 1) != 0;
    s_.forceFeedback = o.GetInt("Settings", "Force Feedback Enabled", 0) != 0;
    s_.audioOn = o.GetInt("Audio", "Audio On", 1) != 0;
    s_.musicOn = o.GetInt("Audio", "Music On", 1) != 0;
    s_.musicVol = o.GetFloat("Audio", "Music Volume", 1.0f);
    s_.sfxVol = o.GetFloat("Audio", "SFX Volume", 1.0f);
    s_.eax = o.GetInt("Audio", "Use EAX", 0) != 0;
    dirty_ = false;
}

bool App::SaveSettings() {
    bool ok = true;
    if (gameValid_) {
        Ini m;
        if (!m.Load(gameDir_ / "szModern.ini")) {
            m.SetPath(gameDir_ / "szModern.ini");
            m.SetCrlf(true);
        }
        m.SetInt("Display", "Windowed", s_.windowed);
        m.SetInt("Display", "Width", s_.windowed ? s_.winW : 0);
        m.SetInt("Display", "Height", s_.windowed ? s_.winH : 0);
        m.SetInt("Display", "VSync", s_.vsync);
        m.SetInt("Display", "FrameLimit", s_.frameLimit);
        m.SetInt("Display", "Widescreen", s_.widescreen);
        m.SetInt("Quality", "MSAA", s_.msaa);
        m.SetFloat("Quality", "RenderScale", s_.renderScale, 3);
        m.SetInt("Quality", "Anisotropy", s_.aniso);
        m.SetInt("Quality", "GenerateMipmaps", s_.mips);
        m.SetInt("Quality", "TrueColor", s_.trueColor);
        m.SetFloat("Quality", "LightmapScale", s_.lightmapScale, 2);
        m.SetInt("Quality", "OpaqueCutout", s_.cutout);
        m.SetFloat("Post", "Sharpen", s_.sharpen, 2);
        m.SetInt("Post", "Filter", s_.filter);
        m.SetFloat("Post", "FilterStrength", s_.filterStrength, 2);
        m.SetInt("Post", "Scanlines", s_.scanlines);
        m.Set("Post", "Gamma", "0");
        ok &= m.Save();

        Ini o;
        if (o.Load(gameDir_ / "szOptions.ini")) {
            o.SetInt("Video", "Display Mode Wanted", s_.renderer);
            o.SetInt("Video", "Wait For VSync", s_.vsync);
            o.SetInt("Video", "Use True Color Textures", 1);
            o.SetInt("Video", "Windowed Mode", 0);
            o.SetFloat("Settings", "Gamma", s_.gamma);
            o.SetFloat("Settings", "Mouse Sensitivity", s_.mouseSens);
            o.SetInt("Settings", "Invert Mouse", s_.invertMouse);
            o.SetInt("Settings", "Show Crosshair", s_.crosshair);
            o.SetInt("Settings", "Force Feedback Enabled", s_.forceFeedback);
            o.SetInt("Audio", "Audio On", s_.audioOn);
            o.SetInt("Audio", "Music On", s_.musicOn);
            o.SetFloat("Audio", "Music Volume", s_.musicVol);
            o.SetFloat("Audio", "SFX Volume", s_.sfxVol);
            o.SetInt("Audio", "Use EAX", s_.eax);
            o.SetInt("Audio", "Use Hardware", s_.eax);
            ok &= o.Save();
        }
    }

    fs::create_directories(platform::ConfigDir());
    Ini lc;
    lc.Load(platform::ConfigDir() / "launcher.ini");
    lc.SetPath(platform::ConfigDir() / "launcher.ini");
    lc.Set("Launcher", "GameDir", gameDir_.u8string());
    lc.SetInt("Launcher", "CloseOnPlay", s_.closeOnPlay);
    if (!runners_.empty() && s_.runnerIndex < (int)runners_.size())
        lc.Set("Linux", "Runner", runners_[s_.runnerIndex].path.u8string());
    lc.SetInt("Linux", "Gamescope", s_.gamescope);
    lc.SetInt("Linux", "MangoHud", s_.mangohud);
    lc.SetInt("Linux", "GameMode", s_.gamemode);
    lc.Set("Linux", "ExtraEnv", s_.extraEnv);
    ok &= lc.Save();
    if (ok) dirty_ = false;
    return ok;
}

// ============================================================================
// Install / repair
// ============================================================================

void App::RefreshStatus() {
    rendererStatus_ = exeStatus_ = Status::Missing;
    if (!gameValid_) return;

    auto dll = ReadFile(gameDir_ / kDll);
    if (!dll.empty())
        rendererStatus_ = (dll.size() == kSzModernDllSize && std::memcmp(dll.data(), kSzModernDll, dll.size()) == 0)
                              ? Status::Ok : Status::Outdated;
    Ini hw;
    if (!hw.Load(gameDir_ / "szHardware.ini") || hw.Get("Display 0", "Renderer Name") != kDll)
        if (rendererStatus_ == Status::Ok) rendererStatus_ = Status::Outdated;

    auto exe = ReadFile(gameDir_ / kExe);
    if (exe.size() == kExeSize) {
        bool patched = true, original = true;
        for (auto& p : kExePatch) {
            patched &= exe[p.off] == 0x90;
            original &= exe[p.off] == p.orig;
        }
        exeStatus_ = patched ? Status::Ok : original ? Status::Outdated : Status::Missing;
    }
}

bool App::Install(std::string& msg) {
    if (!gameValid_) { msg = "Choose the Slave Zero folder first."; return false; }
    std::error_code ec;

    // renderer DLL
    if (!WriteFile(gameDir_ / kDll, kSzModernDll, kSzModernDllSize)) {
        msg = "Could not write szmodern.dll (is the game running?)";
        return false;
    }
    // display modes: Modern first, Glide kept as the classic option
    Ini hw;
    hw.SetPath(gameDir_ / "szHardware.ini");
    hw.SetCrlf(true);
    hw.SetInt("Main", "Num Display Modes", 2);
    const char* names[2][3] = {{kDll, "Slave Zero Modern Renderer", "Modern"},
                               {"3dfx.dll", "3dfx Glide Device Driver", "Glide"}};
    for (int i = 0; i < 2; ++i) {
        std::string sec = "Display " + std::to_string(i);
        hw.Set(sec, "Renderer Name", names[i][0]);
        hw.Set(sec, "Device ID", names[i][1]);
        hw.Set(sec, "Menu Description", names[i][2]);
        hw.SetInt(sec, "Device Index", 0);
        hw.SetInt(sec, "Width", 640);
        hw.SetInt(sec, "Height", 480);
        hw.SetInt(sec, "Bpp", 16);
    }
    if (!hw.Save()) { msg = "Could not write szHardware.ini"; return false; }

    // executable compatibility fix
    auto exe = ReadFile(gameDir_ / kExe);
    if (exe.size() != kExeSize) {
        msg = "SlaveZero.exe is not the GOG version this launcher knows; left untouched.";
    } else {
        bool needs = false;
        for (auto& p : kExePatch) needs |= exe[p.off] == p.orig;
        if (needs) {
            if (!fs::exists(gameDir_ / kExeBackup)) fs::copy_file(gameDir_ / kExe, gameDir_ / kExeBackup, ec);
            for (auto& p : kExePatch)
                if (exe[p.off] == p.orig) exe[p.off] = 0x90;
            if (!WriteFile(gameDir_ / kExe, exe.data(), exe.size())) {
                msg = "Could not patch SlaveZero.exe";
                return false;
            }
        }
    }
    SaveSettings();
    RefreshStatus();
    if (msg.empty()) msg = "Modern renderer installed.";
    return true;
}

// ============================================================================
// Launch
// ============================================================================

void App::Notify(const std::string& msg, bool error) {
    message_ = msg;
    messageError_ = error;
    messageTime_ = ImGui::GetTime();
}

void App::Play() {
    if (!gameValid_) { Notify("Choose the Slave Zero folder first.", true); return; }
    if (rendererStatus_ != Status::Ok || exeStatus_ != Status::Ok) {
        std::string m;
        if (!Install(m)) { Notify(m, true); return; }
    }
    if (!SaveSettings()) { Notify("Could not save settings (is the game folder writable?)", true); return; }

    LaunchOptions o;
    o.gameDir = gameDir_;
    if (!platform::IsWindows()) {
        if (runners_.empty()) { Notify("No Proton or Wine found. Install GE-Proton or Wine first.", true); return; }
        o.runner = runners_[std::min<size_t>(s_.runnerIndex, runners_.size() - 1)];
        o.prefix = prefix_;
        o.gamescope = s_.gamescope;
        o.mangohud = s_.mangohud;
        o.gamemode = s_.gamemode;
        o.extraEnv = s_.extraEnv;
        o.outW = s_.windowed ? s_.winW : display_.width;
        o.outH = s_.windowed ? s_.winH : display_.height;
        o.refresh = display_.refresh;
    }
    std::string err;
    if (!platform::Launch(o, err)) { Notify("Launch failed: " + err, true); return; }
    Notify("Starting Slave Zero...");
    if (s_.closeOnPlay) quit_ = true;
}

// ============================================================================
// UI
// ============================================================================

bool App::Frame() {
    platform::Poll();
    // debug: SZL_REPORT=file writes what the launcher detected and exits
    static int frames = 0;
    if (const char* rep = std::getenv("SZL_REPORT"); rep && ++frames == 5) {
        std::ofstream f(rep);
        f << "platform=" << platform::Name() << "\ngameDir=" << gameDir_.u8string() << "\ngameValid=" << gameValid_
          << "\nrenderer=" << (int)rendererStatus_ << "\nexe=" << (int)exeStatus_ << "\ndisplay=" << display_.width << "x"
          << display_.height << "@" << display_.refresh << "\nprefix=" << prefix_.u8string() << "\n";
        for (auto& r : runners_) f << "runner=" << r.name << " | " << r.path.u8string() << "\n";
        quit_ = true;
    }
    if (std::getenv("SZL_AUTOPLAY") && frames++ == 5) {  // debug: press Play once, keep the launcher open
        s_.closeOnPlay = false;
        Play();
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawHeader();
    DrawGameFolder();
    ImGui::Spacing();

    float footerH = ImGui::GetFrameHeight() * 2.4f;
    ImGui::BeginChild("##tabs", ImVec2(0, -footerH), ImGuiChildFlags_None);
    ImGui::BeginDisabled(!gameValid_);
    if (ImGui::BeginTabBar("##tabbar")) {
        static const char* forced = std::getenv("SZL_TAB");  // debug screenshots
        auto flag = [](const char* n) {
            return forced && std::strcmp(forced, n) == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Display", nullptr, flag("display"))) { DrawDisplayTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Graphics", nullptr, flag("graphics"))) { DrawGraphicsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Game & Audio", nullptr, flag("game"))) { DrawGameTab(); ImGui::EndTabItem(); }
        if (!platform::IsWindows() && ImGui::BeginTabItem("Linux", nullptr, flag("linux"))) { DrawLinuxTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    DrawFooter();
    ImGui::End();
    return !quit_;
}

void App::DrawHeader() {
    ImGui::PushFont(title_, title_->LegacySize);
    ImGui::TextColored(kAccent, "SLAVE ZERO");
    float titleBottom = ImGui::GetItemRectMax().y;
    ImGui::PopFont();
    ImGui::SameLine(0, ImGui::GetFontSize());
    float y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, titleBottom - ImGui::GetTextLineHeight() - (titleBottom - y) * 0.12f));
    ImGui::TextColored(kMuted, "modern launcher  /  %s", platform::Name());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

void App::DrawGameFolder() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Game folder");
    ImGui::SameLine();
    float btn = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue))
        SetGameDir(fs::u8path(pathBuf_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) platform::PickFolder([this](const fs::path& p) { SetGameDir(p); dirty_ = true; });

    auto chip = [](const char* label, bool ok, bool warn) {
        ImGui::SameLine();
        ImGui::TextColored(ok ? kAccent : warn ? kWarn : kErr, "%s %s", ok ? "[ok]" : warn ? "[!]" : "[x]", label);
    };
    ImGui::TextColored(kMuted, "Status:");
    chip("game found", gameValid_, false);
    chip(rendererStatus_ == Status::Ok ? "modern renderer" : rendererStatus_ == Status::Outdated
                                                                 ? "renderer needs update" : "renderer not installed",
         rendererStatus_ == Status::Ok, true);
    chip(exeStatus_ == Status::Ok ? "compatibility fix" : exeStatus_ == Status::Outdated ? "compatibility fix pending"
                                                                                          : "unknown executable",
         exeStatus_ == Status::Ok, exeStatus_ == Status::Outdated);
    if (!platform::IsWindows() && gameValid_) {
        ImGui::SameLine();
        ImGui::TextColored(prefix_.empty() ? kWarn : kMuted, "  prefix: %s",
                           prefix_.empty() ? "(not inside a Wine prefix)" : prefix_.u8string().c_str());
    }
}

void App::DrawDisplayTab() {
    if (!BeginForm("display")) return;
    bool modern = s_.renderer == 0;

    Section("Renderer");
    Label("Renderer");
    const char* rnames[] = {"Modern  (Direct3D 11, native resolution)", "Classic  (original Glide via nGlide)"};
    if (ImGui::Combo("##renderer", &s_.renderer, rnames, 2)) dirty_ = true;

    ImGui::BeginDisabled(!modern);
    Section("Screen");
    Label("Display mode");
    int mode = s_.windowed ? 1 : 0;
    const char* modes[] = {"Fullscreen (borderless)", "Windowed"};
    if (ImGui::Combo("##mode", &mode, modes, 2)) { s_.windowed = mode == 1; dirty_ = true; }

    if (s_.windowed) {
        Label("Window size");
        static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
        char cur[48];
        std::snprintf(cur, sizeof cur, "%d x %d", s_.winW, s_.winH);
        if (ImGui::BeginCombo("##winsize", cur)) {
            for (auto& sz : sizes) {
                if (sz[0] > display_.width || sz[1] > display_.height) continue;
                char l[48];
                std::snprintf(l, sizeof l, "%d x %d", sz[0], sz[1]);
                if (ImGui::Selectable(l, s_.winW == sz[0] && s_.winH == sz[1])) { s_.winW = sz[0]; s_.winH = sz[1]; dirty_ = true; }
            }
            ImGui::EndCombo();
        }
    }

    Label("Render resolution");
    {
        int ow = s_.windowed ? s_.winW : display_.width, oh = s_.windowed ? s_.winH : display_.height;
        static const float scales[] = {0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};
        auto label = [&](float k, char* out, size_t n) {
            std::snprintf(out, n, "%d x %d%s", (int)std::lround(ow * k), (int)std::lround(oh * k),
                          k == 1.0f ? "  (native)" : k > 1.0f ? "  (supersampled)" : "");
        };
        char cur[64];
        label(s_.renderScale, cur, sizeof cur);
        if (ImGui::BeginCombo("##rscale", cur)) {
            for (float k : scales) {
                char l[64];
                label(k, l, sizeof l);
                if (ImGui::Selectable(l, std::fabs(s_.renderScale - k) < 0.01f)) { s_.renderScale = k; dirty_ = true; }
            }
            ImGui::EndCombo();
        }
        Help("Resolution the 3D scene is drawn at before it is scaled to the screen. Above native is supersampling: "
             "sharper edges and textures at a GPU cost.");
    }

    Label("Aspect ratio");
    int wide = s_.widescreen ? 0 : 1;
    const char* aspects[] = {"Widescreen  (wider field of view, Hor+)", "Original 4:3  (black side bars)"};
    if (ImGui::Combo("##aspect", &wide, aspects, 2)) { s_.widescreen = wide == 0; dirty_ = true; }

    Section("Frame rate");
    Label("VSync");
    if (ImGui::Checkbox("##vsync", &s_.vsync)) dirty_ = true;
    Help("Sync to the display refresh rate. Prevents tearing.");
    Label("Frame rate limit");
    {
        static const int caps[] = {0, 30, 60, 75, 90, 120, 144, 165, 240};
        char cur[32];
        if (s_.frameLimit > 0) std::snprintf(cur, sizeof cur, "%d fps", s_.frameLimit);
        else std::snprintf(cur, sizeof cur, "Unlimited");
        if (ImGui::BeginCombo("##fps", cur)) {
            for (int c : caps) {
                char l[32];
                if (c) std::snprintf(l, sizeof l, "%d fps", c);
                else std::snprintf(l, sizeof l, "Unlimited");
                if (ImGui::Selectable(l, s_.frameLimit == c)) { s_.frameLimit = c; dirty_ = true; }
            }
            ImGui::EndCombo();
        }
        Help("Use 60 if the game feels too fast at high refresh rates.");
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
    if (!modern)
        ImGui::TextColored(kMuted, "The classic renderer uses nGlide's own settings (nglide_config.exe in the game folder).");
}

void App::DrawGraphicsTab() {
    if (!BeginForm("graphics")) return;
    ImGui::BeginDisabled(s_.renderer != 0);
    Section("Image quality");
    Label("Anti-aliasing");
    {
        static const int v[] = {1, 2, 4, 8};
        const char* n[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        int i = std::max(0, IndexOf(std::vector<int>(v, v + 4), s_.msaa));
        if (ImGui::Combo("##msaa", &i, n, 4)) { s_.msaa = v[i]; dirty_ = true; }
    }
    Label("Texture filtering");
    {
        static const int v[] = {1, 2, 4, 8, 16};
        const char* n[] = {"Trilinear", "2x anisotropic", "4x anisotropic", "8x anisotropic", "16x anisotropic"};
        int i = std::max(0, IndexOf(std::vector<int>(v, v + 5), s_.aniso));
        if (ImGui::Combo("##aniso", &i, n, 5)) { s_.aniso = v[i]; dirty_ = true; }
    }
    Label("Sharpening");
    if (ImGui::SliderFloat("##sharpen", &s_.sharpen, 0.0f, 1.0f, s_.sharpen <= 0.001f ? "Off" : "%.2f")) dirty_ = true;
    Label("True-colour textures");
    if (ImGui::Checkbox("##tc", &s_.trueColor)) dirty_ = true;
    Help("Keeps the game's 32-bit textures (effects, glows) at full precision instead of 16-bit.");
    Label("Generate mipmaps");
    if (ImGui::Checkbox("##mips", &s_.mips)) dirty_ = true;
    Help("Builds smaller texture levels so distant surfaces do not shimmer.");
    Label("See-through details");
    if (ImGui::Checkbox("##cutout", &s_.cutout)) dirty_ = true;
    Help("Draws fences, grates and railings with their holes, as on period hardware.");

    Section("Screen filter");
    Label("Filter");
    {
        const char* n[] = {"Off", "Scanlines", "CRT (aperture grille)", "CRT (curved screen)", "Retro 640x480 pixels", "VHS tape"};
        if (ImGui::Combo("##filter", &s_.filter, n, 6)) dirty_ = true;
        Help("Press F9 in game to cycle through the filters.");
    }
    ImGui::BeginDisabled(s_.filter == 0);
    Label("Filter strength");
    if (ImGui::SliderFloat("##fstr", &s_.filterStrength, 0.1f, 1.0f, "%.2f")) dirty_ = true;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(s_.filter < 1 || s_.filter > 3);
    Label("Scanline count");
    {
        static const int v[] = {240, 360, 480, 540, 720};
        char cur[32];
        std::snprintf(cur, sizeof cur, "%d lines", s_.scanlines);
        if (ImGui::BeginCombo("##lines", cur)) {
            for (int x : v) {
                char l[48];
                std::snprintf(l, sizeof l, "%d lines%s", x, x == 240 ? "  (arcade / low-res)" : x == 480 ? "  (original VGA)" : "");
                if (ImGui::Selectable(l, s_.scanlines == x)) { s_.scanlines = x; dirty_ = true; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();

    Section("Lighting");
    Label("Lightmap intensity");
    if (ImGui::SliderFloat("##lm", &s_.lightmapScale, 1.0f, 2.0f, "%.2f")) dirty_ = true;
    Help("2.0 matches the original Direct3D renderer. 1.0 matches Voodoo2-class Glide cards (darker).");
    ImGui::EndDisabled();
    Label("Brightness (gamma)");
    if (ImGui::SliderFloat("##gamma", &s_.gamma, 0.5f, 3.0f, "%.2f")) dirty_ = true;
    Help("Same as the in-game gamma slider. The game's default is 1.87.");
    ImGui::EndTable();
}

void App::DrawGameTab() {
    if (!BeginForm("game")) return;
    Section("Controls");
    Label("Mouse sensitivity");
    if (ImGui::SliderFloat("##sens", &s_.mouseSens, 0.1f, 5.0f, "%.2f")) dirty_ = true;
    Label("Invert mouse");
    if (ImGui::Checkbox("##invert", &s_.invertMouse)) dirty_ = true;
    Label("Show crosshair");
    if (ImGui::Checkbox("##cross", &s_.crosshair)) dirty_ = true;
    Label("Force feedback");
    if (ImGui::Checkbox("##ff", &s_.forceFeedback)) dirty_ = true;

    Section("Audio");
    Label("Sound");
    if (ImGui::Checkbox("##audio", &s_.audioOn)) dirty_ = true;
    Label("Music");
    if (ImGui::Checkbox("##music", &s_.musicOn)) dirty_ = true;
    Label("Music volume");
    if (ImGui::SliderFloat("##mvol", &s_.musicVol, 0.0f, 1.0f, "%.2f")) dirty_ = true;
    Label("Effects volume");
    if (ImGui::SliderFloat("##svol", &s_.sfxVol, 0.0f, 1.0f, "%.2f")) dirty_ = true;
    Label("Hardware 3D audio (EAX)");
    if (ImGui::Checkbox("##eax", &s_.eax)) dirty_ = true;
    Help("Only for sound cards with real EAX/DirectSound3D hardware. Leave off on modern systems and on Linux.");
    ImGui::EndTable();
}

void App::DrawLinuxTab() {
    if (!BeginForm("linux")) return;
    Section("Compatibility layer");
    Label("Proton / Wine");
    if (runners_.empty()) {
        ImGui::TextColored(kErr, "None found. Install GE-Proton (e.g. with ProtonUp-Qt) or Wine.");
    } else {
        s_.runnerIndex = std::min<int>(s_.runnerIndex, (int)runners_.size() - 1);
        if (ImGui::BeginCombo("##runner", runners_[s_.runnerIndex].name.c_str())) {
            for (size_t i = 0; i < runners_.size(); ++i)
                if (ImGui::Selectable(runners_[i].name.c_str(), (int)i == s_.runnerIndex)) { s_.runnerIndex = (int)i; dirty_ = true; }
            ImGui::EndCombo();
        }
    }
    Label("Wine prefix");
    ImGui::TextWrapped("%s", prefix_.empty() ? "(game folder is not inside a Wine prefix)" : prefix_.u8string().c_str());

    Section("Extras");
    Label("Gamescope");
    if (ImGui::Checkbox("##gs", &s_.gamescope)) dirty_ = true;
    Help("Runs the game inside Valve's gamescope compositor at the chosen resolution and refresh rate.");
    Label("MangoHud overlay");
    if (ImGui::Checkbox("##mh", &s_.mangohud)) dirty_ = true;
    Label("GameMode");
    if (ImGui::Checkbox("##gm", &s_.gamemode)) dirty_ = true;
    Label("Extra environment");
    {
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s", s_.extraEnv.c_str());
        if (ImGui::InputTextWithHint("##env", "e.g. DXVK_HUD=fps PROTON_ENABLE_WAYLAND=1", buf, sizeof buf)) {
            s_.extraEnv = buf;
            dirty_ = true;
        }
    }
    ImGui::EndTable();
}

void App::DrawFooter() {
    ImGui::Separator();
    ImGui::Spacing();
    float h = ImGui::GetFrameHeight() * 1.5f;

    if (ImGui::Button("Open folder", ImVec2(0, h)) && gameValid_) platform::OpenFolder(gameDir_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!gameValid_);
    if (ImGui::Button("Install / repair", ImVec2(0, h))) {
        std::string m;
        bool ok = Install(m);
        Notify(m, !ok);
    }
    ImGui::SameLine();
    if (ImGui::Button(dirty_ ? "Save *" : "Save", ImVec2(0, h)))
        Notify(SaveSettings() ? "Settings saved." : "Could not save settings.", !gameValid_);
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(0, h))) {
        Settings d;
        d.runnerIndex = s_.runnerIndex;
        d.gamescope = s_.gamescope;
        d.mangohud = s_.mangohud;
        d.gamemode = s_.gamemode;
        d.extraEnv = s_.extraEnv;
        s_ = d;
        dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Close launcher on play", &s_.closeOnPlay);

    // message
    if (!message_.empty() && ImGui::GetTime() - messageTime_ < 8.0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(messageError_ ? kErr : kAccent, "  %s", message_.c_str());
    }

    // PLAY
    ImGui::PushFont(big_, big_->LegacySize);
    float pw = ImGui::CalcTextSize("PLAY").x + ImGui::GetFontSize() * 2.5f;
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - pw);
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.02f, 0.05f, 0.02f, 1.0f));
    ImGui::PushFont(big_, big_->LegacySize);
    ImGui::BeginDisabled(!gameValid_);
    float bh = std::max(h, ImGui::GetFrameHeight());
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (bh - h) * 0.5f);
    if (ImGui::Button("PLAY", ImVec2(pw, bh))) Play();
    ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
}
