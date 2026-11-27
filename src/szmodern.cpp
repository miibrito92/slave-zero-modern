// Slave Zero modern renderer: a Direct3D 11 implementation of the game's hwAPI plugin
// interface. Drop-in replacement for 3dfx.dll / d3d.dll; see docs/hwapi.md.
//
// Key idea: the engine is told the screen is 640x480 (the resolution its HUD scripts
// hard-code), while everything is rasterised at the real output resolution.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "config.h"
#include "engine.h"
#include "overlay.h"
#include "crash.h"
#include "shaders.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb_image.h"

#define EXPORT extern "C" __declspec(dllexport)

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// ============================================================================
// Config + logging
// ============================================================================

Config g_cfg;
char g_gameDir[MAX_PATH];
char g_dllPath[MAX_PATH];
static char g_dllName[MAX_PATH];
static FILE* g_log;
static int g_testCrashFrame = 0;  // SZM_TEST_CRASH=<frame>: crash on purpose (tests the crash report)

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static float IniFloat(const char* path, const char* sec, const char* key, float def) {
    char buf[64], defs[64];
    snprintf(defs, sizeof defs, "%g", def);
    GetPrivateProfileStringA(sec, key, defs, buf, sizeof buf, path);
    return (float)atof(buf);
}

static void LoadConfig() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\szModern.ini", g_gameDir);
    Config& c = g_cfg;
    c.windowed = GetPrivateProfileIntA("Display", "Windowed", c.windowed, path);
    c.width = GetPrivateProfileIntA("Display", "Width", c.width, path);
    c.height = GetPrivateProfileIntA("Display", "Height", c.height, path);
    c.vsync = GetPrivateProfileIntA("Display", "VSync", c.vsync, path);
    c.widescreen = GetPrivateProfileIntA("Display", "Widescreen", c.widescreen, path);
    c.hudEdges = GetPrivateProfileIntA("Display", "HudAtEdges", c.hudEdges, path);
    c.menuPillarbox = GetPrivateProfileIntA("Display", "MenusIn4x3", c.menuPillarbox, path);
    c.trueColor = GetPrivateProfileIntA("Quality", "TrueColor", c.trueColor, path);
    c.texturePack = GetPrivateProfileIntA("Quality", "TexturePack", c.texturePack, path);
    c.normalMaps = GetPrivateProfileIntA("Quality", "NormalMaps", c.normalMaps, path);
    c.frameLimit = GetPrivateProfileIntA("Display", "FrameLimit", c.frameLimit, path);
    c.msaa = GetPrivateProfileIntA("Quality", "MSAA", c.msaa, path);
    c.renderScale = IniFloat(path, "Quality", "RenderScale", c.renderScale);
    c.anisotropy = GetPrivateProfileIntA("Quality", "Anisotropy", c.anisotropy, path);
    c.generateMips = GetPrivateProfileIntA("Quality", "GenerateMipmaps", c.generateMips, path);
    c.lightmapScale = IniFloat(path, "Quality", "LightmapScale", c.lightmapScale);
    c.opaqueCutout = GetPrivateProfileIntA("Quality", "OpaqueCutout", c.opaqueCutout, path);
    c.sharpen = IniFloat(path, "Post", "Sharpen", c.sharpen);
    c.filter = GetPrivateProfileIntA("Post", "Filter", c.filter, path);
    c.filterStrength = std::min(std::max(IniFloat(path, "Post", "FilterStrength", c.filterStrength), 0.0f), 1.0f);
    c.scanlines = std::min(std::max((int)GetPrivateProfileIntA("Post", "Scanlines", c.scanlines, path), 120), 1080);
    c.gammaOverride = IniFloat(path, "Post", "Gamma", c.gammaOverride);
    c.log = GetPrivateProfileIntA("Debug", "Log", c.log, path);
    char key[16];
    GetPrivateProfileStringA("Overlay", "Key", "F10", key, sizeof key, path);
    if ((key[0] == 'F' || key[0] == 'f') && atoi(key + 1) >= 1 && atoi(key + 1) <= 12) c.overlayKey = VK_F1 + atoi(key + 1) - 1;
    c.overlayHint = GetPrivateProfileIntA("Overlay", "ShowHint", c.overlayHint, path);
    c.pauseInOverlay = GetPrivateProfileIntA("Overlay", "PauseGame", c.pauseInOverlay, path);
    c.startDirectly = GetPrivateProfileIntA("Launcher", "StartDirectly", c.startDirectly, path);

    c.renderScale = std::min(std::max(c.renderScale, 0.25f), 4.0f);
    c.anisotropy = std::min(std::max(c.anisotropy, 1), 16);

    char env[512];
    if (GetEnvironmentVariableA("SZM_SHOTS", env, sizeof env)) {
        for (char* t = strtok(env, ","); t; t = strtok(nullptr, ",")) c.shotFrames.push_back(atoi(t));
    }
    if (GetEnvironmentVariableA("SZM_EXIT", env, sizeof env)) c.exitFrame = atoi(env);
    if (GetEnvironmentVariableA("SZM_KEYS", env, sizeof env)) {
        for (char* t = strtok(env, ","); t; t = strtok(nullptr, ",")) {
            int f = 0, vk = 0, hold = 4;
            if (sscanf(t, "%d:%x:%d", &f, &vk, &hold) >= 2) c.keys.push_back({f, vk, hold});
        }
    }
}

static void IniPut(const char* path, const char* sec, const char* key, const char* fmt, double v) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, v);
    WritePrivateProfileStringA(sec, key, buf, path);
}

void SaveConfig() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\szModern.ini", g_gameDir);
    const Config& c = g_cfg;
    IniPut(path, "Display", "Windowed", "%.0f", c.windowed);
    IniPut(path, "Display", "Width", "%.0f", c.width);
    IniPut(path, "Display", "Height", "%.0f", c.height);
    IniPut(path, "Display", "VSync", "%.0f", c.vsync);
    IniPut(path, "Display", "FrameLimit", "%.0f", c.frameLimit);
    IniPut(path, "Display", "Widescreen", "%.0f", c.widescreen);
    IniPut(path, "Display", "HudAtEdges", "%.0f", c.hudEdges);
    IniPut(path, "Display", "MenusIn4x3", "%.0f", c.menuPillarbox);
    IniPut(path, "Quality", "MSAA", "%.0f", c.msaa);
    IniPut(path, "Quality", "RenderScale", "%.3f", c.renderScale);
    IniPut(path, "Quality", "Anisotropy", "%.0f", c.anisotropy);
    IniPut(path, "Quality", "GenerateMipmaps", "%.0f", c.generateMips);
    IniPut(path, "Quality", "LightmapScale", "%.2f", c.lightmapScale);
    IniPut(path, "Quality", "OpaqueCutout", "%.0f", c.opaqueCutout);
    IniPut(path, "Quality", "TrueColor", "%.0f", c.trueColor);
    IniPut(path, "Quality", "TexturePack", "%.0f", c.texturePack);
    IniPut(path, "Quality", "NormalMaps", "%.0f", c.normalMaps);
    IniPut(path, "Post", "Sharpen", "%.2f", c.sharpen);
    IniPut(path, "Post", "Filter", "%.0f", c.filter);
    IniPut(path, "Post", "FilterStrength", "%.2f", c.filterStrength);
    IniPut(path, "Post", "Scanlines", "%.0f", c.scanlines);
    char key[8];
    snprintf(key, sizeof key, "F%d", c.overlayKey - VK_F1 + 1);
    WritePrivateProfileStringA("Overlay", "Key", key, path);
    IniPut(path, "Overlay", "ShowHint", "%.0f", c.overlayHint);
    IniPut(path, "Overlay", "PauseGame", "%.0f", c.pauseInOverlay);
    IniPut(path, "Launcher", "StartDirectly", "%.0f", c.startDirectly);
}

// ============================================================================
// Widescreen: widen the engine's view frustum, keep its 640x480 projection
// ============================================================================

// Horizontal widening factor relative to 4:3 (1.0 = no widescreen).
static float g_wideK = 1.0f;
static bool g_exeKnown = false;  // SlaveZero.exe matched the build these addresses come from
static bool g_inMenu = false;    // current level is the front end / intro (m00_*)

// The exe keeps the current level's short name (e.g. "m00_startup", "M01_A") at 0x4dc2c4.
static void UpdateLevelState() {
    if (!g_exeKnown) return;
    const char* name = (const char*)0x4dc2c4;
    g_inMenu = (name[0] == 'm' || name[0] == 'M') && name[1] == '0' && name[2] == '0' && name[3] == '_';
}

typedef void(__cdecl* SetupPerspFn)(int* persp);
static const uintptr_t kSetupPersp = 0x40c750;
static const uintptr_t kSetupPerspCalls[] = {0x40c947, 0x435283, 0x438078, 0x43939f};

// Wraps the engine's perspective setup (FUN_0040c750): run it, then widen the
// left/right frustum extents (persp+0x44/+0x48) so geometry beyond the 4:3 edges
// survives culling and clipping. Focal length and centre (used by the HUD) are untouched.
static void __cdecl SetupPerspWide(int* persp) {
    ((SetupPerspFn)kSetupPersp)(persp);
    float* p = (float*)persp;
    p[0x0d] /= g_wideK;  // +0x34 focal / left extent   (used by the world clippers)
    p[0x0e] /= g_wideK;  // +0x38 focal / right extent
    p[0x11] *= g_wideK;  // +0x44 left extent / focal   (outcodes, frustum planes)
    p[0x12] *= g_wideK;  // +0x48 right extent / focal
}

static bool InstallWidescreenHook() {
    static const uint8_t kProlog[] = {0x8b, 0x4c, 0x24, 0x04, 0x83, 0xec, 0x08, 0xdb, 0x01};
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)kSetupPersp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (memcmp((void*)kSetupPersp, kProlog, sizeof kProlog) != 0) return false;
    g_exeKnown = true;
    for (uintptr_t site : kSetupPerspCalls) {
        const uint8_t* b = (const uint8_t*)site;
        if (b[0] != 0xe8 || site + 5 + *(const int32_t*)(b + 1) != kSetupPersp) return false;
    }
    for (uintptr_t site : kSetupPerspCalls) {
        DWORD old;
        VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(site + 1) = (int32_t)((uintptr_t)&SetupPerspWide - (site + 5));
        VirtualProtect((void*)site, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// ============================================================================
// PLS: per-texture-frame / per-lightmap GPU record, stored in engine-visible memory
// ============================================================================

constexpr uint32_t kPlsMagic = 0x504d5a53;  // 'SZMP'

struct Pls {
    uint32_t magic;
    uint32_t isLightmap;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    const void* owner;  // texture or lightmap struct
    uint32_t frame;
    uint32_t w, h;
    int32_t liveIndex;
    ID3D11Texture2D* nrmTex;  // optional normal map from the texture pack
    ID3D11ShaderResourceView* nrm;
};
static_assert(sizeof(Pls) <= kPlsStride, "PLS record too large");

// ============================================================================
// Renderer state
// ============================================================================

struct Vtx {
    float x, y, oow, bias;
    float r, g, b, a;
    float u0, v0, u1, v1;
    uint32_t flags;
    float cx, cy, cz;  // camera-space position (per-pixel lighting)
};

enum : uint32_t {
    ModeVC = 0, ModeTex = 1, ModeTexVC = 2, ModeTexLM = 3,
    AlphaOne = 0 << 2, AlphaConst = 1 << 2, AlphaTex = 2 << 2, AlphaTexConst = 3 << 2,
    FlagCutout = 16, FlagAlphaTest = 32, FlagNormal = 64,
};

enum BlendMode { BlendOpaque, BlendAlpha, BlendCount };
enum DepthMode { DepthWrite, DepthTestOnly, DepthSky, DepthOff, DepthCount };
enum SamplerSet { SampWorld, SampHud };

struct GpuState {
    ID3D11ShaderResourceView* t0;
    ID3D11ShaderResourceView* t1;
    uint8_t blend, depth, samp;
    bool operator==(const GpuState& o) const {
        return t0 == o.t0 && t1 == o.t1 && blend == o.blend && depth == o.depth && samp == o.samp;
    }
};

struct Batch {
    GpuState st;
    uint32_t first, count;
};

struct DeferredPoly {  // translucent polys and 2D blits, sorted at EndFrame
    float key;
    uint32_t seq;
    GpuState st;
    uint32_t first, count;  // into deferred vertex array
};

struct Renderer {
    void* rc = nullptr;
    HWND hwnd = nullptr;
    LONG oldStyle = 0, oldExStyle = 0;
    HMENU oldMenu = nullptr;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* backRtv = nullptr;
    int outW = 0, outH = 0;

    // scene targets (4:3 area of the output, times render scale)
    int sceneW = 0, sceneH = 0, msaa = 1;
    ID3D11Texture2D* sceneTex = nullptr;
    ID3D11RenderTargetView* sceneRtv = nullptr;
    ID3D11Texture2D* resolveTex = nullptr;
    ID3D11ShaderResourceView* resolveSrv = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT presentVp = {};

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11VertexShader* postVs = nullptr;
    ID3D11PixelShader* postPs = nullptr;
    ID3D11Buffer* frameCb = nullptr;
    ID3D11Buffer* postCb = nullptr;
    ID3D11Buffer* vb = nullptr;
    uint32_t vbCapacity = 0, vbPos = 0;

    ID3D11BlendState* blend[BlendCount] = {};
    ID3D11DepthStencilState* depth[DepthCount] = {};
    ID3D11RasterizerState* raster = nullptr;
    ID3D11SamplerState* sampWrap = nullptr;
    ID3D11SamplerState* sampClamp = nullptr;
    ID3D11SamplerState* sampHud = nullptr;
    ID3D11ShaderResourceView* white = nullptr;

    // per-frame
    std::vector<Vtx> verts;
    std::vector<Batch> batches;
    std::vector<Vtx> defVerts;
    std::vector<DeferredPoly> translucent, blits;
    uint32_t seq = 0;
    bool inSky = false;
    int savedFog = 0;
    int decalLevel = 0;
    float gamma = 1.0f;
    float light[3] = {0.0f, 1.0f, 0.0f};  // overhead light in camera space
    int virtW = 640, virtH = 480;
    bool frameCbDirty = true;
    uint64_t frame = 0;
    LARGE_INTEGER lastPresent = {}, qpcFreq = {};

    std::vector<Pls*> live;  // resident GPU textures, for TextureExit

    // stats
    uint32_t statPolys = 0, statDraws = 0, statUploads = 0;
    uint32_t unknownFlagsLogged = 0;
};

static Renderer* R;

// ============================================================================
// Engine callbacks
// ============================================================================

static void* EAlloc(uint32_t n) { return ((AllocFn)F<void*>(R->rc, rc::CbAlloc))(n); }
static void EError(void* rcp, const char* msg, int fatal) {
    Log("engine error (%d): %s", fatal, msg);
    ((ErrorFn)F<void*>(rcp, rc::CbError))(msg, fatal);
}

// ============================================================================
// D3D helpers
// ============================================================================

static ID3DBlob* Compile(const char* src, size_t len, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, len, "szmodern", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        Log("shader %s/%s failed (0x%08lx): %s", entry, target, hr,
            err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return nullptr;
    }
    SafeRelease(err);
    return code;
}

static void ReleaseSceneTargets() {
    SafeRelease(R->sceneRtv);
    SafeRelease(R->sceneTex);
    SafeRelease(R->resolveSrv);
    SafeRelease(R->resolveTex);
    SafeRelease(R->dsv);
    SafeRelease(R->depthTex);
}

static bool CreateSceneTargets() {
    ReleaseSceneTargets();
    // 4:3 (or widened) area centred in the output (pillarbox / letterbox)
    const float aspect = 4.0f / 3.0f * g_wideK;
    float areaW = (float)R->outW, areaH = (float)R->outH;
    if (areaW > areaH * aspect + 1.0f) areaW = areaH * aspect;
    else if (areaW < areaH * aspect - 1.0f) areaH = areaW / aspect;
    R->presentVp.Width = floorf(areaW);
    R->presentVp.Height = floorf(areaH);
    R->presentVp.TopLeftX = floorf((R->outW - R->presentVp.Width) * 0.5f);
    R->presentVp.TopLeftY = floorf((R->outH - R->presentVp.Height) * 0.5f);
    R->presentVp.MaxDepth = 1.0f;
    R->sceneW = std::max(1, (int)lroundf(R->presentVp.Width * g_cfg.renderScale));
    R->sceneH = std::max(1, (int)lroundf(R->presentVp.Height * g_cfg.renderScale));

    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT samples = 1;
    for (UINT s = (UINT)std::max(1, g_cfg.msaa); s > 1; s >>= 1) {
        UINT q = 0;
        if (SUCCEEDED(R->dev->CheckMultisampleQualityLevels(fmt, s, &q)) && q > 0) { samples = s; break; }
    }
    R->msaa = (int)samples;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = R->sceneW;
    td.Height = R->sceneH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &R->sceneTex))) return false;
    if (FAILED(R->dev->CreateRenderTargetView(R->sceneTex, nullptr, &R->sceneRtv))) return false;
    if (samples > 1) {
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &R->resolveTex))) return false;
    } else {
        R->resolveTex = R->sceneTex;
        R->resolveTex->AddRef();
    }
    if (FAILED(R->dev->CreateShaderResourceView(R->resolveTex, nullptr, &R->resolveSrv))) return false;

    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = samples;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &R->depthTex))) return false;
    if (FAILED(R->dev->CreateDepthStencilView(R->depthTex, nullptr, &R->dsv))) return false;

    Log("output %dx%d, scene %dx%d (widen %.3f, area %gx%g at %g,%g), MSAA %dx", R->outW, R->outH, R->sceneW,
        R->sceneH, g_wideK, R->presentVp.Width, R->presentVp.Height, R->presentVp.TopLeftX, R->presentVp.TopLeftY,
        R->msaa);
    return true;
}

static bool CreateBackbufferView() {
    SafeRelease(R->backRtv);
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(R->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    HRESULT hr = R->dev->CreateRenderTargetView(bb, nullptr, &R->backRtv);
    bb->Release();
    return SUCCEEDED(hr);
}

struct FindWindowCtx {
    DWORD pid;
    HWND found;
};

static BOOL CALLBACK FindWindowProc(HWND w, LPARAM lp) {
    FindWindowCtx* c = (FindWindowCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == c->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        c->found = w;
        return FALSE;
    }
    return TRUE;
}

static HWND FindGameWindow() {
    HWND h = GetActiveWindow();
    if (h) return h;
    FindWindowCtx c = {GetCurrentProcessId(), nullptr};
    EnumWindows(FindWindowProc, (LPARAM)&c);
    return c.found;
}

static void CreateSamplers() {
    SafeRelease(R->sampWrap);
    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = g_cfg.anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.MaxAnisotropy = std::min(std::max(g_cfg.anisotropy, 1), 16);
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    R->dev->CreateSamplerState(&sm, &R->sampWrap);
}

static bool CreateDevice() {
    R->hwnd = FindGameWindow();
    if (!R->hwnd) { Log("no game window found"); return false; }

    // borderless fullscreen on the window's monitor
    HMONITOR mon = MonitorFromWindow(R->hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(mon, &mi);
    int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    R->oldStyle = GetWindowLongA(R->hwnd, GWL_STYLE);
    R->oldExStyle = GetWindowLongA(R->hwnd, GWL_EXSTYLE);
    R->oldMenu = GetMenu(R->hwnd);
    if (R->oldMenu) SetMenu(R->hwnd, nullptr);
    if (g_cfg.windowed) {
        int cw = g_cfg.width > 0 ? g_cfg.width : 1920, ch = g_cfg.height > 0 ? g_cfg.height : 1080;
        cw = std::min(cw, mw);
        ch = std::min(ch, mh);
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        RECT wr = {0, 0, cw, ch};
        AdjustWindowRect(&wr, style, FALSE);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        SetWindowLongA(R->hwnd, GWL_STYLE, style);
        SetWindowLongA(R->hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(R->hwnd, HWND_TOP, mi.rcMonitor.left + (mw - ww) / 2, mi.rcMonitor.top + (mh - wh) / 2, ww, wh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        RECT cr;
        GetClientRect(R->hwnd, &cr);
        R->outW = std::max(1L, cr.right - cr.left);
        R->outH = std::max(1L, cr.bottom - cr.top);
    } else {
        SetWindowLongA(R->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(R->hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(R->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        R->outW = mw;
        R->outH = mh;
    }
    while (ShowCursor(FALSE) >= 0) {}  // the game window's class cursor would otherwise show over the scene
    Log("window %p, monitor %dx%d, output %dx%d", (void*)R->hwnd, mw, mh, R->outW, R->outH);

    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   want, 2, D3D11_SDK_VERSION, &R->dev, &got, &R->ctx);
    if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08lx", hr); return false; }
    Log("D3D11 device, feature level 0x%x", got);

    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    R->dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    dxdev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_ADAPTER_DESC ad;
    if (SUCCEEDED(adapter->GetDesc(&ad))) {
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
        Log("adapter: %s", name);
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = R->outW;
    sd.Height = R->outH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory ? factory->CreateSwapChainForHwnd(R->dev, R->hwnd, &sd, nullptr, nullptr, &R->swap) : E_FAIL;
    if (factory) factory->MakeWindowAssociation(R->hwnd, DXGI_MWA_NO_ALT_ENTER);
    SafeRelease(factory);
    SafeRelease(adapter);
    SafeRelease(dxdev);
    if (FAILED(hr)) { Log("CreateSwapChainForHwnd failed 0x%08lx", hr); return false; }
    if (!CreateBackbufferView() || !CreateSceneTargets()) { Log("render target creation failed"); return false; }

    // shaders
    ID3DBlob* vsb = Compile(kSceneHlsl, sizeof kSceneHlsl - 1, "vs_main", "vs_5_0");
    ID3DBlob* psb = Compile(kSceneHlsl, sizeof kSceneHlsl - 1, "ps_main", "ps_5_0");
    ID3DBlob* pvb = Compile(kPostHlsl, sizeof kPostHlsl - 1, "vs_post", "vs_5_0");
    ID3DBlob* ppb = Compile(kPostHlsl, sizeof kPostHlsl - 1, "ps_post", "ps_5_0");
    if (!pvb || !ppb) {
        Log("filter shader unavailable, using plain output");
        SafeRelease(pvb);
        SafeRelease(ppb);
        pvb = Compile(kPostFallbackHlsl, sizeof kPostFallbackHlsl - 1, "vs_post", "vs_5_0");
        ppb = Compile(kPostFallbackHlsl, sizeof kPostFallbackHlsl - 1, "ps_post", "ps_5_0");
    }
    if (!vsb || !psb || !pvb || !ppb) return false;
    R->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &R->vs);
    R->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &R->ps);
    R->dev->CreateVertexShader(pvb->GetBufferPointer(), pvb->GetBufferSize(), nullptr, &R->postVs);
    R->dev->CreatePixelShader(ppb->GetBufferPointer(), ppb->GetBufferSize(), nullptr, &R->postPs);
    D3D11_INPUT_ELEMENT_DESC ie[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vtx, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vtx, r), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vtx, u0), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, offsetof(Vtx, flags), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vtx, cx), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    R->dev->CreateInputLayout(ie, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &R->layout);
    SafeRelease(vsb);
    SafeRelease(psb);
    SafeRelease(pvb);
    SafeRelease(ppb);
    if (!R->vs || !R->ps || !R->postVs || !R->postPs || !R->layout) { Log("shader object creation failed"); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 48;
    R->dev->CreateBuffer(&bd, nullptr, &R->frameCb);
    bd.ByteWidth = 64;
    R->dev->CreateBuffer(&bd, nullptr, &R->postCb);
    R->vbCapacity = 1u << 20;  // vertices
    bd.ByteWidth = R->vbCapacity * sizeof(Vtx);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(R->dev->CreateBuffer(&bd, nullptr, &R->vb))) { Log("vertex buffer creation failed"); return false; }

    // fixed-function state
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    R->dev->CreateBlendState(&bl, &R->blend[BlendOpaque]);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    R->dev->CreateBlendState(&bl, &R->blend[BlendAlpha]);

    // reversed-Z: larger depth is nearer, cleared to 0
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    R->dev->CreateDepthStencilState(&ds, &R->depth[DepthWrite]);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    R->dev->CreateDepthStencilState(&ds, &R->depth[DepthTestOnly]);
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    R->dev->CreateDepthStencilState(&ds, &R->depth[DepthSky]);
    ds.DepthEnable = FALSE;
    R->dev->CreateDepthStencilState(&ds, &R->depth[DepthOff]);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = FALSE;
    rs.MultisampleEnable = TRUE;
    R->dev->CreateRasterizerState(&rs, &R->raster);

    CreateSamplers();
    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.MaxAnisotropy = 1;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    R->dev->CreateSamplerState(&sm, &R->sampClamp);
    R->dev->CreateSamplerState(&sm, &R->sampHud);

    // 1x1 white for unused texture slots
    uint32_t px = 0xffffffff;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {&px, 4, 4};
    ID3D11Texture2D* wt = nullptr;
    R->dev->CreateTexture2D(&td, &init, &wt);
    R->dev->CreateShaderResourceView(wt, nullptr, &R->white);
    SafeRelease(wt);

    QueryPerformanceFrequency(&R->qpcFreq);
    QueryPerformanceCounter(&R->lastPresent);
    overlay::Init(R->hwnd, R->dev, R->ctx, R->outH);
    return true;
}

// ============================================================================
// Textures
// ============================================================================

static inline uint32_t Expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t Expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static inline uint32_t Expand4(uint32_t v) { return (v << 4) | v; }

enum PixFmt { Fmt565, Fmt1555, Fmt4444, Fmt8888 };

static void ConvertPixels(const void* srcv, uint32_t* dst, uint32_t n, PixFmt fmt) {
    if (fmt == Fmt8888) {  // engine layout 0xAARRGGBB -> RGBA8
        const uint32_t* src = (const uint32_t*)srcv;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t p = src[i];
            dst[i] = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
        }
        return;
    }
    const uint16_t* src = (const uint16_t*)srcv;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t p = src[i], r, g, b, a;
        switch (fmt) {
            case Fmt565:
                r = Expand5(p >> 11); g = Expand6((p >> 5) & 63); b = Expand5(p & 31); a = 255;
                break;
            case Fmt1555:
                a = (p & 0x8000) ? 255 : 0; r = Expand5((p >> 10) & 31); g = Expand5((p >> 5) & 31); b = Expand5(p & 31);
                break;
            default:
                a = Expand4(p >> 12); r = Expand4((p >> 8) & 15); g = Expand4((p >> 4) & 15); b = Expand4(p & 15);
                break;
        }
        dst[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

static inline int Log2i(uint32_t v) {
    int l = 0;
    while ((1u << (l + 1)) <= v) ++l;
    return l;
}

// Byte size of one animation frame (matches the Glide renderer's stride computation).
static uint32_t BytesPerPixel(uint32_t flags) { return (flags & Tf8888) ? 4 : 2; }

static uint32_t FrameBytes(uint32_t w, uint32_t h, uint32_t flags) {
    if (!(flags & TfMips)) return w * h * BytesPerPixel(flags);
    uint32_t total = 0;
    for (int i = Log2i(std::max(w, h)); i >= 0; --i) total += (w >> i) * (h >> i);
    return total * BytesPerPixel(flags);
}

static void PlsTrack(Pls* p) {
    if (p->liveIndex >= 0) return;
    p->liveIndex = (int32_t)R->live.size();
    R->live.push_back(p);
}

static void PlsRelease(Pls* p) {
    if (p->magic != kPlsMagic) return;
    SafeRelease(p->srv);
    SafeRelease(p->tex);
    SafeRelease(p->nrm);
    SafeRelease(p->nrmTex);
    if (p->liveIndex >= 0 && R) {
        uint32_t i = (uint32_t)p->liveIndex;
        if (i < R->live.size() && R->live[i] == p) {
            R->live[i] = R->live.back();
            R->live[i]->liveIndex = (int32_t)i;
            R->live.pop_back();
        }
    }
    p->liveIndex = -1;
}

static void PlsInit(Pls* p, bool isLightmap, const void* owner, uint32_t frame) {
    memset(p, 0, kPlsStride);
    p->magic = kPlsMagic;
    p->isLightmap = isLightmap;
    p->owner = owner;
    p->frame = frame;
    p->liveIndex = -1;
}

static const char* TexName(const void* t);

// Loads <game>/szmodern/textures/<file>.png into an RGBA8 texture with a full mip chain.
static bool LoadPackPng(const char* file, ID3D11Texture2D** texOut, ID3D11ShaderResourceView** srvOut, uint32_t* w, uint32_t* h) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\szmodern\\textures\\%s.png", g_gameDir, file);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;
    int iw = 0, ih = 0, comp = 0;
    stbi_uc* px = stbi_load(path, &iw, &ih, &comp, 4);
    if (!px) { Log("pack: cannot decode %s", path); return false; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = iw;
    td.Height = ih;
    td.MipLevels = Log2i(std::max(iw, ih)) + 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    bool ok = SUCCEEDED(R->dev->CreateTexture2D(&td, nullptr, texOut));
    if (ok) {
        R->ctx->UpdateSubresource(*texOut, 0, nullptr, px, iw * 4, 0);
        ok = SUCCEEDED(R->dev->CreateShaderResourceView(*texOut, nullptr, srvOut));
        if (ok) R->ctx->GenerateMips(*srvOut);
    }
    stbi_image_free(px);
    if (w) *w = iw;
    if (h) *h = ih;
    return ok;
}

static bool UploadFromPack(const void* t, Pls* p) {
    if (!g_cfg.texturePack) return false;
    char name[64];
    snprintf(name, sizeof name, "%s", TexName(t));
    if (!name[0]) return false;
    for (char* c = name; *c; ++c) *c = (char)tolower((unsigned char)*c);
    char file[80];
    if (F<uint16_t>(t, tex::Frames) > 1) snprintf(file, sizeof file, "%s_f%u", name, p->frame);
    else snprintf(file, sizeof file, "%s", name);
    if (!LoadPackPng(file, &p->tex, &p->srv, &p->w, &p->h)) {
        SafeRelease(p->srv);
        SafeRelease(p->tex);
        return false;
    }
    if (g_cfg.normalMaps) {
        snprintf(file, sizeof file, "%s_n", name);
        if (!LoadPackPng(file, &p->nrmTex, &p->nrm, nullptr, nullptr)) { SafeRelease(p->nrm); SafeRelease(p->nrmTex); }
    }
    PlsTrack(p);
    ++R->statUploads;
    return true;
}

static bool UploadTexture(const void* t, Pls* p) {
    if (UploadFromPack(t, p)) return true;
    uint32_t w = F<uint32_t>(t, tex::Width), h = F<uint32_t>(t, tex::Height);
    uint32_t flags = F<uint32_t>(t, tex::Flags);
    const uint8_t* data = F<const uint8_t*>(t, tex::Data);
    if (!data || w == 0 || h == 0 || w > 4096 || h > 4096) {
        Log("bad texture %p: %ux%u flags %x data %p", t, w, h, flags, data);
        return false;
    }
    const uint8_t* src = data + FrameBytes(w, h, flags) * p->frame;
    PixFmt fmt = (flags & Tf8888) ? Fmt8888 : (flags & Tf1555) ? Fmt1555 : (flags & Tf4444) ? Fmt4444 : Fmt565;
    const uint32_t bpp = BytesPerPixel(flags);

    int provided = 1;
    if (flags & TfMips) provided = Log2i(std::min(w, h)) + 1;
    bool gen = g_cfg.generateMips && provided == 1 && (w > 1 || h > 1);
    int levels = gen ? Log2i(std::max(w, h)) + 1 : provided;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = levels;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (gen ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = gen ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &p->tex))) return false;
    std::vector<uint32_t> buf(w * h);
    uint32_t lw = w, lh = h;
    for (int l = 0; l < provided; ++l) {
        ConvertPixels(src, buf.data(), lw * lh, fmt);
        R->ctx->UpdateSubresource(p->tex, l, nullptr, buf.data(), lw * 4, 0);
        src += lw * lh * bpp;
        lw = std::max(1u, lw >> 1);
        lh = std::max(1u, lh >> 1);
    }
    R->dev->CreateShaderResourceView(p->tex, nullptr, &p->srv);
    if (gen && p->srv) R->ctx->GenerateMips(p->srv);
    p->w = w;
    p->h = h;
    PlsTrack(p);
    ++R->statUploads;
    return p->srv != nullptr;
}

static void FlushOpaque();

static bool UploadLightmap(const void* l, Pls* p, const void* pixels) {
    uint8_t dims = F<uint8_t>(l, lm::Dims);
    uint32_t w = 1u << (dims >> 4), h = 1u << (dims & 15);
    if (!pixels || w > 1024 || h > 1024) return false;
    std::vector<uint32_t> buf(w * h);
    ConvertPixels((const uint16_t*)pixels, buf.data(), w * h, Fmt1555);
    if (!p->tex || p->w != w || p->h != h) {
        PlsRelease(p);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &p->tex))) return false;
        R->dev->CreateShaderResourceView(p->tex, nullptr, &p->srv);
        p->w = w;
        p->h = h;
        PlsTrack(p);
    } else {
        FlushOpaque();  // earlier queued draws must keep the previous contents
    }
    R->ctx->UpdateSubresource(p->tex, 0, nullptr, buf.data(), w * 4, 0);
    ++R->statUploads;
    return true;
}

static const char* TexName(const void* t) {
    const char* n = (const char*)t;
    for (int i = 0; i < 32; ++i) {
        if (n[i] == 0) return i ? n : "";
        if ((unsigned char)n[i] < 32 || (unsigned char)n[i] > 126) return "";
    }
    return "";
}

// Texture SRV for a primitive / blit, resolving animation frames.
static ID3D11ShaderResourceView* TextureSrv(const void* t, const void* frameIdx, bool advance, Pls** plsOut = nullptr) {
    if (!t) return nullptr;
    static bool logTex = GetEnvironmentVariableA("SZM_LOGTEX", nullptr, 0) > 0;
    if (logTex) {
        Pls* p0 = (Pls*)F<uint8_t*>(t, tex::Pls);
        if (p0 && p0->magic == kPlsMagic && !p0->srv)
            Log("tex %-32s %s %ux%u flags %08x frames %u", TexName(t), advance ? "world" : "blit ",
                F<uint32_t>(t, tex::Width), F<uint32_t>(t, tex::Height), F<uint32_t>(t, tex::Flags), F<uint16_t>(t, tex::Frames));
    }
    uint8_t* plsArr = F<uint8_t*>(t, tex::Pls);
    if (!plsArr) return nullptr;
    uint32_t frames = F<uint16_t>(t, tex::Frames);
    uint32_t frame = 0;
    if (frames > 1 && frameIdx) {
        if (advance) ((AnimFn)F<void*>(R->rc, rc::CbAnimTexture))((void*)frameIdx, frames);
        frame = *(const uint16_t*)frameIdx;
        if (frame >= frames) frame = 0;
    }
    Pls* p = (Pls*)(plsArr + frame * kPlsStride);
    if (p->magic != kPlsMagic) return nullptr;
    if (!p->srv && !UploadTexture(t, p)) return nullptr;
    if (plsOut) *plsOut = p;
    return p->srv;
}

// Lightmap SRV, following the Glide renderer's dynamic-lighting protocol.
static ID3D11ShaderResourceView* LightmapSrv(const void* pr) {
    uint8_t* l = F<uint8_t*>(pr, prim::Lightmap);
    if (!l) return nullptr;
    Pls* p = F<Pls*>(l, lm::Pls);
    if (!p || p->magic != kPlsMagic) return nullptr;
    int16_t dyn = F<int16_t>(pr, prim::LmDynamic);
    uint8_t& lflags = F<uint8_t>(l, lm::Flags);
    if (dyn != 0) {
        lflags |= LfDynamicShown;
        UploadLightmap(l, p, F<void*>(pr, prim::LmDynData));
    } else if (lflags & LfDynamicShown) {
        lflags &= ~LfDynamicShown;
        UploadLightmap(l, p, F<void*>(l, lm::Data));
    } else if (!p->srv) {
        UploadLightmap(l, p, F<void*>(l, lm::Data));
    }
    return p->srv;
}

// ============================================================================
// Drawing
// ============================================================================

static void UpdateFrameCb() {
    if (!R->frameCbDirty) return;
    // virtual pixel -> NDC: x' = (x - cx) * ax, y' = (cy - y) * ay; the 640x480 virtual
    // screen occupies the centre 4:3 of a wider scene when widescreen is on
    struct { float ax, ay, cx, cy, zn, biasStep, lmScale, alphaRef, lx, ly, lz, pad; } cb = {
        2.0f / (R->virtW * g_wideK), 2.0f / R->virtH, R->virtW * 0.5f, R->virtH * 0.5f,
        1.0f / 65536.0f, g_cfg.decalBias, g_cfg.lightmapScale, 0.5f / 255.0f, R->light[0], R->light[1], R->light[2], 0};
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(R->ctx->Map(R->frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof cb);
        R->ctx->Unmap(R->frameCb, 0);
    }
    R->frameCbDirty = false;
}

static void BindScenePipeline() {
    R->ctx->OMSetRenderTargets(1, &R->sceneRtv, R->dsv);
    D3D11_VIEWPORT vp = {0, 0, (float)R->sceneW, (float)R->sceneH, 0, 1};
    R->ctx->RSSetViewports(1, &vp);
    R->ctx->RSSetState(R->raster);
    R->ctx->IASetInputLayout(R->layout);
    R->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vtx), off = 0;
    R->ctx->IASetVertexBuffers(0, 1, &R->vb, &stride, &off);
    R->ctx->VSSetShader(R->vs, nullptr, 0);
    R->ctx->PSSetShader(R->ps, nullptr, 0);
    UpdateFrameCb();
    R->ctx->VSSetConstantBuffers(0, 1, &R->frameCb);
    R->ctx->PSSetConstantBuffers(0, 1, &R->frameCb);
}

// Upload vertices and issue the given batches.
static void Submit(const Vtx* v, size_t nv, const Batch* b, size_t nb) {
    if (!nv || !nb) return;
    if (nv > R->vbCapacity) { Log("vertex overflow (%u)", (unsigned)nv); nv = R->vbCapacity; }
    D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (R->vbPos + nv > R->vbCapacity) { R->vbPos = 0; mapType = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(R->ctx->Map(R->vb, 0, mapType, 0, &m))) return;
    memcpy((Vtx*)m.pData + R->vbPos, v, nv * sizeof(Vtx));
    R->ctx->Unmap(R->vb, 0);
    BindScenePipeline();
    GpuState cur = {};
    bool first = true;
    for (size_t i = 0; i < nb; ++i) {
        const GpuState& s = b[i].st;
        if (first || !(s == cur)) {
            ID3D11ShaderResourceView* srv[2] = {s.t0 ? s.t0 : R->white, s.t1 ? s.t1 : R->white};
            R->ctx->PSSetShaderResources(0, 2, srv);
            ID3D11SamplerState* smp[2] = {s.samp == SampHud ? R->sampHud : R->sampWrap, R->sampClamp};
            R->ctx->PSSetSamplers(0, 2, smp);
            R->ctx->OMSetBlendState(R->blend[s.blend], nullptr, 0xffffffff);
            R->ctx->OMSetDepthStencilState(R->depth[s.depth], 0);
            cur = s;
            first = false;
        }
        if (b[i].first + b[i].count <= nv) R->ctx->Draw(b[i].count, R->vbPos + b[i].first);
        ++R->statDraws;
    }
    R->vbPos += (uint32_t)nv;
}

static void FlushOpaque() {
    Submit(R->verts.data(), R->verts.size(), R->batches.data(), R->batches.size());
    R->verts.clear();
    R->batches.clear();
}

static void Emit(std::vector<Vtx>& out, const Vtx* poly, uint32_t n, uint32_t start) {
    // fan triangulation from vertex `start`, exactly like the original renderers
    uint32_t i1 = start + 1, i2;
    if (i1 < n) { i2 = start + 2; if (i2 >= n) i2 = 0; }
    else { i1 = 0; i2 = 1; }
    for (uint32_t t = 0; t + 2 < n; ++t) {
        out.push_back(poly[start]);
        out.push_back(poly[i1]);
        out.push_back(poly[i2]);
        i1 = i2;
        if (++i2 >= n) i2 = 0;
    }
}

static void QueueOpaque(const GpuState& st, const Vtx* poly, uint32_t n, uint32_t start) {
    if (R->verts.size() + n * 3 > R->vbCapacity / 2) FlushOpaque();
    uint32_t first = (uint32_t)R->verts.size();
    Emit(R->verts, poly, n, start);
    uint32_t cnt = (uint32_t)R->verts.size() - first;
    if (!R->batches.empty() && R->batches.back().st == st && R->batches.back().first + R->batches.back().count == first)
        R->batches.back().count += cnt;
    else
        R->batches.push_back({st, first, cnt});
}

static void QueueDeferred(std::vector<DeferredPoly>& list, float key, const GpuState& st, const Vtx* poly, uint32_t n,
                          uint32_t start) {
    uint32_t first = (uint32_t)R->defVerts.size();
    Emit(R->defVerts, poly, n, start);
    list.push_back({key, R->seq++, st, first, (uint32_t)R->defVerts.size() - first});
}

static void DrawDeferred(std::vector<DeferredPoly>& list) {
    if (list.empty()) return;
    std::stable_sort(list.begin(), list.end(), [](const DeferredPoly& a, const DeferredPoly& b) { return a.key < b.key; });
    std::vector<Vtx> v;
    std::vector<Batch> b;
    v.reserve(R->defVerts.size());
    for (const DeferredPoly& d : list) {
        uint32_t first = (uint32_t)v.size();
        v.insert(v.end(), R->defVerts.begin() + d.first, R->defVerts.begin() + d.first + d.count);
        if (!b.empty() && b.back().st == d.st) b.back().count += d.count;
        else b.push_back({d.st, first, d.count});
        if (v.size() > R->vbCapacity / 2) {
            Submit(v.data(), v.size(), b.data(), b.size());
            v.clear();
            b.clear();
        }
    }
    Submit(v.data(), v.size(), b.data(), b.size());
    list.clear();
}

static uint32_t AlphaBits(uint32_t pf, bool textured) {
    if (!(pf & PfTranslucent)) return AlphaOne | (textured && g_cfg.opaqueCutout ? FlagCutout : 0);
    bool texAlpha = (pf & (PfTexAlphaA | PfTexAlphaB)) != 0;
    uint32_t a;
    if (pf & PfConstAlpha) a = texAlpha ? AlphaTexConst : AlphaConst;
    else a = AlphaTex;
    if (!textured) a = (pf & PfConstAlpha) ? AlphaConst : AlphaOne;
    return a | FlagAlphaTest;
}

// ============================================================================
// hwAPI exports
// ============================================================================

EXPORT int hwAPI_EnumerateDisplayModes(uint8_t* modes, int* count) {
    if (!modes || !count || *count >= 30) return 0;
    uint8_t* m = modes + (uint32_t)*count * mode::Size;
    memset(m, 0, mode::Size);
    snprintf((char*)m + mode::Renderer, 256, "%s", g_dllName);
    snprintf((char*)m + mode::Menu, 256, "Modern");
    snprintf((char*)m + mode::Device, 256, "Slave Zero Modern Renderer");
    F<int>(m, mode::Index) = 0;
    F<int>(m, mode::Width) = 640;
    F<int>(m, mode::Height) = 480;
    F<int>(m, mode::Bpp) = 16;
    ++*count;
    return 1;
}

EXPORT int hwAPI_StartUp(void* rcp) {
    if (g_cfg.log && !g_log) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\szModern.log", g_gameDir);
        g_log = fopen(path, "w");
    }
    Log("Slave Zero modern renderer starting (%s), widescreen factor %.3f", g_dllName, g_wideK);
    crash::LogSystemInfo();
    crash::Install();  // again: the game may have set its own filter since the DLL loaded
    void* o = F<void*>(rcp, rc::Opt);
    void* pp = F<void*>(rcp, rc::Persp);
    Log("mode %dx%d, persp %dx%d, lightmaps %d, vsync %d", F<int>(o, opt::ModeWidth), F<int>(o, opt::ModeHeight),
        pp ? F<int>(pp, persp::Width) : -1, pp ? F<int>(pp, persp::Height) : -1, F<int>(o, opt::Lightmaps),
        F<int>(o, opt::VSync));

    R = new Renderer();
    R->rc = rcp;
    F<void*>(rcp, rc::Priv) = R;
    if (!CreateDevice()) {
        Log("startup failed");
        F<void*>(rcp, rc::Priv) = nullptr;
        delete R;
        R = nullptr;
        EError(rcp, "hwAPI_StartUp: modern renderer could not initialise Direct3D 11 (see szModern.log)", 1);
        return 0;
    }
    int mw = F<int>(o, opt::ModeWidth), mh = F<int>(o, opt::ModeHeight);
    R->virtW = mw > 0 ? mw : 640;
    R->virtH = mh > 0 ? mh : 480;
    R->gamma = F<float>(rcp, rc::Gamma);
    Log("startup ok, virtual screen %dx%d", R->virtW, R->virtH);
    return 1;
}

EXPORT int hwAPI_ShutDown(void* rcp) {
    if (!rcp || !R) return 0;
    Log("shutdown after %llu frames", (unsigned long long)R->frame);
    overlay::Shutdown();
    for (Pls* p : R->live) { SafeRelease(p->srv); SafeRelease(p->tex); p->liveIndex = -1; }
    R->live.clear();
    if (R->ctx) R->ctx->ClearState();
    ReleaseSceneTargets();
    SafeRelease(R->white);
    SafeRelease(R->sampHud);
    SafeRelease(R->sampClamp);
    SafeRelease(R->sampWrap);
    SafeRelease(R->raster);
    for (auto& d : R->depth) SafeRelease(d);
    for (auto& b : R->blend) SafeRelease(b);
    SafeRelease(R->vb);
    SafeRelease(R->postCb);
    SafeRelease(R->frameCb);
    SafeRelease(R->layout);
    SafeRelease(R->postPs);
    SafeRelease(R->postVs);
    SafeRelease(R->ps);
    SafeRelease(R->vs);
    SafeRelease(R->backRtv);
    SafeRelease(R->swap);
    SafeRelease(R->ctx);
    SafeRelease(R->dev);
    while (ShowCursor(TRUE) < 0) {}
    if (R->hwnd && IsWindow(R->hwnd)) {
        SetWindowLongA(R->hwnd, GWL_STYLE, R->oldStyle);
        SetWindowLongA(R->hwnd, GWL_EXSTYLE, R->oldExStyle);
        if (R->oldMenu) SetMenu(R->hwnd, R->oldMenu);
    }
    delete R;
    R = nullptr;
    F<void*>(rcp, rc::Priv) = nullptr;
    return 1;
}

EXPORT int hwAPI_ClearViewSurface(void* rcp, uint32_t color) {
    if (!R) return 0;
    FlushOpaque();
    float c[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, 1.0f};
    R->ctx->ClearRenderTargetView(R->sceneRtv, c);
    R->ctx->ClearDepthStencilView(R->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    return 1;
}

EXPORT void hwAPI_BeginFrame(void* rcp) {
    if (!R) return;
    R->decalLevel = 0;
    bool wasMenu = g_inMenu;
    UpdateLevelState();
    if (wasMenu != g_inMenu) Log("level state: %s", g_inMenu ? "front end" : "mission");
    // overhead light direction in camera space, via the engine's own camera transform
    typedef void*(__cdecl * GetCameraFn)();
    typedef void(__cdecl * XformFn)(const float*, float*, void*);
    GetCameraFn getCam = F<GetCameraFn>(rcp, 0x274);
    XformFn xform = F<XformFn>(rcp, 0x270);
    if (getCam && xform) {
        if (void* cam = getCam()) {
            const float sky[3] = {0.35e6f, 1.0e6f, 0.25e6f};
            float o[3];
            xform(sky, o, cam);
            float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len > 0.0f) {
                R->light[0] = o[0] / len;
                R->light[1] = o[1] / len;
                R->light[2] = o[2] / len;
                R->frameCbDirty = true;
            }
        }
    }
}

EXPORT void hwAPI_ProcessBatchOfPrimatives(void* rcp, uint8_t* pr, int /*unused*/, int /*flush*/) {
    if (!R || !pr) return;
    uint32_t n = F<uint16_t>(pr, prim::Count);
    if (n < 3 || n > 64) return;
    uint32_t start = F<uint16_t>(pr, prim::FanStart);
    if (start >= n) start = 0;
    uint32_t pf = F<uint32_t>(pr, prim::Flags);
    uint32_t pf2 = F<uint32_t>(pr, prim::Flags2);
    bool translucent = (pf & PfTranslucent) != 0;

    uint32_t mode;
    if (pf & PfTexture) mode = ModeTex;
    else if (pf & PfLightmapped) mode = ModeTexLM;
    else if (pf & PfTextureLit) mode = ModeTexVC;
    else if (pf & PfVertexColor) mode = ModeVC;
    else {
        if (R->unknownFlagsLogged++ < 16) Log("unknown primitive flags %08x", pf);
        return;
    }
    if (translucent && (mode == ModeVC || mode == ModeTexLM)) {
        if (R->unknownFlagsLogged++ < 16) Log("translucent flags on mode %u (%08x)", mode, pf);
        mode = mode == ModeTexLM ? ModeTexVC : mode;
    }

    void* t = F<void*>(pr, prim::Tex);
    GpuState st = {};
    st.samp = SampWorld;
    Pls* texPls = nullptr;
    if (mode != ModeVC) {
        st.t0 = TextureSrv(t, F<void*>(pr, prim::FrameIdx), true, &texPls);
        if (!st.t0) return;
    }
    if (mode == ModeTexLM) {
        void* o = F<void*>(rcp, rc::Opt);
        if (F<int>(o, opt::Lightmaps) && F<int>(rcp, rc::ShadowMode) != 1) st.t1 = LightmapSrv(pr);
        if (!st.t1) mode = ModeTex;
    }

    float bias = 0.0f;
    if (pf2 & P2Decal) bias = (float)(++R->decalLevel);

    uint32_t flags = mode | AlphaBits(pf, mode != ModeVC);
    if (texPls && texPls->nrm && (mode == ModeTex || mode == ModeTexVC)) {
        st.t1 = texPls->nrm;
        flags |= FlagNormal;
    }
    float ca = (pf & PfConstAlpha) ? (pf & 0xff) / 255.0f : 1.0f;

    void* pp = F<void*>(rcp, rc::Persp);
    const float sX = F<float>(pp, persp::ScaleX), sY = F<float>(pp, persp::ScaleY);
    const float cX = F<float>(pp, persp::CenterX) + F<float>(rcp, rc::OffX);
    const float cY = F<float>(pp, persp::HeightF) - F<float>(pp, persp::CenterY) + F<float>(rcp, rc::OffY);

    Vtx poly[64];
    uint8_t** vp = F<uint8_t**>(pr, prim::Verts);
    float oowSum = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* v = vp[i];
        Vtx& o = poly[i];
        float oow = F<float>(v, vtx::Oow);
        o.x = oow * F<float>(v, vtx::X) * sX + cX;
        o.y = cY - oow * F<float>(v, vtx::Y) * sY;
        o.oow = oow;
        o.bias = bias;
        o.r = F<float>(v, vtx::R);
        o.g = F<float>(v, vtx::G);
        o.b = F<float>(v, vtx::B);
        o.a = ca;
        o.u0 = F<float>(v, vtx::U);
        o.v0 = F<float>(v, vtx::V);
        o.u1 = F<float>(v, vtx::LU);
        o.v1 = F<float>(v, vtx::LV);
        o.flags = flags;
        o.cx = F<float>(v, vtx::X);
        o.cy = F<float>(v, vtx::Y);
        o.cz = F<float>(v, vtx::Z);
        oowSum += oow;
    }
    ++R->statPolys;

    if (translucent) {
        st.blend = BlendAlpha;
        st.depth = R->inSky ? DepthSky : DepthTestOnly;
        float key = (pf2 & P2Decal) ? 0.0f : oowSum / n;
        if (pf2 & P2SortEarly) key -= 5000.0f;
        QueueDeferred(R->translucent, key, st, poly, n, start);
    } else {
        st.blend = BlendOpaque;
        st.depth = R->inSky ? DepthSky : DepthWrite;
        QueueOpaque(st, poly, n, start);
    }
}

EXPORT int hwAPI_BlitPoly(void* rcp, void* t, void* frameIdx, float x, float y, float depth, float w, float h,
                          float alpha, float u0, float v0, float u1, float v1, int lit, float r, float g, float b) {
    if (!R) return 0;
    GpuState st = {};
    st.t0 = TextureSrv(t, frameIdx, false);
    if (!st.t0) return 1;
    st.blend = BlendAlpha;
    st.depth = DepthOff;
    st.samp = SampHud;

    void* pp = F<void*>(rcp, rc::Persp);
    float W = F<float>(pp, persp::WidthF), H = F<float>(pp, persp::HeightF);
    float px = x * W, py = y * H, pw = w * W, ph = h * H * (4.0f / 3.0f);
    if (g_wideK > 1.0f && x <= 0.002f && x + w >= 0.998f) {
        // full-screen overlays (fades, backdrops) span the whole widescreen area
        px = W * 0.5f * (1.0f - g_wideK);
        pw = W * g_wideK;
    } else if (g_wideK > 1.0f && g_cfg.hudEdges && !g_inMenu) {
        // corner HUD (gauges and their numbers) moves out to the real screen edges;
        // reticules and lock-on markers stay put because they track 3D positions
        const char* n = TexName(t);
        bool hud = _strnicmp(n, "i_hud", 5) == 0;
        bool glyph = _strnicmp(n, "i_font", 6) == 0 && y > 0.70f;
        float cx = x + w * 0.5f;
        if ((hud || glyph) && (cx < 0.25f || cx > 0.75f)) px += (cx < 0.5f ? -1.0f : 1.0f) * W * 0.5f * (g_wideK - 1.0f);
    }
    uint32_t tf = F<uint32_t>(t, tex::Flags);
    uint32_t pf = PfConstAlpha | (tf & TfAlpha ? PfTexAlphaA : 0) | (tf & TfAlpha2 ? PfTexAlphaB : 0);
    uint32_t flags = (lit ? ModeTexVC : ModeTex) | AlphaBits(pf, true);
    float a = std::min(std::max(alpha, 0.0f), 1.0f);
    a = floorf(a * 255.0f + 0.5f) / 255.0f;

    Vtx q[4];
    const float pos[4][2] = {{px, py}, {px, py + ph}, {px + pw, py + ph}, {px + pw, py}};
    const float uv[4][2] = {{u0, v1}, {u0, v0}, {u1, v0}, {u1, v1}};
    for (int i = 0; i < 4; ++i) {
        q[i] = {pos[i][0], pos[i][1], 1.0f, 0.0f, r, g, b, a, uv[i][0], uv[i][1], 0, 0, flags, 0, 0, 1};
    }
    QueueDeferred(R->blits, depth != 0.0f ? 1.0f / depth : 0.0f, st, q, 4, 0);
    return 1;
}

EXPORT void hwAPI_EndFrame(void* rcp) {
    if (!R) return;
    FlushOpaque();
    DrawDeferred(R->translucent);
    DrawDeferred(R->blits);
    R->defVerts.clear();
}

static void SendKey(int vk, bool up) {
    INPUT in = {};
    if (vk == VK_LBUTTON || vk == VK_RBUTTON) {
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = vk == VK_LBUTTON ? (up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN)
                                         : (up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN);
        SendInput(1, &in, sizeof in);
        return;
    }
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, 0 /*MAPVK_VK_TO_VSC*/);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}

static void RunKeySchedule(uint64_t frame) {
    for (const Config::Key& k : g_cfg.keys) {
        if ((uint64_t)k.frame == frame) {
            SetForegroundWindow(R->hwnd);
            SendKey(k.vk, false);
            Log("test key %02x down at frame %llu", k.vk, (unsigned long long)frame);
        } else if ((uint64_t)(k.frame + k.hold) == frame) {
            SendKey(k.vk, true);
        }
    }
}

static void WriteShot(const char* path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(R->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(R->dev->CreateTexture2D(&td, nullptr, &st))) {
        R->ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(R->ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = fopen(path, "wb");
            if (f) {
                uint32_t rowBytes = td.Width * 3, pad = (4 - rowBytes % 4) % 4, img = (rowBytes + pad) * td.Height;
                uint8_t hdr[54] = {'B', 'M'};
                *(uint32_t*)(hdr + 2) = 54 + img;
                *(uint32_t*)(hdr + 10) = 54;
                *(uint32_t*)(hdr + 14) = 40;
                *(int32_t*)(hdr + 18) = td.Width;
                *(int32_t*)(hdr + 22) = -(int32_t)td.Height;
                *(uint16_t*)(hdr + 26) = 1;
                *(uint16_t*)(hdr + 28) = 24;
                *(uint32_t*)(hdr + 34) = img;
                fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> row(rowBytes + pad, 0);
                for (uint32_t y = 0; y < td.Height; ++y) {
                    const uint8_t* s = (const uint8_t*)m.pData + y * m.RowPitch;
                    for (uint32_t x = 0; x < td.Width; ++x) memcpy(&row[x * 3], s + x * 4, 3);  // BGRA -> BGR
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
                Log("wrote %s", path);
            }
            R->ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

EXPORT int hwAPI_ShowViewSurface(void* rcp) {
    if (!R) return 0;
    FlushOpaque();

    // window may have been resized by the game or the compositor
    RECT cr;
    if (GetClientRect(R->hwnd, &cr)) {
        int w = cr.right - cr.left, h = cr.bottom - cr.top;
        if (w > 0 && h > 0 && (w != R->outW || h != R->outH)) {
            SafeRelease(R->backRtv);
            R->ctx->ClearState();
            if (SUCCEEDED(R->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
                R->outW = w;
                R->outH = h;
                CreateSceneTargets();
            }
            CreateBackbufferView();
            Log("resized output to %dx%d", w, h);
        }
    }

    if (R->msaa > 1) R->ctx->ResolveSubresource(R->resolveTex, 0, R->sceneTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D11ShaderResourceView* nullSrv[2] = {};
    R->ctx->PSSetShaderResources(0, 2, nullSrv);
    float black[4] = {0, 0, 0, 1};
    R->ctx->OMSetRenderTargets(1, &R->backRtv, nullptr);
    R->ctx->ClearRenderTargetView(R->backRtv, black);
    R->ctx->RSSetViewports(1, &R->presentVp);
    R->ctx->OMSetBlendState(R->blend[BlendOpaque], nullptr, 0xffffffff);
    R->ctx->OMSetDepthStencilState(R->depth[DepthOff], 0);
    R->ctx->IASetInputLayout(nullptr);
    R->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    R->ctx->VSSetShader(R->postVs, nullptr, 0);
    R->ctx->PSSetShader(R->postPs, nullptr, 0);
    R->gamma = F<float>(rcp, rc::Gamma);
    float g = g_cfg.gammaOverride > 0 ? g_cfg.gammaOverride : R->gamma;
    if (!(g > 0.1f)) g = 1.0f;
    struct {
        float invGamma, sharpen, tx, ty;
        float outW, outH, outX, outY;
        float mode, strength, lines, time;
        float virtW, virtH, pad[2];
    } pc = {1.0f / g, g_cfg.filter == 0 ? g_cfg.sharpen : g_cfg.sharpen * 0.5f, 1.0f / R->sceneW, 1.0f / R->sceneH,
            R->presentVp.Width, R->presentVp.Height, R->presentVp.TopLeftX, R->presentVp.TopLeftY,
            (float)g_cfg.filter, g_cfg.filterStrength, (float)g_cfg.scanlines, (float)(R->frame % 100000) / 60.0f,
            R->virtW * g_wideK, (float)R->virtH,
            {(g_inMenu && g_cfg.menuPillarbox && g_wideK > 1.0f) ? 0.5f / g_wideK : 0.0f, 0}};
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(R->ctx->Map(R->postCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &pc, sizeof pc);
        R->ctx->Unmap(R->postCb, 0);
    }
    R->ctx->PSSetConstantBuffers(0, 1, &R->postCb);
    R->ctx->PSSetShaderResources(0, 1, &R->resolveSrv);
    R->ctx->PSSetSamplers(0, 1, &R->sampClamp);
    R->ctx->Draw(3, 0);
    R->ctx->PSSetShaderResources(0, 2, nullSrv);

    ++R->frame;
    RunKeySchedule(R->frame);
    // F9 cycles the screen filter while playing
    static bool f9Down = false;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && GetForegroundWindow() == R->hwnd && !overlay::IsOpen();
    if (f9 && !f9Down) {
        static const char* names[] = {"off", "scanlines", "CRT", "CRT curved", "retro 640x480", "VHS"};
        g_cfg.filter = (g_cfg.filter + 1) % 6;
        Log("filter: %s", names[g_cfg.filter]);
    }
    f9Down = f9;
    {
        D3D11_VIEWPORT full = {0, 0, (float)R->outW, (float)R->outH, 0, 1};
        R->ctx->RSSetViewports(1, &full);
        overlay::Render(R->backRtv, R->outW, R->outH, &F<float>(rcp, rc::Gamma), g_inMenu);
    }
    for (int f : g_cfg.shotFrames) {
        if ((uint64_t)f == R->frame) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\szm_shot_%05d.bmp", g_gameDir, f);
            WriteShot(path);
        }
    }

    if (g_cfg.frameLimit > 0) {
        LARGE_INTEGER now;
        const double target = 1.0 / g_cfg.frameLimit;
        for (;;) {
            QueryPerformanceCounter(&now);
            double dt = double(now.QuadPart - R->lastPresent.QuadPart) / R->qpcFreq.QuadPart;
            if (dt >= target) break;
            if (target - dt > 0.002) Sleep(1);
        }
    }
    void* o = F<void*>(rcp, rc::Opt);
    int vsync = g_cfg.vsync >= 0 ? g_cfg.vsync : F<int>(o, opt::VSync);
    HRESULT hr = R->swap->Present(vsync ? 1 : 0, 0);
    QueryPerformanceCounter(&R->lastPresent);
    if (FAILED(hr)) {
        static bool removedLogged = false;
        if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && !removedLogged) {
            removedLogged = true;  // GPU driver crashed or reset (TDR): the game can't recover from this
            Log("Present failed 0x%08lx: GPU device lost, reason 0x%08lx, frame %llu", hr,
                R->dev->GetDeviceRemovedReason(), (unsigned long long)R->frame);
        } else if (R->frame % 600 == 1) {
            Log("Present failed 0x%08lx", hr);
        }
    }
    crash::NoteFrame(R->frame);
    if (g_testCrashFrame > 0 && R->frame >= (uint64_t)g_testCrashFrame) {
        Log("SZM_TEST_CRASH reached, crashing on purpose");
        *(volatile int*)(uintptr_t)0x10 = 1;
    }
    if (unsigned apply = overlay::TakeApply()) {  // settings changed in the overlay
        if (apply & overlay::ApplyTargets) {
            R->ctx->ClearState();
            CreateSceneTargets();
        }
        if (apply & overlay::ApplySamplers) CreateSamplers();
        R->frameCbDirty = true;
    }

    if (R->frame % 600 == 0) {
        static LARGE_INTEGER lastStat;
        double secs = lastStat.QuadPart ? double(R->lastPresent.QuadPart - lastStat.QuadPart) / R->qpcFreq.QuadPart : 0;
        lastStat = R->lastPresent;
        Log("frame %llu: %.1f fps, %u polys, %u draws, %u uploads, %u live textures", (unsigned long long)R->frame,
            secs > 0 ? 600.0 / secs : 0.0, R->statPolys, R->statDraws, R->statUploads, (unsigned)R->live.size());
    }
    R->statPolys = R->statDraws = R->statUploads = 0;
    if (g_cfg.exitFrame > 0 && R->frame >= (uint64_t)g_cfg.exitFrame) {
        Log("SZM_EXIT reached, exiting");
        if (g_log) fflush(g_log);
        ExitProcess(0);
    }
    return 1;
}

EXPORT int hwAPI_ReadFrameBuffer(void* rcp) {
    if (!R) return 0;
    void* pp = F<void*>(rcp, rc::Persp);
    int w = F<int>(pp, persp::Width), h = F<int>(pp, persp::Height);
    if (w <= 0 || h <= 0) return 0;
    uint16_t* out = (uint16_t*)EAlloc(w * h * 2);
    F<void*>(rcp, rc::ReadFb) = out;
    if (!out) {
        EError(rcp, "hwAPI_ReadFrameBuffer: MemAlloc failed", 1);
        return 0;
    }
    memset(out, 0, w * h * 2);
    FlushOpaque();
    if (R->msaa > 1) R->ctx->ResolveSubresource(R->resolveTex, 0, R->sceneTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D11_TEXTURE2D_DESC td;
    R->resolveTex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(R->dev->CreateTexture2D(&td, nullptr, &st))) return 1;
    R->ctx->CopyResource(st, R->resolveTex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(R->ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < h; ++y) {
            uint32_t sy = std::min<uint32_t>(td.Height - 1, (uint32_t)((y + 0.5f) * td.Height / h));
            const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + sy * m.RowPitch);
            for (int x = 0; x < w; ++x) {
                float fx = (((x + 0.5f) / w) - 0.5f) / g_wideK + 0.5f;
                uint32_t sx = std::min<uint32_t>(td.Width - 1, (uint32_t)(fx * td.Width));
                uint32_t p = row[sx];
                uint32_t r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255;
                out[y * w + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
        R->ctx->Unmap(st, 0);
    }
    st->Release();
    return 1;
}

EXPORT void hwAPI_cam2ScreenCoord(void* rcp, const float* cam, float* outX, float* outY) {
    void* pp = F<void*>(rcp, rc::Persp);
    float oow = 1.0f / cam[2];
    *outX = F<float>(pp, persp::ScaleX) * cam[0] * oow + F<float>(pp, persp::CenterX);
    *outY = F<float>(pp, persp::HeightF) - (F<float>(pp, persp::ScaleY) * cam[1] * oow + F<float>(pp, persp::CenterY));
}

EXPORT void hwAPI_DrawSkyBegin(void* rcp) {
    if (!R) return;
    R->inSky = true;
    void* o = F<void*>(rcp, rc::Opt);
    R->savedFog = F<int>(o, opt::Fog);
    F<int>(o, opt::Fog) = 0;
}

EXPORT void hwAPI_DrawSkyEnd(void* rcp) {
    if (!R) return;
    R->inSky = false;
    F<int>(F<void*>(rcp, rc::Opt), opt::Fog) = R->savedFog;
}

// ---- textures ----

EXPORT void hwAPI_TextureInit(void* rcp) {}

EXPORT void hwAPI_TextureExit(void* rcp) {
    if (!R) return;
    FlushOpaque();
    std::vector<Pls*> all;
    all.swap(R->live);
    for (Pls* p : all) {
        SafeRelease(p->srv);
        SafeRelease(p->tex);
        p->liveIndex = -1;
    }
}

EXPORT void hwAPI_AllocInitTexturePLS(void* rcp, uint8_t* t) {
    uint32_t frames = F<uint16_t>(t, tex::Frames);
    void* (*alloc)(uint32_t) = (AllocFn)F<void*>(rcp, rc::CbAlloc);
    uint8_t* arr = (uint8_t*)alloc(std::max(1u, frames) * kPlsStride);
    F<uint8_t*>(t, tex::Pls) = arr;
    if (!arr) return;
    for (uint32_t i = 0; i < frames; ++i) PlsInit((Pls*)(arr + i * kPlsStride), false, t, i);
}

EXPORT void hwAPI_ShutdownTexturePLS(void* rcp, uint8_t* t) {
    uint8_t* arr = F<uint8_t*>(t, tex::Pls);
    if (!arr) return;
    if (R) FlushOpaque();
    uint32_t frames = F<uint16_t>(t, tex::Frames);
    for (uint32_t i = 0; i < frames; ++i) PlsRelease((Pls*)(arr + i * kPlsStride));
    ((FreeFn)F<void*>(rcp, rc::CbFree))(arr);
    F<uint8_t*>(t, tex::Pls) = nullptr;
}

EXPORT void hwAPI_InitTexturePLS(void* rcp, uint8_t* l, Pls* p) {
    void* o = F<void*>(rcp, rc::Opt);
    if (!F<int>(o, opt::Lightmaps) || !p) return;
    PlsInit(p, true, l, 0);
}

EXPORT void hwAPI_UnInitTexturePLS(void* rcp, Pls* p) {
    if (!p || p->magic != kPlsMagic) return;
    if (R) FlushOpaque();
    PlsRelease(p);
}

EXPORT int hwAPI_SizeofPLS(void* rcp) {
    void* o = F<void*>(rcp, rc::Opt);
    return F<int>(o, opt::Lightmaps) ? (int)kPlsStride : 1;
}

EXPORT void hwAPI_TextureForceCache(void* rcp, uint8_t* t) {
    if (!R || !t) return;
    uint8_t* arr = F<uint8_t*>(t, tex::Pls);
    if (!arr) return;
    uint32_t frames = F<uint16_t>(t, tex::Frames);
    for (uint32_t i = 0; i < frames; ++i) {
        Pls* p = (Pls*)(arr + i * kPlsStride);
        if (p->magic == kPlsMagic && !p->srv) UploadTexture(t, p);
    }
}

EXPORT void hwAPI_ShadowMapForceCache(void* rcp, uint8_t* l) {
    if (!R || !l) return;
    Pls* p = F<Pls*>(l, lm::Pls);
    if (p && p->magic == kPlsMagic && !p->srv) UploadLightmap(l, p, F<void*>(l, lm::Data));
}

// ---- misc ----

EXPORT void hwAPI_BuildFogTable(void* rcp, uint32_t color, float nearW, float farW, int disable) {}

EXPORT int hwAPI_SetGammaCorrection(void* rcp) {
    float& g = F<float>(rcp, rc::Gamma);
    if (g < 0.5f) g = 0.5f;
    if (g > 4.0f) g = 4.0f;
    if (R) R->gamma = g;
    return 1;
}

EXPORT int hwAPI_CanSetGamma(void* rcp) { return 1; }
EXPORT int hwAPI_CanDoFogging(void* rcp) { return 1; }
EXPORT int hwAPI_CanBumpmap(void* rcp) { return 0; }
EXPORT int hwAPI_GetBumpmappingStyle(void* rcp) { return 0; }
EXPORT int hwAPI_CanUseTrueColor(void* rcp) { return g_cfg.trueColor ? 1 : 0; }
EXPORT int hwAPI_CanUseTextureCompression(void* rcp) { return 0; }
EXPORT int hwAPI_NeedsColorKey(void* rcp) { return 0; }
EXPORT void hwAPI_PolygonDebug(void* rcp) {}
EXPORT void hwAPI_ToggleShadowMaps(void* rcp) {}

// ============================================================================

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH];
        GetModuleFileNameA(inst, path, sizeof path);
        snprintf(g_dllPath, sizeof g_dllPath, "%s", path);
        char* slash = strrchr(path, '\\');
        snprintf(g_dllName, sizeof g_dllName, "%s", slash ? slash + 1 : path);
        if (slash) *slash = 0;
        snprintf(g_gameDir, sizeof g_gameDir, "%s", path);
        LoadConfig();
        crash::Install();
        if (const char* e = getenv("SZM_TEST_CRASH")) g_testCrashFrame = atoi(e);
        if (g_cfg.widescreen) {
            int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
            if (g_cfg.windowed) {
                w = g_cfg.width > 0 ? g_cfg.width : 1920;
                h = g_cfg.height > 0 ? g_cfg.height : 1080;
            }
            float k = (h > 0) ? ((float)w / h) / (4.0f / 3.0f) : 1.0f;
            if (k > 1.001f && InstallWidescreenHook()) g_wideK = std::min(k, 3.0f);
        }
    }
    if (reason == DLL_PROCESS_DETACH) {
        Log("process exiting normally");  // missing from the log = the process was killed or crashed
    }
    return TRUE;
}
