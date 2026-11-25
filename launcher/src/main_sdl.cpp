// Linux front end: SDL3 + OpenGL 3.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

SDL_Window* g_window = nullptr;
void LauncherSetupImGui(App& app, float scale);

int main(int, char**) {
    SDL_SetHint(SDL_HINT_APP_ID, "slavezero-launcher");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) scale = 1.0f;
    // debug: SZL_SHOT=file.ppm renders a few frames, saves a screenshot and exits; SZL_TAB selects a tab
    const char* shot = std::getenv("SZL_SHOT");
    if (const char* sc = std::getenv("SZL_SCALE")) scale = (float)std::atof(sc);
    g_window = SDL_CreateWindow("Slave Zero Launcher", (int)(1000 * scale), (int)(700 * scale),
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(g_window, (int)(820 * scale), (int)(560 * scale));
    SDL_GLContext gl = SDL_GL_CreateContext(g_window);
    SDL_GL_MakeCurrent(g_window, gl);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    App app;
    LauncherSetupImGui(app, scale);
    ImGui_ImplSDL3_InitForOpenGL(g_window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");
    app.Init();

    bool running = true;
    int frameNo = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window)) running = false;
        }
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!app.Frame()) running = false;
        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x),
                   (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y));
        glClearColor(0.045f, 0.06f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frameNo == 8) {
            int w = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), h = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
            std::vector<unsigned char> px((size_t)w * h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(shot, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; --y) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
                std::fclose(f);
            }
            running = false;
        }
        SDL_GL_SwapWindow(g_window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
