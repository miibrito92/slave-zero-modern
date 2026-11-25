// In-game settings overlay (Dear ImGui on the renderer's D3D11 device).
// Opened with F10 (configurable) or Back+Start on a gamepad. While it is open the game
// gets no input and, optionally, its clock stands still.
#pragma once
#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace overlay {
enum Apply : unsigned { ApplyTargets = 1, ApplySamplers = 2 };

void Init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, int outH);
void Shutdown();
// Call once per frame after the scene is composited, before Present. `gamma` is the
// engine's gamma value (edited live by the brightness slider).
void Render(ID3D11RenderTargetView* rtv, int outW, int outH, float* gamma, bool inMenu);
bool IsOpen();
// Resources the renderer must rebuild after settings changed (Apply flags).
unsigned TakeApply();
}  // namespace overlay
