// In-game settings overlay. See overlay.h.
//
// Input: the game reads the keyboard through DirectInput (Scripts/wInputKBM.dll) and also polls
// GetAsyncKeyState / GetKeyState / GetCursorPos. While the overlay is open those calls report
// nothing (hooked in the game's own modules and in dinput's device vtable), window input
// messages go to ImGui only, and the DirectInput devices are unacquired so the cursor is free.
// Pause: the engine's frame timer is redirected; while paused, frames have a time step of 0.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "config.h"
#include "fonts.h"  // generated: kFontExo2 / kFontOrbitron
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace overlay {
namespace {

HWND g_hwnd;
WNDPROC g_oldProc;
bool g_ready = false;
bool g_open = false;
bool g_releaseGuard = false;  // keep blocking game input until the closing keys are released
unsigned g_apply = 0;
float g_scale = 1.0f;
ImFont* g_body = nullptr;
ImFont* g_title = nullptr;
Config g_start;             // settings at startup, to show which changes need a restart
LARGE_INTEGER g_qpcFreq, g_initTime;
bool g_viaLauncher = false;  // started by our launcher (it relaunches the game for us)
const char* g_help = nullptr;

bool InputBlocked() { return g_open || g_releaseGuard; }

// ---------------------------------------------------------------------------
// Import hooks in the game's modules
// ---------------------------------------------------------------------------

bool PatchImport(HMODULE mod, const char* dll, const char* fn, void* hook) {
    uint8_t* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, fn) != 0) continue;
            if ((void*)iat->u1.Function == hook) { done = true; continue; }
            DWORD old;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                iat->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                done = true;
            }
        }
    }
    return done;
}

// Pause: the engine's per-frame timer (FUN_0042d310, called once per frame from 0x40a3e8)
// busy-waits for at least one 1/120 s tick and stores the frame's time step at 0x4e8494. While
// paused, the call is answered with a step of 0 and the game clock follows real time, so the
// game keeps rendering, nothing moves, and nothing jumps when it resumes.
bool g_paused = false;
const uintptr_t kFrameTimer = 0x42d310, kFrameTimerCall = 0x40a3e8, kClockTicks = 0x42d290;
void __cdecl FrameTimerHook() {
    if (!g_paused) {
        ((void(__cdecl*)())kFrameTimer)();
        return;
    }
    if (*(int*)0x4dbd00 == 0) {  // real-time mode (the others step by fixed amounts)
        float now = (float)((long double(__cdecl*)())kClockTicks)();
        *(float*)0x4e84b0 = now;  // game time
        *(float*)0x4e8490 = now;  // previous frame's game time
    }
    *(float*)0x4e8494 = 0.0f;  // this frame's step
}
bool InstallPauseHook() {
    static const uint8_t kProlog[] = {0xa1, 0x00, 0xbd, 0x4d, 0x00, 0x85, 0xc0};
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)kFrameTimer, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (memcmp((void*)kFrameTimer, kProlog, sizeof kProlog) != 0) return false;
    const uint8_t* b = (const uint8_t*)kFrameTimerCall;
    if (b[0] != 0xe8 || kFrameTimerCall + 5 + *(const int32_t*)(b + 1) != kFrameTimer) return false;
    DWORD old;
    VirtualProtect((void*)kFrameTimerCall, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t*)(kFrameTimerCall + 1) = (int32_t)((uintptr_t)&FrameTimerHook - (kFrameTimerCall + 5));
    VirtualProtect((void*)kFrameTimerCall, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}
bool g_canPause = false;
void SetPaused(bool on) { g_paused = on && g_canPause; }

// keyboard / mouse polling
POINT g_cursorFrozen;
SHORT WINAPI HookGetAsyncKeyState(int vk) { return InputBlocked() ? 0 : GetAsyncKeyState(vk); }
SHORT WINAPI HookGetKeyState(int vk) { return InputBlocked() ? 0 : GetKeyState(vk); }
BOOL WINAPI HookGetCursorPos(POINT* p) {
    if (InputBlocked() && p) { *p = g_cursorFrozen; return TRUE; }
    return GetCursorPos(p);
}
BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (InputBlocked()) return TRUE;
    return SetCursorPos(x, y);
}

// DirectInput devices: vtable hooks (Acquire 7, Unacquire 8, GetDeviceState 9, GetDeviceData 10)
typedef HRESULT(WINAPI* DevFn)(void*);
typedef HRESULT(WINAPI* GetStateFn)(void*, DWORD, void*);
typedef HRESULT(WINAPI* GetDataFn)(void*, DWORD, void*, DWORD*, DWORD);
struct VtblHooks {
    void** vtbl;
    DevFn acquire, unacquire;
    GetStateFn getState;
    GetDataFn getData;
};
std::vector<VtblHooks> g_vtbls;
std::vector<void*> g_devices;  // devices the game used, to unacquire / reacquire around the overlay

const VtblHooks* HooksFor(void* self) {
    void** v = *(void***)self;
    for (auto& h : g_vtbls)
        if (h.vtbl == v) return &h;
    return nullptr;
}
void Remember(void* self) {
    if (std::find(g_devices.begin(), g_devices.end(), self) == g_devices.end()) g_devices.push_back(self);
}
HRESULT WINAPI HookAcquire(void* self) {
    const VtblHooks* h = HooksFor(self);
    Remember(self);
    if (InputBlocked()) return S_OK;  // stay unacquired while the overlay is open
    return h ? h->acquire(self) : E_FAIL;
}
HRESULT WINAPI HookGetDeviceState(void* self, DWORD cb, void* data) {
    const VtblHooks* h = HooksFor(self);
    Remember(self);
    if (InputBlocked()) {
        if (data) memset(data, 0, cb);
        return S_OK;
    }
    return h ? h->getState(self, cb, data) : E_FAIL;
}
HRESULT WINAPI HookGetDeviceData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
    const VtblHooks* h = HooksFor(self);
    Remember(self);
    if (InputBlocked()) {
        if (inOut) *inOut = 0;
        return S_OK;
    }
    return h ? h->getData(self, cbObj, rgdod, inOut, flags) : E_FAIL;
}

void HookVtbl(void** v) {
    for (auto& h : g_vtbls)
        if (h.vtbl == v) return;
    VtblHooks h = {v, (DevFn)v[7], (DevFn)v[8], (GetStateFn)v[9], (GetDataFn)v[10]};
    DWORD old;
    if (!VirtualProtect(v + 7, 4 * sizeof(void*), PAGE_READWRITE, &old)) return;
    v[7] = (void*)&HookAcquire;
    v[9] = (void*)&HookGetDeviceState;
    v[10] = (void*)&HookGetDeviceData;
    VirtualProtect(v + 7, 4 * sizeof(void*), old, &old);
    g_vtbls.push_back(h);
}

// Device vtables live in dinput.dll, so hooking our own throwaway devices covers the game's.
void HookDirectInput() {
    HMODULE di = LoadLibraryA("dinput.dll");
    if (!di) return;
    typedef HRESULT(WINAPI * CreateFn)(HINSTANCE, DWORD, void**, void*);
    auto create = (CreateFn)GetProcAddress(di, "DirectInputCreateA");
    if (!create) return;
    void* dinput = nullptr;
    if (FAILED(create(GetModuleHandleA(nullptr), 0x0500, &dinput, nullptr)) || !dinput) return;
    static const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const GUID kDevice2A = {0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    typedef HRESULT(WINAPI * CreateDeviceFn)(void*, const GUID&, void**, void*);
    typedef HRESULT(WINAPI * QiFn)(void*, const GUID&, void**);
    typedef ULONG(WINAPI * ReleaseFn)(void*);
    void** div = *(void***)dinput;
    for (const GUID* g : {&kSysKeyboard, &kSysMouse}) {
        void* dev = nullptr;
        if (FAILED(((CreateDeviceFn)div[3])(dinput, *g, &dev, nullptr)) || !dev) continue;
        HookVtbl(*(void***)dev);
        void* dev2 = nullptr;
        if (SUCCEEDED(((QiFn)(*(void***)dev)[0])(dev, kDevice2A, &dev2)) && dev2) {
            HookVtbl(*(void***)dev2);
            ((ReleaseFn)(*(void***)dev2)[2])(dev2);
        }
        ((ReleaseFn)(*(void***)dev)[2])(dev);
    }
    ((ReleaseFn)div[2])(dinput);
    Log("overlay: hooked %u DirectInput device vtable(s)", (unsigned)g_vtbls.size());
}

// Hooks every module loaded from the game folder (the exe, MRGPlay.dll, Scripts/*.dll), except
// ourselves. Scripts load per level, so this runs again whenever the overlay opens.
void HookGameModules() {
    HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&HookGameModules, &self);
    size_t dirLen = strlen(g_gameDir);
    for (DWORD i = 0; i < std::min<DWORD>(need / sizeof(HMODULE), 512); ++i) {
        char path[MAX_PATH];
        if (mods[i] == self || !GetModuleFileNameA(mods[i], path, sizeof path)) continue;
        if (_strnicmp(path, g_gameDir, dirLen) != 0 || path[dirLen] != '\\') continue;
        PatchImport(mods[i], "user32.dll", "GetAsyncKeyState", (void*)&HookGetAsyncKeyState);
        PatchImport(mods[i], "user32.dll", "GetKeyState", (void*)&HookGetKeyState);
        PatchImport(mods[i], "user32.dll", "GetCursorPos", (void*)&HookGetCursorPos);
        PatchImport(mods[i], "user32.dll", "SetCursorPos", (void*)&HookSetCursorPos);
    }
}

// ---------------------------------------------------------------------------
// Window messages and the open/close state
// ---------------------------------------------------------------------------

bool IsInputMessage(UINT m) {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP) && (int)w == g_cfg.overlayKey)
        return 0;  // ours (and F10 would otherwise open the window menu)
    if (g_open) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        if (IsInputMessage(m)) return 0;
    } else if (g_releaseGuard && IsInputMessage(m)) {
        return 0;
    }
    return CallWindowProcA(g_oldProc, h, m, w, l);
}

void SetOpen(bool on) {
    if (on == g_open) return;
    if (on) {
        HookGameModules();
        GetCursorPos(&g_cursorFrozen);
        for (void* d : g_devices)
            if (const VtblHooks* h = HooksFor(d)) h->unacquire(d);
        if (g_cfg.pauseInOverlay) SetPaused(true);
        // start with the cursor in the middle of the window, where the panel is
        RECT r;
        GetClientRect(g_hwnd, &r);
        POINT c = {(r.right - r.left) / 2, (r.bottom - r.top) / 2};
        ClientToScreen(g_hwnd, &c);
        SetCursorPos(c.x, c.y);
        ImGui::GetIO().AddMousePosEvent((float)((r.right - r.left) / 2), (float)((r.bottom - r.top) / 2));
        Log("overlay opened");
    } else {
        g_releaseGuard = true;
        SetCursorPos(g_cursorFrozen.x, g_cursorFrozen.y);
        Log("overlay closed");
    }
    g_open = on;
}

// XInput, for the Back+Start chord when the overlay is closed
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput;
WORD PadButtons() {
    static DWORD lastScan = 0;
    static bool connected[4];
    if (!g_xinput) return 0;
    DWORD now = GetTickCount();
    bool rescan = now - lastScan > 2000;
    if (rescan) lastScan = now;
    WORD b = 0;
    for (DWORD i = 0; i < 4; ++i) {
        if (!connected[i] && !rescan) continue;
        XINPUT_STATE st = {};
        connected[i] = g_xinput(i, &st) == ERROR_SUCCESS;
        if (connected[i]) b |= st.Gamepad.wButtons;
    }
    return b;
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---------------------------------------------------------------------------
// Leaving the game
// ---------------------------------------------------------------------------

void TouchFlag(const char* name) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", g_gameDir, name);
    if (FILE* f = fopen(path, "w")) fclose(f);
}

DWORD WINAPI ExitWatchdog(LPVOID) {
    Sleep(3000);
    ExitProcess(0);
}

// Asks the game to close; if it has not gone a few seconds later, ends the process.
void ExitGame() {
    SaveConfig();
    Log("overlay: leaving the game");
    PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    CreateThread(nullptr, 0, &ExitWatchdog, nullptr, 0, nullptr);
}

void RestartGame() {
    if (g_viaLauncher) {
        TouchFlag("szmodern.restart");  // the launcher starts the game again once this one exits
    } else {
        // no launcher waiting: a helper process waits for us to exit and starts the game again
        char cmd[MAX_PATH * 2 + 64];
        snprintf(cmd, sizeof cmd, "rundll32.exe \"%s\",SzmRestartGame %lu", g_dllPath, GetCurrentProcessId());
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            Log("overlay: could not start the restart helper (%lu)", GetLastError());
        }
    }
    ExitGame();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

const ImVec4 kAccent(0.44f, 0.88f, 0.29f, 1.0f), kMuted(0.55f, 0.63f, 0.56f, 1.0f), kWarn(0.95f, 0.75f, 0.30f, 1.0f);

void SetupStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowPadding = ImVec2(24, 20);
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.CellPadding = ImVec2(8, 6);
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowRounding = 8;
    st.ChildRounding = 6;
    st.PopupRounding = 4;
    st.ScrollbarRounding = 4;
    st.WindowBorderSize = 1;
    st.FrameBorderSize = 1;
    st.ScaleAllSizes(g_scale);
    ImVec4* c = st.Colors;
    const ImVec4 bg(0.035f, 0.05f, 0.04f, 0.97f), panel(0.08f, 0.11f, 0.09f, 1.0f), edge(0.18f, 0.28f, 0.19f, 1.0f);
    const ImVec4 accDim(0.22f, 0.45f, 0.15f, 1.0f), accLow(0.14f, 0.27f, 0.11f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.86f, 0.94f, 0.85f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.49f, 0.43f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.07f, 0.06f, 1.0f);
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = edge;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = accLow;
    c[ImGuiCol_FrameBgActive] = accDim;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = accDim;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = accLow;
    c[ImGuiCol_ButtonHovered] = accDim;
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_Header] = accLow;
    c[ImGuiCol_HeaderHovered] = accDim;
    c[ImGuiCol_HeaderActive] = accDim;
    c[ImGuiCol_Separator] = edge;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = accDim;
    c[ImGuiCol_TabSelected] = accLow;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
}

// Two-column settings table; the hovered or focused setting's description shows on the right.
bool g_changed = false;
void Label(const char* text, bool needsRestart = false) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    if (needsRestart) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "*");
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
void Help(const char* text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused() || ImGui::IsItemActive()) g_help = text;
}
void Section(const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
    ImGui::TextColored(kAccent, "%s", text);
}
bool BeginForm(const char* id) {
    return ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
}
bool Combo(const char* id, int* v, const char* const* names, int n) {
    bool r = ImGui::Combo(id, v, names, n);
    g_changed |= r;
    return r;
}
bool Check(const char* id, int* v) {
    bool b = *v != 0;
    bool r = ImGui::Checkbox(id, &b);
    if (r) { *v = b ? 1 : 0; g_changed = true; }
    return r;
}
template <class T> int IndexOf(const T* arr, int n, T v) {
    for (int i = 0; i < n; ++i)
        if (arr[i] == v) return i;
    return 0;
}

void DrawDisplay() {
    if (!BeginForm("display")) return;
    Config& c = g_cfg;
    Section("Screen");
    Label("Display mode", true);
    {
        const char* n[] = {"Fullscreen (borderless)", "Windowed"};
        Combo("##mode", &c.windowed, n, 2);
        Help("Borderless fullscreen covers the monitor at its native resolution. Windowed uses the size below.");
    }
    if (c.windowed) {
        Label("Window size", true);
        static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
        int mw = GetSystemMetrics(SM_CXSCREEN), mh = GetSystemMetrics(SM_CYSCREEN);
        char cur[48];
        snprintf(cur, sizeof cur, "%d x %d", c.width > 0 ? c.width : 1920, c.height > 0 ? c.height : 1080);
        if (ImGui::BeginCombo("##winsize", cur)) {
            for (auto& sz : sizes) {
                if (sz[0] > mw || sz[1] > mh) continue;
                char l[48];
                snprintf(l, sizeof l, "%d x %d", sz[0], sz[1]);
                if (ImGui::Selectable(l, c.width == sz[0] && c.height == sz[1])) { c.width = sz[0]; c.height = sz[1]; g_changed = true; }
            }
            ImGui::EndCombo();
        }
        Help("Size of the game window.");
    }
    Label("Aspect ratio", true);
    {
        int v = c.widescreen ? 0 : 1;
        const char* n[] = {"Widescreen  (wider field of view)", "Original 4:3  (side bars)"};
        if (Combo("##aspect", &v, n, 2)) c.widescreen = v == 0;
        Help("Widescreen shows more to the left and right (Hor+). 4:3 is the original view with black bars.");
    }
    ImGui::BeginDisabled(!c.widescreen);
    Label("HUD position");
    {
        const char* n[] = {"Original  (4:3 area)", "Screen edges"};
        Combo("##hud", &c.hudEdges, n, 2);
        Help("Moves the health and ammo gauges to the corners of a widescreen display. The crosshair and "
             "lock-on markers are unaffected.");
    }
    Label("Menus in 4:3");
    Check("##menus43", &c.menuPillarbox);
    Help("The main menu and intro were only built for a 4:3 view; this shows them with side bars instead of empty space.");
    ImGui::EndDisabled();

    Section("Frame rate");
    Label("VSync");
    Check("##vsync", &c.vsync);
    Help("Waits for the display's refresh so the picture never tears.");
    Label("Frame rate limit");
    {
        static const int caps[] = {0, 30, 60, 75, 90, 120, 144, 165, 240};
        char cur[32];
        if (c.frameLimit > 0) snprintf(cur, sizeof cur, "%d fps", c.frameLimit);
        else snprintf(cur, sizeof cur, "Unlimited");
        if (ImGui::BeginCombo("##fps", cur)) {
            for (int cap : caps) {
                char l[32];
                if (cap) snprintf(l, sizeof l, "%d fps", cap);
                else snprintf(l, sizeof l, "Unlimited");
                if (ImGui::Selectable(l, c.frameLimit == cap)) { c.frameLimit = cap; g_changed = true; }
            }
            ImGui::EndCombo();
        }
        Help("Use 60 if anything in the game feels too fast at high refresh rates.");
    }
    ImGui::EndTable();
}

void DrawGraphics(float* gamma) {
    if (!BeginForm("graphics")) return;
    Config& c = g_cfg;
    Section("Image quality");
    Label("Render resolution");
    {
        static const float scales[] = {0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};
        static const char* names[] = {"50%", "67%", "75%", "100%  (native)", "150%  (supersampled)", "200%  (supersampled)"};
        int i = 3;
        for (int k = 0; k < 6; ++k)
            if (fabsf(c.renderScale - scales[k]) < 0.01f) i = k;
        if (Combo("##rscale", &i, names, 6)) { c.renderScale = scales[i]; g_apply |= ApplyTargets; }
        Help("Resolution the 3D scene is drawn at before it is scaled to the screen. Above 100% is supersampling: "
             "sharper edges and textures at a GPU cost.");
    }
    Label("Anti-aliasing");
    {
        static const int v[] = {1, 2, 4, 8};
        const char* n[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        int i = IndexOf(v, 4, c.msaa);
        if (Combo("##msaa", &i, n, 4)) { c.msaa = v[i]; g_apply |= ApplyTargets; }
        Help("Smooths the edges of 3D geometry.");
    }
    Label("Texture filtering");
    {
        static const int v[] = {1, 2, 4, 8, 16};
        const char* n[] = {"Trilinear", "2x anisotropic", "4x anisotropic", "8x anisotropic", "16x anisotropic"};
        int i = IndexOf(v, 5, c.anisotropy);
        if (Combo("##aniso", &i, n, 5)) { c.anisotropy = v[i]; g_apply |= ApplySamplers; }
        Help("Keeps floors and walls sharp at shallow angles.");
    }
    Label("Sharpening");
    if (ImGui::SliderFloat("##sharpen", &c.sharpen, 0.0f, 1.0f, c.sharpen <= 0.001f ? "Off" : "%.2f")) g_changed = true;
    Help("Contrast-adaptive sharpening of the final image.");

    Section("Textures and lighting");
    Label("HD texture pack", true);
    Check("##pack", &c.texturePack);
    Help("Uses the AI-upscaled textures in szmodern/textures (build them from the launcher).");
    Label("Per-pixel lighting", true);
    Check("##nrm", &c.normalMaps);
    Help("Lights surfaces that have normal maps in the texture pack (the player's mech) per pixel.");
    Label("True-colour textures", true);
    Check("##tc", &c.trueColor);
    Help("Keeps the game's 32-bit textures (effects, glows) at full precision instead of 16-bit.");
    Label("Generate mipmaps", true);
    Check("##mips", &c.generateMips);
    Help("Builds smaller texture levels so distant surfaces do not shimmer.");
    Label("See-through details");
    Check("##cutout", &c.opaqueCutout);
    Help("Draws fences, grates and railings with their holes, as on period hardware.");
    Label("Lightmap intensity");
    if (ImGui::SliderFloat("##lm", &c.lightmapScale, 1.0f, 2.0f, "%.2f")) g_changed = true;
    Help("2.0 matches the original Direct3D renderer. 1.0 matches Voodoo2-class Glide cards (darker).");
    Label("Brightness (gamma)");
    if (gamma) ImGui::SliderFloat("##gamma", gamma, 0.5f, 3.0f, "%.2f");
    Help("Same as the in-game gamma slider; the game saves it with its own options. The default is 1.87.");
    ImGui::EndTable();
}

void DrawFilter() {
    if (!BeginForm("filter")) return;
    Config& c = g_cfg;
    Section("Screen filter");
    Label("Filter");
    {
        const char* n[] = {"Off", "Scanlines", "CRT (aperture grille)", "CRT (curved screen)", "Retro 640x480 pixels", "VHS tape"};
        Combo("##filter", &c.filter, n, 6);
        Help("A period look for the final image. F9 cycles through the filters while you play.");
    }
    ImGui::BeginDisabled(c.filter == 0);
    Label("Strength");
    if (ImGui::SliderFloat("##fstr", &c.filterStrength, 0.1f, 1.0f, "%.2f")) g_changed = true;
    Help("How strongly the filter is applied.");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(c.filter < 1 || c.filter > 3);
    Label("Scanline count");
    {
        static const int v[] = {240, 360, 480, 540, 720};
        const char* n[] = {"240  (arcade / low-res)", "360", "480  (original VGA)", "540", "720"};
        int i = IndexOf(v, 5, c.scanlines);
        if (Combo("##lines", &i, n, 5)) c.scanlines = v[i];
        Help("Number of scanlines across the screen height.");
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
}

void DrawOverlayTab() {
    if (!BeginForm("overlay")) return;
    Config& c = g_cfg;
    Section("This menu");
    Label("Open with");
    {
        const char* n[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        int i = std::min(std::max(c.overlayKey - VK_F1, 0), 11);
        if (Combo("##key", &i, n, 12)) c.overlayKey = VK_F1 + i;
        Help("Key that opens and closes this menu. On a gamepad, press Back and Start together.");
    }
    Label("Pause the game");
    Check("##pause", &c.pauseInOverlay);
    Help("Stops the game while this menu is open.");
    Label("Show the hint at startup");
    Check("##hint", &c.overlayHint);
    Help("Shows which key opens this menu for a few seconds after the game starts.");
    Section("Launcher");
    Label("Start the game directly");
    Check("##direct", &c.startDirectly);
    Help("Skips the launcher window when you start the game; everything is adjustable here instead. "
         "To see the launcher again, untick this, use \"Open launcher\" below, or start it with --launcher.");
    ImGui::EndTable();
}

bool RestartPending() {
    const Config& a = g_cfg;
    const Config& b = g_start;
    return a.windowed != b.windowed || (a.windowed && (a.width != b.width || a.height != b.height)) ||
           a.widescreen != b.widescreen || a.texturePack != b.texturePack || a.normalMaps != b.normalMaps ||
           a.trueColor != b.trueColor || a.generateMips != b.generateMips;
}

void DrawPanel(int outW, int outH, float* gamma, bool inMenu) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    ImVec2 size(std::min(1500.0f * g_scale, outW * 0.94f), std::min(780.0f * g_scale, outH * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(outW * 0.5f, outH * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("##szoverlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    // header
    ImGui::PushFont(g_title, g_title->LegacySize);
    ImGui::TextColored(kAccent, "SLAVE ZERO");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "  settings%s", g_paused ? "   (game paused)" : "");
    {
        char keyName[8];
        snprintf(keyName, sizeof keyName, "F%d", g_cfg.overlayKey - VK_F1 + 1);
        char hint[64];
        snprintf(hint, sizeof hint, "%s / Esc  close", keyName);
        float w = ImGui::CalcTextSize(hint).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w);
        ImGui::TextColored(kMuted, "%s", hint);
    }
    ImGui::Separator();

    // body: settings on the left, description on the right
    float footerH = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2;
    float bodyH = ImGui::GetContentRegionAvail().y - footerH;
    float helpW = ImGui::GetContentRegionAvail().x * 0.34f;
    g_help = nullptr;
    ImGui::BeginChild("##settings", ImVec2(ImGui::GetContentRegionAvail().x - helpW - ImGui::GetStyle().ItemSpacing.x, bodyH),
                      ImGuiChildFlags_NavFlattened);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Display")) { DrawDisplay(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Graphics")) { DrawGraphics(gamma); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Screen filter")) { DrawFilter(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Menu & launcher")) { DrawOverlayTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##help", ImVec2(helpW, bodyH), ImGuiChildFlags_Borders);
    if (g_help) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(g_help);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(kMuted, "Point at a setting to see what it does.");
    }
    ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kMuted, "Changes are saved as you make them and apply at once.");
    ImGui::TextColored(kWarn, "* applies when the game restarts.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // footer
    ImGui::Separator();
    bool pending = RestartPending();
    if (pending) ImGui::TextColored(kWarn, "Restart the game to apply the changes marked *.");
    else ImGui::TextColored(kMuted, inMenu ? "Restarting returns to the main menu." : "Restarting returns to the main menu; save your progress first.");
    if (ImGui::Button("Resume")) SetOpen(false);
    ImGui::SameLine();
    if (pending) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.36f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.49f, 0.12f, 1.0f));
    }
    if (ImGui::Button("Restart game")) RestartGame();
    if (pending) ImGui::PopStyleColor(2);
    if (g_viaLauncher) {
        ImGui::SameLine();
        if (ImGui::Button("Open launcher")) {
            TouchFlag("szmodern.launcher");
            ExitGame();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) ExitGame();
    ImGui::End();

    // Esc or gamepad B (with nothing being edited) closes the menu
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!popup && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        SetOpen(false);
}

void DrawHint(int outW, int outH) {
    char text[64];
    snprintf(text, sizeof text, "F%d   Settings", g_cfg.overlayKey - VK_F1 + 1);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(text);
    float pad = 14 * g_scale;
    ImVec2 p0(outW * 0.5f - ts.x * 0.5f - pad, outH - ts.y - pad * 2 - 40 * g_scale);
    ImVec2 p1(p0.x + ts.x + pad * 2, p0.y + ts.y + pad * 1.4f);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 14, 10, 200), 18 * g_scale);
    dl->AddRect(p0, p1, IM_COL32(60, 110, 50, 200), 18 * g_scale, 0, 1.5f * g_scale);
    dl->AddText(ImVec2(p0.x + pad, p0.y + pad * 0.7f), IM_COL32(200, 235, 195, 255), text);
}

}  // namespace

// ---------------------------------------------------------------------------

void Init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, int outH) {
    if (g_ready) return;
    g_hwnd = hwnd;
    g_start = g_cfg;
    char env[8];
    g_viaLauncher = GetEnvironmentVariableA("SZM_LAUNCHER", env, sizeof env) > 0;
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_initTime);
    g_scale = std::min(std::max(outH / 1080.0f, 1.0f), 3.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.MouseDrawCursor = true;  // the renderer hides the system cursor
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    g_body = io.Fonts->AddFontFromMemoryTTF((void*)kFontExo2, (int)kFontExo2Size, 21.0f * g_scale, &fc);
    g_title = io.Fonts->AddFontFromMemoryTTF((void*)kFontOrbitron, (int)kFontOrbitronSize, 30.0f * g_scale, &fc);
    io.FontDefault = g_body;
    SetupStyle();
    if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX11_Init(dev, ctx)) {
        Log("overlay: ImGui initialisation failed");
        ImGui::DestroyContext();
        return;
    }
    for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(name)) {
            g_xinput = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
            if (g_xinput) break;
        }
    HookDirectInput();
    HookGameModules();
    g_canPause = InstallPauseHook();
    g_oldProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    g_ready = true;
    Log("overlay ready (key F%d%s%s)", g_cfg.overlayKey - VK_F1 + 1, g_canPause ? "" : ", pause unavailable",
        g_viaLauncher ? ", started by the launcher" : "");
}

void Shutdown() {
    if (!g_ready) return;
    SetPaused(false);
    if (g_oldProc && IsWindow(g_hwnd)) SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
    g_open = false;
}

bool IsOpen() { return g_open; }

unsigned TakeApply() {
    unsigned a = g_apply;
    g_apply = 0;
    return a;
}

void Render(ID3D11RenderTargetView* rtv, int outW, int outH, float* gamma, bool inMenu) {
    if (!g_ready) return;
    // toggle: the hotkey (edge) or Back+Start on a gamepad
    bool focused = GetForegroundWindow() == g_hwnd;
    static bool keyWas = false, padWas = false;
    bool key = focused && KeyDown(g_cfg.overlayKey);
    WORD pad = focused ? PadButtons() : 0;
    bool chord = (pad & XINPUT_GAMEPAD_BACK) && (pad & XINPUT_GAMEPAD_START);
    if ((key && !keyWas) || (chord && !padWas)) SetOpen(!g_open);
    keyWas = key;
    padWas = chord;
    if (g_releaseGuard && !g_open) {
        bool held = KeyDown(VK_ESCAPE) || KeyDown(VK_RETURN) || KeyDown(VK_SPACE) || KeyDown(VK_LBUTTON) ||
                    KeyDown(VK_RBUTTON) || KeyDown(g_cfg.overlayKey) ||
                    (pad & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK));
        if (!held) {
            g_releaseGuard = false;
            SetPaused(false);
            for (void* d : g_devices)
                if (const VtblHooks* h = HooksFor(d)) h->acquire(d);
        }
    }

    static int testFrame = -1, frameNo = 0;  // tests: SZM_TEST_RESTART=<frame> presses "Restart game"
    if (testFrame < 0) {
        char env[16];
        testFrame = GetEnvironmentVariableA("SZM_TEST_RESTART", env, sizeof env) ? atoi(env) : 0;
    }
    static int applyFrame = -1;  // tests: SZM_TEST_APPLY=<frame> changes MSAA, render scale and filtering live
    if (applyFrame < 0) {
        char env[16];
        applyFrame = GetEnvironmentVariableA("SZM_TEST_APPLY", env, sizeof env) ? atoi(env) : 0;
    }
    ++frameNo;
    if (testFrame > 0 && frameNo == testFrame) RestartGame();
    if (applyFrame > 0 && frameNo == applyFrame) {
        g_cfg.msaa = 2;
        g_cfg.renderScale = 1.5f;
        g_cfg.anisotropy = 4;
        g_apply |= ApplyTargets | ApplySamplers;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double since = double(now.QuadPart - g_initTime.QuadPart) / g_qpcFreq.QuadPart;
    bool hint = g_cfg.overlayHint && !g_open && since > 1.0 && since < 9.0;
    if (!g_open && !hint) return;

    g_changed = false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_open;
    if (g_open) DrawPanel(outW, outH, gamma, inMenu);
    else DrawHint(outW, outH);
    ImGui::Render();
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Device* dev = nullptr;
    rtv->GetDevice(&dev);
    dev->GetImmediateContext(&ctx);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->Release();
    dev->Release();
    if (g_changed || g_apply) SaveConfig();
}

}  // namespace overlay

// rundll32 entry: waits for the given game process to exit, then starts the game again.
extern "C" __declspec(dllexport) void CALLBACK SzmRestartGame(HWND, HINSTANCE, LPSTR cmdLine, int) {
    DWORD pid = (DWORD)strtoul(cmdLine ? cmdLine : "0", nullptr, 10);
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, 20000);
        CloseHandle(h);
    }
    char exe[MAX_PATH];
    snprintf(exe, sizeof exe, "%s\\SlaveZero.exe", g_gameDir);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    char cmd[MAX_PATH + 4];
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    if (CreateProcessA(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
