// Windows front end: Win32 + Direct3D 11.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <objbase.h>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
void LauncherSetupImGui(App& app, float scale);

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_swap;
static ID3D11RenderTargetView* g_rtv;
static UINT g_resizeW, g_resizeH;

static void CreateRtv() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
            return 0;
        case WM_GETMINMAXINFO: {
            float s = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
            ((MINMAXINFO*)lp)->ptMinTrackSize = {(LONG)(840 * s), (LONG)(600 * s)};
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, L"APPICON"), LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"SlaveZeroLauncher", nullptr};
    RegisterClassExW(&wc);
    int w = (int)(1000 * scale), h = (int)(700 * scale);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Slave Zero Launcher", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                              inst, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"Slave Zero Launcher", MB_ICONERROR);
        return 1;
    }
    CreateRtv();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    App app;
    if (cmdLine && wcsstr(cmdLine, L"--launcher")) app.ForceWindow();
    LauncherSetupImGui(app, scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    app.Init();

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (!app.Tick()) break;
        {  // hidden while the game runs and when starting the game directly
            bool hide = app.WindowHidden();
            bool visible = IsWindowVisible(hwnd) != 0;
            if (hide && visible) ShowWindow(hwnd, SW_HIDE);
            if (!hide && !visible) {
                ShowWindow(hwnd, SW_SHOWDEFAULT);
                SetForegroundWindow(hwnd);
            }
            if (hide) {
                Sleep(100);
                continue;
            }
        }
        if (g_resizeW && g_resizeH) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRtv();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (!app.Frame()) running = false;
        ImGui::Render();
        const float clear[4] = {0.045f, 0.06f, 0.05f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    g_swap->Release();
    g_ctx->Release();
    g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
