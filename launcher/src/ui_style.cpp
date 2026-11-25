// Theme + fonts shared by both front ends.
#include "imgui.h"
#include "app.h"
#include "fonts.h"  // generated: kFontExo2 / kFontOrbitron

void LauncherSetupImGui(App& app, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* body = io.Fonts->AddFontFromMemoryTTF((void*)kFontExo2, (int)kFontExo2Size, 19.0f, &cfg);
    ImFont* title = io.Fonts->AddFontFromMemoryTTF((void*)kFontOrbitron, (int)kFontOrbitronSize, 34.0f, &cfg);
    ImFont* big = io.Fonts->AddFontFromMemoryTTF((void*)kFontOrbitron, (int)kFontOrbitronSize, 24.0f, &cfg);
    io.FontDefault = body;
    app.SetFonts(body, title, big);

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowPadding = ImVec2(22, 18);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.CellPadding = ImVec2(6, 5);
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.ScrollbarRounding = 4;
    st.WindowBorderSize = 0;
    st.FrameBorderSize = 1;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;

    ImVec4* c = st.Colors;
    const ImVec4 bg(0.045f, 0.06f, 0.05f, 1.0f), panel(0.08f, 0.11f, 0.09f, 1.0f), edge(0.18f, 0.28f, 0.19f, 1.0f);
    const ImVec4 acc(0.44f, 0.88f, 0.29f, 1.0f), accDim(0.22f, 0.45f, 0.15f, 1.0f), accLow(0.14f, 0.27f, 0.11f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.84f, 0.93f, 0.83f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.49f, 0.43f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = edge;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = accLow;
    c[ImGuiCol_FrameBgActive] = accDim;
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_SliderGrab] = accDim;
    c[ImGuiCol_SliderGrabActive] = acc;
    c[ImGuiCol_Button] = accLow;
    c[ImGuiCol_ButtonHovered] = accDim;
    c[ImGuiCol_ButtonActive] = acc;
    c[ImGuiCol_Header] = accLow;
    c[ImGuiCol_HeaderHovered] = accDim;
    c[ImGuiCol_HeaderActive] = accDim;
    c[ImGuiCol_Separator] = edge;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = accDim;
    c[ImGuiCol_TabSelected] = accLow;
    c[ImGuiCol_TabSelectedOverline] = acc;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = accLow;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
}
